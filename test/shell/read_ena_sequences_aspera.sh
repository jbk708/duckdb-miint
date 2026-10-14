#!/bin/bash
# read_ena_sequences when ascp fails before sending any data, as it does when the
# server rejects the key. test/shell/aspera_stubs/ascp stands in for ascp: it
# prompts for a password on stdin and exits 1 when none comes.
#
# - download_method => 'aspera' raises an error that carries ascp's message.
# - download_method => 'auto' (the default) falls back to HTTP with a warning.
#
# Neither may hang: a stub that inherits a closed stdin blocks on its prompt,
# so every query runs under a hard timeout. Needs live ENA for run resolution.

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
FAILED=0

# run_query SQL: sets OUT (stdout + stderr) and RC. SIGKILL, because ascp ignores
# SIGTERM and a stalled one would outlive a gentler timeout.
run_query() {
    RC=0
    OUT="$("$TIMEOUT_CMD" -s KILL 120 "$DUCKDB" -noheader -list -c "LOAD httpfs; $1" 2>&1)" || RC=$?
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

echo "Running: download_method => 'auto' falls back to HTTP"
run_query "SELECT COUNT(*) FROM read_ena_sequences('ERR1074767', max_sequences => 10);"
ok=0
if [ "$RC" -eq 0 ] && echo "$OUT" | grep -qx "10" && echo "$OUT" | grep -q "falling back to HTTP"; then ok=1; fi
check "10 rows over HTTP, with a warning" "$ok"

if [ "$FAILED" -ne 0 ]; then
    exit 1
fi
echo "All Aspera failure tests passed"
