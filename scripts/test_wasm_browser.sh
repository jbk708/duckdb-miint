#!/usr/bin/env bash
# Browser check for the miint WASM extension: load it into duckdb-wasm the way
# the playground does, in headless Chrome, and run scripts/wasm_browser/steps.json.
#
# This, not test_wasm_load.sh, is the check for Rust code paths: its harness is a
# different main module and JS glue from the duckdb-wasm release users run, and
# Rust code depends on exactly the helpers that differ (docs/wasm-testing.md).
#
# Prerequisites:
#   - Node.js and npm (puppeteer-core is installed into scripts/wasm_browser/ on first run)
#   - Google Chrome or Chromium; set CHROME to its binary if it is not found
#   - Network access: duckdb-wasm is fetched from cdn.jsdelivr.net, as in the playground
#   - An extension build: ./scripts/build_wasm.sh
#
# Usage:
#   ./scripts/test_wasm_browser.sh [extension.wasm]   # default: the build/wasm_eh build
#   ./scripts/test_wasm_browser.sh --native [duckdb]  # same steps natively (default: build/release/duckdb)
#
# --native is how expected values are produced: a new step's `expect` is what the
# native build returns.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DRIVER_DIR="$SCRIPT_DIR/wasm_browser"

if ! command -v node &>/dev/null; then
    echo "ERROR: node not found. Install Node.js."
    exit 1
fi

if [ "${1:-}" = "--native" ]; then
    DUCKDB="${2:-$REPO_DIR/build/release/duckdb}"
    [ -x "$DUCKDB" ] || { echo "ERROR: $DUCKDB not found. Build it with: bash build.sh"; exit 1; }
    exec node "$DRIVER_DIR/driver.mjs" --native "$DUCKDB"
fi

WASM_FILE="${1:-$(find "$REPO_DIR/build/wasm_eh" -name "miint.duckdb_extension.wasm" 2>/dev/null | head -1)}"
if [ -z "$WASM_FILE" ] || [ ! -f "$WASM_FILE" ]; then
    echo "ERROR: no extension .wasm found. Build it with: ./scripts/build_wasm.sh"
    exit 1
fi

if [ -z "${CHROME:-}" ]; then
    for c in "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
             google-chrome google-chrome-stable chromium chromium-browser; do
        if [ -x "$c" ] || command -v "$c" &>/dev/null; then
            CHROME="$(command -v "$c" || echo "$c")"
            break
        fi
    done
fi
if [ -z "${CHROME:-}" ]; then
    echo "ERROR: Chrome or Chromium not found. Set CHROME to its binary."
    exit 1
fi

if [ ! -d "$DRIVER_DIR/node_modules/puppeteer-core" ]; then
    echo "Installing puppeteer-core into $DRIVER_DIR ..."
    npm ci --prefix "$DRIVER_DIR" --ignore-scripts --no-audit --no-fund
fi

echo "=== Browser check: $WASM_FILE ==="
exec node "$DRIVER_DIR/driver.mjs" --wasm "$WASM_FILE" --chrome "$CHROME"
