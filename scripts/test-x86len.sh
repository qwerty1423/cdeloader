#!/usr/bin/env bash
# Unit-test the hook engine's x86 instruction-length decoder under wine.
#
#   scripts/test-x86len.sh
#
# Compiles src/modloader/x86len.cpp with its test harness as a 32-bit exe and
# checks a table of known encodings (including a "refuse" case).
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work="$root/build/modloader"
mkdir -p "$work"

CXX=${CXX:-i686-w64-mingw32-g++}
"$CXX" -std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ \
    -I"$root/src/modloader" \
    -o "$work/test_x86len.exe" \
    "$root/src/modloader/test_x86len.cpp" \
    "$root/src/modloader/x86len.cpp"

( cd "$work" && wine test_x86len.exe )
