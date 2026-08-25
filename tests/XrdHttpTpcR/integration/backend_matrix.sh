#!/usr/bin/env bash
#
# WP-6: backend semantics test matrix (the M2 gate; 03-IMPLEMENTATION-PLAN).
#
# For each OSS backend available on this testbed, verify the primitives the
# journal/resume design depends on:
#   (a) sync() durability: data fsync()ed before a kill -9 is present
#       afterwards (validates the API ordering contract; true power-loss
#       durability needs hardware and is out of scope here -- recorded).
#   (b) reopen O_CREAT|O_WRONLY *without* O_TRUNC + write at offset W:
#       prefix intact, write lands (the resume open mode, FR-20).
#   (c) sidecar create/write/fsync/close/rename sequence atomicity under
#       kill -9 at random points (XRD-5 ordering): an observer must only
#       ever see a complete old or complete new journal, never a torn one.
#   (d) POSC interaction (XRD-1, T-I13): under `ofs.persist auto hold N` a
#       crashed transfer's partial is unpersisted after the hold window and
#       the TPCR startup warning fires; under default persistence the
#       partial survives a server restart.
#
# Backends: POSIX localroot is what this testbed offers.  EC, CephFS and
# proxy/PSS paths are recorded unsupported-until-tested (XRD-4) in
# DECISIONS.md / QUESTIONS.md.
#
# Usage: backend_matrix.sh <build_dir>   (exit 127 = missing prerequisites)

set -u
BUILD_DIR="${1:?usage: backend_matrix.sh <build_dir>}"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
LIB_DIR="$BUILD_DIR/lib"
# The plugin suffix tracks the tree version (a tagless CI checkout builds
# -4, a v6.1.1 clone builds -6): discover it, never assume it.
XRDHTTP_LIB=$(ls "$LIB_DIR"/libXrdHttp-[0-9]*.so 2>/dev/null | head -1)
TPCR_LIB=$(ls "$LIB_DIR"/libXrdHttpTPCR-[0-9]*.so 2>/dev/null | head -1)
STOCK_TPC_LIB=$(ls "$LIB_DIR"/libXrdHttpTPC-[0-9]*.so 2>/dev/null | head -1)
[ -n "$XRDHTTP_LIB" ] || { echo "SKIP: libXrdHttp plugin missing"; exit 127; }
[ -n "$TPCR_LIB" ]    || { echo "SKIP: libXrdHttpTPCR plugin missing"; exit 127; }

command -v python3 >/dev/null || { echo "SKIP: python3"; exit 127; }
[ -x "$BUILD_DIR/bin/xrootd" ] || { echo "SKIP: xrootd binary"; exit 127; }

WORK="$(mktemp -d /tmp/tpcr-matrix-XXXXXX)"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

FAILURES=0
fail() { echo "FAIL: $*"; FAILURES=$((FAILURES + 1)); }
pass() { echo "PASS: $*"; }
pick_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()'; }

# ---------------------------------------------------------------------------
# (a) sync-then-kill durability (POSIX leg)
# ---------------------------------------------------------------------------
if python3 - "$WORK" <<'EOF'
import os, signal, sys
work = sys.argv[1]
path = os.path.join(work, "sync-durability.bin")
pid = os.fork()
if pid == 0:
    fd = os.open(path, os.O_CREAT | os.O_WRONLY, 0o644)
    os.pwrite(fd, b"A" * 4096, 0)
    os.fsync(fd)                      # SUB-1: the checkpoint's data sync
    os.kill(os.getpid(), signal.SIGKILL)  # die without close/flush
os.waitpid(pid, 0)
data = open(path, "rb").read()
assert data == b"A" * 4096, "fsynced data missing after kill -9"
EOF
then pass "(a) fsync survives kill -9 (POSIX; power-loss leg needs hardware, recorded)"
else fail "(a) sync durability"; fi

# ---------------------------------------------------------------------------
# (b) reopen without truncate + write at offset W (the FR-20 resume open)
# ---------------------------------------------------------------------------
if python3 - "$WORK" <<'EOF'
import os, sys
work = sys.argv[1]
path = os.path.join(work, "reopen.bin")
with open(path, "wb") as out:
    out.write(b"P" * 8192)            # the committed prefix [0, W)
# The resume open: create-if-missing, write-only, NO truncate.
fd = os.open(path, os.O_CREAT | os.O_WRONLY, 0o644)
os.pwrite(fd, b"N" * 4096, 8192)      # write at W
os.fsync(fd)
os.close(fd)
data = open(path, "rb").read()
assert data == b"P" * 8192 + b"N" * 4096, "prefix damaged or write misplaced"
EOF
then pass "(b) reopen-no-trunc + write at W keeps the prefix (POSIX)"
else fail "(b) reopen at W"; fi

# ---------------------------------------------------------------------------
# (c) sidecar temp/fsync/rename atomicity under kill -9 (XRD-5)
# ---------------------------------------------------------------------------
if python3 - "$WORK" <<'EOF'
import os, random, signal, sys, time
work = sys.argv[1]
journal = os.path.join(work, "dest.xrdtpcr")

def write_journal(generation):
    # The exact WP-7 update sequence: temp -> write -> fsync -> close -> rename.
    tmp = journal + ".tmp"
    payload = ("JRNL%08d" % generation).encode() * 64  # self-consistent record
    fd = os.open(tmp, os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
    os.write(fd, payload)
    os.fsync(fd)
    os.close(fd)
    os.rename(tmp, journal)

def valid(data):
    if len(data) != 12 * 64:
        return None
    stamp = data[:12]
    if data != stamp * 64 or not stamp.startswith(b"JRNL"):
        return None
    return int(stamp[4:])

write_journal(0)
rng = random.Random(42)
for iteration in range(1, 300):
    pid = os.fork()
    if pid == 0:
        # Child updates the journal; the parent SIGKILLs it at a random
        # moment, landing anywhere in the write/fsync/rename sequence.
        while True:
            write_journal(iteration)
    time.sleep(rng.uniform(0, 0.004))
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    generation = valid(open(journal, "rb").read())
    assert generation is not None, \
        "torn journal after kill at iteration %d" % iteration
try:
    os.unlink(journal + ".tmp")
except FileNotFoundError:
    pass
EOF
then pass "(c) temp+fsync+rename never yields a torn journal (300 random kills)"
else fail "(c) sidecar rename atomicity"; fi

# ---------------------------------------------------------------------------
# (d) POSC interaction (XRD-1): ofs.persist auto unlinks crashed partials
# ---------------------------------------------------------------------------
run_posc_leg() {
    local persist_line="$1" expect_survives="$2" tag="$3"
    local port dir
    port=$(pick_port)
    dir="$WORK/$tag"
    mkdir -p "$dir/admin" "$dir/data"
    cat > "$dir/xrootd.cfg" <<EOF
all.export /
all.adminpath $dir/admin
all.pidpath $dir/admin
oss.localroot $dir/data
xrd.port $port
xrd.protocol XrdHttp:$port $XRDHTTP_LIB
http.desthttps false
tpc.allow local
tpc.allow private
http.exthandler xrdtpcr +notls $TPCR_LIB
$persist_line
tpcr.blocksize 1m
EOF
    LD_LIBRARY_PATH="$LIB_DIR" "$BUILD_DIR/bin/xrootd" -c "$dir/xrootd.cfg" \
        -l "$dir/xrootd.log" -n srv > /dev/null 2>&1 &
    local xrd_pid=$!
    PIDS+=($xrd_pid)
    for _ in $(seq 1 150); do   # slow CI containers need up to ~30s
        curl -s -o /dev/null "http://127.0.0.1:$port/" && break; sleep 0.2
    done

    # Slow source so the kill lands mid-transfer.
    python3 -c "import random; n=8*1024*1024; rng=random.Random(3); open('$dir/ref.bin','wb').write(rng.getrandbits(n*8).to_bytes(n,'little'))"
    local mock_port; mock_port=$(pick_port)
    python3 "$SRC_DIR/mock_source.py" --port "$mock_port" --file "$dir/ref.bin" \
        --throttle $((512 * 1024)) > "$dir/mock.log" 2>&1 &
    local mock_pid=$!
    PIDS+=($mock_pid)
    # Wait until the mock actually serves (slow CI runners).
    for _ in $(seq 1 100); do
        curl -s -o /dev/null "http://127.0.0.1:$mock_port/ctl" && break; sleep 0.2
    done
    curl -s -N -X COPY "http://127.0.0.1:$port/crash.bin" \
        -H "Source: http://127.0.0.1:$mock_port/src.bin" \
        -H "X-Number-Of-Streams: 2" -H "Overwrite: T" > /dev/null 2>&1 &
    local copy_pid=$!
    # Kill only after the partial demonstrably exists and is growing
    # (fixed sleeps fired before the transfer started on slow CI runners).
    local deadline=$(( $(date +%s) + 60 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        [ "$(cat "$dir/data/crash.bin" 2>/dev/null | wc -c || echo 0)" \
          -ge $((512*1024)) ] 2>/dev/null && break
        sleep 0.5
    done
    kill -9 "$xrd_pid" 2>/dev/null   # gateway crash
    wait "$copy_pid" 2>/dev/null
    kill "$mock_pid" 2>/dev/null

    [ -f "$dir/data/crash.bin" ] || { fail "($tag) partial absent right after crash"; return; }

    # Restart: POSC recovery runs at initialization.
    LD_LIBRARY_PATH="$LIB_DIR" "$BUILD_DIR/bin/xrootd" -c "$dir/xrootd.cfg" \
        -l "$dir/restart.log" -n srv > /dev/null 2>&1 &
    local xrd2_pid=$!
    PIDS+=($xrd2_pid)
    sleep 6                          # past the 3s hold window
    kill "$xrd2_pid" 2>/dev/null

    if [ "$expect_survives" = yes ]; then
        if [ -f "$dir/data/crash.bin" ]; then
            pass "($tag) partial survives restart (resume-compatible persistence)"
        else
            fail "($tag) partial deleted under default persistence?!"
        fi
    else
        if [ -f "$dir/data/crash.bin" ]; then
            fail "($tag) partial survived under ofs.persist auto (expected unlink, XRD-1)"
        else
            pass "($tag) POSC auto unpersisted the crashed partial (XRD-1 confirmed)"
        fi
        if grep -q "POSC.*defeats\|defeats TPCR cross-session resume" "$dir/srv/restart.log" 2>/dev/null; then
            pass "($tag) TPCR hasPOSC startup warning fired"
        else
            fail "($tag) TPCR hasPOSC startup warning missing"
        fi
    fi
}

run_posc_leg "ofs.persist auto hold 3" no posc-auto
run_posc_leg "" yes persist-default

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "BACKEND MATRIX (POSIX leg): ALL CHECKS PASSED"
    echo "EC / CephFS / proxy-PSS backends unavailable on this testbed:"
    echo "recorded unsupported-until-tested (XRD-4) in DECISIONS.md."
    exit 0
fi
echo "$FAILURES backend matrix check(s) FAILED"
exit 1
