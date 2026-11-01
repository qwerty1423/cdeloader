#!/usr/bin/env bash
# Build the CDE mod framework from source and install it into a game folder.
#
#   ./build-and-install.sh /path/to/CoaDE
#   ./build-and-install.sh /path/to/CoaDE --check     stop after the checks
#
# Everything is built here, from the sources in this package. No prebuilt DLL is shipped and
# nothing is downloaded. The game folder supplies CDE.exe (for its hash) and CDE.pdb (for the
# symbol table), which is why it has to exist before the build.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
game=""
check_only=""
for argument in "$@"; do
    case "$argument" in
        --check) check_only=1 ;;
        -*) echo "unknown option: $argument" >&2; exit 2 ;;
        *) game="$argument" ;;
    esac
done

if [ -z "$game" ] || [ ! -f "$game/CDE.exe" ]; then
    echo "usage: ./build-and-install.sh /path/to/CoaDE [--check]" >&2
    echo "the folder must contain CDE.exe" >&2
    exit 2
fi
game=$(cd "$game" && pwd)

missing=""
have() { command -v "$1" >/dev/null 2>&1; }
have i686-w64-mingw32-g++ || missing="$missing i686-w64-mingw32-g++"
have python3               || missing="$missing python3"
have llvm-pdbutil          || missing="$missing llvm-pdbutil"
if [ -n "$missing" ]; then
    echo "missing tool(s):$missing" >&2
    echo
    echo "  i686-w64-mingw32-g++  from mingw-w64 (the 64-bit compiler will not work)" >&2
    echo "  python3               for the installer and the symbol generator" >&2
    echo "  llvm-pdbutil          reads ${game##*/}/CDE.pdb to build the symbol table" >&2
    exit 1
fi
if [ ! -f "$game/CDE.pdb" ]; then
    echo "no CDE.pdb in $game" >&2
    echo "the symbol table is generated from it; it ships beside CDE.exe" >&2
    exit 1
fi

echo "compiler: $(i686-w64-mingw32-g++ --version | head -1)"
echo "llvm:     $(llvm-pdbutil --version 2>/dev/null | head -1)"
echo "game:     $game"

echo
echo "== building the loader, the proxy and the plugins"
"$here/scripts/build-modloader.sh"
"$here/scripts/build-proxy.sh"

echo
echo "== recording the forwarder hash for install.py"
forwarder="$here/build/proxy/vcruntime140.dll"
if [ -f "$forwarder" ]; then
    forwarder_sha=$(sha256sum "$forwarder" | cut -d' ' -f1)
    python3 - "$here/install.py" "$forwarder_sha" <<'PY'
import sys
path, digest = sys.argv[1], sys.argv[2]
text = open(path, encoding="utf-8").read()
open(path, "w", encoding="utf-8").write(text.replace("@@FORWARDER_SHA@@", digest))
PY
else
    echo "no vcruntime140.dll was built; the installer will not check the forwarder" >&2
fi

echo
echo "== generating the symbol table from ${game##*/}/CDE.pdb"
python3 "$here/scripts/pdb-functions.py" "$game/CDE.pdb" >/dev/null
bash "$here/scripts/cdefw.sh" sdk -o "$here/build/modloader/cdeloader.symbols" \
    --exe "$game/CDE.exe" --table "$here/decomp/symbols/functions.tsv"

if [ -n "$check_only" ]; then
    echo
    echo "== built, not installed"
    for f in "$here/build/modloader/cdeloader.dll" "$here/build/proxy/vcruntime140.dll"; do
        [ -f "$f" ] && echo "  $(sha256sum "$f" | cut -c1-16)  ${f#$here/}"
    done
    echo "  $(wc -l < "$here/build/modloader/cdeloader.symbols") lines in cdeloader.symbols"
    exit 0
fi

echo
echo "== packaging what was just built"
# The installer is the same layout as the release download, produced here from local build output.
payload="$here/_payload"
rm -rf "$payload"
mkdir -p "$payload"
bash "$here/scripts/cdefw.sh" package --version "${CDELOADER_LOCAL_VERSION:-local}" --exe "$game/CDE.exe" --zip "$payload/cdeloader.zip" --no-source --no-source-installer >/dev/null
python3 - "$payload" <<'PY'
import glob, sys, zipfile
# --zip names the framework package; the installer is written beside it, and that is the one that
# carries the plugins, their settings and install.py.
targets = sorted(glob.glob(sys.argv[1] + "/cdeloader-installer-*.zip"))
if not targets:
    raise SystemExit("no installer was produced in " + sys.argv[1])
zipfile.ZipFile(targets[-1]).extractall(sys.argv[1])
PY

echo
echo "== installing"
python3 "$payload/install.py" "$game"
echo
echo "done. start the game and press F2."
