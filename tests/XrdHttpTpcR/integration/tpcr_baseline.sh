#!/usr/bin/env bash
#
# T-P1 baseline: wall-clock throughput of TPCR vs the stock TPC handler on
# the same testbed (NFR-2: TPCR must not regress at equal streams).
#
# Also documents the BUG-8 outcome: the dead streams x 16 pipelining pool is
# gone; read-ahead comes from the reorder window, and the multi-stream
# numbers below show the effect.
#
# NOTE: this is a *loopback* baseline against the python mock source -- it
# measures relative handler overhead, not network behavior.  The
# RTT-sensitive comparison (tc netem) runs in WP-12; results append to
# test-results/t-p1-baseline.md.
#
# Usage: tpcr_baseline.sh <build_dir> <results_file> [size_mib]

set -u
BUILD_DIR="${1:?usage: tpcr_baseline.sh <build_dir> <results_file> [size_mib]}"
RESULTS="${2:?results file}"
SIZE_MIB="${3:-256}"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
LIB_DIR="$BUILD_DIR/lib"
WORK="$(mktemp -d /tmp/tpcr-perf-XXXXXX)"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

pick_port() {
    python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()'
}

# Reference payload.
python3 - "$WORK/ref.bin" $((SIZE_MIB * 1024 * 1024)) <<'EOF'
import sys, random
path, size = sys.argv[1], int(sys.argv[2])
rng = random.Random(7)
with open(path, "wb") as out:
    remaining = size
    while remaining:
        n = min(remaining, 1 << 20)
        out.write(rng.randbytes(n))
        remaining -= n
EOF

MOCK_PORT=$(pick_port)
python3 "$SRC_DIR/mock_source.py" --port "$MOCK_PORT" --file "$WORK/ref.bin" \
    > "$WORK/mock.log" 2>&1 &
PIDS+=($!)
sleep 1

# start_server <name> <exthandler line>; sets SERVER_PORT
start_server() {
    local name="$1"; local handler="$2"
    SERVER_PORT=$(pick_port)
    mkdir -p "$WORK/$name/admin" "$WORK/$name/data"
    cat > "$WORK/$name.cfg" <<EOF
all.export /
all.adminpath $WORK/$name/admin
all.pidpath $WORK/$name/admin
oss.localroot $WORK/$name/data
xrd.port $SERVER_PORT
xrd.protocol XrdHttp:$SERVER_PORT $LIB_DIR/libXrdHttp-6.so
http.desthttps false
tpc.allow local
tpc.allow private
$handler
EOF
    LD_LIBRARY_PATH="$LIB_DIR" "$BUILD_DIR/bin/xrootd" -c "$WORK/$name.cfg" \
        -l "$WORK/$name.log" -n "$name" &
    PIDS+=($!)
    for _ in $(seq 1 50); do
        curl -s -o /dev/null "http://127.0.0.1:$SERVER_PORT/" && return 0
        sleep 0.2
    done
    echo "server $name failed to start"; exit 1
}

run_copy() {  # run_copy <port> <streams> <dest>; prints elapsed seconds
    local port="$1" streams="$2" dest="$3"
    local t0 t1
    t0=$(date +%s.%N)
    local resp
    resp=$(curl -s -X COPY "http://127.0.0.1:$port$dest" \
        -H "Source: http://127.0.0.1:$MOCK_PORT/src.bin" \
        -H "X-Number-Of-Streams: $streams" -H "Overwrite: T")
    t1=$(date +%s.%N)
    if ! printf '%s' "$resp" | grep -q "success: Created"; then
        echo "-1"
        return
    fi
    python3 -c "print(f'{$t1 - $t0:.2f}')"
}

start_server tpcr "http.exthandler xrdtpcr +notls $LIB_DIR/libXrdHttpTPCR-6.so"
TPCR_PORT=$SERVER_PORT
start_server stock "http.exthandler xrdtpc +notls $LIB_DIR/libXrdHttpTPC-6.so"
STOCK_PORT=$SERVER_PORT

{
    echo "## T-P1 loopback baseline -- $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Payload: ${SIZE_MIB} MiB via python mock source on loopback."
    echo "(Relative handler comparison only; netem runs land with WP-12.)"
    echo
    echo "| streams | stock secs | tpcr secs |"
    echo "|---------|------------|-----------|"
    for streams in 1 4 8; do
        stock_t=$(run_copy "$STOCK_PORT" "$streams" "/perf-stock-$streams.bin")
        tpcr_t=$(run_copy "$TPCR_PORT" "$streams" "/perf-tpcr-$streams.bin")
        echo "| $streams | $stock_t | $tpcr_t |"
    done
    echo
} | tee -a "$RESULTS"
