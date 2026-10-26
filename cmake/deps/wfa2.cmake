# WFA2-lib (v2.3.6): wavefront pairwise alignment, behind the
# align_pairwise_wfa2 functions. Always built, using its Makefile (matching the
# minimap2 pattern).

# Keep -fPIC on every platform: required for WASM side module linking
# (-sSIDE_MODULE=2) too.
set(WFA2_FPIC "-fPIC")
set(WFA2_ARCH_CFLAGS "${MIINT_OSX_ARCH_CFLAGS} ${EMSCRIPTEN_THREAD_CFLAGS}")

ExternalProject_Add(
  wfa2_build
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ext/WFA2-lib
  CONFIGURE_COMMAND ""
  BUILD_COMMAND make -C ${CMAKE_CURRENT_SOURCE_DIR}/ext/WFA2-lib clean
  COMMAND
    make -C ${CMAKE_CURRENT_SOURCE_DIR}/ext/WFA2-lib all "BUILD_TOOLS=0"
    "BUILD_EXAMPLES=0" "BUILD_WFA_PARALLEL=0"
    "CC_FLAGS=-Wall -O2 ${WFA2_FPIC} ${WFA2_ARCH_CFLAGS}"
  INSTALL_COMMAND
    ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/WFA2-lib/lib/libwfa.a
    ${CMAKE_CURRENT_BINARY_DIR}/wfa2/lib/libwfa.a
  COMMAND
    ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/WFA2-lib/lib/libwfacpp.a
    ${CMAKE_CURRENT_BINARY_DIR}/wfa2/lib/libwfacpp.a
  BUILD_IN_SOURCE 1
  BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/wfa2/lib/libwfa.a
                   ${CMAKE_CURRENT_BINARY_DIR}/wfa2/lib/libwfacpp.a)

# Imported libraries for WFA2. The C++ bindings depend on the C core, so wfa2cpp
# links before wfa2.
add_library(wfa2 STATIC IMPORTED GLOBAL)
set_target_properties(
  wfa2 PROPERTIES IMPORTED_LOCATION
                  ${CMAKE_CURRENT_BINARY_DIR}/wfa2/lib/libwfa.a)
add_dependencies(wfa2 wfa2_build)

add_library(wfa2cpp STATIC IMPORTED GLOBAL)
set_target_properties(
  wfa2cpp PROPERTIES IMPORTED_LOCATION
                     ${CMAKE_CURRENT_BINARY_DIR}/wfa2/lib/libwfacpp.a)
add_dependencies(wfa2cpp wfa2_build)

miint_git_version(WFA2 ext/WFA2-lib)
list(APPEND MIINT_INCLUDE_DIRS ext/WFA2-lib)
list(APPEND MIINT_LINK_LIBRARIES wfa2cpp wfa2)
list(APPEND MIINT_WASM_LIBS ${CMAKE_CURRENT_BINARY_DIR}/wfa2/lib/libwfacpp.a
     ${CMAKE_CURRENT_BINARY_DIR}/wfa2/lib/libwfa.a)
