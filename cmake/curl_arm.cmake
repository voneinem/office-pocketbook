# libcurl for the reader: the app carries its own, built statically by scripts/build-curl-arm.sh, because the reader's
# libcurl (8.12.0) and the SDK's (8.16.0) drop the contents of IMAP literals (research R-01, docs/device-findings.md).
# Defines the imported target CURL::libcurl. Included from core/CMakeLists.txt for TARGET_TYPE=ARM.
set(PBO_CURL_PREFIX "${CMAKE_SOURCE_DIR}/build-curl-arm" CACHE PATH "Prefix of the static libcurl built by scripts/build-curl-arm.sh")
if(NOT EXISTS "${PBO_CURL_PREFIX}/lib/libcurl.a")
    message(FATAL_ERROR
        "The static libcurl for the reader is missing: ${PBO_CURL_PREFIX}/lib/libcurl.a\n"
        "Build it once with:  POCKETBOOK_SDK=<sdk> scripts/build-curl-arm.sh")
endif()
file(STRINGS "${PBO_CURL_PREFIX}/include/curl/curlver.h" PBO_CURL_VERSION_LINE REGEX "^#define LIBCURL_VERSION \"")
message(STATUS "Reader libcurl: ${PBO_CURL_VERSION_LINE} (static, ${PBO_CURL_PREFIX})")

add_library(CURL::libcurl STATIC IMPORTED GLOBAL)
set_target_properties(CURL::libcurl PROPERTIES
    IMPORTED_LOCATION "${PBO_CURL_PREFIX}/lib/libcurl.a"
    INTERFACE_INCLUDE_DIRECTORIES "${PBO_CURL_PREFIX}/include"
    # OpenSSL and zlib are the reader's own shared libraries (the SDK sysroot has the same versions)
    INTERFACE_LINK_LIBRARIES "${SYSROOT}/usr/lib/libssl.so;${SYSROOT}/usr/lib/libcrypto.so;${SYSROOT}/usr/lib/libz.so;pthread"
    # keep the bundled curl symbols out of the program's dynamic symbol table: libinkview.so keeps using the system libcurl
    INTERFACE_LINK_OPTIONS "LINKER:--exclude-libs,libcurl.a")
