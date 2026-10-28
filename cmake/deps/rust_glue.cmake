# The rype, sylph, sc and st3 Rust crates, built as one archive through the
# miint_rust_glue umbrella crate. Always built; sylph and st3 are cargo features
# gated by MIINT_ENABLE_SYLPH / MIINT_ENABLE_ST3.

# rype, sylph, sc and st3 are independent Rust crates; each emits a staticlib
# that statically embeds its own copy of the Rust std runtime
# (rust_eh_personality, EMPTY_PANIC, ...). Linking them as separate .a archives
# produces duplicate-symbol errors on every supported linker (GNU ld, Apple
# ld-prime, Mach-O classic, lld) — see the Rust Reference (Linkage chapter).
#
# Fix: an umbrella `miint_rust_glue` crate (ext/miint-rust-glue/) lists them as
# path dependencies, cargo compiles them through a shared std, and the resulting
# libmiint_rust_glue.a contains a single deduplicated copy of std plus the
# #[no_mangle] C ABI surface from every crate. This is the canonical pattern
# (Mozilla's "rul" super-crate for Firefox/Gecko).
find_program(CARGO cargo REQUIRED)
message(STATUS "Found cargo: ${CARGO}")

# The glue's wasm cargo build runs on an exact toolchain, not whatever "stable"
# is today: a newer rustc can emit wasm target-features that the emsdk's
# binaryen rejects at link time, and the -Z flags the wasm_eh build needs
# (below) can change between releases. Verified against the emsdk that
# extension-ci-tools uses for DuckDB v2.0 (6.0.9) with scripts/build_wasm.sh,
# which builds wasm_eh and checks its imports. Re-run it when bumping either.
set(MIINT_WASM_RUST_TOOLCHAIN
    "1.96.0"
    CACHE STRING "Rust toolchain used to cross-compile the rust glue for WASM")

set(GLUE_LIB_NAME "libmiint_rust_glue.a")
set(GLUE_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/miint_rust_glue_target)

# Cross-compile flags mirror the original per-crate logic.
set(GLUE_CARGO_TARGET_FLAG "")
set(GLUE_LIB_SUBDIR "release")
set(GLUE_CONFIGURE_CMD "")
# Rust toolchain to run the glue build under (empty = the default toolchain).
# Set for EMSCRIPTEN so cargo emits binaryen-compatible objects (see
# MIINT_WASM_RUST_TOOLCHAIN above); propagated to the build via
# RUSTUP_TOOLCHAIN.
set(GLUE_RUSTUP_TOOLCHAIN "")
if(EMSCRIPTEN)
  # Match the C++ side's exception model: docs/internals/embedded-tools.md,
  # "Emscripten exceptions", says why. The flag, not the platform name, decides:
  # extension-ci-tools builds wasm_threads without it. Recorded in the cache for
  # scripts/build_wasm.sh's import check.
  if(CMAKE_CXX_FLAGS MATCHES "-fwasm-exceptions")
    set(MIINT_WASM_NATIVE_EH
        ON
        CACHE INTERNAL "Rust glue built with wasm-native exceptions")
  else()
    set(MIINT_WASM_NATIVE_EH
        OFF
        CACHE INTERNAL "Rust glue built with wasm-native exceptions")
  endif()
  # wasm_eh is the platform we ship, and its glue must not fall back to JS
  # exceptions because a flag went missing.
  if(DUCKDB_EXPLICIT_PLATFORM STREQUAL "wasm_eh" AND NOT MIINT_WASM_NATIVE_EH)
    message(
      FATAL_ERROR
        "DUCKDB_EXPLICIT_PLATFORM is wasm_eh, but CMAKE_CXX_FLAGS (\"${CMAKE_CXX_FLAGS}\") lacks "
        "-fwasm-exceptions, so the Rust glue would be built with JS exceptions. extension-ci-tools "
        "sets the flag in WASM_CXX_EH_FLAGS; configure wasm_eh through its makefile (make wasm_eh)."
    )
  endif()
  find_program(RUSTUP rustup)
  set(GLUE_CARGO_TARGET_FLAG "--target" "wasm32-unknown-emscripten")
  set(GLUE_LIB_SUBDIR "wasm32-unknown-emscripten/release")
  if(RUSTUP)
    # Idempotently ensure the pinned toolchain + wasm target are installed, then
    # build cargo under it (RUSTUP_TOOLCHAIN, set in GLUE_BUILD_ENV).
    # Bracket-argument ([=[ ]=]) so CMake does NOT expand ${ver} — the shell
    # must see it verbatim at build time.
    set(GLUE_WASM_RUST_PIN_SH ${CMAKE_CURRENT_BINARY_DIR}/glue_wasm_rust_pin.sh)
    file(
      WRITE ${GLUE_WASM_RUST_PIN_SH}
      [=[#!/bin/sh
# Install the pinned toolchain for the miint rust-glue WASM build.
set -e
ver="$1"
if ! rustup toolchain list | grep -q "^${ver}"; then
    rustup toolchain install "${ver}" --profile minimal
fi
rustup target add --toolchain "${ver}" wasm32-unknown-emscripten
# Wasm-native exceptions rebuild std from its source (-Zbuild-std).
if [ "$2" = "rust-src" ]; then
    rustup component add --toolchain "${ver}" rust-src
fi
]=])
    # rust-src goes on the command line, not only in the script, because
    # ExternalProject re-runs this step when its command changes but not when
    # the script's contents do.
    set(GLUE_RUST_SRC_ARG "")
    if(MIINT_WASM_NATIVE_EH)
      set(GLUE_RUST_SRC_ARG "rust-src")
    endif()
    set(GLUE_CONFIGURE_CMD sh ${GLUE_WASM_RUST_PIN_SH}
                           ${MIINT_WASM_RUST_TOOLCHAIN} ${GLUE_RUST_SRC_ARG})
    set(GLUE_RUSTUP_TOOLCHAIN ${MIINT_WASM_RUST_TOOLCHAIN})
    message(
      STATUS
        "Rust glue: pinning WASM build to toolchain ${MIINT_WASM_RUST_TOOLCHAIN}"
    )
  elseif(MIINT_WASM_NATIVE_EH)
    message(
      FATAL_ERROR
        "The C++ uses wasm-native exceptions (-fwasm-exceptions), so the Rust glue must too, "
        "which takes unstable flags on the pinned toolchain ${MIINT_WASM_RUST_TOOLCHAIN} with "
        "its rust-src component. rustup was not found to provide them. Install rustup "
        "(https://rustup.rs) and reconfigure.")
  endif()
  message(STATUS "Rust glue cross-compiling for wasm32-unknown-emscripten")
elseif(WIN32)
  find_program(RUSTUP rustup)
  set(GLUE_CARGO_TARGET_FLAG "--target" "x86_64-pc-windows-gnu")
  set(GLUE_LIB_SUBDIR "x86_64-pc-windows-gnu/release")
  if(RUSTUP)
    set(GLUE_CONFIGURE_CMD ${RUSTUP} target add x86_64-pc-windows-gnu)
  endif()
  message(
    STATUS "Rust glue targeting x86_64-pc-windows-gnu for MinGW compatibility")
elseif(APPLE AND OSX_BUILD_ARCH STREQUAL "arm64")
  find_program(RUSTUP rustup)
  set(GLUE_CARGO_TARGET_FLAG "--target" "aarch64-apple-darwin")
  set(GLUE_LIB_SUBDIR "aarch64-apple-darwin/release")
  if(RUSTUP)
    set(GLUE_CONFIGURE_CMD ${RUSTUP} target add aarch64-apple-darwin)
  endif()
endif()

# Feature selection: rype's fastx (needletail) is off on WASM and Windows
# (matches the original rype_build wiring). sylph is off on WASM/Windows
# (matches MIINT_ENABLE_SYLPH gating). st3 follows MIINT_ENABLE_ST3 everywhere.
set(GLUE_FEATURES "")
if(NOT EMSCRIPTEN AND NOT WIN32)
  list(APPEND GLUE_FEATURES "rype-fastx")
endif()
if(MIINT_ENABLE_SYLPH)
  list(APPEND GLUE_FEATURES "with-sylph")
endif()
if(MIINT_ENABLE_ST3)
  list(APPEND GLUE_FEATURES "with-st3")
endif()
string(REPLACE ";" "," GLUE_FEATURES_CSV "${GLUE_FEATURES}")
if(GLUE_FEATURES_CSV)
  set(GLUE_FEATURES_ARGS "--features" "${GLUE_FEATURES_CSV}")
else()
  set(GLUE_FEATURES_ARGS "")
endif()

# Per-target RUSTFLAGS for the umbrella's cargo build. Cargo's RUSTFLAGS env var
# overrides .cargo/config.toml's rustflags rather than merging, so all
# platform-specific flags must accumulate here.
set(GLUE_RUSTFLAGS_LIST "")

if(EMSCRIPTEN)
  # PIC for WASM side modules. The umbrella itself is a staticlib (no link
  # step), but rype's and st3-capi's [lib] declare a cdylib crate type, and
  # cargo unconditionally builds the cdylib for the wasm32-unknown-emscripten
  # path-deps. -sSIDE_MODULE=2 tells wasm-ld that rype.wasm is a side module
  # rather than a standalone wasm — so no `main` symbol is required. We never
  # actually link rype.wasm into anything; only libmiint_rust_glue.a goes into
  # the extension. This just unblocks the cargo build.
  list(APPEND GLUE_RUSTFLAGS_LIST "-C" "relocation-model=pic")
  list(APPEND GLUE_RUSTFLAGS_LIST "-C" "link-arg=-sSIDE_MODULE=2")
endif()

# Wasm-native exceptions (MIINT_WASM_NATIVE_EH, above). The pinned rustc offers
# them only as an unstable flag, which needs std rebuilt from source (rust-src)
# and RUSTC_BOOTSTRAP to accept -Z on a stable toolchain.
set(GLUE_CARGO_UNSTABLE_ARGS "")
if(EMSCRIPTEN AND MIINT_WASM_NATIVE_EH)
  list(APPEND GLUE_RUSTFLAGS_LIST "-Zemscripten-wasm-eh")
  set(GLUE_CARGO_UNSTABLE_ARGS "-Zbuild-std=std,panic_unwind")
  message(
    STATUS
      "Rust glue: wasm-native exceptions, matching the C++ (-fwasm-exceptions)")
endif()

if(WIN32 AND MINGW)
  # rtools42's MinGW64 doesn't ship libgcc_eh.a (modern GCC folded EH into
  # libgcc), but rustc's windows-gnu linker spec still passes -lgcc_eh. Same
  # situation as the WASM case above — cargo builds rype's and st3-capi's
  # cdylibs even though we never link them. Stub an empty archive in a -L search
  # path so the cdylib link step finds something and proceeds.
  set(GLUE_MINGW_STUB_DIR ${CMAKE_CURRENT_BINARY_DIR}/mingw_libgcc_eh_stub)
  file(MAKE_DIRECTORY ${GLUE_MINGW_STUB_DIR})
  if(NOT EXISTS ${GLUE_MINGW_STUB_DIR}/libgcc_eh.a)
    execute_process(COMMAND ${CMAKE_AR} rcs ${GLUE_MINGW_STUB_DIR}/libgcc_eh.a)
  endif()
  list(APPEND GLUE_RUSTFLAGS_LIST "-C" "link-arg=-L${GLUE_MINGW_STUB_DIR}")
endif()

# sc-capi's build.rs writes sc.h to SC_HEADER_DIR; without it the header lands
# in Cargo's OUT_DIR, where no include path finds it.
set(SC_HEADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/sc_include)
set(GLUE_ENV_ARGS "SC_HEADER_DIR=${SC_HEADER_DIR}")
if(GLUE_RUSTFLAGS_LIST)
  string(JOIN " " GLUE_RUSTFLAGS_STR ${GLUE_RUSTFLAGS_LIST})
  list(APPEND GLUE_ENV_ARGS "RUSTFLAGS=${GLUE_RUSTFLAGS_STR}")
endif()
if(GLUE_RUSTUP_TOOLCHAIN)
  # Force cargo (the rustup shim) onto the pinned toolchain for this build.
  list(APPEND GLUE_ENV_ARGS "RUSTUP_TOOLCHAIN=${GLUE_RUSTUP_TOOLCHAIN}")
endif()
if(EMSCRIPTEN AND MIINT_WASM_NATIVE_EH)
  list(APPEND GLUE_ENV_ARGS "RUSTC_BOOTSTRAP=1")
endif()
set(GLUE_BUILD_ENV ${CMAKE_COMMAND} -E env ${GLUE_ENV_ARGS})

set(GLUE_LIB_PATH ${GLUE_TARGET_DIR}/${GLUE_LIB_SUBDIR}/${GLUE_LIB_NAME})

ExternalProject_Add(
  miint_rust_glue_build
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ext/miint-rust-glue
  CONFIGURE_COMMAND "${GLUE_CONFIGURE_CMD}"
  BUILD_COMMAND
    ${GLUE_BUILD_ENV} ${CARGO} build --release ${GLUE_CARGO_UNSTABLE_ARGS}
    ${GLUE_FEATURES_ARGS} --target-dir ${GLUE_TARGET_DIR} --manifest-path
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/miint-rust-glue/Cargo.toml
    ${GLUE_CARGO_TARGET_FLAG}
  INSTALL_COMMAND ""
  BUILD_IN_SOURCE 0
  # Run the cargo build on EVERY build, deferring change detection to cargo's
  # own fingerprinting. ExternalProject does not track SOURCE_DIR (or
  # path-dependency, e.g. ext/rype / ext/sylph / ext/st3) file contents, so
  # without this an incremental build after a submodule bump or a local crate
  # edit would silently relink the STALE libmiint_rust_glue.a — the dev gets a
  # binary that doesn't match their source and has no way to notice. Cargo's
  # no-op when nothing changed is ~1-3s. (Clean builds — CI's fresh
  # ubuntu-latest runners, or `make clean` locally — were always correct; this
  # only closes the incremental-build footgun.)
  BUILD_ALWAYS 1
  BUILD_BYPRODUCTS ${GLUE_LIB_PATH} ${SC_HEADER_DIR}/sc.h)

# `rype` is the sole IMPORTED STATIC target for the merged archive. The archive
# contains the rype, sylph, sc and st3 symbols, so linking `rype` provides
# everything — there is no separate `sylph`, `sc` or `st3` link target.
#
# Why no second target: declaring an additional IMPORTED STATIC target pointing
# at the same archive caused libmiint_rust_glue.a to appear twice on the link
# line. Combined with -Wl,--allow-multiple-definition + --gc-sections, each pass
# resolved the Rust runtime symbols (__rust_alloc / rust_eh_personality /
# EMPTY_PANIC / ...) from a different instance and pruned the other; allocations
# made through one resolution freed via the other corrupted the heap, producing
# non-deterministic SIGSEGVs in unrelated code paths (mzXML reader, abPOA
# consensus, ...) on Linux glibc CI runners. Local Red Hat builds happened to
# dodge it.
add_library(rype STATIC IMPORTED GLOBAL)
set_target_properties(rype PROPERTIES IMPORTED_LOCATION ${GLUE_LIB_PATH})
add_dependencies(rype miint_rust_glue_build)

miint_git_version(RYPE ext/rype)
miint_git_version(SC ext/sc)
# SC_HEADER_DIR holds sc.h, which sc-capi's build.rs generates with cbindgen
# during the rust-glue build; a fresh build directory lacks it. A target
# compiling a file that includes it must build after miint_rust_glue_build, so
# every target depends on it (the extensions also do through their `rype` link).
list(APPEND MIINT_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/ext/rype
     ${SC_HEADER_DIR})
list(APPEND MIINT_TARGET_DEPENDENCIES miint_rust_glue_build)
list(APPEND MIINT_LINK_LIBRARIES rype)
list(APPEND MIINT_WASM_LIBS ${GLUE_LIB_PATH})

if(MIINT_ENABLE_SYLPH)
  miint_git_version(SYLPH ext/sylph)
  list(APPEND MIINT_DEFINITIONS MIINT_HAS_SYLPH)
  list(APPEND MIINT_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/ext/sylph)
  list(APPEND MIINT_EXTENSION_SOURCES src/SylphDatabase.cpp
       src/sylph_profile.cpp src/sylph_index_create.cpp)
  # Tests only exercise SylphDatabaseHandle + the FFI default getters from
  # libsylph.a; sylph_profile.cpp itself isn't linked into the tests binary
  # because it pulls in DuckDB-internal symbols (ArrowArrayWrapper) + miint's
  # PerSampleGlobalState that the tests target doesn't otherwise depend on. The
  # behavioral coverage for sylph_profile lives in the SQL tests, which run
  # against the full extension binary.
  list(APPEND MIINT_TEST_SOURCES src/SylphDatabase.cpp
       test/cpp/test_SylphDatabase.cpp)
endif()

if(MIINT_ENABLE_ST3)
  miint_git_version(ST3 ext/st3)
  list(APPEND MIINT_DEFINITIONS MIINT_HAS_ST3)
  list(APPEND MIINT_INCLUDE_DIRS
       ${CMAKE_CURRENT_SOURCE_DIR}/ext/st3/crates/st3-capi/include)
  list(APPEND MIINT_EXTENSION_SOURCES src/sourcetracker_dataset.cpp
       src/sourcetracker_function.cpp)
  # The dataset layer is DuckDB-free by design (see sourcetracker_dataset.hpp),
  # so it is unit-tested here; the SQL layer is covered by the SQL tests.
  list(APPEND MIINT_TEST_SOURCES src/sourcetracker_dataset.cpp
       test/cpp/test_St3.cpp test/cpp/test_SourcetrackerDataset.cpp)
endif()
