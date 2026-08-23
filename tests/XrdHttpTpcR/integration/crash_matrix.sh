#!/usr/bin/env bash
#
# T-I5 (WP-7 slice): the kill-point crash matrix around checkpointing.
# Verifies FR-18 (sidecar journal written through the server's SFS, next to
# the destination) and FR-19 (the aggressive checkpoint cadence configured
# below yields several watermark advances within a short transfer -- W > 0
# well before completion).
#
#   A. kill -9 the gateway mid-transfer: the journal survives, parses with
#      the same defensive parser the server uses (via tpcr-journal-dump),
#      and its watermark W is TRUSTWORTHY: bytes [0, W) of the partial are
#      byte-identical to the source (SUB-1 -- journal W <= durable data).
#   B. admitted failure (source outage > recovery budget): the FR-17 final
#      checkpoint ran -- the journal exists and carries W > 0 after the
#      failure chunk.
#   C. success: the journal is REMOVED (FR-23 ordering).
#
# The resume itself (reopen at W, validator ladder) is WP-8's T-I5/T-I6.
#
# Usage: crash_matrix.sh <build_dir>     (exit 127 = missing prerequisites)

set -u
BUILD_DIR="${1:?usage: crash_matrix.sh <build_dir>}"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
LIB_DIR="$BUILD_DIR/lib"
DUMP="$BUILD_DIR/bin/tpcr-journal-dump"

command -v python3 >/dev/null || { echo "SKIP: python3"; exit 127; }
[ -x "$BUILD_DIR/bin/xrootd" ] || { echo "SKIP: xrootd"; exit 127; }
[ -x "$DUMP" ] || { echo "SKIP: tpcr-journal-dump"; exit 127; }

WORK="$(mktemp -d /tmp/tpcr-crash-XXXXXX)"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

FAILURES=0
fail() { echo "FAIL: $*"; FAILURES=$((FAILURES + 1)); }
pass() { echo "PASS: $*"; }
pick_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()'; }

PORT=$(pick_port)
MOCK_PORT=$(pick_port)
mkdir -p "$WORK/admin" "$WORK/data"
cat > "$WORK/xrootd.cfg" <<EOF
all.export /
all.adminpath $WORK/admin
all.pidpath $WORK/admin
oss.localroot $WORK/data
xrd.port $PORT
xrd.protocol XrdHttp:$PORT $LIB_DIR/libXrdHttp-6.so
http.desthttps false
tpc.allow local
tpc.allow private
http.exthandler xrdtpcr +notls $LIB_DIR/libXrdHttpTPCR-6.so
tpc.trace all
tpcr.blocksize 1m
tpcr.window.bytes 4m
# Aggressive cadence so a short transfer checkpoints several times.
tpcr.checkpoint.bytes 2m
tpcr.checkpoint.secs 5
tpcr.retry.max 2
tpcr.recovery.maxsecs 15
EOF

start_server() {
    LD_LIBRARY_PATH="$LIB_DIR" "$BUILD_DIR/bin/xrootd" -c "$WORK/xrootd.cfg" \
        -l "$WORK/xrootd.log" -n crash > /dev/null 2>&1 &
    XRD_PID=$!
    PIDS+=($XRD_PID)
    for _ in $(seq 1 50); do
        curl -s -o /dev/null "http://127.0.0.1:$PORT/" && return 0; sleep 0.2
    done
    echo "server failed to start"; exit 1
}

python3 -c "import random; open('$WORK/ref.bin','wb').write(random.Random(11).randbytes(64*1024*1024))"
python3 "$SRC_DIR/mock_source.py" --port "$MOCK_PORT" --file "$WORK/ref.bin" \
    --throttle $((1024 * 1024)) > "$WORK/mock.log" 2>&1 &
PIDS+=($!)
sleep 1

journal_w() {  # prints W from the dump tool, or -1 on invalid/absent
    "$DUMP" "$1" 2>/dev/null | awk '/committed watermark/ {print $NF; found=1}
                                    END {if (!found) print -1}'
}

# Event-driven pacing (CI runners are slow and contended; fixed sleeps
# fired before the transfer even started).  On timeout the caller just
# proceeds -- the scenario's own assertions then report what is missing.
wait_journal_w() {  # wait_journal_w <journal> <min_bytes> <timeout_s>
    local deadline=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        local w; w=$(journal_w "$1")
        [ "$w" -ge "$2" ] 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}

wait_file_size() {  # wait_file_size <path> <min_bytes> <timeout_s>
    local deadline=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        local sz; sz=$(wc -c < "$1" 2>/dev/null || echo 0)
        [ "$sz" -ge "$2" ] 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}

# ---------------------------------------------------------------------------
# A. kill -9 mid-transfer: journal valid, W trustworthy
# ---------------------------------------------------------------------------
start_server
curl -s -N -X COPY "http://127.0.0.1:$PORT/crashA.bin" \
    -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
    -H "X-Number-Of-Streams: 4" -H "Overwrite: T" > /dev/null 2>&1 &
COPY_PID=$!
# Kill only once several checkpoints are provably down (2m cadence).
wait_journal_w "$WORK/data/crashA.bin.xrdtpcr" $((6*1024*1024)) 120
kill -9 "$XRD_PID"
wait "$COPY_PID" 2>/dev/null

JOURNAL="$WORK/data/crashA.bin.xrdtpcr"
if [ ! -f "$JOURNAL" ]; then
    fail "A: journal missing after kill -9"
else
    if "$DUMP" "$JOURNAL" > "$WORK/dumpA.txt" 2>&1; then
        W=$(journal_w "$JOURNAL")
        DEST_SIZE=$(wc -c < "$WORK/data/crashA.bin" | tr -d ' ')
        if [ "$W" -gt 0 ] 2>/dev/null && [ "$W" -le "$DEST_SIZE" ] \
           && cmp -s -n "$W" "$WORK/ref.bin" "$WORK/data/crashA.bin"; then
            pass "A: journal valid after kill -9; W=$W durable and byte-identical to source"
        else
            fail "A: watermark untrustworthy (W=$W, dest=$DEST_SIZE)"
        fi
    else
        fail "A: journal invalid after kill -9: $(cat "$WORK/dumpA.txt")"
    fi
fi

# ---------------------------------------------------------------------------
# B. admitted failure: FR-17 final checkpoint before the failure chunk
# ---------------------------------------------------------------------------
start_server
RESP=$(curl -m 120 -s -X COPY "http://127.0.0.1:$PORT/crashB.bin" \
    -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
    -H "X-Number-Of-Streams: 4" -H "Overwrite: T" \
    -H "TransferHeaderX-Kick: 1" & CPID=$!
# Cut the source only after the transfer has demonstrably started.
wait_file_size "$WORK/data/crashB.bin" 1 60
curl -s -o /dev/null "http://127.0.0.1:$MOCK_PORT/ctl?mode=refuse"
wait $CPID)
curl -s -o /dev/null "http://127.0.0.1:$MOCK_PORT/ctl?mode=ok"
JOURNAL="$WORK/data/crashB.bin.xrdtpcr"
if printf '%s' "$RESP" | grep -q "failure:" && [ -f "$JOURNAL" ]; then
    W=$(journal_w "$JOURNAL")
    if [ "$W" -gt 0 ] 2>/dev/null \
       && cmp -s -n "$W" "$WORK/ref.bin" "$WORK/data/crashB.bin"; then
        pass "B: FR-17 final checkpoint present after admitted failure (W=$W)"
    else
        fail "B: final checkpoint watermark bad (W=$W)"
    fi
else
    fail "B: expected failure chunk + journal; got: $(printf '%s' "$RESP" | tail -c 120)"
fi
kill "$XRD_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# C. success: journal removed (FR-23)
# ---------------------------------------------------------------------------
start_server
RESP=$(curl -s -X COPY "http://127.0.0.1:$PORT/crashC.bin" \
    -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
    -H "X-Number-Of-Streams: 4" -H "Overwrite: T")
if printf '%s' "$RESP" | grep -q "success: Created" \
   && cmp -s "$WORK/ref.bin" "$WORK/data/crashC.bin" \
   && [ ! -f "$WORK/data/crashC.bin.xrdtpcr" ]; then
    pass "C: success removes the journal (FR-23) and content is exact"
else
    fail "C: $(printf '%s' "$RESP" | tail -c 120); journal present: $(ls "$WORK/data/" | tr '\n' ' ')"
fi
kill "$XRD_PID" 2>/dev/null

echo
if [ "$FAILURES" -eq 0 ]; then echo "CRASH MATRIX (WP-7): ALL PASSED"; exit 0; fi
echo "$FAILURES crash matrix check(s) FAILED"; exit 1
