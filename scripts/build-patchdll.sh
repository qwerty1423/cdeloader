#!/usr/bin/env bash
# Build the helper DLL template as a 32-bit Windows PE.
#
#   scripts/build-patchdll.sh [output-name.dll]
#
# Default output: build/patchdll/myhelper.dll
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
name=${1:-myhelper.dll}
outdir="$root/build/patchdll"
mkdir -p "$outdir"

CXX=${CXX:-i686-w64-mingw32-g++}
"$CXX" -std=c++17 -O2 -Wall -Wextra \
    -shared -fno-exceptions -fno-rtti \
    -static -static-libgcc -static-libstdc++ \
    -o "$outdir/$name" \
    "$root/src/patchdll/patchdll.cpp" \
    -lkernel32 -luser32

echo "built $outdir/$name"
