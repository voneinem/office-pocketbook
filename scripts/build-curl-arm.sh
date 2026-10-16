#!/usr/bin/env bash
# Builds libcurl for the reader (32-bit ARM) as a STATIC library with the PocketBook SDK, so the app carries the very
# libcurl version it is tested with. The reader's own libcurl (8.12.0) drops the contents of literals in IMAP answers
# (docs/device-findings.md, research R-01); only curl 8.18.0 and newer deliver them.
#
#   POCKETBOOK_SDK=/path/to/arm-buildroot-linux-gnueabihf_sdk-buildroot scripts/build-curl-arm.sh [prefix]
#
# prefix defaults to build-curl-arm in the repository root (not tracked by git). The ARM CMake build reads it from PBO_CURL_PREFIX.
# The source tarball is pinned by SHA-256 and its signature is checked against the curl release key when gpg exists.
# OpenSSL and zlib stay the SDK's shared libraries (the reader has them: its own libcurl uses OpenSSL 3.5.4).
set -euo pipefail

VERSION=8.18.0
URL="https://curl.se/download/curl-$VERSION.tar.xz"
SHA256=40df79166e74aa20149365e11ee4c798a46ad57c34e4f68fd13100e2c9a91946
KEY_URL="https://daniel.haxx.se/mykey.asc"
KEY_FPR=27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2   # Daniel Stenberg, curl release signing key

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${1:-$ROOT/build-curl-arm}"
SDK="${POCKETBOOK_SDK:-}"
[ -n "$SDK" ] || { echo "set POCKETBOOK_SDK to the arm-buildroot-linux-gnueabihf_sdk-buildroot directory" >&2; exit 1; }
TRIPLE=arm-buildroot-linux-gnueabihf
CC="$SDK/bin/$TRIPLE-gcc"
SYSROOT="$SDK/$TRIPLE/sysroot"
[ -x "$CC" ] || { echo "not an SDK directory: $SDK" >&2; exit 1; }

if [ -f "$PREFIX/lib/libcurl.a" ] && grep -q "\"$VERSION\"" "$PREFIX/include/curl/curlver.h" 2>/dev/null; then
    echo "libcurl $VERSION is already built in $PREFIX"
    exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
echo "downloading curl $VERSION ..."
curl -fsSLO "$URL"
echo "$SHA256  curl-$VERSION.tar.xz" | sha256sum -c - >/dev/null || { echo "SHA-256 of the curl tarball does not match the pinned value" >&2; exit 1; }
if command -v gpg >/dev/null 2>&1; then
    export GNUPGHOME="$work/gnupg"; mkdir -m 700 "$GNUPGHOME"
    curl -fsSLO "$URL.asc"
    curl -fsSL "$KEY_URL" -o key.asc
    gpg --batch --quiet --import key.asc 2>/dev/null
    if ! gpg --batch --status-fd 1 --verify "curl-$VERSION.tar.xz.asc" "curl-$VERSION.tar.xz" 2>/dev/null | grep -q "VALIDSIG $KEY_FPR"; then
        echo "the signature of the curl tarball is not valid for the curl release key" >&2
        exit 1
    fi
    echo "signature ok (key $KEY_FPR)"
else
    echo "gpg not found: only the pinned SHA-256 was checked" >&2
fi
tar -xf "curl-$VERSION.tar.xz"
cd "curl-$VERSION"

./configure --host="$TRIPLE" --prefix="$PREFIX" CC="$CC" \
    --enable-static --disable-shared \
    --with-openssl="$SYSROOT/usr" \
    --without-libpsl --without-nghttp2 --without-brotli --without-zstd --without-libidn2 --without-librtmp --without-libssh2 --without-ca-bundle --without-ca-path \
    --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-gopher \
    --disable-smb --disable-mqtt --disable-ftp --disable-file --disable-websockets --disable-ntlm --disable-manual --disable-docs \
    --disable-ares --disable-unix-sockets --disable-cookies --disable-hsts --disable-alt-svc \
    > "$work/configure.log" 2>&1 || { tail -30 "$work/configure.log" >&2; exit 1; }
make -j"$(nproc)" > "$work/make.log" 2>&1 || { tail -30 "$work/make.log" >&2; exit 1; }
make install > "$work/install.log" 2>&1 || { tail -30 "$work/install.log" >&2; exit 1; }
echo "libcurl $VERSION built in $PREFIX"
