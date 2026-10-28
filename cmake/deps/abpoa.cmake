# abPOA partial-order alignment (the-miint fork), built via add_subdirectory:
# the fork's CMakeLists.txt is subdirectory-friendly (no global flag pollution,
# gated install targets, multi-ISA SIMD dispatch + WASM). Off on Windows.
if(APPLE AND OSX_BUILD_ARCH)
  set(CMAKE_OSX_ARCHITECTURES ${OSX_BUILD_ARCH})
endif()
add_subdirectory(ext/abpoa EXCLUDE_FROM_ALL)
if(APPLE AND OSX_BUILD_ARCH)
  unset(CMAKE_OSX_ARCHITECTURES)
endif()

miint_git_version(ABPOA ext/abpoa)
list(APPEND MIINT_DEFINITIONS MIINT_HAS_ABPOA)
list(APPEND MIINT_INCLUDE_DIRS ext/abpoa/include)
list(APPEND MIINT_EXTENSION_SOURCES src/AbpoaAligner.cpp src/align_abpoa.cpp
     src/consensus_abpoa.cpp)
list(APPEND MIINT_TEST_SOURCES src/AbpoaAligner.cpp
     test/cpp/test_AbpoaAligner.cpp)
# Linked by archive path, not target name: miint_extension goes into DuckDB's
# install(EXPORT) set, which rejects link dependencies on targets outside it.
list(APPEND MIINT_LINK_LIBRARIES $<TARGET_FILE:abpoa>)
list(APPEND MIINT_TARGET_DEPENDENCIES abpoa)
list(APPEND MIINT_WASM_LIBS $<TARGET_FILE:abpoa>)
