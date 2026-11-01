#!/usr/bin/env bash
# Build the no-exe-patch loader proxy (see docs/12 §4a).
#
#   scripts/build-proxy.sh
#
# Output: build/proxy/vcruntime140.dll
#
# The DLL forwards every vcruntime140 export to vcruntime140_orig.dll (the real
# runtime, renamed at install time) and loads cdeloader.dll from DllMain.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/proxy"
mkdir -p "$out"

CC=${CC:-i686-w64-mingw32-gcc}

# Static libgcc: the proxy must not add a libgcc_s_dw2-1.dll dependency to the
# game process (that was a real startup failure: winmm-style proxy loads before
# the CRT is available).
"$CC" -std=c11 -O2 -Wall -Wextra -shared -static -static-libgcc \
    -Wl,--no-insert-timestamp \
    -Wl,--image-base,0x63100000 \
    -o "$out/vcruntime140.dll" \
    "$root/src/proxy/proxy.c" "$root/src/proxy/vcruntime140.def" -luser32

echo "built $out/vcruntime140.dll"

# No additive variant: imagehlp (the other app-local-looking import) is a
# KnownDLL, so the loader always takes System32 and ignores the game folder.
# See notes/FINDINGS.md F67.
