# libcurl powers the streaming HTTPS-PUT / FTP / FTPS transport in
# `ena_upload_reads`.
#
# Off on Apple because vcpkg's libcurl pulls in OpenSSL's libcrypto, which
# exports `MD5_*` / `SHA1_*` symbols that vsearch's vendored copies also export.
# GNU ld supports `--allow-multiple-definition` to resolve this; Apple's ld does
# not. Since vsearch is first-class on macOS and not all users need the
# streaming transport (Aspera + file:// still work), we drop libcurl on Apple
# rather than drop vsearch.
#
# Off under Emscripten for a different reason: there is no network stack to
# stream to, and a loadable extension is a wasm SIDE MODULE, so every undefined
# symbol silently becomes an import. Building with libcurl therefore produced an
# extension carrying unresolved `env.curl_*` imports that duckdb-wasm cannot
# supply -- and those trap only when CALLED, as "null function or function
# signature mismatch" / "table index is out of bounds", not at load time. That
# is what made `miint_versions()` fail in wasm for so long: its bind reads
# `GetCurlVersion()`, so a version query was the first thing to touch libcurl.
# Every other libcurl path (`ena_upload_reads`, the ENA surface) had the same
# latent fault. Measured by scanning the built .wasm import section:
# `curl_version` was present as an import, while statically-linked `zlibVersion`
# was not.
#
# This supersedes a narrower wasm_threads-only disable, whose reason still holds
# and is worth keeping on record: vcpkg's libcurl port for wasm32-emscripten is
# not built with -pthread, so its objects lack atomics/bulk-memory and wasm-ld
# rejects them against wasm_threads' --shared-memory. wasm_eh / wasm_mvp linked
# fine, which is exactly why the unresolved-import fault reached them and not
# wasm_threads.

# Try VCPKG-style CONFIG package first (production builds), fall back to the
# system FindCURL Module (apt-installed libcurl4-openssl-dev, macOS Homebrew,
# etc.). Soft-disable if neither is available so the tidy / no-vcpkg CI lanes
# don't fail at configure time before tidy has a chance to run.
find_package(CURL CONFIG QUIET)
if(NOT CURL_FOUND)
  find_package(CURL QUIET)
endif()
if(NOT CURL_FOUND)
  message(
    STATUS
      "libcurl not found (no VCPKG package, no system FindCURL); disabling MIINT_ENABLE_CURL"
  )
  set(MIINT_ENABLE_CURL OFF)
  return()
endif()

list(APPEND MIINT_DEFINITIONS MIINT_HAS_CURL)
list(APPEND MIINT_EXTENSION_SOURCES src/curl_send.cpp)
list(APPEND MIINT_TEST_SOURCES src/curl_send.cpp test/cpp/test_curl_send.cpp)
list(APPEND MIINT_LINK_LIBRARIES CURL::libcurl)
