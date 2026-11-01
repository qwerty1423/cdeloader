#!/usr/bin/env bash
# Build the CDE mod loader and its plugins as 32-bit Windows PEs.
#
#   scripts/build-modloader.sh                  everything (loader + every plugin)
#   scripts/build-modloader.sh laser-cap         only that plugin
#   scripts/build-modloader.sh laser-cap laser-m2
#   scripts/build-modloader.sh loader            only cdeloader.dll, which also
#                                                relinks the import library
#
# A plugin needs no loader rebuild unless the cde_ui_* API or the import library
# changed; editing one mod's source builds just that plugin.
#
# Compilation is parallel: each plugin is its own translation unit, and the loader's
# sources are compiled separately and then linked, so the work spreads over the cores
# available.  The default is one job less than the CPU count, so the machine stays
# responsive; JOBS=N or -jN overrides it.
#
# Requires i686-w64-mingw32-g++ (see docs/02-tooling.md).
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/modloader"
inc="$root/src/modloader"
imgui="$root/third_party/imgui"

CXX=${CXX:-i686-w64-mingw32-g++}

# Dependency-free 32-bit PE; static libgcc/libstdc++ so the game's MSVC runtime
# is untouched.  -fno-exceptions/-fno-rtti keep the boundary simple.
# -Wl,--no-insert-timestamp keeps build time out of the PE header, and
# --image-base pins the DLL base: MinGW's default auto-image-base hashes the
# output path, which otherwise makes the same tree build differently depending
# on where it is extracted.  -ffile-prefix-map drops absolute paths from __FILE__.
common=(-std=c++17 -O2 -DNDEBUG -Wall -Wextra -fno-exceptions -fno-rtti
        -ffile-prefix-map="$root"=.
        -static -static-libgcc -static-libstdc++
        -Wl,--no-insert-timestamp -Wl,--image-base,0x10000000)

cores=$(nproc 2>/dev/null || echo 4)
jobs=$((cores > 1 ? cores - 1 : 1))
want=()
for arg in "$@"; do
    case "$arg" in
        -j*) jobs=${arg#-j} ;;
        --jobs=*) jobs=${arg#--jobs=} ;;
        -j|--jobs) shift; jobs=${1:-$jobs} ;;
        *) want+=("$arg") ;;
    esac
done
[ -n "${JOBS:-}" ] && jobs=$JOBS
case "$jobs" in
    ''|*[!0-9]*) jobs=4 ;;
esac
[ "$jobs" -lt 1 ] && jobs=1

# run_parallel <jobs> <command>... : run the commands at most <jobs> at a time and
# fail the build when one fails.  Each command is a plain shell string, so no
# quoting gymnastics are needed at the call sites.
run_parallel() {
    local max=$1
    shift
    local running=0 rc=0 cmd
    for cmd in "$@"; do
        eval "$cmd" &
        running=$((running + 1))
        if [ "$running" -ge "$max" ]; then
            wait -n || rc=1
            running=$((running - 1))
        fi
    done
    while [ "$running" -gt 0 ]; do
        wait -n || rc=1
        running=$((running - 1))
    done
    return $rc
}

want_target() {
    local name=$1 w
    if [ ${#want[@]} -eq 0 ]; then return 0; fi
    for w in "${want[@]}"; do
        [ "$w" = "$name" ] && return 0
    done
    return 1
}

# plugin <target> <source> [link-with-loader]
#
# Declares a plugin build; plugin_commands are run together once every plugin that
# links against the loader has been declared, so the import library exists first.
plugin_commands=()
plugin() {
    local target=$1 source=$2 link=${3:-yes} extra=${4:-}
    want_target "$target" || return 0
    # A published source tree carries only the plugins that ship, so a development plugin listed
    # here can legitimately have no source file. Say so and carry on rather than failing the build.
    if [ ! -f "$source" ]; then
        echo "skipping $target: $source is not in this tree"
        return 0
    fi
    mkdir -p "$out/$target"
    if [ "$link" = yes ]; then
        plugin_commands+=("$CXX ${common[*]} -shared -I$inc -Wl,--image-base,0x62000000 -o $out/$target/plugin.dll $source -L$out -lcdeloader $extra && echo built $out/$target/plugin.dll")
    else
        plugin_commands+=("$CXX ${common[*]} -shared -I$inc -Wl,--image-base,0x62000000 -o $out/$target/plugin.dll $source && echo built $out/$target/plugin.dll")
    fi
}

# The loader embeds Dear ImGui (MIT, third_party/imgui) for the overlay and
# exports the cde_ui_* API; --out-implib lets plugins link against it.
if want_target loader; then
    objdir="$out/obj"
    mkdir -p "$objdir"
    loader_sources=(
        "$root/src/modloader/modloader.cpp"
        "$root/src/modloader/x86len.cpp"
        "$root/src/modloader/overlay.cpp"
        "$root/src/modloader/console.cpp"
        "$imgui/imgui.cpp" "$imgui/imgui_draw.cpp" "$imgui/imgui_tables.cpp"
        "$imgui/imgui_widgets.cpp" "$imgui/imgui_demo.cpp"
        "$imgui/backends/imgui_impl_win32.cpp"
        "$imgui/backends/imgui_impl_opengl3.cpp"
        "$imgui/backends/imgui_impl_opengl2.cpp"
    )
    objects=()
    compile_cmds=()
    for src in "${loader_sources[@]}"; do
        obj="$objdir/$(basename "${src%.cpp}").o"
        objects+=("$obj")
        compile_cmds+=("$CXX ${common[*]} -c -I$inc -I$imgui -I$imgui/backends -DCDEMOD_UI_BUILD -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -DIMGUI_WIN32_NO_MOUSE_CAPTURE -o $obj $src")
    done
    run_parallel "$jobs" "${compile_cmds[@]}"
    "$CXX" -shared -o "$out/cdeloader.dll" "${objects[@]}" \
        -Wl,--no-insert-timestamp -Wl,--image-base,0x10000000 \
        -Wl,--out-implib,"$out/libcdeloader.a" \
        -static -static-libgcc -static-libstdc++ \
        -lkernel32 -luser32 -lgdi32 -lopengl32 -ldwmapi -ldbghelp
    echo "built $out/cdeloader.dll"
fi

# Development plugins used by scripts/test-modloader.sh and the probes.
plugin sample       "$root/src/modloader/sample/sample_plugin.cpp"
plugin faulty       "$root/src/modloader/sample/faulty_plugin.cpp" no
plugin ui           "$root/src/modloader/sample/ui_plugin.cpp"
plugin probe        "$root/src/modloader/sample/probe_plugin.cpp"
plugin autocontinue "$root/src/modloader/sample/auto_continue.cpp"
plugin community    "$root/src/modloader/sample/community_fixes.cpp" no
plugin turbo        "$root/src/modloader/sample/turboelectric_fix.cpp"

# Shipped mods.
plugin laser-cap      "$root/src/modloader/sample/laser_cap.cpp"       # M7
plugin laser-jitter   "$root/src/modloader/sample/laser_jitter.cpp"    # M24
plugin laser-m2       "$root/src/modloader/sample/laser_m2.cpp"        # M22
plugin auto-intercept "$root/src/modloader/sample/auto_intercept.cpp"  # M25
plugin node-editor   "$root/src/modloader/sample/node_editor.cpp"       # M19
plugin update        "$root/src/modloader/sample/update.cpp" yes -lwinhttp  # F9 updater

run_parallel "$jobs" "${plugin_commands[@]}"

# M23: the example plugins (notes/M23-EXAMPLE-MODS.md, docs/18).
if want_target examples; then
    "$root/examples/build.sh"
fi
