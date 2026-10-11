#!/bin/sh
# Compile a guest test (C or C++) for i386: tests/guest/build.sh <src> <out.o>
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
STD=$ROOT/third_party/apple-libstdcxx/libstdcxx-60/include/c++/4.2.1
case "$1" in
  *.cpp|*.cc|*.mm)
    exec clang++ -target i386-apple-macosx10.8 -O1 -nostdinc++ -std=gnu++98 \
      -I"$ROOT/src/guest/libstdcxx/build/include" -I"$STD" -I"$STD/backward" -c "$1" -o "$2" ;;
  *)
    exec clang -target i386-apple-macosx10.8 -O1 -c "$1" -o "$2" ;;
esac
