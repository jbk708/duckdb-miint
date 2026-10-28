# SortMeRNA: embedded library for rRNA filtering and alignment. Upstream CMake
# produces several archives; smr_api is the C-API entry point. smr_objs, cmph,
# alp, build_version are CMake OBJECT libraries whose object files get archived
# into libsmr_api.a via the STATIC→OBJECT link rule.
#
# Off on MinGW: SortMeRNA transitively pulls in cmph, whose cmph_time.h assumes
# POSIX <sys/time.h> / gettimeofday. MinGW headers don't provide these, so the
# sub-build fails with "storage size of 'e_time' isn't known". Upstream
# sortmerna CI tests MSVC, not MinGW; we disable it rather than patching cmph.
# Still enabled on Linux/macOS/MSVC. Off under Emscripten too.
#
# Like the HDF5 deps file: clang-tidy builds don't have vcpkg wired up, so
# RocksDB (the only hard external dependency) may be absent. In that case we
# silently drop sortmerna from the analysis set rather than fail the whole tidy
# pipeline. Regular builds still REQUIRE RocksDB.
if(CLANG_TIDY)
  find_package(RocksDB CONFIG QUIET)
  if(NOT RocksDB_FOUND)
    message(
      STATUS
        "RocksDB not found for tidy-check - SortMeRNA support will be excluded from analysis"
    )
    set(MIINT_ENABLE_SORTMERNA OFF)
    return()
  endif()
  message(STATUS "RocksDB found for tidy-check - SortMeRNA support enabled")
else()
  find_package(RocksDB CONFIG REQUIRED)
endif()

set(SORTMERNA_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ext/sortmerna)
set(SORTMERNA_BIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/sortmerna_build)
set(SORTMERNA_API_LIB ${SORTMERNA_BIN_DIR}/src/smr_api/libsmr_api.a)
set(SORTMERNA_BUNDLE_LIB
    ${CMAKE_CURRENT_BINARY_DIR}/sortmerna/libsortmerna_bundle.a)
set(CONCURRENTQUEUE_HOME ${CMAKE_CURRENT_SOURCE_DIR}/ext/concurrentqueue)

# macOS: forward OSX_BUILD_ARCH as CMAKE_OSX_ARCHITECTURES, or the sub-CMake
# links sortmerna's CLI target for its default arch and trips on vcpkg's
# RocksDB, which is built for the target arch.
set(SORTMERNA_OSX_ARCH_ARG "")
if(APPLE AND OSX_BUILD_ARCH)
  set(SORTMERNA_OSX_ARCH_ARG "-DCMAKE_OSX_ARCHITECTURES=${OSX_BUILD_ARCH}")
endif()

# alp's sources use the deprecated `register` keyword, which is a hard error on
# Clang 16+. Suppress it so the library still compiles. The other -Wno-* flags
# silence cosmetic warnings from upstream sortmerna/cmph/alp code (type-punned
# pointer casts, intentional switch fall-through without [[fallthrough]],
# unchecked fread return values). All would require upstream patches to fix
# properly.
set(SORTMERNA_WARNING_SUPPRESSIONS
    "-Wno-register -Wno-strict-aliasing -Wno-implicit-fallthrough -Wno-unused-result"
)
ExternalProject_Add(
  sortmerna_build
  SOURCE_DIR ${SORTMERNA_SRC_DIR}
  BINARY_DIR ${SORTMERNA_BIN_DIR}
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_CXX_FLAGS=${SORTMERNA_WARNING_SUPPRESSIONS}
    -DCMAKE_C_FLAGS=${SORTMERNA_WARNING_SUPPRESSIONS}
    -DWITH_TESTS=OFF
    -DCONCURRENTQUEUE_HOME=${CONCURRENTQUEUE_HOME}
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
    -DVCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}
    -DVCPKG_MANIFEST_MODE=OFF
    ${SORTMERNA_OSX_ARCH_ARG}
    # Force sortmerna's FindRocksDB branch to use vcpkg's CONFIG file. Without
    # this, its else-branch runs MODULE mode which hits system /usr/lib rocksdb
    # first on Debian/Ubuntu.
    -DROCKSDB_DIST=${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/rocksdb
    # vcpkg builds static-only; switch sortmerna off its default shared target.
    -DROCKSDB_USE_STATIC_LIBS=ON
  BUILD_BYPRODUCTS ${SORTMERNA_API_LIB}
  INSTALL_COMMAND "")

# Post-build: bundle cmph/alp/build_version OBJECT library outputs into a single
# archive so consumers only need to link one -lsortmerna equivalent.
add_custom_command(
  OUTPUT ${SORTMERNA_BUNDLE_LIB}
  COMMAND
    ${CMAKE_COMMAND} -DSORTMERNA_BIN_DIR=${SORTMERNA_BIN_DIR}
    -DSORTMERNA_API_LIB=${SORTMERNA_API_LIB}
    -DSORTMERNA_BUNDLE_LIB=${SORTMERNA_BUNDLE_LIB} -DCMAKE_AR=${CMAKE_AR} -P
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/bundle_sortmerna.cmake
  DEPENDS sortmerna_build
  COMMENT "Bundling sortmerna object libraries into ${SORTMERNA_BUNDLE_LIB}")
add_custom_target(sortmerna_bundle DEPENDS ${SORTMERNA_BUNDLE_LIB})

add_library(sortmerna STATIC IMPORTED GLOBAL)
set_target_properties(
  sortmerna
  PROPERTIES IMPORTED_LOCATION ${SORTMERNA_BUNDLE_LIB}
             INTERFACE_INCLUDE_DIRECTORIES ${SORTMERNA_SRC_DIR}/include)
add_dependencies(sortmerna sortmerna_bundle)

list(APPEND MIINT_DEFINITIONS MIINT_HAS_SORTMERNA)
list(APPEND MIINT_EXTENSION_SOURCES src/SortMeRNAAligner.cpp
     src/align_sortmerna.cpp src/align_sortmerna_rrna.cpp)
list(APPEND MIINT_TEST_SOURCES src/SortMeRNAAligner.cpp
     test/cpp/test_sortmerna_build.cpp test/cpp/test_SortMeRNAAligner.cpp)
list(APPEND MIINT_LINK_LIBRARIES sortmerna RocksDB::rocksdb)
