#!/bin/bash
# read_ena_sequences when ascp fails before sending any data, as it does when the
# server rejects the key. test/shell/aspera_stubs/ascp stands in for ascp: it
# prompts for a password on stdin and exits 1 when none comes.
#
# - download_method => 'aspera' raises an error that carries ascp's message,
#   also when DuckDB itself runs with stdin closed.
# - download_method => 'auto' (the default) falls back to HTTP with a warning,
#   once per scan: later runs go straight to HTTP.
# - an ascp that exits 0 without sending anything is a failure too.
#
# None may hang: a stub that inherits a closed stdin blocks on its prompt, so
# every query runs under a hard timeout. Needs live ENA for run resolution.

set -u

DUCKDB="./build/release/duckdb"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ -z "${ENA_AVAILABLE:-}" ]; then
    echo "ENA_AVAILABLE not set, skipping Aspera failure tests"
    exit 0
fi
TIMEOUT_CMD="$(command -v timeout || command -v gtimeout || true)"
if [ -z "$TIMEOUT_CMD" ]; then
    echo "ERROR: no timeout command; a regression here hangs instead of failing" >&2
    exit 1
fi

export PATH="$HERE/aspera_stubs:$PATH"
# Key lookup tries $CONDA_PREFIX/etc first; the placeholder there keeps it from
# downloading EBI's key, so the test does not depend on that download.
export CONDA_PREFIX="$HERE/aspera_stubs"
FAILED=0

# run_query SQL [VAR=value ...]: sets OUT (stdout + stderr) and RC. The VAR=value
# pairs go to the environment; CLOSE_STDIN=1 runs DuckDB with stdin closed.
# SIGKILL, because ascp ignores SIGTERM and a stalled one would outlive a
# gentler timeout.
run_query() {
    local sql="LOAD httpfs; $1"
    shift
    RC=0
    if [ -n "${CLOSE_STDIN:-}" ]; then
        OUT="$(env "$@" "$TIMEOUT_CMD" -s KILL 120 "$DUCKDB" -noheader -list -c "$sql" 2>&1 0<&-)" || RC=$?
    else
        OUT="$(env "$@" "$TIMEOUT_CMD" -s KILL 120 "$DUCKDB" -noheader -list -c "$sql" 2>&1)" || RC=$?
    fi
    # The shell colours warnings, which leaves escape codes on the result line.
    OUT="$(printf '%s\n' "$OUT" | sed 's/\x1b\[[0-9;]*m//g')"
}

check() {
    local name="$1" ok="$2"
    if [ "$ok" = 1 ]; then
        echo "  PASS: $name"
    else
        echo "  FAIL: $name (exit $RC)"
        while IFS= read -r line; do echo "    $line"; done <<<"$OUT"
        FAILED=1
    fi
}

echo "Running: download_method => 'aspera' raises ascp's error"
run_query "SELECT COUNT(*) FROM read_ena_sequences('ERR1074767', download_method => 'aspera', max_sequences => 10);"
ok=0
if [ "$RC" -ne 0 ] && [ "$RC" -ne 137 ] && echo "$OUT" | grep -q "failed to authenticate"; then ok=1; fi
check "error names ascp's failure" "$ok"

# filepath names where the rows came from: the HTTPS URL, not the Aspera path.
echo "Running: download_method => 'auto' falls back to HTTP"
run_query "SELECT COUNT(*), bool_and(filepath LIKE 'https://%') FROM read_ena_sequences('ERR1074767', include_filepath => true, max_sequences => 10);"
ok=0
if [ "$RC" -eq 0 ] && echo "$OUT" | grep -qx "10|true" && echo "$OUT" | grep -q "falling back to HTTP"; then ok=1; fi
check "10 rows over HTTP, with a warning and HTTPS filepaths" "$ok"

# A parent with fd 0 closed hands the lowest free descriptor to whatever the
# child opens; the child's stdin must still be /dev/null.
echo "Running: download_method => 'aspera' with DuckDB's stdin closed"
CLOSE_STDIN=1 run_query "SELECT COUNT(*) FROM read_ena_sequences('ERR1074767', download_method => 'aspera', max_sequences => 10);"
ok=0
if [ "$RC" -ne 0 ] && [ "$RC" -ne 137 ] && echo "$OUT" | grep -q "failed to authenticate"; then ok=1; fi
check "error names ascp's failure" "$ok"

# One thread, so the second run opens after the first has failed over.
echo "Running: download_method => 'auto' over two runs falls back once"
run_query "SET threads = 1; SELECT COUNT(*) FROM read_ena_sequences(['ERR1074767', 'ERR10360000'], max_sequences => 10);"
ok=0
# The shell prints each warning twice, so count the runs the warnings name.
fell_back=$(echo "$OUT" | grep "falling back to HTTP" | grep -o "for run '[^']*'" | sort -u | wc -l)
if [ "$RC" -eq 0 ] && echo "$OUT" | grep -qx "20" && [ "$fell_back" -eq 1 ]; then ok=1; fi
check "20 rows over HTTP, with a warning for one run" "$ok"

echo "Running: an ascp that exits 0 without sending data"
run_query "SELECT COUNT(*) FROM read_ena_sequences('ERR1074767', download_method => 'aspera', max_sequences => 10);" \
    MIINT_ASCP_STUB_MODE=empty
ok=0
if [ "$RC" -ne 0 ] && [ "$RC" -ne 137 ] && echo "$OUT" | grep -q "exited with code 0 before sending any data"; then ok=1; fi
check "error says no data came" "$ok"

if [ "$FAILED" -ne 0 ]; then
    exit 1
fi
echo "All Aspera failure tests passed"
