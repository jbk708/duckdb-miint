# WASM Extension Testing

This document describes how to build and verify the miint WASM extension
locally **before** publishing to community extensions. There are four levels
of verification, each catching different classes of bugs.

## Prerequisites

- **emscripten** (emsdk) installed and activated (`emcc` in PATH)
- **VCPKG_TOOLCHAIN_PATH** set, or `vcpkg/` symlinked in repo root
- **Rust** via rustup. The build installs the pinned toolchain (1.86.0) with the
  `wasm32-unknown-emscripten` target and the `rust-src` component it needs
- **Node.js** (for import verification and headless load test)
- **Google Chrome** or Chromium, and network access, for the browser check (Level 4)

## Quick Start

```bash
# Full pipeline: build extension, verify imports, load test, browser check
./scripts/build_wasm.sh
./scripts/test_wasm_load.sh
./scripts/test_wasm_browser.sh
```

## Level 1: Build Succeeds

Catches: linker errors, missing symbols, `-fPIC` issues, Rust PIC problems.

```bash
./scripts/build_wasm.sh --build-only
```

This runs `make wasm_eh` which builds the extension as a WASM side module.
The output is `build/wasm_eh/extension/miint/miint.duckdb_extension.wasm`.

Use `--clean` for a full rebuild (necessary after changing CMake flags or
updating submodules):

```bash
./scripts/build_wasm.sh --clean --build-only
```

### What it verifies

- All C libraries (htslib, minimap2, WFA2, vsearch) compile for WASM
- Rust rype library compiles with PIC for WASM
- The emcc post-build linking step succeeds (all static libraries linked
  into the side module via `DUCKDB_EXTENSION_MIINT_LINKED_LIBS`)
- A `.wasm` file is produced

The Catch2 `tests` target (`add_executable(tests …)` in `CMakeLists.txt`) is not gated on
Emscripten, so `test/cpp/` is held to the same rule as `src/`: a POSIX-only call outside a
`MIINT_ENABLE_*` or `__EMSCRIPTEN__` guard breaks the WASM build. Pull-request CI does not
build `wasm_eh`; it builds on tag pushes, so such a break first appears at release unless
this level is run locally.

### What it does NOT catch

- Unresolved symbols that become WASM imports (deferred to load time)
- ABI mismatches between extension and DuckDB runtime
- Runtime errors in extension code

## Level 2: Import Verification

Catches: unresolved library symbols that would fail at load time (like the
`gzclose` bug).

```bash
./scripts/build_wasm.sh    # builds + verifies (default)
```

This uses Node.js to compile the `.wasm` file and inspect its imports/exports.
It checks that no symbols from our static libraries (zlib, htslib, minimap2,
WFA2, expat, rype) leak as unresolved imports.

### How PIC side modules work

In a PIC WASM side module (`-sSIDE_MODULE=2`), library symbols appear as:
- `GOT.func.X` / `GOT.mem.X` **imports** (PIC mechanism)
- Direct **exports** of `X` (the actual symbol definition)

A GOT import with a matching export is correct — the emscripten runtime
resolves it at load time by pointing the GOT entry at the module's own export.
A GOT import **without** a matching export means the library wasn't linked.

The verification script checks:
1. No direct `env.*` imports of library symbols (= not linked at all)
2. No `GOT.*` imports without matching exports (= linked but PIC broken)
3. `miint_duckdb_cpp_init` export exists (extension entry point)
4. In a build with wasm-native exceptions (`wasm_eh`), no imports of
   Emscripten's JS exception-handling helpers (`invoke_*`,
   `__cxa_find_matching_catch_*`, `__resumeException`). The build records its
   exception model as `MIINT_WASM_NATIVE_EH` in `CMakeCache.txt`; a build that
   does not record it fails the check rather than skipping it

### Why the Rust glue uses wasm-native exceptions

Rust code built with Emscripten's JS exceptions calls through `invoke_*` helpers
that duckdb-wasm generates in JS. On that path Emscripten truncates Rust symbol
names at `$`, so calls trap with `function signature mismatch`, and sign-extends
the low word of i64 arguments, so a Rust function receives a wrong value and
returns a wrong answer without any error. A Rust panic also aborts the module
instead of becoming a SQL error (#315). So on `wasm_eh`, where the C++ uses
`-fwasm-exceptions`, `CMakeLists.txt` builds the glue with
`-Zemscripten-wasm-eh` and rebuilds std with it (`-Zbuild-std`), and check 4
fails a build that still imports the JS helpers. `wasm_mvp`, and `wasm_threads`
as extension-ci-tools configures it, compile the C++ with JS exceptions; their
glue stays on JS exceptions, and neither is shipped.

## Level 3: Headless Load Test

Catches: ABI mismatches, extension initialization failures, runtime errors.

```bash
./scripts/test_wasm_load.sh
```

This is the **full end-to-end verification**. It:

1. Builds DuckDB WASM (`libduckdb_static.a`) from the `duckdb/` submodule
   using emscripten — guaranteeing the same ABI as the extension
2. Compiles `scripts/test_wasm_extension.c` as a WASM `MAIN_MODULE` linked
   against DuckDB, with the miint extension embedded via `--preload-file`
3. Runs the resulting `test_wasm_harness.js` in Node.js
4. Verifies: DuckDB opens, json extension loads, miint extension loads,
   `miint_version()` returns successfully

### First run

The first run builds DuckDB WASM from source (~5 minutes). Subsequent runs
reuse the cached build:

```bash
# First time: builds DuckDB + harness + runs test
./scripts/test_wasm_load.sh

# After code changes to extension only (DuckDB core unchanged):
./scripts/build_wasm.sh --build-only    # rebuild extension
./scripts/test_wasm_load.sh --harness-only  # recompile harness + run test

# Full clean rebuild:
./scripts/test_wasm_load.sh --clean
```

### Why not use the npm @duckdb/duckdb-wasm package?

The npm package bundles a pre-built DuckDB WASM, but:
- Its DuckDB version may not match our submodule exactly
- Even with matching version strings, different emsdk versions produce
  incompatible WASM binaries (different function signatures)
- This caused `LinkError: imported function does not match the expected type`
  in testing

Building DuckDB from our submodule eliminates all ABI mismatch issues.

## Level 4: Browser Check

Catches: Rust code that fails or computes a different answer in the runtime the
playground uses.

```bash
./scripts/test_wasm_browser.sh            # the build/wasm_eh extension
./scripts/test_wasm_browser.sh path/to/miint.duckdb_extension.wasm
```

It boots duckdb-wasm in headless Chrome the way the playground does
(`@duckdb/duckdb-wasm@1.33.1-dev64.0`, `selectBundle`, the eh bundle,
`allowUnsignedExtensions`), serves the extension with no cross-origin isolation
headers, like GitHub Pages, `LOAD`s it, and runs
`scripts/wasm_browser/steps.json`. Each step with an `expect` must return that
value. It exits nonzero on any mismatch or error.

**This is the check for Rust code paths, not Level 3.** The load test links its
own main module and JS glue from the `duckdb/` submodule, while the browser check
runs the duckdb-wasm release users get. The two differ in the JS helpers
Emscripten generates for a side module, which is where Rust code is exposed
(see "Why the Rust glue uses wasm-native exceptions"). When the load test was
used to judge Rust code, it reported every Rust-backed function as crashing,
including ones that work in the playground.

### Adding steps

A step is `{"name", "sql", "expect"}`; steps run in order on one connection, and a
step without `expect` (setup) only has to succeed. Make a checked step return one
VARCHAR cell (an `md5` over an ordered `string_agg` for anything bigger), so both
runtimes print it the same way. Take `expect` from a native build:

```bash
./scripts/test_wasm_browser.sh --native   # build/release/duckdb, same steps
```

## CI Integration

Two CI jobs in `.github/workflows/MainDistributionPipeline.yml`:

- **`verify-wasm`** (Level 2) — Downloads the extension `.wasm` artifacts
  from the distribution build and checks for unresolved library imports.
  Runs in ~30 seconds.

- **`test-wasm-load`** (Level 3) — Builds the extension and DuckDB WASM
  from source (matching emsdk 3.1.71 + Rust 1.86.0 + vcpkg), compiles the
  test harness, and runs the headless load test. This is a full independent
  build that catches ABI mismatches. Runs in ~10-15 minutes.

## Build Artifacts

These files are generated by the scripts and should **not** be committed:

- `scripts/test_wasm_harness.js` — emscripten-generated JS loader
- `scripts/test_wasm_harness.wasm` — compiled test harness WASM binary
- `scripts/test_wasm_harness.data` — preloaded extension data
- `scripts/wasm_browser/node_modules/` — puppeteer-core, installed by `test_wasm_browser.sh`
- `build/wasm_eh/` — WASM extension build directory
- `build/wasm_shell/` — DuckDB WASM build directory

## Troubleshooting

### `bad export type for 'X': undefined`

A library symbol wasn't linked into the WASM side module. Check that
`DUCKDB_EXTENSION_MIINT_LINKED_LIBS` in CMakeLists.txt includes the library.

### `relocation R_WASM_MEMORY_ADDR_SLEB cannot be used against symbol`

The library wasn't compiled with `-fPIC`. WASM side modules require PIC for
all linked objects. Check the EMSCRIPTEN blocks in CMakeLists.txt — each
external library needs `-fPIC` in its compile flags.

### `imported function does not match the expected type`

ABI mismatch between the extension and DuckDB WASM runtime. This happens when
they were built from different DuckDB versions or different emsdk versions.
Use `test_wasm_load.sh` which builds both from the same source.

### Extension loads but `miint_version()` fails

The extension loaded but initialization threw an error. Common cause: a
dependency extension (like `json`) isn't available. The test harness
pre-loads the json extension to avoid this.
