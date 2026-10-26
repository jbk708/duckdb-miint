# Libraries found on the system or in vcpkg rather than built here. Included
# before project(), as the finds always were.

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be
# added in ./vcpkg.json and then used in cmake with find_package. Note that it
# should also be removed from vcpkg.json to prevent needlessly installing it.
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(expat CONFIG REQUIRED)

# Eigen (header-only, MPL-2.0) backs the SYM/ARD Mk maximum-likelihood
# ancestral-state reconstruction: a SelfAdjointEigenSolver eigendecomposition
# builds P(t) = exp(Q*t) for the non-ER rate matrices. EIGEN_MPL2_ONLY restricts
# to Eigen's pure-MPL2 subset so no stronger-copyleft file is pulled in, keeping
# miint's Modified-BSD license clean. This is miint's own decoupled Eigen
# dependency (vcpkg eigen3), NOT skbb's UNIFRAC-gated copy.
#
# Production and local builds resolve Eigen from vcpkg (or a system install) via
# the CONFIG package. The Code-Quality "Tidy Check" CI lane, however, runs a
# no-vcpkg CMake configure (it installs only ninja/clang-tidy) yet still has to
# parse NewickTree.cpp's Eigen includes, so a hard find_package(... REQUIRED)
# fails that configure before clang-tidy can run. When the CONFIG package is
# absent, fall back to a pinned, SHA-verified header-only Eigen fetched at
# configure time (header-only: no build step; same 3.4.0 tarball + SHA that
# ext/scikit-bio-binaries/scripts/fetch_eigen.sh trusts). SOURCE_SUBDIR points
# at a nonexistent dir so FetchContent only downloads/extracts and does NOT
# add_subdirectory Eigen's own CMake (which would pull in its test/GPL targets).
# Real builds never take the fallback -- the CONFIG package is found first.
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
add_compile_definitions(EIGEN_MPL2_ONLY)

# The extension links OpenMP (libssu is built with -fopenmp), so `_OPENMP` is
# defined for miint's own sources too, and Eigen/Core:65 would then enable its
# parallel GEMM path. mmvec's objective contains matrix products whose reduction
# runs over a long dimension (d1 = 2720 at cystic-fibrosis scale), so a
# thread-count-dependent blocking would make a seeded fit non-reproducible --
# which is exactly the guarantee mmvec's own RNG transforms exist to provide.
#
# Measured at cystic-fibrosis scale, seed 0, 500 iterations: with this define
# removed, the same fit returns a DIFFERENT model as soon as OMP_NUM_THREADS > 1
# -- final loss 1709324046711880.2 against 1709329920514833.8, and the parameter
# checksum moving by 0.6%. A user-visible reproducibility break controlled by an
# environment variable. Keeping the define costs 3-5% of the fit's wall clock
# (median 4.82 s against 4.66 s, interleaved A/B over five pairs, the parallel
# build given 8 threads), which is the right trade for a seeded fit.
#
# Set globally rather than per-source: Eigen's macros affect templates
# instantiated in whichever translation unit includes the headers, so a per-file
# define would leave two differently-configured instantiations of the same
# symbols for the linker to choose between. Nothing here loses anything
# measurable -- the only other Eigen user is NewickTree.cpp's k x k rate
# matrices (k = the character-state count, so single digits), orders of
# magnitude below the size at which Eigen would consider threading a product at
# all.
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
