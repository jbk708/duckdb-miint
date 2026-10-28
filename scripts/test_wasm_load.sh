#!/usr/bin/env bash
# Headless end-to-end WASM extension load test.
#
# Builds a DuckDB WASM main module from our submodule source, compiles a
# C test harness that loads the miint extension as a WASM side module,
# and runs it in Node.js. This guarantees ABI compatibility because both
# DuckDB and the extension are built from the same source tree.
#
# Prerequisites:
#   - emscripten (emsdk) installed and activated
#   - VCPKG_TOOLCHAIN_PATH set (or vcpkg/ in repo root)
#   - Node.js
#   - Extension already built: ./scripts/build_wasm.sh --build-only
#
# Usage:
#   ./scripts/test_wasm_load.sh                    # Build DuckDB + test harness, run test
#   ./scripts/test_wasm_load.sh --harness-only     # Skip DuckDB build (reuse existing)
#   ./scripts/test_wasm_load.sh --clean            # Clean and rebuild everything

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

HARNESS_ONLY=false
CLEAN=false

for arg in "$@"; do
    case "$arg" in
        --harness-only) HARNESS_ONLY=true ;;
        --clean)        CLEAN=true ;;
        -h|--help)
            echo "Usage: $0 [--harness-only] [--clean]"
            echo ""
            echo "  Builds DuckDB WASM from source, compiles a test harness, and"
            echo "  loads the miint extension as a WASM side module via Node.js."
            echo ""
            echo "  --harness-only  Skip DuckDB WASM build (reuse build/wasm_shell/)"
            echo "  --clean         Remove build/wasm_shell/ and rebuild"
            exit 0
            ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

# Check prerequisites
if ! command -v emcc &>/dev/null; then
    echo "ERROR: emcc not found. Install and activate emsdk."
    exit 1
fi
if ! command -v node &>/dev/null; then
    echo "ERROR: node not found. Install Node.js."
    exit 1
fi

if [ -z "${VCPKG_TOOLCHAIN_PATH:-}" ]; then
    if [ -d "$REPO_DIR/vcpkg" ]; then
        export VCPKG_TOOLCHAIN_PATH="$REPO_DIR/vcpkg/scripts/buildsystems/vcpkg.cmake"
    else
        echo "ERROR: VCPKG_TOOLCHAIN_PATH not set and no vcpkg/ directory found."
        exit 1
    fi
fi

cd "$REPO_DIR"

# Check that the extension has been built
WASM_EXT="build/wasm_eh/extension/miint/miint.duckdb_extension.wasm"
JSON_EXT="build/wasm_eh/extension/json/json.duckdb_extension.wasm"
if [ ! -f "$WASM_EXT" ]; then
    echo "ERROR: Extension not found at $WASM_EXT"
    echo "Run ./scripts/build_wasm.sh --build-only first."
    exit 1
fi
if [ ! -f "$JSON_EXT" ]; then
    echo "ERROR: json extension not found at $JSON_EXT"
    echo "It should have been built alongside miint by make wasm_eh."
    exit 1
fi

DUCKDB_BUILD="build/wasm_shell"

# LOAD refuses an extension stamped for another DuckDB version, so the core is
# configured with the version in the extension's metadata footer (the 32-byte
# fields in its last 512 bytes): v2.0.0 when it was built with DUCKDB_VERSION
# set, where a checkout's git describe gives an untagged v2.0.0-devN.
EXT_DUCKDB_VERSION="$(python3 -c '
import sys
tail = open(sys.argv[1], "rb").read()[-512:]
print(tail[5 * 32:6 * 32].rstrip(b"\0").decode())' "$WASM_EXT")"
if [ -z "$EXT_DUCKDB_VERSION" ]; then
    echo "ERROR: no DuckDB version in the metadata footer of $WASM_EXT"
    exit 1
fi

# Step 1: Build DuckDB WASM core from our submodule
if $CLEAN; then
    echo "Cleaning DuckDB WASM build..."
    rm -rf "$DUCKDB_BUILD"
fi

if ! $HARNESS_ONLY; then
    if [ ! -f "$DUCKDB_BUILD/src/libduckdb_static.a" ]; then
        echo "=== Building DuckDB WASM core (this takes a few minutes) ==="

        # Find Emscripten.cmake
        EMSCRIPTEN_CMAKE=""
        if [ -n "${EMSDK:-}" ]; then
            EMSCRIPTEN_CMAKE="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
        else
            EMSCRIPTEN_CMAKE="$(dirname "$(which emcc)")/../cmake/Modules/Platform/Emscripten.cmake"
        fi
        if [ ! -f "$EMSCRIPTEN_CMAKE" ]; then
            echo "ERROR: Cannot find Emscripten.cmake. Set EMSDK environment variable."
            exit 1
        fi

        mkdir -p "$DUCKDB_BUILD"
        emcmake cmake \
            -DVCPKG_BUILD=1 \
            -DVCPKG_MANIFEST_DIR="$REPO_DIR" \
            -DCMAKE_TOOLCHAIN_FILE="$VCPKG_TOOLCHAIN_PATH" \
            -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$EMSCRIPTEN_CMAKE" \
            -DWASM_LOADABLE_EXTENSIONS=1 \
            -DCMAKE_CXX_FLAGS="-fwasm-exceptions -DWEBDB_FAST_EXCEPTIONS=1" \
            -DDUCKDB_EXPLICIT_PLATFORM=wasm_eh \
            -DDUCKDB_CAPABILITIES=loadable_extensions \
            -DSTATICALLY_LINK_EXTENSIONS="parquet core_functions" \
            -DOVERRIDE_GIT_DESCRIBE="$EXT_DUCKDB_VERSION" \
            -S duckdb -B "$DUCKDB_BUILD" 2>&1 | tail -5

        emmake make -j$(nproc) -C "$DUCKDB_BUILD" duckdb 2>&1 | tail -5
        echo "$EXT_DUCKDB_VERSION" > "$DUCKDB_BUILD/miint_duckdb_version"
        echo ""
    else
        echo "DuckDB WASM core already built (use --clean to rebuild, e.g. when"
        echo "LOAD reports that the extension was built for another version)"
    fi
fi

if [ ! -f "$DUCKDB_BUILD/src/libduckdb_static.a" ]; then
    echo "ERROR: libduckdb_static.a not found. Run without --harness-only."
    exit 1
fi
# A cached core keeps the version it was configured with; one that recorded
# none predates the record and is treated as a mismatch.
CORE_VERSION_FILE="$DUCKDB_BUILD/miint_duckdb_version"
CORE_VERSION="$(cat "$CORE_VERSION_FILE" 2>/dev/null || echo "unrecorded")"
if [ "$CORE_VERSION" != "$EXT_DUCKDB_VERSION" ]; then
    echo "ERROR: the cached core in $DUCKDB_BUILD is DuckDB $CORE_VERSION,"
    echo "but the extension is stamped $EXT_DUCKDB_VERSION. Rerun with --clean."
    exit 1
fi

# Step 2: Compile test harness.
# - DuckDB v2 registers statically linked extensions through a generated
#   duckdb_register_static_extensions(), which the harness calls before opening
#   the database. loadable_extensions is the capability LOAD of a .wasm needs.
# - The harness is C, but DuckDB is C++, so the link goes through em++: emcc
#   links no C++ runtime (__cxa_throw, the wasm exception personality), and the
#   main module would then fail when it starts.
# - DuckDB references sem_timedwait, which emscripten's single-threaded libc
#   lacks, so undefined symbols cannot be a link error. Emscripten reports some
#   undefined symbols at link time (the missing C++ runtime was reported), and
#   any other one fails the build here. The rest the main-module link imports
#   silently; those fail when the module starts, in step 3.
echo "=== Compiling test harness ==="
LINK_EXTENSIONS="loadable_extensions parquet core_functions"
python3 duckdb/scripts/generate_static_extension_loader.py \
    -o "$DUCKDB_BUILD/static_extension_loader.c" $LINK_EXTENSIONS
for src in scripts/test_wasm_extension.c "$DUCKDB_BUILD/static_extension_loader.c"; do
    emcc -c "$src" -I duckdb/src/include -fwasm-exceptions -O2 \
        -o "$DUCKDB_BUILD/$(basename "${src%.c}").o"
done
LINK_LOG="$DUCKDB_BUILD/test_wasm_harness_link.log"
# The extension archives go before libduckdb_static.a (see the generated loader).
if ! em++ "$DUCKDB_BUILD/test_wasm_extension.o" "$DUCKDB_BUILD/static_extension_loader.o" \
    "$DUCKDB_BUILD/src/main/extension/libduckdb_loadable_extensions.a" \
    "$DUCKDB_BUILD/extension/parquet/libparquet_extension.a" \
    "$DUCKDB_BUILD/extension/core_functions/libcore_functions_extension.a" \
    "$DUCKDB_BUILD/src/libduckdb_static.a" \
    "$DUCKDB_BUILD/vcpkg_installed/wasm32-emscripten/lib/libz.a" \
    -sMAIN_MODULE=1 \
    -sALLOW_MEMORY_GROWTH=1 \
    -sINITIAL_MEMORY=256MB \
    -sERROR_ON_UNDEFINED_SYMBOLS=0 \
    -sEXPORT_EXCEPTION_HANDLING_HELPERS=1 \
    -fwasm-exceptions \
    -O2 \
    --preload-file "$WASM_EXT"@/extensions/miint.duckdb_extension \
    --preload-file "$JSON_EXT"@/extensions/json.duckdb_extension \
    -o scripts/test_wasm_harness.js > "$LINK_LOG" 2>&1; then
    cat "$LINK_LOG"
    echo "ERROR: linking the test harness failed"
    exit 1
fi
grep -i 'warning' "$LINK_LOG" | grep -v 'undefined symbol: sem_timedwait ' || true
UNEXPECTED="$(grep 'undefined symbol:' "$LINK_LOG" | grep -v 'undefined symbol: sem_timedwait ' || true)"
if [ -n "$UNEXPECTED" ]; then
    echo "$UNEXPECTED"
    echo "ERROR: the test harness has undefined symbols (full log: $LINK_LOG)"
    exit 1
fi
echo ""

# Step 3: Run the test
echo "=== Running headless WASM load test ==="
cd scripts && node test_wasm_harness.js
