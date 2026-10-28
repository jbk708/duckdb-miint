# ---------------------------------------------------------------------------
# Runtime instruction-set dispatch
# ---------------------------------------------------------------------------
#
# Eigen picks its packet width at COMPILE time from __AVX__ / __AVX512F__ and
# does not runtime-dispatch; __attribute__((target)) does not help, because
# under it those macros are undefined at preprocessing time and Eigen still
# emits SSE2. Reaching a wider instruction set therefore means compiling a
# source AGAIN with different flags and choosing between the results at load
# time.
#
# This is the arrangement htslib's htscodecs already uses, and the shipped CI
# artifact proves it survives the distribution pipeline unchanged -- the
# published linux_amd64 binary carries rans_*_avx2 / rans_*_avx512, abPOA's
# AVX-512 aligner and OpenSSL's AVX-512 AES. Nothing here needs a CI or
# distribution change: the extra objects live inside the one artifact we already
# publish, which matters because DuckDB's platform string has no CPU-feature
# component and so cannot select a per-ISA download.
#
# Probe rather than assume, mirroring htslib's hts_probe_cc.sh: an old toolchain
# degrades to fewer variants instead of failing the build. x86-64 only -- arm64
# and wasm vectorize unconditionally, so there is nothing to choose between.
include(CheckCXXSourceCompiles)
set(MIINT_ISA_AVX2_FLAGS -mavx2 -mfma)
set(MIINT_ISA_AVX512_FLAGS -mavx512f -mavx512dq -mavx512vl -mavx512bw -mfma)
set(MIINT_HAS_AVX2 OFF)
set(MIINT_HAS_AVX512 OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND NOT EMSCRIPTEN)
  # Compile a real intrinsic, not just the flag: a compiler can accept -mavx512f
  # and still lack the headers we would need.
  #
  # CMAKE_REQUIRED_FLAGS is a space-separated STRING, not a list -- assigning
  # the list directly hands the compiler "-mavx2;-mfma" and every probe fails.
  string(JOIN " " MIINT_ISA_AVX2_FLAGSTR ${MIINT_ISA_AVX2_FLAGS})
  string(JOIN " " MIINT_ISA_AVX512_FLAGSTR ${MIINT_ISA_AVX512_FLAGS})
  set(CMAKE_REQUIRED_FLAGS ${MIINT_ISA_AVX2_FLAGSTR})
  check_cxx_source_compiles(
    "#include <immintrin.h>
         int main() { __m256d a = _mm256_set1_pd(1.0); a = _mm256_fmadd_pd(a, a, a); return (int)a[0]; }"
    MIINT_PROBE_AVX2)
  set(CMAKE_REQUIRED_FLAGS ${MIINT_ISA_AVX512_FLAGSTR})
  check_cxx_source_compiles(
    "#include <immintrin.h>
         int main() { __m512d a = _mm512_set1_pd(1.0); a = _mm512_fmadd_pd(a, a, a); return (int)_mm512_cvtsd_f64(a); }"
    MIINT_PROBE_AVX512)
  unset(CMAKE_REQUIRED_FLAGS)
  set(MIINT_HAS_AVX2 ${MIINT_PROBE_AVX2})
  # AVX-512 without AVX2 is not a configuration we build for; the dispatch tiers
  # are ordered and DetectIsa() clamps through Avx2 on its way down.
  if(MIINT_PROBE_AVX512 AND MIINT_PROBE_AVX2)
    set(MIINT_HAS_AVX512 ON)
  endif()
endif()
message(
  STATUS "miint ISA variants: avx2=${MIINT_HAS_AVX2} avx512=${MIINT_HAS_AVX512}"
)

# Compile `sources` once per available instruction set, each into its own OBJECT
# library and its own C++ namespace (via -DMIINT_ISA_VARIANT), and add their
# objects to MIINT_OBJECT_SOURCES, which every target consumes.
#
# Deliberately generic: mmvec is its first caller, not its intended only one.
# Any future dense kernel gets its variants with one call rather than a copy of
# this apparatus. Keep it OPT-IN per kernel -- each variant multiplies compile
# time and binary size for the sources it covers, which is negligible for one
# kernel and would not be if applied wholesale.
#
# OBJECT rather than static libraries, matching miint_nanoarrow_ipc: consumers
# take the object files directly, avoiding archives that would then have to join
# DuckDB's install export set. EIGEN_MPL2_ONLY and EIGEN_DONT_PARALLELIZE reach
# them through add_compile_definitions (cmake/system_deps.cmake).
function(miint_add_isa_variants basename)
  set(variants baseline)
  if(MIINT_HAS_AVX2)
    list(APPEND variants avx2)
  endif()
  if(MIINT_HAS_AVX512)
    list(APPEND variants avx512)
  endif()
  foreach(variant ${variants})
    set(target miint_${basename}_${variant})
    add_library(${target} OBJECT ${ARGN})
    target_include_directories(${target}
                               PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
    target_link_libraries(${target} PUBLIC Eigen3::Eigen)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(${target} PRIVATE MIINT_ISA_VARIANT=${variant})
    # The baseline variant takes NO extra flags -- it must stay bit-identical to
    # what the build produced before dispatch existed, because it is what the
    # carved expected values are carved against.
    if(NOT variant STREQUAL baseline)
      string(TOUPPER ${variant} variant_upper)
      target_compile_options(${target}
                             PRIVATE ${MIINT_ISA_${variant_upper}_FLAGS})
    endif()
    list(APPEND MIINT_OBJECT_SOURCES $<TARGET_OBJECTS:${target}>)
  endforeach()
  set(MIINT_OBJECT_SOURCES
      "${MIINT_OBJECT_SOURCES}"
      PARENT_SCOPE)
endfunction()

# Every target that consumes the variants also needs to know which exist, so the
# dispatcher's #ifdefs and the declarations in mmvec_kernel.hpp agree with
# reality.
if(MIINT_HAS_AVX2)
  list(APPEND MIINT_DEFINITIONS MIINT_HAS_AVX2)
endif()
if(MIINT_HAS_AVX512)
  list(APPEND MIINT_DEFINITIONS MIINT_HAS_AVX512)
endif()
