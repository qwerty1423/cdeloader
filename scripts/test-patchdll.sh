#!/usr/bin/env bash
# End-to-end test of the injection mechanism at runtime:
#   1. build the helper DLL and a trivial host exe
#   2. inject an import of the helper into a copy of the host (cdetool inject)
#   3. run the patched host under wine
#   4. assert the loader loaded the helper (DllMain wrote cdepatch.log)
#
# This exercises the same .cdei mechanism used on CDE.exe, without running the
# game.  Requires i686-w64-mingw32-g++ and wine.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work="$root/build/patchdll"
mkdir -p "$work"

echo "== build helper DLL and host =="
"$root/scripts/build-patchdll.sh" myhelper.dll
i686-w64-mingw32-gcc -O2 -o "$work/host.exe" "$root/src/patchdll/host.c"

echo "== inject import of myhelper.dll!CdeInit into the host =="
rm -f "$work/host_patched.exe" "$work/cdepatch.log"
"$root/scripts/cdetool.sh" inject "$work/host.exe" -o "$work/host_patched.exe" \
    --import 'myhelper.dll:CdeInit'

echo "== run patched host under wine =="
( cd "$work" && wine host_patched.exe )

echo "== check the helper was loaded =="
if [ ! -f "$work/cdepatch.log" ]; then
    echo "FAIL: cdepatch.log was not written (helper DLL did not load)" >&2
    exit 1
fi
cat "$work/cdepatch.log"
grep -q "loaded, host at" "$work/cdepatch.log"
echo "OK: injected import loaded the helper DLL and its DllMain ran"
