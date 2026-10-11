#!/bin/sh
# Re-creates third_party/apple-libstdcxx/ from Apple's open source repository.
# The checked-in tree is exactly what this script produces.
#
#   libstdcxx-60  (macOS 10.9)  -> whole tag: libstdc++ sources + headers
#   libstdcxx-39  (macOS 10.6)  -> only the GCC 4.2.1 libsupc++ subset
#
# Usage: fetch-sources.sh [destdir]   (default: <repo>/third_party/apple-libstdcxx)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
DEST=${1:-"$ROOT/third_party/apple-libstdcxx"}
REPO=https://github.com/apple-oss-distributions/libstdcxx.git
TAG_STDCXX=libstdcxx-60     # commit 0527c3e87c76a5316239e24cb93a687f2912cbfb
TAG_SUPCXX=libstdcxx-39     # commit dac27f80215d2c496945244c2cc83fadca676335

TMP=$(mktemp -d "${TMPDIR:-/tmp}/libstdcxx-src.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

git init -q "$TMP/git"
git -C "$TMP/git" fetch -q --depth 1 "$REPO" "refs/tags/$TAG_STDCXX:refs/tags/$TAG_STDCXX"
git -C "$TMP/git" fetch -q --depth 1 "$REPO" "refs/tags/$TAG_SUPCXX:refs/tags/$TAG_SUPCXX"

mkdir -p "$DEST/$TAG_STDCXX" "$DEST/$TAG_SUPCXX-libsupc++"
git -C "$TMP/git" archive "$TAG_STDCXX" | tar -x -C "$DEST/$TAG_STDCXX"
git -C "$TMP/git" archive "$TAG_SUPCXX" \
    libstdcxx/COPYING \
    libstdcxx/COPYING.LIB \
    libstdcxx/libstdc++-v3/ChangeLog \
    libstdcxx/libstdc++-v3/libsupc++ \
    libstdcxx/libstdc++-v3/config/cpu/generic/cxxabi_tweaks.h \
    libstdcxx/libstdc++-v3/config/os/bsd/darwin \
    libstdcxx/libstdc++-v3/config/locale/generic \
    libstdcxx/libstdc++-v3/config/locale/darwin \
    libstdcxx/gcc/unwind-pe.h \
    libstdcxx/gcc/unwind-generic.h \
    libstdcxx/libiberty/cp-demangle.c \
    libstdcxx/libiberty/cp-demangle.h \
    libstdcxx/include/ansidecl.h \
    libstdcxx/include/libiberty.h \
    libstdcxx/include/demangle.h \
  | tar -x -C "$DEST/$TAG_SUPCXX-libsupc++" --strip-components=1

echo "sources in $DEST"
