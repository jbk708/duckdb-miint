#!/bin/bash
# Run tutorial code blocks extracted from markdown files.
#
# Usage:
#   bash onboarding-tutorials/run_tutorial_tests.sh [duckdb_path]
#   MIINT_TUTORIAL_LIVE=1 bash onboarding-tutorials/run_tutorial_tests.sh   # ENA itself, not the mock
#   MIINT_TUTORIAL_LIVE_ATTEMPTS=1 MIINT_TUTORIAL_LIVE=1 bash ...              # no retries when live
#
# duckdb_path defaults to ./build/release/duckdb
#
# For each tutorial:
#   - beginner.md: concatenate all ```python blocks into one script, run it
#   - intermediate.md: concatenate all ```sql blocks into one session, run via duckdb
#   - advanced.md: run ```bash blocks independently, ```sql blocks as one session
#
# The INSTALL/LOAD miint statements are rewritten to use the local build.

set -euo pipefail

DUCKDB="${1:-./build/release/duckdb}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="${TMPDIR:-/tmp}/miint_tutorial_tests"
mkdir -p "$OUTDIR"

# Locate the built extension for the Python test. The CLI has miint statically
# linked so "LOAD miint" works, but the Python duckdb package needs the
# .duckdb_extension file loaded by path with unsigned extensions enabled.
EXT_PATH="$(cd "$(dirname "$DUCKDB")" && pwd)/extension/miint/miint.duckdb_extension"
if [ ! -f "$EXT_PATH" ]; then
    EXT_PATH="$(pwd)/build/release/extension/miint/miint.duckdb_extension"
fi

# The tutorials read ERR1074767 from ENA, or, unless MIINT_TUTORIAL_LIVE=1, from the ENA
# mock (docs/testing.md, "Guards and skipped tests").
source "$SCRIPT_DIR/../test/scripts/ena_mock_env.sh"
if [ "${MIINT_TUTORIAL_LIVE:-0}" != "1" ]; then
    # Set first, so an exit while the mock boots still stops it.
    trap 'kill ${ENA_MOCK_PID:-} 2>/dev/null || true' EXIT
    if ! start_ena_mock; then
        echo "ERROR: the ENA mock did not start" >&2
        exit 1
    fi
    echo "ENA: the mock at $ENA_WEBIN_MOCK_URL (MIINT_TUTORIAL_LIVE=1 reads from ENA itself)"
else
    unset_ena_mock_env
    echo "ENA: live"
fi

PASS=0
FAIL=0

# Live, ENA can fail a run that no change caused, e.g. by dropping the FASTQ transfer
# partway (docs/testing.md, "Guards and skipped tests"), so each tutorial gets
# MIINT_TUTORIAL_LIVE_ATTEMPTS attempts (default 3). Against the mock a failure is ours,
# so it gets one.
if [ "${MIINT_TUTORIAL_LIVE:-0}" = "1" ]; then
    ATTEMPTS="${MIINT_TUTORIAL_LIVE_ATTEMPTS:-3}"
else
    ATTEMPTS=1
fi
RETRY_DELAY_SECONDS=60

# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

extract_blocks() {
    # Extract fenced code blocks of a given language from a markdown file.
    # Usage: extract_blocks <file> <language>
    # Outputs the concatenated content of all matching blocks.
    local file="$1"
    local lang="$2"
    awk -v lang="$lang" '
        /^```'"$lang"'/ { capture=1; next }
        /^```/ { if (capture) { capture=0; print "" } ; next }
        capture { print }
    ' "$file"
}

rewrite_extension_loading_cli() {
    # For the built CLI binary which has miint statically linked:
    # just strip the INSTALL, keep LOAD miint as-is.
    sed \
        -e "s|INSTALL miint FROM community; ||g" \
        -e "s|INSTALL miint FROM community;||g"
}

rewrite_extension_loading_python() {
    # For the Python duckdb package which needs the .duckdb_extension file:
    # load the unsigned local build by path when available.
    # allow_unsigned_extensions must be set at connect() time in Python,
    # so we patch the connect() call and the LOAD statement separately.
    if [ -f "$EXT_PATH" ]; then
        sed \
            -e "s|duckdb.connect()|duckdb.connect(config={'allow_unsigned_extensions': 'true'})|g" \
            -e "s|INSTALL miint FROM community; LOAD miint;|LOAD '${EXT_PATH}';|g" \
            -e "s|INSTALL miint FROM community;||g" \
            -e "s|LOAD miint;|LOAD '${EXT_PATH}';|g"
    else
        sed \
            -e "s|INSTALL miint FROM community; ||g" \
            -e "s|INSTALL miint FROM community;||g"
    fi
}

run_tutorial() {
    # Run a tutorial, up to $ATTEMPTS times, and count it as passed or failed.
    # Usage: run_tutorial <name> <command> [args...]
    # A pass after a failed attempt is reported as a GitHub Actions warning, so a flaky
    # ENA stays visible on a green run.
    local name="$1"
    shift
    local attempt
    for attempt in $(seq 1 "$ATTEMPTS"); do
        if "$@" 2>&1; then
            if [ "$attempt" -gt 1 ]; then
                echo "::warning::$name passed on attempt $attempt of $ATTEMPTS"
            fi
            echo "PASS: $name"
            PASS=$((PASS + 1))
            return
        fi
        if [ "$attempt" -lt "$ATTEMPTS" ]; then
            echo "RETRY: $name failed on attempt $attempt of $ATTEMPTS; retrying in ${RETRY_DELAY_SECONDS}s"
            sleep "$RETRY_DELAY_SECONDS"
        fi
    done
    echo "FAIL: $name"
    FAIL=$((FAIL + 1))
}

run_sql_file() {
    "$DUCKDB" < "$1"
}

# --------------------------------------------------------------------------
# Test: beginner.md (Python)
# --------------------------------------------------------------------------

echo "=== Testing beginner.md ==="

BEGINNER_PY="$OUTDIR/beginner_test.py"
extract_blocks "$SCRIPT_DIR/beginner.md" "python" \
    | rewrite_extension_loading_python \
    > "$BEGINNER_PY"

PYTHON="${PYTHON:-conda run -n duckdb-151 python3}"
# $PYTHON is unquoted on purpose: it may be a command with arguments.
run_tutorial beginner.md $PYTHON "$BEGINNER_PY"

# --------------------------------------------------------------------------
# Test: intermediate.md (SQL)
# --------------------------------------------------------------------------

echo ""
echo "=== Testing intermediate.md ==="

INTERMEDIATE_SQL="$OUTDIR/intermediate_test.sql"
extract_blocks "$SCRIPT_DIR/intermediate.md" "sql" \
    | rewrite_extension_loading_cli \
    > "$INTERMEDIATE_SQL"

run_tutorial intermediate.md run_sql_file "$INTERMEDIATE_SQL"

# --------------------------------------------------------------------------
# Test: advanced.md
# --------------------------------------------------------------------------

echo ""
echo "=== Testing advanced.md (SQL blocks) ==="

ADVANCED_SQL="$OUTDIR/advanced_test.sql"
extract_blocks "$SCRIPT_DIR/advanced.md" "sql" \
    | rewrite_extension_loading_cli \
    > "$ADVANCED_SQL"

run_tutorial "advanced.md (SQL)" run_sql_file "$ADVANCED_SQL"

echo ""
echo "=== Testing advanced.md (bash blocks) ==="

# Extract bash blocks, rewrite duckdb references and extension loading
ADVANCED_BASH="$OUTDIR/advanced_test.sh"
extract_blocks "$SCRIPT_DIR/advanced.md" "bash" \
    | sed "s|duckdb |${DUCKDB} |g" \
    | rewrite_extension_loading_cli \
    > "$ADVANCED_BASH"

run_tutorial "advanced.md (bash)" bash "$ADVANCED_BASH"

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------

echo ""
echo "==============================="
echo "Tutorial tests: $PASS passed, $FAIL failed"
echo "==============================="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
