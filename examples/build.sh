#!/usr/bin/env bash
# Build the M23 example plugins into build/examples/.
#
#   examples/build.sh            # both plugins
#   examples/build.sh ui-panel   # just one
#
# They need the loader import library, so run scripts/build-modloader.sh first
# (or just use that script, which also builds these).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
CXX=${CXX:-i686-w64-mingw32-g++}
out=$root/build/examples
inc=$root/src/modloader
lib=$root/build/modloader
[ -f "$lib/libcdeloader.a" ] || { echo "build the loader first: scripts/build-modloader.sh" >&2; exit 2; }

# Same flags as scripts/build-modloader.sh: no external runtime DLLs, fixed PE
# timestamp, pinned image base (so the output is reproducible).
common=(-std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wno-unused-parameter
        -fno-exceptions -fno-rtti -ffile-prefix-map="$root"=.
        -static -static-libgcc -static-libstdc++
        -Wl,--no-insert-timestamp -Wl,--image-base,0x10000000)

build() {
    local name=$1 src=$2
    mkdir -p "$out/$name"
    "$CXX" "${common[@]}" -shared -I"$inc" -o "$out/$name/plugin.dll" \
        "$src" -L"$lib" -lcdeloader
    echo "built $out/$name/plugin.dll"
}

want=${1:-all}
case "$want" in
    all)      build ui-panel "$root/examples/ui-panel/plugin.cpp"
              build hook     "$root/examples/hook/plugin.cpp" ;;
    ui-panel) build ui-panel "$root/examples/ui-panel/plugin.cpp" ;;
    hook)     build hook     "$root/examples/hook/plugin.cpp" ;;
    *) echo "usage: examples/build.sh [all|ui-panel|hook]" >&2; exit 2 ;;
esac
