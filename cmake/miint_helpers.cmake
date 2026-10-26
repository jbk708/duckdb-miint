# Helpers shared by CMakeLists.txt and cmake/deps/*.cmake. The registration
# lists they fill are described in docs/internals/build-system.md.

# Record `git describe` of the submodule at <dir> as <name>_GIT_VERSION="..." in
# MIINT_DEFINITIONS, for miint_versions() to report. "unknown" outside a git
# checkout (e.g. a source tarball).
function(miint_git_version name dir)
  execute_process(
    COMMAND git describe --tags --dirty --always
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${dir}
    OUTPUT_VARIABLE version
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT version)
    set(version "unknown")
  endif()
  list(APPEND MIINT_DEFINITIONS "${name}_GIT_VERSION=\"${version}\"")
  list(APPEND MIINT_SUBMODULE_VERSIONS "${name}=${version}")
  set(MIINT_DEFINITIONS
      "${MIINT_DEFINITIONS}"
      PARENT_SCOPE)
  set(MIINT_SUBMODULE_VERSIONS
      "${MIINT_SUBMODULE_VERSIONS}"
      PARENT_SCOPE)
endfunction()

# Apply the registration lists to one of the three targets: the static
# extension, the loadable extension, or `tests`. Called once per target, so the
# three cannot drift apart; what is specific to one target is added next to its
# call in CMakeLists.txt.
#
# The plain target_link_libraries signature (no PRIVATE/PUBLIC) is deliberate:
# DuckDB's build_*_extension macros use it on the extension targets, and CMake
# rejects mixing the two signatures on one target.
function(miint_configure_target target)
  target_sources(${target} PRIVATE ${MIINT_OBJECT_SOURCES})
  target_compile_definitions(${target} PRIVATE ${MIINT_DEFINITIONS})
  target_link_libraries(${target} ${MIINT_LINK_LIBRARIES})
  if(MIINT_TARGET_DEPENDENCIES)
    add_dependencies(${target} ${MIINT_TARGET_DEPENDENCIES})
  endif()
endfunction()

# Cross-compilation flags for the C libraries built with their own Makefiles or
# autotools (htslib, minimap2, WFA2, vsearch, MAFFT), which do not see
# CMAKE_C_FLAGS. At most one platform branch applies.
set(MIINT_OSX_ARCH_CFLAGS "") # -arch for an osx cross build
set(MIINT_AUTOTOOLS_HOST "") # --host= for an autotools configure
if(EMSCRIPTEN)
  set(MIINT_AUTOTOOLS_HOST "--host=wasm32-unknown-emscripten")
elseif(APPLE AND OSX_BUILD_ARCH STREQUAL "x86_64")
  set(MIINT_OSX_ARCH_CFLAGS "-arch x86_64")
  set(MIINT_AUTOTOOLS_HOST "--host=x86_64-apple-darwin")
elseif(APPLE AND OSX_BUILD_ARCH STREQUAL "arm64")
  set(MIINT_OSX_ARCH_CFLAGS "-arch arm64")
  set(MIINT_AUTOTOOLS_HOST "--host=aarch64-apple-darwin")
endif()

# Detect threading flags for Emscripten wasm_threads builds. DuckDB passes
# -pthread in CMAKE_CXX_FLAGS for wasm_threads but not wasm_eh/wasm_mvp.
# ExternalProject builds (htslib, minimap2, WFA2) use explicit CFLAGS and don't
# inherit CMAKE_CXX_FLAGS, so we propagate -pthread to their compile flags here.
# Without this, object files like kthread.o lack atomics/bulk-memory features
# and the linker rejects them when --shared-memory is used. Note:
# CMAKE_CXX_FLAGS may not be visible here (set via -D on parent cmake), so we
# also check DUCKDB_EXPLICIT_PLATFORM which is reliably propagated.
set(EMSCRIPTEN_THREAD_CFLAGS "")
if(EMSCRIPTEN AND (CMAKE_CXX_FLAGS MATCHES "-pthread"
                   OR DUCKDB_EXPLICIT_PLATFORM STREQUAL "wasm_threads"))
  set(EMSCRIPTEN_THREAD_CFLAGS "-pthread")
  message(
    STATUS
      "Emscripten threading detected: propagating -pthread to C library builds")
endif()

# Fail on a source listed twice in <list_name>. The likely cause is a feature's
# source also added to the core list in CMakeLists.txt, which would compile it
# even with that feature OFF.
function(miint_check_unique list_name)
  set(seen "")
  foreach(item ${${list_name}})
    if(item IN_LIST seen)
      message(FATAL_ERROR "${item} is listed twice in ${list_name}")
    endif()
    list(APPEND seen ${item})
  endforeach()
endfunction()
