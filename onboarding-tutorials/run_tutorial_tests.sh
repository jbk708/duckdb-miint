#!/bin/bash
# Run tutorial code blocks extracted from markdown files.
#
# Usage:
#   bash onboarding-tutorials/run_tutorial_tests.sh [duckdb_path]
#   MIINT_TUTORIAL_LIVE=1 bash onboarding-tutorials/run_tutorial_tests.sh   # ENA itself, not the mock
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

# The tutorials read ERR1074767 from ENA. Unless MIINT_TUTORIAL_LIVE=1, ENA's Portal API
# is test/scripts/ena_webin_mock.py, which serves that run from data/ena/: EBI's outages
# would otherwise fail these tests for reasons no change made. CI runs them live nightly.
if [ "${MIINT_TUTORIAL_LIVE:-0}" != "1" ]; then
    MOCK_BOOT=$(mktemp)
    python3 "$SCRIPT_DIR/../test/scripts/ena_webin_mock.py" 0 > "$MOCK_BOOT" 2>&1 &
    MOCK_PID=$!
    trap 'kill $MOCK_PID 2>/dev/null || true; rm -f "$MOCK_BOOT"' EXIT
    for _ in $(seq 1 50); do
        grep -q '^ENA_WEBIN_MOCK_URL=' "$MOCK_BOOT" 2>/dev/null && break
        sleep 0.1
    done
    MOCK_URL=$(sed -n 's/^ENA_WEBIN_MOCK_URL=//p' "$MOCK_BOOT")
    if [ -z "$MOCK_URL" ]; then
        echo "ERROR: the ENA mock did not start:" >&2
        cat "$MOCK_BOOT" >&2
        exit 1
    fi
    export MIINT_ENA_PORTAL_URL_BASE="$MOCK_URL/portal/api"
    echo "ENA: the mock at $MOCK_URL (MIINT_TUTORIAL_LIVE=1 reads from ENA itself)"
else
    echo "ENA: live"
fi

PASS=0
FAIL=0

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

# --------------------------------------------------------------------------
# Test: beginner.md (Python)
# --------------------------------------------------------------------------

echo "=== Testing beginner.md ==="

BEGINNER_PY="$OUTDIR/beginner_test.py"
extract_blocks "$SCRIPT_DIR/beginner.md" "python" \
    | rewrite_extension_loading_python \
    > "$BEGINNER_PY"

PYTHON="${PYTHON:-conda run -n duckdb-151 python3}"
if $PYTHON "$BEGINNER_PY" 2>&1; then
    echo "PASS: beginner.md"
    PASS=$((PASS + 1))
else
    echo "FAIL: beginner.md"
    FAIL=$((FAIL + 1))
fi

# --------------------------------------------------------------------------
# Test: intermediate.md (SQL)
# --------------------------------------------------------------------------

echo ""
echo "=== Testing intermediate.md ==="

INTERMEDIATE_SQL="$OUTDIR/intermediate_test.sql"
extract_blocks "$SCRIPT_DIR/intermediate.md" "sql" \
    | rewrite_extension_loading_cli \
    > "$INTERMEDIATE_SQL"

if "$DUCKDB" < "$INTERMEDIATE_SQL" 2>&1; then
    echo "PASS: intermediate.md"
    PASS=$((PASS + 1))
else
    echo "FAIL: intermediate.md"
    FAIL=$((FAIL + 1))
fi

# --------------------------------------------------------------------------
# Test: advanced.md
# --------------------------------------------------------------------------

echo ""
echo "=== Testing advanced.md (SQL blocks) ==="

ADVANCED_SQL="$OUTDIR/advanced_test.sql"
extract_blocks "$SCRIPT_DIR/advanced.md" "sql" \
    | rewrite_extension_loading_cli \
    > "$ADVANCED_SQL"

if "$DUCKDB" < "$ADVANCED_SQL" 2>&1; then
    echo "PASS: advanced.md (SQL)"
    PASS=$((PASS + 1))
else
    echo "FAIL: advanced.md (SQL)"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=== Testing advanced.md (bash blocks) ==="

# Extract bash blocks, rewrite duckdb references and extension loading
ADVANCED_BASH="$OUTDIR/advanced_test.sh"
extract_blocks "$SCRIPT_DIR/advanced.md" "bash" \
    | sed "s|duckdb |${DUCKDB} |g" \
    | rewrite_extension_loading_cli \
    > "$ADVANCED_BASH"

if bash "$ADVANCED_BASH" 2>&1; then
    echo "PASS: advanced.md (bash)"
    PASS=$((PASS + 1))
else
    echo "FAIL: advanced.md (bash)"
    FAIL=$((FAIL + 1))
fi

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
