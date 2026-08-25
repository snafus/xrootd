#!/usr/bin/env bash
#
# WP-15: two-gateway matrix.  The resume matrix restarts ONE server; this
# runs TWO live xrootd instances (A and B) exporting the SAME data
# directory -- the actual multi-gateway shared-filesystem deployment the
# journal lease (FR-22, XRD-6) exists for:
#
#   1. Writer exclusion across processes: a transfer runs on A; the same
#      destination COPY'd via B gets 409 + Retry-After, journal untouched
#      (WP-14/C2's lease-first gate, cross-process).
#   2. X-Resume: F via B while A writes: still 409 (FR-4 yields to FR-22).
#   3. Cross-gateway resume: A is kill -9'd mid-transfer; after the lease
#      lapses, the retry lands on B -> RESUME_START from A's watermark,
#      content exact, journal gone.
#
# Local ext4 stands in for the shared filesystem: both instances see the
# same bytes and the same rename semantics, which is what the journal
# protocol consumes.  (Real CephFS/NFS behavior remains a site-validation
# item -- Q-3.)
#
# Usage: multi_gateway.sh <build_dir>   (exit 127 = missing prerequisites)

set -u
BUILD_DIR="${1:?usage: multi_gateway.sh <build_dir>}"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
LIB_DIR="$BUILD_DIR/lib"
XRDHTTP_LIB=$(ls "$LIB_DIR"/libXrdHttp-[0-9]*.so 2>/dev/null | head -1)
TPCR_LIB=$(ls "$LIB_DIR"/libXrdHttpTPCR-[0-9]*.so 2>/dev/null | head -1)
[ -n "$XRDHTTP_LIB" ] || { echo "SKIP: libXrdHttp plugin missing"; exit 127; }
[ -n "$TPCR_LIB" ]    || { echo "SKIP: libXrdHttpTPCR plugin missing"; exit 127; }
DUMP="$BUILD_DIR/bin/tpcr-journal-dump"
command -v python3 >/dev/null || { echo "SKIP: python3"; exit 127; }
[ -x "$BUILD_DIR/bin/xrootd" ] || { echo "SKIP: xrootd"; exit 127; }
[ -x "$DUMP" ] || { echo "SKIP: tpcr-journal-dump"; exit 127; }

WORK="$(mktemp -d /tmp/tpcr-multi-XXXXXX)"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

FAILURES=0
fail() { echo "FAIL: $*"; FAILURES=$((FAILURES + 1)); }
pass() { echo "PASS: $*"; }
pick_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()'; }

PORT_A=$(pick_port); PORT_B=$(pick_port); MOCK_PORT=$(pick_port)
mkdir -p "$WORK/adminA" "$WORK/adminB" "$WORK/data"

write_cfg() {  # write_cfg <path> <port> <admindir>
    cat > "$1" <<EOF
all.export /
all.adminpath $3
all.pidpath $3
oss.localroot $WORK/data
xrd.port $2
xrd.protocol XrdHttp:$2 $XRDHTTP_LIB
http.desthttps false
tpc.allow local
tpc.allow private
http.exthandler xrdtpcr +notls $TPCR_LIB
tpc.trace all
tpcr.blocksize 1m
tpcr.window.bytes 4m
tpcr.checkpoint.bytes 2m
tpcr.checkpoint.secs 5
EOF
}
write_cfg "$WORK/a.cfg" "$PORT_A" "$WORK/adminA"
write_cfg "$WORK/b.cfg" "$PORT_B" "$WORK/adminB"

start_gw() {  # start_gw <cfg> <name>; sets GW_PID.  NOT run in a subshell:
    # a startup failure must abort the whole harness, and `exit` inside
    # $(...) only kills the substitution (learned the hard way on EL8).
    # NB: with -n <name>, the log lands at $WORK/<name>/xrootd.log.
    LD_LIBRARY_PATH="$LIB_DIR" "$BUILD_DIR/bin/xrootd" -c "$1" \
        -l "$WORK/xrootd.log" -n "$2" > /dev/null 2>&1 &
    GW_PID=$!
    PIDS+=($GW_PID)
    local port
    port=$(grep "^xrd.port" "$1" | awk '{print $2}')
    for _ in $(seq 1 150); do   # slow CI containers need up to ~30s
        curl -s -o /dev/null "http://127.0.0.1:$port/" && return 0
        sleep 0.2
    done
    echo "gateway $2 failed to start"; exit 1
}
log_b() { cat "$WORK"/gwb/xrootd.log* 2>/dev/null; }

journal_w() {
    "$DUMP" "$1" 2>/dev/null | awk '/committed watermark/ {print $NF; found=1}
                                    END {if (!found) print -1}'
}

python3 -c "import random; n=32*1024*1024; rng=random.Random(16); open('$WORK/ref.bin','wb').write(rng.getrandbits(n*8).to_bytes(n,'little'))"
python3 "$SRC_DIR/mock_source.py" --port "$MOCK_PORT" --file "$WORK/ref.bin" \
    --throttle $((512 * 1024)) > "$WORK/mock.log" 2>&1 &
PIDS+=($!)
for _ in $(seq 1 100); do
    curl -s -o /dev/null "http://127.0.0.1:$MOCK_PORT/ctl" && break; sleep 0.2
done

start_gw "$WORK/a.cfg" gwa; PID_A=$GW_PID
start_gw "$WORK/b.cfg" gwb; PID_B=$GW_PID

# ---------------------------------------------------------------------------
# 1 + 2: A writes; B must refuse to interfere while A's lease is live
# ---------------------------------------------------------------------------
curl -s -N -X COPY "http://127.0.0.1:$PORT_A/mg.bin" \
    -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
    -H "X-Number-Of-Streams: 4" -H "Overwrite: T" > "$WORK/a-copy.txt" 2>&1 &
COPY_PID=$!
deadline=$(( $(date +%s) + 90 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    [ "$(journal_w "$WORK/data/mg.bin.xrdtpcr")" -gt 0 ] 2>/dev/null && break
    sleep 0.5
done

HTTP_CODE=$(curl -s -o "$WORK/b-second.txt" -w '%{http_code}' -X COPY \
    "http://127.0.0.1:$PORT_B/mg.bin" \
    -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
    -H "X-Number-Of-Streams: 2")
if [ "$HTTP_CODE" = "409" ] && [ -f "$WORK/data/mg.bin.xrdtpcr" ] \
   && log_b | grep -q "event=LEASE_CONFLICT"; then
    pass "1: cross-gateway COPY refused with 409 while A's lease is live"
else
    fail "1: expected 409 from B, got $HTTP_CODE: $(cat "$WORK/b-second.txt")"
fi

HTTP_CODE=$(curl -s -o "$WORK/b-force.txt" -w '%{http_code}' -X COPY \
    "http://127.0.0.1:$PORT_B/mg.bin" \
    -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
    -H "X-Resume: F" -H "Overwrite: T" -H "X-Number-Of-Streams: 2")
if [ "$HTTP_CODE" = "409" ] && [ -f "$WORK/data/mg.bin.xrdtpcr" ]; then
    pass "2: X-Resume: F via B still 409 while A writes (journal untouched)"
else
    fail "2: expected 409 from B, got $HTTP_CODE"
fi

# ---------------------------------------------------------------------------
# 3. Kill A mid-transfer; the retry lands on B and resumes A's watermark
# ---------------------------------------------------------------------------
kill -9 "$PID_A"
wait "$COPY_PID" 2>/dev/null
W_CRASH=$(journal_w "$WORK/data/mg.bin.xrdtpcr")
if ! [ "$W_CRASH" -gt 0 ] 2>/dev/null; then
    fail "3: crash setup failed (W=$W_CRASH)"
else
    sleep 11                    # A's lease (2 x 5s) must lapse
    RESP=$(curl -s -X COPY "http://127.0.0.1:$PORT_B/mg.bin" \
        -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
        -H "X-Number-Of-Streams: 4")
    if printf '%s' "$RESP" | grep -q "success: Created" \
       && cmp -s "$WORK/ref.bin" "$WORK/data/mg.bin" \
       && [ ! -f "$WORK/data/mg.bin.xrdtpcr" ] \
       && log_b | grep -q "event=RESUME_START"; then
        pass "3: gateway B resumed A's transfer from W=$W_CRASH; content exact; journal gone"
    else
        fail "3: $(printf '%s' "$RESP" | tail -c 200)"
    fi
fi
kill "$PID_B" 2>/dev/null

echo
if [ "$FAILURES" -eq 0 ]; then echo "MULTI-GATEWAY MATRIX (WP-15): ALL PASSED"; exit 0; fi
echo "$FAILURES multi-gateway check(s) FAILED"; exit 1
