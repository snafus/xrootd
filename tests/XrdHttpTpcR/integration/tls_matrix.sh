#!/usr/bin/env bash
#
# WP-15: TLS integration leg.  Everything else in the estate runs +notls
# plaintext; this matrix proves the two production-shaped TLS paths:
#
#   1. HTTPS client -> gateway (xrd.tls, handler loaded WITHOUT +notls),
#      plain-http source: transfer correct end to end.
#   2. HTTPS *source*: the handler's own curl side -- CA trust via
#      http.cadir (hashed dir), TLS connection reuse across many ranges.
#   3. Resume over TLS: gateway kill -9 mid-transfer against the HTTPS
#      source, retry after lease lapse -> RESUME_START, content exact.
#
# A throwaway CA and an IP-SAN server cert are generated per run.
#
# Usage: tls_matrix.sh <build_dir>     (exit 127 = missing prerequisites)

set -u
BUILD_DIR="${1:?usage: tls_matrix.sh <build_dir>}"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
LIB_DIR="$BUILD_DIR/lib"
# The plugin suffix tracks the tree version (a tagless CI checkout builds
# -4, a v6.1.1 clone builds -6): discover it, never assume it.
XRDHTTP_LIB=$(ls "$LIB_DIR"/libXrdHttp-[0-9]*.so 2>/dev/null | head -1)
TPCR_LIB=$(ls "$LIB_DIR"/libXrdHttpTPCR-[0-9]*.so 2>/dev/null | head -1)
[ -n "$XRDHTTP_LIB" ] || { echo "SKIP: libXrdHttp plugin missing"; exit 127; }
[ -n "$TPCR_LIB" ]    || { echo "SKIP: libXrdHttpTPCR plugin missing"; exit 127; }
command -v python3 >/dev/null || { echo "SKIP: python3"; exit 127; }
command -v openssl >/dev/null || { echo "SKIP: openssl"; exit 127; }
[ -x "$BUILD_DIR/bin/xrootd" ] || { echo "SKIP: xrootd"; exit 127; }

WORK="$(mktemp -d /tmp/tpcr-tls-XXXXXX)"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

FAILURES=0
fail() { echo "FAIL: $*"; FAILURES=$((FAILURES + 1)); }
pass() { echo "PASS: $*"; }
pick_port() { python3 -c '
import random, socket
for _ in range(200):
    p = random.randint(20000, 31999)   # below the ephemeral range: server
    s = socket.socket()                # restarts cannot collide with
    try:                               # kernel-assigned source ports
        s.bind(("127.0.0.1", p)); s.close(); print(p); break
    except OSError:
        s.close()
'; }

# --- throwaway PKI: CA + IP-SAN server cert + hashed CA dir -----------------
openssl req -x509 -newkey rsa:2048 -keyout "$WORK/ca.key" -out "$WORK/ca.pem" \
    -days 2 -nodes -subj "/CN=tpcr-test-ca" 2>/dev/null
openssl req -newkey rsa:2048 -keyout "$WORK/srv.key" -out "$WORK/srv.csr" \
    -nodes -subj "/CN=localhost" 2>/dev/null
printf "subjectAltName=IP:127.0.0.1,DNS:localhost\n" > "$WORK/san.ext"
openssl x509 -req -in "$WORK/srv.csr" -CA "$WORK/ca.pem" -CAkey "$WORK/ca.key" \
    -CAcreateserial -out "$WORK/srv.pem" -days 2 -extfile "$WORK/san.ext" 2>/dev/null
[ -s "$WORK/srv.pem" ] || { echo "SKIP: openssl could not mint certs"; exit 127; }
mkdir -p "$WORK/cadir" && cp "$WORK/ca.pem" "$WORK/cadir/"
ln -sf ca.pem "$WORK/cadir/$(openssl x509 -hash -noout -in "$WORK/ca.pem").0"
# xrootd refuses group/world-writable CA material ("excessive access rights").
chmod 755 "$WORK/cadir"; chmod 644 "$WORK/cadir/ca.pem" "$WORK/srv.pem"
chmod 600 "$WORK/srv.key" "$WORK/ca.key"

PORT=$(pick_port); MOCK_HTTP=$(pick_port); MOCK_TLS=$(pick_port)
mkdir -p "$WORK/admin" "$WORK/data"
cat > "$WORK/tls.cfg" <<EOF
all.export /
all.adminpath $WORK/admin
all.pidpath $WORK/admin
oss.localroot $WORK/data
xrd.port $PORT
xrd.protocol XrdHttp:$PORT $XRDHTTP_LIB
xrd.tls $WORK/srv.pem $WORK/srv.key
xrd.tlsca certfile $WORK/ca.pem
http.desthttps false
http.cadir $WORK/cadir
tpc.allow local
tpc.allow private
http.exthandler xrdtpcr $TPCR_LIB
tpc.trace all
tpcr.blocksize 1m
tpcr.window.bytes 4m
tpcr.checkpoint.bytes 2m
tpcr.checkpoint.secs 5
EOF
# NOTE: no +notls -- on a TLS-enabled server the handler loads the normal way.

XRD_PID=""
start_server() {
    LD_LIBRARY_PATH="$LIB_DIR" "$BUILD_DIR/bin/xrootd" -c "$WORK/tls.cfg" \
        -l "$WORK/xrootd.log" -n tls > /dev/null 2>&1 &
    XRD_PID=$!
    PIDS+=($XRD_PID)
    for _ in $(seq 1 150); do   # slow CI containers need up to ~30s
        curl -s --cacert "$WORK/ca.pem" -o /dev/null "https://127.0.0.1:$PORT/" \
            && return 0
        sleep 0.2
    done
    echo "TLS server failed to start"; exit 1
}
server_log() { cat "$WORK"/tls/xrootd.log* 2>/dev/null; }

journal_w() {
    "$BUILD_DIR/bin/tpcr-journal-dump" "$1" 2>/dev/null \
        | awk '/committed watermark/ {print $NF; found=1}
               END {if (!found) print -1}'
}

python3 -c "import random; n=16*1024*1024; rng=random.Random(15); open('$WORK/ref.bin','wb').write(rng.getrandbits(n*8).to_bytes(n,'little'))"

# ---------------------------------------------------------------------------
# 1. HTTPS client -> gateway, plain-http source
# ---------------------------------------------------------------------------
python3 "$SRC_DIR/mock_source.py" --port "$MOCK_HTTP" --file "$WORK/ref.bin" \
    > "$WORK/mock-http.log" 2>&1 &
PIDS+=($!)
for _ in $(seq 1 100); do
    curl -s -o /dev/null "http://127.0.0.1:$MOCK_HTTP/ctl" && break; sleep 0.2
done
start_server
RESP=$(curl -s --cacert "$WORK/ca.pem" -X COPY "https://127.0.0.1:$PORT/tls1.bin" \
    -H "Source: http://127.0.0.1:$MOCK_HTTP/src.bin" \
    -H "X-Number-Of-Streams: 4" -H "Overwrite: T")
if printf '%s' "$RESP" | grep -q "success: Created" \
   && cmp -s "$WORK/ref.bin" "$WORK/data/tls1.bin"; then
    pass "1: HTTPS client -> gateway transfer correct (handler loaded without +notls)"
else
    fail "1: $(printf '%s' "$RESP" | tail -c 160)"
fi

# ---------------------------------------------------------------------------
# 2. HTTPS source: the handler's curl trusts our CA via http.cadir
# ---------------------------------------------------------------------------
python3 "$SRC_DIR/mock_source.py" --port "$MOCK_TLS" --file "$WORK/ref.bin" \
    --tls-cert "$WORK/srv.pem" --tls-key "$WORK/srv.key" \
    > "$WORK/mock-tls.log" 2>&1 &
PIDS+=($!)
for _ in $(seq 1 100); do
    curl -s --cacert "$WORK/ca.pem" -o /dev/null "https://127.0.0.1:$MOCK_TLS/ctl" \
        && break
    sleep 0.2
done
RESP=$(curl -s --cacert "$WORK/ca.pem" -X COPY "https://127.0.0.1:$PORT/tls2.bin" \
    -H "Source: https://127.0.0.1:$MOCK_TLS/src.bin" \
    -H "X-Number-Of-Streams: 4" -H "Overwrite: T")
if printf '%s' "$RESP" | grep -q "success: Created" \
   && cmp -s "$WORK/ref.bin" "$WORK/data/tls2.bin"; then
    pass "2: HTTPS source pulled through the CA-verified curl path (http.cadir)"
else
    fail "2: $(printf '%s' "$RESP" | tail -c 160)"
fi
kill "$XRD_PID" 2>/dev/null
wait "$XRD_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# 3. Resume over TLS: kill -9 mid-transfer against the HTTPS source
# ---------------------------------------------------------------------------
# A throttled TLS mock on its OWN port (scenario 2's mock still holds
# $MOCK_TLS) so the kill provably lands mid-flight.
MOCK_TLS2=$(pick_port)
python3 "$SRC_DIR/mock_source.py" --port "$MOCK_TLS2" --file "$WORK/ref.bin" \
    --tls-cert "$WORK/srv.pem" --tls-key "$WORK/srv.key" \
    --throttle $((1024 * 1024)) > "$WORK/mock-tls2.log" 2>&1 &
PIDS+=($!)
for _ in $(seq 1 100); do
    curl -s --cacert "$WORK/ca.pem" -o /dev/null \
        "https://127.0.0.1:$MOCK_TLS2/ctl" && break
    sleep 0.2
done
start_server
curl -s --cacert "$WORK/ca.pem" -N -X COPY "https://127.0.0.1:$PORT/tls3.bin" \
    -H "Source: https://127.0.0.1:$MOCK_TLS2/src.bin" \
    -H "X-Number-Of-Streams: 4" -H "Overwrite: T" > /dev/null 2>&1 &
COPY_PID=$!
deadline=$(( $(date +%s) + 90 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    [ "$(journal_w "$WORK/data/tls3.bin.xrdtpcr")" -gt 0 ] 2>/dev/null && break
    sleep 0.5
done
kill -9 "$XRD_PID"
wait "$COPY_PID" 2>/dev/null
W_CRASH=$(journal_w "$WORK/data/tls3.bin.xrdtpcr")
sleep 11                        # lease (2 x 5s) must lapse
start_server
RESP=$(curl -s --cacert "$WORK/ca.pem" -X COPY "https://127.0.0.1:$PORT/tls3.bin" \
    -H "Source: https://127.0.0.1:$MOCK_TLS2/src.bin" \
    -H "X-Number-Of-Streams: 4")
if printf '%s' "$RESP" | grep -q "success: Created" \
   && cmp -s "$WORK/ref.bin" "$WORK/data/tls3.bin" \
   && [ ! -f "$WORK/data/tls3.bin.xrdtpcr" ] \
   && server_log | grep -q "event=RESUME_START"; then
    pass "3: resume over TLS from W=$W_CRASH; content exact; journal gone"
else
    fail "3: $(printf '%s' "$RESP" | tail -c 160)"
fi
kill "$XRD_PID" 2>/dev/null

echo
if [ "$FAILURES" -eq 0 ]; then echo "TLS MATRIX (WP-15): ALL PASSED"; exit 0; fi
echo "$FAILURES TLS matrix check(s) FAILED"; exit 1
