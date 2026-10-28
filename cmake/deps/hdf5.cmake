# HDF5 backs BIOM table support (read_biom, COPY ... (FORMAT biom)). Off under
# Emscripten: HDF5 C++ static class members (e.g. LinkCreatPropList::DEFAULT)
# become unresolvable GOT.mem imports in WASM, causing "bad export type:
# undefined" at extension load time.
#
# clang-tidy builds don't have vcpkg wired up, so HDF5 may be absent there; BIOM
# support is then dropped from the analysis set rather than failing the whole
# tidy pipeline. Regular builds REQUIRE HDF5.
if(CLANG_TIDY)
  find_package(HDF5 COMPONENTS CXX)
  if(NOT HDF5_FOUND)
    message(
      STATUS
        "HDF5 not found for tidy-check - BIOM support will be excluded from analysis"
    )
    set(MIINT_ENABLE_HDF5 OFF)
    return()
  endif()
  message(STATUS "HDF5 found for tidy-check")
else()
  find_package(HDF5 REQUIRED COMPONENTS CXX)
endif()

list(APPEND MIINT_DEFINITIONS MIINT_HAS_HDF5)
list(APPEND MIINT_EXTENSION_SOURCES src/copy_biom.cpp src/BIOMTable.cpp
     src/BIOMReader.cpp src/read_biom.cpp)
list(APPEND MIINT_TEST_SOURCES src/BIOMTable.cpp src/BIOMReader.cpp
     test/cpp/test_BIOMReader.cpp test/cpp/test_BIOMTable.cpp)
list(APPEND MIINT_LINK_LIBRARIES hdf5::hdf5-static hdf5::hdf5_cpp-static)
