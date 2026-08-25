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
# The plugin suffix tracks the tree version (a tagless CI checkout builds
# -4, a v6.1.1 clone builds -6): discover it, never assume it.
XRDHTTP_LIB=$(ls "$LIB_DIR"/libXrdHttp-[0-9]*.so 2>/dev/null | head -1)
TPCR_LIB=$(ls "$LIB_DIR"/libXrdHttpTPCR-[0-9]*.so 2>/dev/null | head -1)
STOCK_TPC_LIB=$(ls "$LIB_DIR"/libXrdHttpTPC-[0-9]*.so 2>/dev/null | head -1)
[ -n "$XRDHTTP_LIB" ] || { echo "SKIP: libXrdHttp plugin missing"; exit 127; }
[ -n "$TPCR_LIB" ]    || { echo "SKIP: libXrdHttpTPCR plugin missing"; exit 127; }

command -v python3 >/dev/null || { echo "SKIP: python3 missing"; exit 127; }
command -v curl >/dev/null    || { echo "SKIP: curl missing"; exit 127; }
[ -x "$XROOTD_BIN" ]          || { echo "SKIP: $XROOTD_BIN missing"; exit 127; }
[ -f "$TPCR_LIB" ] || { echo "SKIP: libXrdHttpTPCR plugin missing"; exit 127; }

WORK="$(mktemp -d /tmp/tpcr-test-XXXXXX)"
XROOTD_PID=""
MOCK_PID=""

cleanup() {
    [ -n "$XROOTD_PID" ] && kill "$XROOTD_PID" 2>/dev/null
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null
    wait 2>/dev/null
    # TPCR_KEEP_WORK=1 preserves the work dir for debugging.
    [ -n "${TPCR_KEEP_WORK:-}" ] || rm -rf "$WORK"
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
xrd.protocol XrdHttp:$HTTP_PORT $XRDHTTP_LIB
http.desthttps false
tpc.allow local
tpc.allow private
http.exthandler xrdtpcr +notls $TPCR_LIB
tpc.trace all
# Small blocks so modest files exercise many ranges; small window to
# exercise the sliding-window admission too.
tpcr.blocksize 1m
tpcr.window.bytes 4m
tpcr.range.timeout 30
# Fast escalation for the T-I7 degraded-state scenarios: exhaust after 2
# attempts (~3s of backoff), 25s recovery budget.
tpcr.retry.max 2
tpcr.recovery.maxsecs 25
xrootd.chksum adler32
EOF

LD_LIBRARY_PATH="$LIB_DIR" "$XROOTD_BIN" -c "$WORK/xrootd.cfg" \
    -l "$WORK/xrootd.log" -n tpcr &
XROOTD_PID=$!

# Wait for the server to accept connections.
for _ in $(seq 1 150); do   # slow CI containers need up to ~30s
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
    for _ in $(seq 1 150); do   # slow CI containers need up to ~30s
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
        out.write(rng.getrandbits(chunk * 8).to_bytes(chunk, "little"))
        remaining -= chunk
EOF
}

# ---------------------------------------------------------------------------
# T-I1: fresh pulls -- size matrix x streams, byte-compare.
# Verifies FR-1 (COPY verb, 202 + chunked, final chunk grammar), FR-7
# (streams=1 runs the same scheduler path), FR-10 (block coverage incl.
# short last range), FR-11 (success only after full committed content),
# CON-2 (libXrdHttpTPCR loaded via http.exthandler xrdtpcr).
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
# T-I1: marker grammar on a throttled transfer (CON-4 wire shape; BUG-13:
# the stripe fields keep the exact stock values clients parse).
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
# T-I2: TransferHeader passthrough to HEAD and every range GET (FR-2:
# header handling behavior-compatible with the stock handler).
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

# Starts a COPY in the background, response streamed to $1.
copy_pull_bg() {
    local out="$1"; local dest="$2"; local streams="$3"; shift 3
    curl -s -N -X COPY "http://127.0.0.1:$HTTP_PORT$dest" \
        -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
        -H "X-Number-Of-Streams: $streams" \
        -H "Overwrite: T" \
        "$@" > "$out" 2>>"$WORK/curl.err" &
    COPY_PID=$!
}

mock_ctl() { curl -s -o /dev/null "http://127.0.0.1:$MOCK_PORT/ctl?$1"; }

# ---------------------------------------------------------------------------
# T-I7: total source outage shorter than the recovery budget -> invisible
# (degraded state rides it out; markers keep flowing; transfer succeeds)
# ---------------------------------------------------------------------------
make_ref "$WORK/ref.bin" $((20 * BLOCK))
start_mock "$WORK/ref.bin" --throttle $((512 * 1024))
copy_pull_bg "$WORK/resp-outage.txt" "/dest-outage.bin" 4
sleep 3                      # let the transfer get going (~10s total)
mock_ctl "mode=refuse"       # total outage: data AND HEAD fail
sleep 8                      # exhaustion (~3s) + degraded probing
mock_ctl "mode=ok"           # source comes back
wait "$COPY_PID"
stop_mock
if grep -q "success: Created" "$WORK/resp-outage.txt" \
   && cmp -s "$WORK/ref.bin" "$WORK/data/dest-outage.bin"; then
    markers=$(grep -c "Perf Marker" "$WORK/resp-outage.txt")
    if [ "$markers" -ge 2 ]; then
        pass "T-I7 outage < budget ridden out invisibly ($markers markers)"
    else
        fail "T-I7 outage < budget: succeeded but only $markers markers (FR-15)"
    fi
else
    fail "T-I7 outage < budget: $(tail -2 "$WORK/resp-outage.txt")"
fi

# ---------------------------------------------------------------------------
# T-I7: outage longer than the recovery budget -> failure admitted only
# after the budget, with the specific message
# ---------------------------------------------------------------------------
make_ref "$WORK/ref.bin" $((20 * BLOCK))
start_mock "$WORK/ref.bin" --throttle $((512 * 1024))
copy_pull_bg "$WORK/resp-outage2.txt" "/dest-outage2.bin" 4
sleep 3
T_OUTAGE_START=$(date +%s)
mock_ctl "mode=refuse"       # and it never comes back
wait "$COPY_PID"
T_FAILED=$(date +%s)
stop_mock
if grep -q "recovery budget" "$WORK/resp-outage2.txt"; then
    ELAPSED=$((T_FAILED - T_OUTAGE_START))
    if [ "$ELAPSED" -ge 20 ]; then
        pass "T-I7 outage > budget admitted after ${ELAPSED}s (budget 25s)"
    else
        fail "T-I7 outage > budget: failed too early (${ELAPSED}s < ~25s budget)"
    fi
else
    fail "T-I7 outage > budget: wrong failure: $(tail -2 "$WORK/resp-outage2.txt")"
fi

# ---------------------------------------------------------------------------
# T-I7: source content changes mid-session -> permanent failure at re-probe
# ---------------------------------------------------------------------------
make_ref "$WORK/ref.bin" $((20 * BLOCK))
start_mock "$WORK/ref.bin" --throttle $((512 * 1024))
copy_pull_bg "$WORK/resp-changed.txt" "/dest-changed.bin" 4
sleep 3
mock_ctl "mode=refuse-data&etag=%22tpcr-mock-etag-CHANGED%22"
wait "$COPY_PID"
stop_mock
if grep -q "source changed mid-transfer" "$WORK/resp-changed.txt"; then
    pass "T-I7 mid-session source change detected at re-probe"
else
    fail "T-I7 source change: $(tail -2 "$WORK/resp-changed.txt")"
fi

# ---------------------------------------------------------------------------
# WP-14/H6: source entity replaced IN PLACE with no transport fault.  The
# If-Range guard (armed from the HEAD's strong ETag) makes the source answer
# 200 instead of 206, which fails the transfer as source-changed -- before
# this guard the changed bytes were only caught if some fault forced a
# degraded re-HEAD (or the source offered a Repr-Digest).
# ---------------------------------------------------------------------------
make_ref "$WORK/ref.bin" $((20 * BLOCK))
start_mock "$WORK/ref.bin" --throttle $((512 * 1024))
copy_pull_bg "$WORK/resp-ifrange.txt" "/dest-ifrange.bin" 4
sleep 3
mock_ctl "etag=%22tpcr-mock-etag-CHANGED%22"   # no refusal: data keeps flowing
wait "$COPY_PID"
stop_mock
if grep -q "If-Range guard" "$WORK/resp-ifrange.txt"; then
    pass "WP-14/H6 in-place source change caught by the If-Range guard"
else
    fail "WP-14/H6 If-Range: $(tail -2 "$WORK/resp-ifrange.txt")"
fi

# ---------------------------------------------------------------------------
# FR-12: 401 mid-session -- one re-probe recovers a flapping gateway...
# ---------------------------------------------------------------------------
make_ref "$WORK/ref.bin" $((4 * BLOCK + 999))
start_mock "$WORK/ref.bin" --fail-first 401:1
copy_pull "/dest-auth1.bin" 4
stop_mock
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$WORK/ref.bin" "$WORK/data/dest-auth1.bin"; then
    pass "FR-12 single 401 flap recovered via one re-probe"
else
    fail "FR-12 single 401: $RESPONSE"
fi

# ...and a persistently-401 source is a prompt permanent failure.
make_ref "$WORK/ref.bin" $((3 * BLOCK))
start_mock "$WORK/ref.bin" --fail-first 401:9999
copy_pull "/dest-auth2.bin" 4
stop_mock
if printf '%s' "$RESPONSE" | grep -q "failure:"; then
    pass "FR-12 persistent 401 is a permanent failure"
else
    fail "FR-12 persistent 401: $RESPONSE"
fi

# ---------------------------------------------------------------------------
# CON-6 push-mode smoke: the verbatim-ported push path still works.  The
# local file is served to the mock's PUT endpoint; byte-compare the upload.
# ---------------------------------------------------------------------------
make_ref "$WORK/data/pushsrc.bin" $((2 * BLOCK + 777))
make_ref "$WORK/ref.bin" 1   # mock needs a file to serve; content irrelevant
: > "$WORK/headers.jsonl"
start_mock "$WORK/ref.bin" --headers-log "$WORK/headers.jsonl"
PUSH_RESPONSE=$(curl -s -X COPY "http://127.0.0.1:$HTTP_PORT/pushsrc.bin" \
    -H "Destination: http://127.0.0.1:$MOCK_PORT/uploaded.bin" 2>>"$WORK/curl.err")
stop_mock
if printf '%s' "$PUSH_RESPONSE" | grep -q "success: Created" \
   && cmp -s "$WORK/data/pushsrc.bin" "$WORK/headers.jsonl.put"; then
    pass "CON-6 push mode smoke (byte-compared upload)"
else
    fail "CON-6 push: $PUSH_RESPONSE"
fi

# ---------------------------------------------------------------------------
# FR-29: source Repr-Digest vs computed destination adler32
# ---------------------------------------------------------------------------
make_ref "$WORK/ref.bin" $((3 * BLOCK + 777))
start_mock "$WORK/ref.bin" --repr-digest good
copy_pull "/dest-digest-ok.bin" 4
stop_mock
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$WORK/ref.bin" "$WORK/data/dest-digest-ok.bin"; then
    pass "FR-29 matching source Repr-Digest accepted"
else
    fail "FR-29 good digest: $RESPONSE"
fi

start_mock "$WORK/ref.bin" --repr-digest bad
copy_pull "/dest-digest-bad.bin" 4
stop_mock
if printf '%s' "$RESPONSE" | grep -q "does not match the source Repr-Digest"; then
    pass "FR-29 mismatched source Repr-Digest fails before the success chunk"
else
    fail "FR-29 bad digest: expected mismatch failure, got: $RESPONSE"
fi

# ---------------------------------------------------------------------------
# T-I10 (FR-27/XRD-2): the computed adler32 is injected into the checksum
# store (xattr via SFS FAttr) after close, bound to the settled mtime.
# ---------------------------------------------------------------------------
EXPECTED_ADLER=$(python3 -c "import zlib; print('%08x' % (zlib.adler32(open('$WORK/ref.bin','rb').read()) & 0xffffffff))")
ATTR_HEX=$(python3 - "$WORK/data/dest-digest-ok.bin" <<'EOF2'
import os, subprocess, sys
path = sys.argv[1]

def entries():
    if hasattr(os, "listxattr"):          # Linux
        for name in os.listxattr(path):
            yield name, os.getxattr(path, name)
    else:                                 # macOS: python os lacks xattr APIs
        listing = subprocess.run(["xattr", path], capture_output=True,
                                 text=True).stdout.split()
        for name in listing:
            dump = subprocess.run(["xattr", "-px", name, path],
                                  capture_output=True, text=True).stdout
            yield name, bytes.fromhex("".join(dump.split()))

for name, value in entries():
    if "adler" in name.lower() or "cks" in name.lower():
        print(value.hex())
        break
else:
    print("NOATTR")
EOF2
)
if [ "$ATTR_HEX" = "NOATTR" ]; then
    fail "T-I10 checksum attribute missing on the destination"
elif printf '%s' "$ATTR_HEX" | grep -qi "$EXPECTED_ADLER"; then
    pass "T-I10 checksum attribute present and carries the computed adler32"
else
    fail "T-I10 attr present but adler $EXPECTED_ADLER not found in: $ATTR_HEX"
fi

# The store answers a Want-Digest query with the injected value...
DIGEST_HDR=$(curl -s -I -H "Want-Digest: adler32" \
    "http://127.0.0.1:$HTTP_PORT/dest-digest-ok.bin" | tr -d '\r' | grep -i '^Digest:')
if printf '%s' "$DIGEST_HDR" | grep -qi "adler32=$EXPECTED_ADLER"; then
    pass "T-I10 Want-Digest query answered with the injected adler32"
else
    fail "T-I10 Want-Digest: got '$DIGEST_HDR', expected adler32=$EXPECTED_ADLER"
fi

# ...and the mtime binding is real (XRD-2 negative test): touching the file
# makes the stored attribute stale, so the SAME query must still return the
# CORRECT value -- proving the manager recalculates rather than trusting a
# stale attribute (silently slower, never wrong).
sleep 1; touch "$WORK/data/dest-digest-ok.bin"
DIGEST_HDR=$(curl -s -I -H "Want-Digest: adler32" \
    "http://127.0.0.1:$HTTP_PORT/dest-digest-ok.bin" | tr -d '\r' | grep -i '^Digest:')
if printf '%s' "$DIGEST_HDR" | grep -qi "adler32=$EXPECTED_ADLER"; then
    pass "T-I10 stale-mtime attribute triggers recalculation, not a wrong answer"
else
    fail "T-I10 negative: got '$DIGEST_HDR' after mtime change"
fi

# ---------------------------------------------------------------------------
# T-I8: wire grammar (CON-4) -- a strict gfal/davix-style parser must accept
# fresh-success, throttled-with-markers, resumed-shape, and failed responses;
# FR-3 OPTIONS capability; FR-6 resumable-from suffix on failures with a
# checkpointed journal.
# ---------------------------------------------------------------------------
printf '%s' "$RESPONSE" > "$WORK/grammar-last.txt"   # persistent-401 failure
if python3 "$SRC_DIR/check_grammar.py" "$WORK/grammar-last.txt" failure; then
    pass "T-I8 failed-transfer response parses under the stock grammar"
else
    fail "T-I8 failed-transfer grammar"
fi
if python3 "$SRC_DIR/check_grammar.py" "$WORK/resp-outage.txt" success; then
    pass "T-I8 marker-rich recovered response parses under the stock grammar"
else
    fail "T-I8 recovered-response grammar"
fi
if grep -q "resumable-from: " "$WORK/resp-outage2.txt"; then
    if python3 "$SRC_DIR/check_grammar.py" "$WORK/resp-outage2.txt" failure; then
        pass "T-I8/FR-6 failure carries resumable-from and still parses"
    else
        fail "T-I8/FR-6 resumable-from broke the failure grammar"
    fi
else
    fail "FR-6 resumable-from missing from checkpointed failure"
fi
OPTIONS_HEADERS=$(curl -s -i -X OPTIONS "http://127.0.0.1:$HTTP_PORT/any" | tr -d '\r')
if printf '%s' "$OPTIONS_HEADERS" | grep -q "X-Transfer-Capabilities: resume/1"; then
    pass "FR-3 OPTIONS advertises X-Transfer-Capabilities: resume/1"
else
    fail "FR-3 OPTIONS capability missing: $(printf '%s' "$OPTIONS_HEADERS" | head -8)"
fi

# ---------------------------------------------------------------------------
# FR-31: the structured log events for everything the suite exercised must
# be present in the server log (checked last, after all scenarios ran).
# ---------------------------------------------------------------------------
# Exercise the streams clamp once so its event exists (NFR-1/BUG-10).
make_ref "$WORK/ref.bin" $BLOCK
start_mock "$WORK/ref.bin"
copy_pull "/dest-clamp.bin" 99
stop_mock
printf '%s' "$RESPONSE" | grep -q "success: Created" || fail "clamp transfer failed: $RESPONSE"

for event in DEGRADED_ENTER DEGRADED_EXIT RANGE_RETRY SOURCE_CHANGED STREAMS_CLAMPED; do
    if grep -q "event=$event" "$WORK"/tpcr/xrootd.log* 2>/dev/null; then
        pass "FR-31 structured event $event logged"
    else
        fail "FR-31 structured event $event missing from server log"
    fi
done

# ---------------------------------------------------------------------------
echo
if [ "$FAILURES" -eq 0 ]; then
    echo "ALL INTEGRATION TESTS PASSED"
    exit 0
fi
echo "$FAILURES integration test(s) FAILED"
exit 1
