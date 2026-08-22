#!/usr/bin/env bash
#
# TPCR integration tests: T-I1 (fresh pulls, single & multi stream, size
# matrix, byte-compare, marker grammar), T-I2 (header passthrough), plus
# early slices of T-I3 (transient faults invisible) and T-I4 (range-
# dishonoring source fails promptly with the specific message).
#
# Runs a real xrootd server loaded with libXrdHttpTPCR against the
# controllable mock source.  Self-contained: temp dirs, dynamic ports,
# full cleanup.  Exit 127 = missing prerequisites (ctest SKIP_RETURN_CODE).
#
# Usage: tpcr_test.sh <build_dir>

set -u

BUILD_DIR="${1:?usage: tpcr_test.sh <build_dir>}"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
XROOTD_BIN="$BUILD_DIR/bin/xrootd"
LIB_DIR="$BUILD_DIR/lib"

command -v python3 >/dev/null || { echo "SKIP: python3 missing"; exit 127; }
command -v curl >/dev/null    || { echo "SKIP: curl missing"; exit 127; }
[ -x "$XROOTD_BIN" ]          || { echo "SKIP: $XROOTD_BIN missing"; exit 127; }
[ -f "$LIB_DIR/libXrdHttpTPCR-6.so" ] || { echo "SKIP: libXrdHttpTPCR-6.so missing"; exit 127; }

WORK="$(mktemp -d /tmp/tpcr-test-XXXXXX)"
XROOTD_PID=""
MOCK_PID=""

cleanup() {
    [ -n "$XROOTD_PID" ] && kill "$XROOTD_PID" 2>/dev/null
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

FAILURES=0
fail() {
    echo "FAIL: $*"
    echo "--- xrootd log tail ---"; tail -30 "$WORK/xrootd.log" 2>/dev/null
    echo "--- mock log tail ---";   tail -10 "$WORK/mock.log" 2>/dev/null
    FAILURES=$((FAILURES + 1))
}
pass() { echo "PASS: $*"; }

pick_port() {
    python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()'
}

HTTP_PORT=$(pick_port)

# ---------------------------------------------------------------------------
# xrootd server with the TPCR handler
# ---------------------------------------------------------------------------
mkdir -p "$WORK/admin" "$WORK/data"
cat > "$WORK/xrootd.cfg" <<EOF
all.export /
all.adminpath $WORK/admin
all.pidpath $WORK/admin
oss.localroot $WORK/data
xrd.port $HTTP_PORT
xrd.protocol XrdHttp:$HTTP_PORT $LIB_DIR/libXrdHttp-6.so
http.desthttps false
tpc.allow local
tpc.allow private
http.exthandler xrdtpcr +notls $LIB_DIR/libXrdHttpTPCR-6.so
tpc.trace all
# Small blocks so modest files exercise many ranges; small window to
# exercise the sliding-window admission too.
tpcr.blocksize 1m
tpcr.window.bytes 4m
tpcr.range.timeout 30
EOF

LD_LIBRARY_PATH="$LIB_DIR" "$XROOTD_BIN" -c "$WORK/xrootd.cfg" \
    -l "$WORK/xrootd.log" -n tpcr &
XROOTD_PID=$!

# Wait for the server to accept connections.
for _ in $(seq 1 50); do
    curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/" && break
    kill -0 "$XROOTD_PID" 2>/dev/null || { echo "xrootd died at startup"; cat "$WORK"/xrootd.log 2>/dev/null | tail -40; exit 1; }
    sleep 0.2
done

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Starts the mock source serving $1 (reference file) with extra args $2...
start_mock() {
    local ref="$1"; shift
    MOCK_PORT=$(pick_port)
    python3 "$SRC_DIR/mock_source.py" --port "$MOCK_PORT" --file "$ref" "$@" \
        > "$WORK/mock.log" 2>&1 &
    MOCK_PID=$!
    for _ in $(seq 1 50); do
        curl -s -o /dev/null "http://127.0.0.1:$MOCK_PORT/ctl" && return 0
        sleep 0.1
    done
    echo "mock source failed to start"; exit 1
}

stop_mock() {
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null
    wait "$MOCK_PID" 2>/dev/null
    MOCK_PID=""
}

# Issues a COPY (pull) and stores the chunked response body in $RESPONSE.
# copy_pull <dest_path> <streams> [extra curl args...]
copy_pull() {
    local dest="$1"; local streams="$2"; shift 2
    RESPONSE=$(curl -s -X COPY "http://127.0.0.1:$HTTP_PORT$dest" \
        -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
        -H "X-Number-Of-Streams: $streams" \
        -H "Overwrite: T" \
        "$@" 2>>"$WORK/curl.err")
}

make_ref() {  # make_ref <path> <size_bytes>
    python3 - "$1" "$2" <<'EOF'
import sys, random
path, size = sys.argv[1], int(sys.argv[2])
rng = random.Random(20260822)
with open(path, "wb") as out:
    remaining = size
    while remaining > 0:
        chunk = min(remaining, 1 << 20)
        out.write(rng.randbytes(chunk))
        remaining -= chunk
EOF
}

# ---------------------------------------------------------------------------
# T-I1: fresh pulls -- size matrix x streams, byte-compare
# ---------------------------------------------------------------------------
BLOCK=$((1024 * 1024))
for size in 0 1 $((BLOCK - 1)) $BLOCK $((BLOCK + 1)) $((8 * BLOCK + 12345)); do
    make_ref "$WORK/ref.bin" "$size"
    for streams in 1 4; do
        start_mock "$WORK/ref.bin"
        copy_pull "/dest-$size-$streams.bin" "$streams"
        stop_mock
        if ! printf '%s' "$RESPONSE" | grep -q "success: Created"; then
            fail "T-I1 size=$size streams=$streams: no success chunk; response: $RESPONSE"
            continue
        fi
        if ! cmp -s "$WORK/ref.bin" "$WORK/data/dest-$size-$streams.bin"; then
            fail "T-I1 size=$size streams=$streams: content mismatch"
            continue
        fi
        pass "T-I1 size=$size streams=$streams"
    done
done

# 16 streams on the biggest file (FR-1 stream matrix).
make_ref "$WORK/ref.bin" $((8 * BLOCK + 12345))
start_mock "$WORK/ref.bin"
copy_pull "/dest-16s.bin" 16
stop_mock
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$WORK/ref.bin" "$WORK/data/dest-16s.bin"; then
    pass "T-I1 streams=16"
else
    fail "T-I1 streams=16: $RESPONSE"
fi

# ---------------------------------------------------------------------------
# T-I1: marker grammar on a throttled transfer (CON-4 wire shape)
# ---------------------------------------------------------------------------
make_ref "$WORK/ref.bin" $((4 * BLOCK))
start_mock "$WORK/ref.bin" --throttle $((512 * 1024))
copy_pull "/dest-markers.bin" 2
stop_mock
if ! printf '%s' "$RESPONSE" | grep -q "success: Created"; then
    fail "markers: transfer failed: $RESPONSE"
else
    ok=1
    for line in "Perf Marker" "Timestamp: " "Stripe Index: 0" \
                "Stripe Bytes Transferred: " "Total Stripe Count: 1" "End"; do
        printf '%s' "$RESPONSE" | grep -q "$line" || { ok=0; fail "markers: missing '$line'"; }
    done
    [ "$ok" = 1 ] && pass "T-I1 marker grammar"
fi

# ---------------------------------------------------------------------------
# T-I2: TransferHeader passthrough to HEAD and every range GET
# ---------------------------------------------------------------------------
make_ref "$WORK/ref.bin" $((3 * BLOCK))
: > "$WORK/headers.jsonl"
start_mock "$WORK/ref.bin" --headers-log "$WORK/headers.jsonl"
copy_pull "/dest-hdrs.bin" 4 -H "TransferHeaderAuthorization: Bearer tok123"
stop_mock
if ! printf '%s' "$RESPONSE" | grep -q "success: Created"; then
    fail "T-I2: transfer failed: $RESPONSE"
elif python3 - "$WORK/headers.jsonl" <<'EOF'
import json, sys
entries = [json.loads(line) for line in open(sys.argv[1])]
requests = [e for e in entries if not e["path"].startswith("/ctl")]
assert requests, "no requests logged"
missing = [e for e in requests
           if e["headers"].get("Authorization") != "Bearer tok123"]
assert not missing, "missing Authorization on: %r" % missing
gets = [e for e in requests if e["method"] == "GET"]
assert gets, "no GET requests logged"
assert any("Range" in e["headers"] for e in gets), "no ranged GETs seen"
EOF
then
    pass "T-I2 header passthrough on every source request"
else
    fail "T-I2 header passthrough"
fi

# ---------------------------------------------------------------------------
# T-I4 (early): a source that ignores Range fails promptly + specifically
# ---------------------------------------------------------------------------
make_ref "$WORK/ref.bin" $((3 * BLOCK))
start_mock "$WORK/ref.bin" --ignore-range
copy_pull "/dest-norange.bin" 4
stop_mock
if printf '%s' "$RESPONSE" | grep -q "does not honor Range requests"; then
    pass "T-I4 range-dishonoring source rejected with specific message"
else
    fail "T-I4: expected specific range failure; got: $RESPONSE"
fi

# ---------------------------------------------------------------------------
# T-I3 (early): transient faults below the retry cap are invisible
# ---------------------------------------------------------------------------
make_ref "$WORK/ref.bin" $((4 * BLOCK + 999))
start_mock "$WORK/ref.bin" --fail-first 502:3
copy_pull "/dest-retry.bin" 4
stop_mock
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$WORK/ref.bin" "$WORK/data/dest-retry.bin"; then
    pass "T-I3 injected 502s retried invisibly"
else
    fail "T-I3 (502s): $RESPONSE"
fi

make_ref "$WORK/ref.bin" $((4 * BLOCK + 999))
start_mock "$WORK/ref.bin" --reset-after 65536:2
copy_pull "/dest-reset.bin" 4
stop_mock
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$WORK/ref.bin" "$WORK/data/dest-reset.bin"; then
    pass "T-I3 mid-body connection resets retried invisibly"
else
    fail "T-I3 (resets): $RESPONSE"
fi

# ---------------------------------------------------------------------------
echo
if [ "$FAILURES" -eq 0 ]; then
    echo "ALL INTEGRATION TESTS PASSED"
    exit 0
fi
echo "$FAILURES integration test(s) FAILED"
exit 1
