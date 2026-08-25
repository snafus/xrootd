#!/usr/bin/env bash
#
# T-I6 (WP-8): the resume decision matrix, end to end against a real server.
# Every rejection path degrades to a correct fresh transfer -- the CON-3
# safety property (resume never fails a transfer stock would carry).
# Scenario 4 is BUG-12's disposition made live: the partial's length lies
# (truncated below W) and only the journal watermark is trusted (FR-25).
#
#   1. Happy path: kill -9 mid-transfer, retry -> RESUME_START with W > 0,
#      session 2 fetches nothing below W (NFR-4 spirit), content exact,
#      journal removed on success.
#   2. X-Resume: F forces a fresh transfer (FR-4).
#   3. Validator mismatch (ETag + Last-Modified flip) -> RESUME_REJECTED
#      (validator) -> fresh -> correct content (FR-20 case (c), FR-21).
#   4. Destination truncated below W -> RESUME_REJECTED (dest-truncated).
#   5. Case (e): journal without a partial (POSC-style) -> discarded ->
#      fresh (XRD-1, T-I13 leg).
#   6. Lazy GC: journal older than tpcr.gc.age -> GC_DISCARD -> fresh (FR-24).
#   7. Live lease -> second COPY gets 409 + Retry-After (FR-22).
#   8. Expired lease (crashed owner) -> stolen, resume proceeds (FR-22).
#   9. Overwrite: F with a valid journal -> resume still legitimate (SUB-2).
#  10. Overwrite: F with a journal-less partial -> fails as stock (SUB-2).
#
# Usage: resume_matrix.sh <build_dir>    (exit 127 = missing prerequisites)

set -u
BUILD_DIR="${1:?usage: resume_matrix.sh <build_dir>}"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
LIB_DIR="$BUILD_DIR/lib"
# The plugin suffix tracks the tree version (a tagless CI checkout builds
# -4, a v6.1.1 clone builds -6): discover it, never assume it.
XRDHTTP_LIB=$(ls "$LIB_DIR"/libXrdHttp-[0-9]*.so 2>/dev/null | head -1)
TPCR_LIB=$(ls "$LIB_DIR"/libXrdHttpTPCR-[0-9]*.so 2>/dev/null | head -1)
STOCK_TPC_LIB=$(ls "$LIB_DIR"/libXrdHttpTPC-[0-9]*.so 2>/dev/null | head -1)
[ -n "$XRDHTTP_LIB" ] || { echo "SKIP: libXrdHttp plugin missing"; exit 127; }
[ -n "$TPCR_LIB" ]    || { echo "SKIP: libXrdHttpTPCR plugin missing"; exit 127; }
DUMP="$BUILD_DIR/bin/tpcr-journal-dump"

command -v python3 >/dev/null || { echo "SKIP: python3"; exit 127; }
[ -x "$BUILD_DIR/bin/xrootd" ] || { echo "SKIP: xrootd"; exit 127; }
[ -x "$DUMP" ] || { echo "SKIP: tpcr-journal-dump"; exit 127; }

WORK="$(mktemp -d /tmp/tpcr-resume-XXXXXX)"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done; wait 2>/dev/null; [ -n "${TPCR_KEEP_WORK:-}" ] || rm -rf "$WORK"; }
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

PORT=$(pick_port)
MOCK_PORT=$(pick_port)
mkdir -p "$WORK/admin" "$WORK/data"
cat > "$WORK/xrootd.cfg" <<EOF
all.export /
all.adminpath $WORK/admin
all.pidpath $WORK/admin
oss.localroot $WORK/data
xrd.port $PORT
xrd.protocol XrdHttp:$PORT $XRDHTTP_LIB
http.desthttps false
tpc.allow local
tpc.allow private
http.exthandler xrdtpcr +notls $TPCR_LIB
tpc.trace all
tpcr.blocksize 1m
tpcr.window.bytes 4m
tpcr.checkpoint.bytes 2m
tpcr.checkpoint.secs 5
tpcr.gc.age 20s
EOF
# Lease duration = 2 x checkpoint.secs = 10s.

XRD_PID=""
start_server() {
    LD_LIBRARY_PATH="$LIB_DIR" "$BUILD_DIR/bin/xrootd" -c "$WORK/xrootd.cfg" \
        -l "$WORK/xrootd.log" -n resume > /dev/null 2>&1 &
    XRD_PID=$!
    PIDS+=($XRD_PID)
    for _ in $(seq 1 150); do   # slow CI containers need up to ~30s
        curl -s -o /dev/null "http://127.0.0.1:$PORT/" && return 0; sleep 0.2
    done
    echo "server failed to start"
    echo "--- xrootd log tail (diagnostic) ---"
    tail -40 "$WORK"/resume/xrootd.log* 2>/dev/null
    exit 1
}
server_log() { cat "$WORK"/resume/xrootd.log* 2>/dev/null; }

MOCK_PID=""
start_mock() {  # start_mock <file> [args...]
    local ref="$1"; shift
    python3 "$SRC_DIR/mock_source.py" --port "$MOCK_PORT" --file "$ref" "$@" \
        > "$WORK/mock.log" 2>&1 &
    MOCK_PID=$!
    PIDS+=($MOCK_PID)
    for _ in $(seq 1 150); do   # slow CI containers need up to ~30s
        curl -s -o /dev/null "http://127.0.0.1:$MOCK_PORT/ctl" && return 0
        sleep 0.1
    done
    echo "mock failed to start"; exit 1
}
stop_mock() { [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null; wait "$MOCK_PID" 2>/dev/null; MOCK_PID=""; }

copy() {  # copy <dest> [curl args...]; sets RESPONSE
    local dest="$1"; shift
    RESPONSE=$(curl -s -X COPY "http://127.0.0.1:$PORT$dest" \
        -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
        -H "X-Number-Of-Streams: 4" "$@" 2>>"$WORK/curl.err")
}

journal_w() {
    "$DUMP" "$1" 2>/dev/null | awk '/committed watermark/ {print $NF; found=1}
                                    END {if (!found) print -1}'
}

make_ref() { python3 -c "import random; n=$3; rng=random.Random($2); open('$1','wb').write(rng.getrandbits(n*8).to_bytes(n,'little'))"; }

# Crashes a throttled transfer mid-flight, leaving partial + journal.
# Afterwards: the server is DOWN; W_CRASH holds the journal watermark.
crash_transfer() {  # crash_transfer <dest>
    local dest="$1"
    start_server
    curl -s -N -X COPY "http://127.0.0.1:$PORT$dest" \
        -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
        -H "X-Number-Of-Streams: 4" -H "Overwrite: T" > /dev/null 2>&1 &
    local copy_pid=$!
    # Kill only once the first checkpoint is provably down (W > 0); a
    # fixed sleep fired before the transfer started on slow CI runners.
    local deadline=$(( $(date +%s) + 90 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        [ "$(journal_w "$WORK/data$dest.xrdtpcr")" -gt 0 ] 2>/dev/null && break
        sleep 0.5
    done
    kill -9 "$XRD_PID"
    wait "$copy_pid" 2>/dev/null
    W_CRASH=$(journal_w "$WORK/data$dest.xrdtpcr")
    if ! [ "$W_CRASH" -gt 0 ] 2>/dev/null; then
        echo "crash setup failed for $dest (W=$W_CRASH)"; exit 1
    fi
}

REF="$WORK/ref.bin"
make_ref "$REF" 21 $((64*1024*1024))
start_mock "$REF" --throttle $((512*1024)) --headers-log "$WORK/hdrs.jsonl"

# ---------------------------------------------------------------------------
# 1. Happy-path resume after a gateway crash
# ---------------------------------------------------------------------------
crash_transfer "/r1.bin"
sleep 11                        # the dead session's lease (10s) must lapse
                                # first -- an immediate retry correctly gets
                                # 409 (scenario 7); orchestrator retries are
                                # never this fast in practice
: > "$WORK/hdrs.jsonl"          # session-2 request log starts here
start_server
copy "/r1.bin"                  # NO Overwrite header: resume permitted
if ! printf '%s' "$RESPONSE" | grep -q "success: Created"; then
    fail "1: resumed transfer failed: $(printf '%s' "$RESPONSE" | tail -c 200)"
elif ! cmp -s "$REF" "$WORK/data/r1.bin"; then
    fail "1: content mismatch after resume"
elif [ -f "$WORK/data/r1.bin.xrdtpcr" ]; then
    fail "1: journal not removed after successful resume"
elif ! server_log | grep -q "event=RESUME_START"; then
    fail "1: RESUME_START event missing"
else
    # NFR-4 spirit: session 2 must not fetch below the journal watermark.
    MIN_START=$(python3 - "$WORK/hdrs.jsonl" <<'EOF'
import json, sys
starts = []
for line in open(sys.argv[1]):
    entry = json.loads(line)
    range_header = entry["headers"].get("Range")
    if entry["method"] == "GET" and range_header:
        starts.append(int(range_header.split("=")[1].split("-")[0]))
print(min(starts) if starts else -1)
EOF
)
    if [ "$MIN_START" -ge "$W_CRASH" ] 2>/dev/null; then
        pass "1: resumed from W=$W_CRASH; no re-fetch below W (min GET start $MIN_START); content exact; journal gone"
    else
        fail "1: session 2 fetched below W ($MIN_START < $W_CRASH)"
    fi
fi
kill "$XRD_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# 2. X-Resume: F forces a fresh transfer (FR-4)
# ---------------------------------------------------------------------------
crash_transfer "/r2.bin"
sleep 11                        # WP-14/C2: the lease gate now precedes
                                # every destructive branch -- wait out the
                                # dead session's lease before the retry
: > "$WORK/hdrs.jsonl"
start_server
copy "/r2.bin" -H "X-Resume: F" -H "Overwrite: T"
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$REF" "$WORK/data/r2.bin" \
   && server_log | grep -q "reason=client-forced"; then
    MIN_START=$(python3 -c "
import json
starts=[int(json.loads(l)['headers']['Range'].split('=')[1].split('-')[0])
        for l in open('$WORK/hdrs.jsonl')
        if json.loads(l)['method']=='GET' and 'Range' in json.loads(l)['headers']]
print(min(starts) if starts else -1)")
    if [ "$MIN_START" -eq 0 ]; then
        pass "2: X-Resume: F discarded the journal and re-fetched from 0"
    else
        fail "2: fresh transfer did not start at 0 (min=$MIN_START)"
    fi
else
    fail "2: $(printf '%s' "$RESPONSE" | tail -c 120)"
fi
kill "$XRD_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# 3. Validator mismatch -> RESUME_REJECTED -> fresh (FR-20c, FR-21)
# ---------------------------------------------------------------------------
crash_transfer "/r3.bin"
sleep 11                        # WP-14/C2: the lease gate now precedes
                                # every destructive branch -- wait out the
                                # dead session's lease before the retry
curl -s -o /dev/null "http://127.0.0.1:$MOCK_PORT/ctl?etag=%22flipped%22&lastmod=Thu,+02+Jan+2025+00:00:00+GMT"
start_server
copy "/r3.bin" -H "Overwrite: T"
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$REF" "$WORK/data/r3.bin" \
   && server_log | grep -q "reason=validator"; then
    pass "3: flipped validators rejected the resume; fresh transfer correct"
else
    fail "3: $(printf '%s' "$RESPONSE" | tail -c 120)"
fi
kill "$XRD_PID" 2>/dev/null
curl -s -o /dev/null "http://127.0.0.1:$MOCK_PORT/ctl?etag=%22tpcr-mock-etag-1%22&lastmod=Wed,+01+Jan+2025+00:00:00+GMT"

# ---------------------------------------------------------------------------
# 4. Destination truncated below W -> RESUME_REJECTED (FR-20/FR-25)
# ---------------------------------------------------------------------------
crash_transfer "/r4.bin"
sleep 11                        # WP-14/C2: the lease gate now precedes
                                # every destructive branch -- wait out the
                                # dead session's lease before the retry
truncate -s 1000 "$WORK/data/r4.bin"
start_server
copy "/r4.bin" -H "Overwrite: T"
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$REF" "$WORK/data/r4.bin" \
   && server_log | grep -q "reason=dest-truncated"; then
    pass "4: truncated destination rejected the resume; fresh transfer correct"
else
    fail "4: $(printf '%s' "$RESPONSE" | tail -c 120)"
fi
kill "$XRD_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# 5. Case (e): journal without partial (POSC-style) -> discard -> fresh
# ---------------------------------------------------------------------------
crash_transfer "/r5.bin"
sleep 11                        # WP-14/C2: the lease gate now precedes
                                # every destructive branch -- wait out the
                                # dead session's lease before the retry
rm -f "$WORK/data/r5.bin"       # what POSC recovery would have done (XRD-1)
start_server
copy "/r5.bin" -H "Overwrite: T"
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$REF" "$WORK/data/r5.bin" \
   && server_log | grep -q "reason=no-partial"; then
    pass "5: orphan journal (case e / POSC) discarded; fresh transfer correct"
else
    fail "5: $(printf '%s' "$RESPONSE" | tail -c 120)"
fi
kill "$XRD_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# 6. Lazy GC: journal past tpcr.gc.age -> GC_DISCARD -> fresh (FR-24)
# ---------------------------------------------------------------------------
crash_transfer "/r6.bin"
sleep 11                        # WP-14/C2: the lease gate now precedes
                                # every destructive branch -- wait out the
                                # dead session's lease before the retry
sleep 22                        # gc.age is 20s
start_server
copy "/r6.bin" -H "Overwrite: T"
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$REF" "$WORK/data/r6.bin" \
   && server_log | grep -q "event=GC_DISCARD"; then
    pass "6: aged journal lazily GC'd; fresh transfer correct"
else
    fail "6: $(printf '%s' "$RESPONSE" | tail -c 120)"
fi
kill "$XRD_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# 7. Live lease -> 409 + Retry-After (FR-22)
# ---------------------------------------------------------------------------
start_server
curl -s -N -X COPY "http://127.0.0.1:$PORT/r7.bin" \
    -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
    -H "X-Number-Of-Streams: 2" -H "Overwrite: T" > /dev/null 2>&1 &
COPY_PID=$!
sleep 6                         # past the first checkpoint: lease on disk
HTTP_CODE=$(curl -s -o "$WORK/r7-second.txt" -w '%{http_code}' -X COPY \
    "http://127.0.0.1:$PORT/r7.bin" \
    -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
    -H "X-Number-Of-Streams: 2")
if [ "$HTTP_CODE" = "409" ] && server_log | grep -q "event=LEASE_CONFLICT"; then
    pass "7: concurrent COPY refused with 409 while the lease is live"
else
    fail "7: expected 409, got $HTTP_CODE: $(cat "$WORK/r7-second.txt")"
fi

# WP-14/C2: the lease gate is UNCONDITIONAL -- destructive requests that
# would previously have deleted the live journal and truncated the partial
# under the running session must all get 409 while the lease is live.
# 7b: mismatched validators (retry sees a "changed" source).
curl -s -o /dev/null "http://127.0.0.1:$MOCK_PORT/ctl?etag=%22flip-c2%22"
HTTP_CODE=$(curl -s -o "$WORK/r7-c2a.txt" -w '%{http_code}' -X COPY \
    "http://127.0.0.1:$PORT/r7.bin" \
    -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
    -H "X-Number-Of-Streams: 2")
if [ "$HTTP_CODE" = "409" ] && [ -f "$WORK/data/r7.bin.xrdtpcr" ]; then
    pass "7b: live lease + mismatched validators -> 409, journal untouched (WP-14/C2)"
else
    fail "7b: expected 409 + intact journal, got $HTTP_CODE (journal: $(ls "$WORK/data/r7.bin.xrdtpcr" 2>/dev/null || echo GONE))"
fi
# 7c: X-Resume: F must not evict a live writer either (FR-4 yields to FR-22).
HTTP_CODE=$(curl -s -o "$WORK/r7-c2b.txt" -w '%{http_code}' -X COPY \
    "http://127.0.0.1:$PORT/r7.bin" \
    -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
    -H "X-Resume: F" -H "Overwrite: T" -H "X-Number-Of-Streams: 2")
if [ "$HTTP_CODE" = "409" ] && [ -f "$WORK/data/r7.bin.xrdtpcr" ]; then
    pass "7c: live lease + X-Resume: F -> 409, journal untouched (WP-14/C2)"
else
    fail "7c: expected 409 + intact journal, got $HTTP_CODE"
fi
curl -s -o /dev/null "http://127.0.0.1:$MOCK_PORT/ctl?etag=%22tpcr-mock-etag-1%22"
wait "$COPY_PID" 2>/dev/null
kill "$XRD_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# 8. Expired lease is stolen; resume proceeds (FR-22, SUB-8)
# ---------------------------------------------------------------------------
crash_transfer "/r8.bin"        # dead owner still "holds" the lease
sleep 11                        # lease duration is 10s; let it expire
start_server
copy "/r8.bin"
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$REF" "$WORK/data/r8.bin" \
   && server_log | grep -q "event=LEASE_STEAL"; then
    pass "8: expired lease stolen; resume completed"
else
    fail "8: $(printf '%s' "$RESPONSE" | tail -c 120)"
fi
kill "$XRD_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# 9. Overwrite: F + valid journal -> resume is legitimate (SUB-2)
# ---------------------------------------------------------------------------
crash_transfer "/r9.bin"
sleep 11                        # let the crashed session's lease expire
start_server
copy "/r9.bin" -H "Overwrite: F"
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$REF" "$WORK/data/r9.bin"; then
    pass "9: Overwrite: F still resumes our own partial (SUB-2)"
else
    fail "9: $(printf '%s' "$RESPONSE" | tail -c 200)"
fi
kill "$XRD_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# 10. Overwrite: F + journal-less partial -> fails as stock (SUB-2)
# ---------------------------------------------------------------------------
crash_transfer "/r10.bin"
rm -f "$WORK/data/r10.bin.xrdtpcr"   # partial without journal = foreign file
start_server
HTTP_CODE=$(curl -s -o "$WORK/r10-out.txt" -w '%{http_code}' -X COPY \
    "http://127.0.0.1:$PORT/r10.bin" \
    -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
    -H "X-Number-Of-Streams: 2" -H "Overwrite: F")
if [ "$HTTP_CODE" != "200" ] && [ "$HTTP_CODE" != "202" ]; then
    pass "10: Overwrite: F refuses a journal-less existing file (HTTP $HTTP_CODE)"
else
    fail "10: expected stock refusal, got HTTP $HTTP_CODE: $(cat "$WORK/r10-out.txt" | tail -c 120)"
fi
kill "$XRD_PID" 2>/dev/null
stop_mock

# ---------------------------------------------------------------------------
# 11. Digest continuity across resume (T-U12 end-to-end; FR-26/FR-29/SUB-4):
#     the source advertises its adler32; a crash+resume transfer must still
#     produce the matching whole-file digest from the RESTORED digest state.
# ---------------------------------------------------------------------------
stop_mock
start_mock "$REF" --throttle $((512*1024)) --repr-digest good
crash_transfer "/r11.bin"
sleep 11
start_server
copy "/r11.bin"
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$REF" "$WORK/data/r11.bin" \
   && server_log | grep -q "event=RESUME_START"; then
    pass "11: resumed transfer attested the correct whole-file adler32 (FR-29)"
else
    fail "11: $(printf '%s' "$RESPONSE" | tail -c 200)"
fi
kill "$XRD_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# 12. T-I9 tamper detection (FR-28): flip one byte BELOW W between sessions
#     -> tail verification rejects the resume -> fresh -> correct content.
# ---------------------------------------------------------------------------
crash_transfer "/r12.bin"
python3 - "$WORK/data/r12.bin" "$W_CRASH" <<'EOF2'
import sys
path, watermark = sys.argv[1], int(sys.argv[2])
victim = max(0, watermark - 4096)     # well inside the committed prefix
with open(path, "r+b") as handle:
    handle.seek(victim)
    original = handle.read(1)
    handle.seek(victim)
    handle.write(bytes([original[0] ^ 0xFF]))
EOF2
sleep 11
start_server
copy "/r12.bin" -H "Overwrite: T"
if printf '%s' "$RESPONSE" | grep -q "success: Created" \
   && cmp -s "$REF" "$WORK/data/r12.bin" \
   && server_log | grep -q "reason=tail-verify"; then
    pass "12: single flipped byte below W caught by tail verification (T-I9)"
else
    fail "12: $(printf '%s' "$RESPONSE" | tail -c 200)"
fi
kill "$XRD_PID" 2>/dev/null

echo
if [ "$FAILURES" -eq 0 ]; then echo "RESUME MATRIX (T-I6): ALL PASSED"; exit 0; fi
echo "$FAILURES resume matrix check(s) FAILED"; exit 1
