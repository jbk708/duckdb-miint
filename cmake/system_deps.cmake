# Libraries found on the system or in vcpkg rather than built here. Included
# before project(), as the finds always were.

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be
# added in ./vcpkg.json and then used in cmake with find_package. Note that it
# should also be removed from vcpkg.json to prevent needlessly installing it.
find_package(ZLIB REQUIRED)
# The zlib archive as a path, for the ExternalProject builds (htslib) whose own
# configure would otherwise find a different zlib through -lz.
if(ZLIB_LIBRARY_RELEASE)
  set(ZLIB_LIBRARY_FILE "${ZLIB_LIBRARY_RELEASE}")
elseif(ZLIB_LIBRARY)
  set(ZLIB_LIBRARY_FILE "${ZLIB_LIBRARY}")
else()
  # First real path in ZLIB_LIBRARIES (skip keywords like optimized/debug)
  set(ZLIB_LIBRARY_FILE "")
  foreach(lib ${ZLIB_LIBRARIES})
    if(EXISTS "${lib}" AND NOT ZLIB_LIBRARY_FILE)
      set(ZLIB_LIBRARY_FILE "${lib}")
    endif()
  endforeach()
endif()
if(ZLIB_LIBRARY_FILE)
  get_filename_component(ZLIB_LIBRARY_DIR "${ZLIB_LIBRARY_FILE}" DIRECTORY)
  message(STATUS "Using ZLIB library: ${ZLIB_LIBRARY_FILE}")
else()
  set(ZLIB_LIBRARY_DIR "")
  message(
    WARNING
      "Could not determine the ZLIB library file. ZLIB_LIBRARY_RELEASE=${ZLIB_LIBRARY_RELEASE}, ZLIB_LIBRARY=${ZLIB_LIBRARY}, ZLIB_LIBRARIES=${ZLIB_LIBRARIES}"
  )
endif()
find_package(Threads REQUIRED)
find_package(expat CONFIG REQUIRED)

# Eigen (header-only, vcpkg eigen3). The Tidy Check CI lane configures without
# vcpkg, so when the CONFIG package is absent fall back to a pinned,
# SHA-verified Eigen 3.4.0 tarball. SOURCE_SUBDIR names a missing dir so
# FetchContent only extracts the headers and never runs Eigen's own CMake.
find_package(Eigen3 CONFIG QUIET)
if(NOT TARGET Eigen3::Eigen)
  message(
    STATUS
      "Eigen3 CONFIG package not found; fetching pinned header-only Eigen 3.4.0 (tidy/no-vcpkg fallback)"
  )
  FetchContent_Declare(
    eigen_fallback
    URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
    URL_HASH
      SHA256=8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR __miint_no_eigen_configure__)
  FetchContent_MakeAvailable(eigen_fallback)
  if(NOT TARGET Eigen3::Eigen)
    add_library(Eigen3::Eigen INTERFACE IMPORTED)
    set_target_properties(
      Eigen3::Eigen PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                               "${eigen_fallback_SOURCE_DIR}")
  endif()
endif()
# Both defines are global on purpose; why each exists is in
# docs/internals/embedded-tools.md, "The two Eigen defines".
add_compile_definitions(EIGEN_MPL2_ONLY)
add_compile_definitions(EIGEN_DONT_PARALLELIZE)

# Header-only, so its place in the link order does not matter. ZLIB, expat and
# Threads are linked last, in CMakeLists.txt.
list(APPEND MIINT_LINK_LIBRARIES Eigen3::Eigen)

# libdeflate: accelerator for htslib bgzf gzip (FASTQ/FASTA/BAM COPY), ~3x over
# zlib-parallel bgzf and ~37x over the original single-threaded gzip writer
# (measured 2026-06-03). Linux only, gated in the platform matrix. A missing
# vcpkg port soft-disables it here, so default-ON degrades gracefully.
if(MIINT_ENABLE_LIBDEFLATE)
  find_package(libdeflate CONFIG QUIET)
  if(TARGET libdeflate::libdeflate_static)
    set(LIBDEFLATE_TARGET libdeflate::libdeflate_static)
  elseif(TARGET libdeflate::libdeflate_shared)
    set(LIBDEFLATE_TARGET libdeflate::libdeflate_shared)
  else()
    message(
      WARNING
        "MIINT_ENABLE_LIBDEFLATE=ON but no libdeflate package found; disabling")
    set(MIINT_ENABLE_LIBDEFLATE OFF)
  endif()
endif()
if(MIINT_ENABLE_LIBDEFLATE)
  list(APPEND MIINT_DEFINITIONS MIINT_HAS_LIBDEFLATE)
endif()

# Find zstd for compression support (optional - build will succeed without it
# but zstd won't work)
find_package(zstd QUIET)
if(zstd_FOUND)
  # Determine which target is available
  if(TARGET zstd::libzstd_static)
    set(ZSTD_TARGET zstd::libzstd_static)
    set(HAVE_LIBZSTD ON)
    message(
      STATUS
        "Found zstd: enabling zstd compression support (target: zstd::libzstd_static)"
    )
  elseif(TARGET zstd::libzstd_shared)
    set(ZSTD_TARGET zstd::libzstd_shared)
    set(HAVE_LIBZSTD ON)
    message(
      STATUS
        "Found zstd: enabling zstd compression support (target: zstd::libzstd_shared)"
    )
  elseif(TARGET zstd::libzstd)
    set(ZSTD_TARGET zstd::libzstd)
    set(HAVE_LIBZSTD ON)
    message(
      STATUS
        "Found zstd: enabling zstd compression support (target: zstd::libzstd)")
  elseif(TARGET zstd)
    set(ZSTD_TARGET zstd)
    set(HAVE_LIBZSTD ON)
    message(
      STATUS "Found zstd: enabling zstd compression support (target: zstd)")
  else()
    message(
      WARNING
        "zstd found but no known target available - disabling zstd support")
    set(HAVE_LIBZSTD OFF)
  endif()
else()
  message(WARNING "zstd not found - building without zstd compression support")
  set(HAVE_LIBZSTD OFF)
endif()
