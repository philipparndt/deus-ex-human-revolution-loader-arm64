#!/bin/sh
# Verifies that build/libstdc++-i386.a defines
#   1. every libstdc++ symbol imported by the game (docs/analysis/hr-imports.tsv)
#   2. every symbol exported by Apple's real 10.9 i386 libstdc++.6.dylib
#      (exports/libstdcxx-i386.exp of libstdcxx-60)
# Prints missing symbols; exits non-zero if any game import is missing.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
LIB="$HERE/build/libstdc++-i386.a"
IMPORTS="$ROOT/docs/analysis/hr-imports.tsv"
EXP="$ROOT/third_party/apple-libstdcxx/libstdcxx-60/exports/libstdcxx-i386.exp"

[ -f "$LIB" ] || { echo "missing $LIB (run make)" >&2; exit 2; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/check-exports.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# Defined (non-undefined) external symbols, including weak definitions.
xcrun nm -g -j -U "$LIB" 2>/dev/null | grep -v ':$' | grep -v '^$' | sort -u > "$TMP/defined"

awk -F'\t' '$1 == "libstdc++" { print $2 }' "$IMPORTS" | sort -u > "$TMP/required"
grep -v '^#' "$EXP" | grep -v '^[[:space:]]*$' | sort -u > "$TMP/dylib-exports"

comm -23 "$TMP/required" "$TMP/defined" > "$TMP/missing-required"
comm -23 "$TMP/dylib-exports" "$TMP/defined" > "$TMP/missing-exports"

nreq=$(wc -l < "$TMP/required" | tr -d ' ')
nmiss=$(wc -l < "$TMP/missing-required" | tr -d ' ')
nexp=$(wc -l < "$TMP/dylib-exports" | tr -d ' ')
nexpmiss=$(wc -l < "$TMP/missing-exports" | tr -d ' ')

echo "game imports from libstdc++: $((nreq - nmiss))/$nreq defined"
if [ "$nmiss" -ne 0 ]; then
    echo "MISSING game imports:"
    sed 's/^/  /' "$TMP/missing-required"
fi
echo "10.9 i386 dylib export list: $((nexp - nexpmiss))/$nexp defined"
if [ "$nexpmiss" -ne 0 ]; then
    echo "missing dylib exports (informational):"
    sed 's/^/  /' "$TMP/missing-exports"
fi

[ "$nmiss" -eq 0 ]
