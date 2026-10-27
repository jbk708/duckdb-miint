#!/bin/bash
# Many DatabaseInstances in one process LOADing the extension at once. miint's HDF5
# build is not threadsafe, so a racing HDF5 call crashes the process; any non-zero exit fails.

set -u

EXT="$PWD/build/release/extension/miint/miint.duckdb_extension"
PY="${MIINT_TEST_PYTHON:-python3}"
PROCS="${MIINT_LOAD_RACE_PROCS:-50}"
# A process takes about 5 s on a 16-core node; past this it is deadlocked, not slow.
PROC_TIMEOUT=60
THREADS=16
LOADS=20

if [ -z "${DUCKDB_PYTHON_AVAILABLE:-}" ]; then
    echo "DUCKDB_PYTHON_AVAILABLE not set, skipping concurrent LOAD tests"
    exit 0
fi
if [ -z "${HDF5_AVAILABLE:-}" ]; then
    echo "HDF5_AVAILABLE not set, skipping concurrent LOAD tests"
    exit 0
fi
TIMEOUT_CMD=$(command -v timeout || command -v gtimeout || true)
if [ -z "$TIMEOUT_CMD" ]; then
    echo "ERROR: no timeout or gtimeout command; a deadlocked LOAD would hang instead of failing" >&2
    exit 1
fi

SCRIPT=$(
    cat <<'EOF'
import sys, threading
import duckdb

ext, threads, loads = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
start = threading.Barrier(threads)

def worker():
    start.wait()
    for _ in range(loads):
        conn = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
        conn.execute(f"LOAD '{ext}'")
        conn.close()

ts = [threading.Thread(target=worker) for _ in range(threads)]
for t in ts:
    t.start()
for t in ts:
    t.join()
EOF
)

echo "Running: $PROCS processes, $THREADS threads each loading miint $LOADS times"
for i in $(seq 1 "$PROCS"); do
    out=$(PYTHONFAULTHANDLER=1 "$TIMEOUT_CMD" --kill-after=10 "$PROC_TIMEOUT" \
        "$PY" -c "$SCRIPT" "$EXT" "$THREADS" "$LOADS" 2>&1)
    status=$?
    if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
        echo "  ✗ FAIL: process $i timed out after ${PROC_TIMEOUT}s or was killed"
        echo "$out" | tail -n 40 | sed 's/^/      /'
        exit 1
    elif [ "$status" -ne 0 ]; then
        echo "  ✗ FAIL: process $i exited $status"
        { echo "$out" | head -n 5; echo "..."; echo "$out" | tail -n 40; } | sed 's/^/      /'
        exit 1
    fi
done
echo "  ✓ PASS"
echo "All tests passed!"
