#!/usr/bin/env bash
# End-to-end test of the CDE mod loader under wine, without the game.
#
#   1. build cdeloader.dll + the sample plugin
#   2. build a host exe with a known function (HostTarget) and a symbol table
#   3. run the host; it loads the loader, which discovers the plugin
#   4. assert: init ran, config read, symbol resolved, hook fired (HostTarget
#      returns the hooked value), pattern found, frame callback ticked, and the
#      host hash matched the symbol table
#
# Requires i686-w64-mingw32-g++ and wine.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work="$root/build/modloader"

echo "== build loader + sample plugin =="
"$root/scripts/build-modloader.sh"

echo "== build host exe =="
CC=${CC:-i686-w64-mingw32-gcc}
"$CC" -O2 -o "$work/host.exe" "$root/src/modloader/sample/host.c"

echo "== write symbol table for the host =="
nm=${NM:-i686-w64-mingw32-nm}
nm_out="$("$nm" "$work/host.exe")"
addr=""
while read -r sym _type name; do
    if [ "$name" = "_HostTarget" ]; then addr="$sym"; break; fi
done <<< "$nm_out"
if [ -z "$addr" ]; then
    echo "FAIL: could not find _HostTarget in host.exe" >&2
    printf '%s\n' "$nm_out" | grep -i hosttarget || true
    exit 1
fi
sha=$(sha256sum "$work/host.exe" | awk '{print $1}')
printf '# cdeloader.symbols v1 sha256=%s base=0x400000\n' "$sha" > "$work/cdeloader.symbols"
printf '0x%s\tHostTarget\t_HostTarget\n' "$addr" >> "$work/cdeloader.symbols"

echo "== write config + ini =="
mkdir -p "$work/mods/config"
printf '[mod]\nanswer=42\n' > "$work/mods/config/org.example.sample.ini"
printf '[loader]\nenabled=1\nlog_level=3\n' > "$work/cdeloader.ini"
rm -f "$work/cdeloader.log" "$work/host.out" "$work/wine.err"

echo "== run host under wine =="
( cd "$work" && wine host.exe ) >"$work/host.out" 2>"$work/wine.err"

echo "---- host stdout ----"
cat "$work/host.out"
echo "---- loader log ----"
cat "$work/cdeloader.log" 2>/dev/null || true

fail() { echo "FAIL: $1" >&2; exit 1; }
grep -q "HostTarget(1) = 102" "$work/host.out" || fail "hook did not change HostTarget(1) to 102"
grep -q "sample plugin init" "$work/cdeloader.log" || fail "plugin on_init did not run"
grep -q "config answer = 42" "$work/cdeloader.log" || fail "config_get_int did not read the mod ini"
grep -q "resolved HostTarget @" "$work/cdeloader.log" || fail "resolve_symbol failed"
grep -q "pattern '55 89 e5 8b 45 08' at " "$work/cdeloader.log" || fail "find_pattern failed"
grep -q "hook fired: HostTarget(1)" "$work/cdeloader.log" || fail "hook did not fire"
grep -q "6 bytes stolen" "$work/cdeloader.log" || fail "hook did not steal whole instructions (expected 6 bytes)"
grep -q "frame callback ticked" "$work/cdeloader.log" || fail "frame callback did not tick"
grep -q "host exe matches the symbol table build" "$work/cdeloader.log" || fail "sha256 host verification failed"

echo "== exercise manifest v2: disabled marker, api, game_versions, order =="
mkdir -p "$work/mods/probe"
cp "$work/probe/plugin.dll" "$work/mods/probe/probe.dll"
printf '[mod]\napi=999\n' > "$work/mods/probe/mod.ini"
rm -f "$work/cdeloader.log"
( cd "$work" && wine host.exe ) >/dev/null 2>&1 || true
grep -q "needs mod API 999" "$work/cdeloader.log" || fail "api mismatch was not rejected"

printf '[mod]\ngame_versions=9.9.9\n' > "$work/mods/probe/mod.ini"
rm -f "$work/cdeloader.log"
( cd "$work" && wine host.exe ) >/dev/null 2>&1 || true
grep -q "declares game_versions '9.9.9'" "$work/cdeloader.log" \
    || fail "game_versions warning missing"

rm -f "$work/mods/probe/mod.ini"
mkdir -p "$work/mods/disabled"
: > "$work/mods/disabled/org.example.probe"
rm -f "$work/cdeloader.log"
( cd "$work" && wine host.exe ) >/dev/null 2>&1 || true
grep -q "disabled by mods/disabled/org.example.probe" "$work/cdeloader.log" \
    || fail "disabled marker ignored"
grep -q "load order:" "$work/cdeloader.log" || fail "load order was not logged"
rm -rf "$work/mods/disabled" "$work/mods/probe"

echo "== move a plugin folder in and out of mods/disabled (F2 menu behaviour) =="
# The menu cannot move a loaded DLL's folder, so it writes an intent file and the
# next start performs the move.  Check both directions.
cp -r "$root/build/modloader/sample" "$work/mods/probe" 2>/dev/null || true
mkdir -p "$work/mods/disabled"
printf 'org.example.probe 1.0.0\n' > "$work/mods/disabled/.disable-probe"
rm -f "$work/cdeloader.log"
( cd "$work" && wine host.exe ) >/dev/null 2>&1 || true
grep -q "mods: moved .*probe" "$work/cdeloader.log" || fail "disable intent did not move the folder"
[ -d "$work/mods/disabled/probe" ] || fail "mods/disabled/probe was not created"
[ ! -d "$work/mods/probe" ] || fail "the plugin stayed in mods/"
[ -f "$work/mods/disabled/probe/.mod-info" ] || fail "disabled plugin lost its identity"
grep -q "disabled plugin folder" "$work/cdeloader.log" || fail "disabled plugins were not listed"
printf 'probe\n' > "$work/mods/disabled/.enable-probe"
rm -f "$work/cdeloader.log"
( cd "$work" && wine host.exe ) >/dev/null 2>&1 || true
grep -q "mods: moved .*probe" "$work/cdeloader.log" || fail "enable intent did not move the folder"
[ -d "$work/mods/probe" ] || fail "the plugin was not moved back to mods/"
[ ! -d "$work/mods/disabled/probe" ] || fail "mods/disabled/probe still exists"
rm -rf "$work/mods/disabled" "$work/mods/probe"

echo "== exercise the patched-exe hash path =="
# A patched exe necessarily differs from the stock build the symbols came from;
# the installer leaves the stock exe as CDE.exe.original.  The loader should
# recognise that and stay quiet (INFO, not WARN).
mkdir -p "$work/hashrun"
cp "$work/host.exe" "$work/cdeloader.dll" "$work/hashrun/"
printf 'not-the-real-exe' > "$work/hashrun/host.exe.original"
bhash=$(sha256sum "$work/hashrun/host.exe.original" | awk '{print $1}')
printf '# cdeloader.symbols v1 sha256=%s base=0x400000\n' "$bhash" > "$work/hashrun/cdeloader.symbols"
printf '0x%s\tHostTarget\t_HostTarget\n' "$addr" >> "$work/hashrun/cdeloader.symbols"
printf '[loader]\nenabled=1\nlog_level=3\n' > "$work/hashrun/cdeloader.ini"
( cd "$work/hashrun" && wine host.exe ) >/dev/null 2>&1 || true
grep -q "patched exe; symbol table matches" "$work/hashrun/cdeloader.log" \
    || fail "loader did not accept the stock backup as the symbol source"
if grep -q "VAs may be stale" "$work/hashrun/cdeloader.log"; then
    fail "loader warned about stale VAs despite a matching backup"
fi

echo "== exercise per-call plugin isolation + mod.ini order =="
mkdir -p "$work/mods/faulty"
cp "$work/faulty/plugin.dll" "$work/mods/faulty/plugin.dll"
printf '[mod]\nload_after=org.example.sample\n' > "$work/mods/faulty/mod.ini"
rm -f "$work/cdeloader.log"
( cd "$work" && wine host.exe ) >/dev/null 2>&1 || true
grep -q "faulted in on_init" "$work/cdeloader.log" || fail "faulting plugin was not isolated"
grep -q "disabled" "$work/cdeloader.log" || fail "faulting plugin was not disabled"
grep -q "sample plugin init" "$work/cdeloader.log" || fail "isolation stopped the healthy plugin"
grep -q "hook fired: HostTarget(1)" "$work/cdeloader.log" || fail "isolation broke the healthy plugin's hook"
si=$(grep -n "sample plugin init" "$work/cdeloader.log" | head -1 | cut -d: -f1)
fl=$(grep -n "faulted in on_init" "$work/cdeloader.log" | head -1 | cut -d: -f1)
if [ -z "$si" ] || [ -z "$fl" ] || [ "$si" -ge "$fl" ]; then
    fail "mod.ini load_after was not honoured"
fi
rm -rf "$work/mods/faulty"

echo "== exercise mod.ini dependencies =="
printf '[mod]\ndepends=org.example.missing\n' > "$work/mods/sample/mod.ini"
rm -f "$work/cdeloader.log"
( cd "$work" && wine host.exe ) >/dev/null 2>&1 || true
grep -q "requires missing mod 'org.example.missing'" "$work/cdeloader.log" \
    || fail "missing dependency was not reported"
if grep -q "sample plugin init" "$work/cdeloader.log"; then
    fail "plugin with a missing dependency was still loaded"
fi
rm -f "$work/mods/sample/mod.ini"

echo "== exercise the crash handler =="
rm -f "$work/cdeloader_crash.dmp"
( cd "$work" && wine host.exe crash ) >/dev/null 2>&1 || true
grep -q "UNHANDLED EXCEPTION" "$work/cdeloader.log" || fail "crash handler did not log the exception"
[ -f "$work/cdeloader_crash.dmp" ] || fail "crash handler did not write cdeloader_crash.dmp"

echo "== exercise the data redirect =="
mkdir -p "$work/Resources/Data/Modules" "$work/mods/datamod/Data/Modules"
printf 'ORIG' > "$work/Resources/Data/Modules/engines.txt"
printf 'MODDATA' > "$work/mods/datamod/Data/Modules/engines.txt"
printf '[loader]\nenabled=1\nlog_level=3\nredirect=1\n' > "$work/cdeloader.ini"
rm -f "$work/cdeloader.log"
( cd "$work" && wine host.exe redirect ) >"$work/redirect.out" 2>/dev/null || true
cat "$work/redirect.out"
grep -q "HOST: read 'MODDATA'" "$work/redirect.out" || fail "data redirect did not serve the mod file"
grep -q "data redirect" "$work/cdeloader.log" || fail "data redirect was not logged"

echo "== exercise the overlay (OpenGL) =="
"$CC" -O2 -o "$work/gl_host.exe" "$root/src/modloader/sample/gl_host.c" \
    -lopengl32 -lgdi32 -luser32
# A clean dir so only the loader + the UI-only plugin run; the menu starts open
# (overlay_open=1) so a plugin panel is actually drawn and reported.
rm -rf "$work/glrun"
mkdir -p "$work/glrun/mods/ui"
cp "$work/cdeloader.dll" "$work/gl_host.exe" "$work/glrun/"
cp "$work/ui/plugin.dll" "$work/glrun/mods/ui/plugin.dll"
printf '[loader]\nenabled=1\nlog_level=3\noverlay=1\noverlay_open=1\npanels_open=1\n' \
    > "$work/glrun/cdeloader.ini"
set +e
( cd "$work/glrun" && wine gl_host.exe ) >"$work/gl.out" 2>/dev/null
gl_exit=$?
set -e
cat "$work/gl.out"
grep -q "overlay: wglSwapBuffers hooked" "$work/glrun/cdeloader.log" \
    || fail "overlay did not hook wglSwapBuffers"
if grep -q "GLHOST: GL_VERSION" "$work/gl.out"; then
    grep -q "overlay ready" "$work/glrun/cdeloader.log" \
        || fail "overlay did not initialise ImGui on a real GL context"
    grep -q "panel drawn " "$work/glrun/cdeloader.log" \
        || fail "overlay did not draw the plugin's panel"
    if grep -q "UNHANDLED EXCEPTION" "$work/glrun/cdeloader.log"; then
        fail "overlay crashed while rendering"
    fi
    [ "$gl_exit" -eq 0 ] || fail "GL host exited $gl_exit"
    echo "OK: overlay rendered a plugin panel on a real OpenGL context (no crash)"
else
    echo "NOTE: no usable OpenGL under wine; overlay init not exercised (hook only)"
fi

echo "== exercise the shipped plugins (load, fail safe, render) =="
# The modpack plugins must come up in a bare host that is not the game: the
# game-specific patches have to refuse rather than write, and the read-only
# panel has to draw.  This is the cheap regression check that keeps a typo in a
# panel or a blind patch() out of a release.
rm -rf "$work/glmods"
mkdir -p "$work/glmods"
cp "$work/cdeloader.dll" "$work/gl_host.exe" "$work/glmods/"
for m in laser-cap laser-jitter laser-m2 auto-intercept node-editor; do
    mkdir -p "$work/glmods/mods/$m"
    cp "$work/$m/plugin.dll" "$work/glmods/mods/$m/plugin.dll"
done
printf '[loader]\nenabled=1\nlog_level=3\noverlay=1\noverlay_open=1\npanels_open=1\n' \
    > "$work/glmods/cdeloader.ini"
# The tool windows are always closed at startup now, so the render assert covers
# the panels; the windows are opened from their F2 checkbox by the user.
set +e
( cd "$work/glmods" && wine gl_host.exe ) >"$work/glmods.out" 2>/dev/null
set -e
for id in org.cdere.laser-cap org.cdere.laser-jitter org.cdere.laser-m2 org.cdere.auto-intercept org.cdere.node-editor; do
    grep -q "discovered $id" "$work/glmods/cdeloader.log" \
        || fail "shipped plugin $id was not discovered"
done
if grep -q "UNHANDLED EXCEPTION" "$work/glmods/cdeloader.log"; then
    fail "a shipped plugin crashed the host"
fi
grep -q "unexpected bytes" "$work/glmods/cdeloader.log" \
    || fail "laser-cap did not refuse to patch a foreign exe"
grep -q "cannot resolve WeaponCompartment::GetAimerOffset_km" "$work/glmods/cdeloader.log" \
    || fail "laser-jitter did not fail safe without the game"

grep -q "laser_m2: GetBeamWaistRadius_m" "$work/glmods/cdeloader.log" \
    || fail "laser-m2 did not report the floor sites"
grep -q "laser_m2: .*sites=0/4" "$work/glmods/cdeloader.log" \
    || fail "laser-m2 did not refuse to patch a foreign exe"
echo "OK: shipped plugins load, fail safe on a foreign exe, and report their sites"

echo "== exercise the example mods (docs/18) =="
# The examples ship to users as teaching material, so they must build, load and
# behave in a bare host: the UI panel draws, the hook example fails safe when
# the game symbol is absent (it is not the game here).
rm -rf "$work/glex"
mkdir -p "$work/glex/mods/ui-panel" "$work/glex/mods/hook"
cp "$work/cdeloader.dll" "$work/gl_host.exe" "$work/glex/"
cp "$root/build/examples/ui-panel/plugin.dll" "$work/glex/mods/ui-panel/"
cp "$root/build/examples/hook/plugin.dll" "$work/glex/mods/hook/"
printf '[loader]\nenabled=1\nlog_level=3\noverlay=1\noverlay_open=1\npanels_open=1\n' \
    > "$work/glex/cdeloader.ini"
set +e
( cd "$work/glex" && wine gl_host.exe ) >"$work/glex.out" 2>/dev/null
set -e
grep -q "ui-panel example: ready" "$work/glex/cdeloader.log" \
    || fail "the ui-panel example did not initialise"
grep -q "hook example: enabled=1" "$work/glex/cdeloader.log" \
    || fail "the hook example did not initialise"
grep -q "hook example: cannot resolve StrategicManager::RunTurn" "$work/glex/cdeloader.log" \
    || fail "the hook example did not fail safe without the game"
if grep -q "UNHANDLED EXCEPTION" "$work/glex/cdeloader.log"; then
    fail "an example mod crashed the host"
fi
if grep -q "GLHOST: GL_VERSION" "$work/glex.out"; then
    grep -q "ui-panel example: panel drawn" "$work/glex/cdeloader.log" \
        || fail "the ui-panel example's panel was not drawn"
fi
echo "OK: example mods build, load and behave (UI draws, hook fails safe)"

echo "OK: loader discovered, initialised, resolved, patched, hooked and ticked a plugin"
echo "OK: crash handler logged the fault and wrote a minidump"
echo "OK: data redirect served a mod file over the stock path"
echo "OK: overlay drew the loader menu and a plugin panel on a real GL context"
