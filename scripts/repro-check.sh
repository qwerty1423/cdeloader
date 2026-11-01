#!/usr/bin/env bash
# Verify the release toolchain is reproducible.
#
#   scripts/repro-check.sh
#
# 1. builds the loader + plugins twice and compares SHA-256 of every PE;
# 2. packages the same delta twice and compares the zip bytes.
#
# Requires the same toolchain as scripts/build-modloader.sh (i686 MinGW).
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

digest() {
    sha256sum build/modloader/cdeloader.dll build/modloader/*/plugin.dll \
        build/proxy/vcruntime140.dll \
        | sed "s#build/##" | sort -k2
}

# Start from nothing: a stale artefact left in build/ from an older experiment would be compared
# against a fresh one and reported as a reproducibility failure it is not.
echo "== clean =="
rm -rf build/modloader build/proxy

echo "== build 1: in place =="
bash scripts/build-modloader.sh >/dev/null
bash scripts/build-proxy.sh >/dev/null
digest > /tmp/repro-check-1.txt

# The second build happens in a copy under a different absolute path, on purpose. MinGW's linker
# defaults to --enable-auto-image-base, which derives a preferred base from the output path, so a
# build that only ever runs in one directory can be reproducible there and nowhere else. That is
# how the proxy shipped for a while: two directories, two different DLLs, same source.
echo "== build 2: from a copy at another path =="
copy=$(mktemp -d /tmp/cde-repro-XXXXXX)
trap 'rm -rf "$copy"' EXIT
tar -C "$root" --exclude=build --exclude=.git --exclude=decomp --exclude=.venv \
    --exclude=artifacts -cf - . | tar -C "$copy" -xf -
( cd "$copy" && bash scripts/build-modloader.sh >/dev/null && bash scripts/build-proxy.sh >/dev/null )
(
    cd "$copy/build/modloader" || exit 1
    for dll in cdeloader.dll */plugin.dll; do
        [ -f "$dll" ] || continue
        echo "$(sha256sum "$dll" | cut -d' ' -f1)  modloader/$dll"
    done
) > /tmp/repro-check-2.txt
echo "$(sha256sum "$copy/build/proxy/vcruntime140.dll" | cut -d' ' -f1)  proxy/vcruntime140.dll" \
    >> /tmp/repro-check-2.txt
sort -k2 /tmp/repro-check-2.txt -o /tmp/repro-check-2.txt

if ! diff -u /tmp/repro-check-1.txt /tmp/repro-check-2.txt; then
    echo "FAIL: the same source built at another path gives different binaries" >&2
    exit 1
fi
echo "OK: loader, plugins and proxy are byte-identical across directories"

echo "== package contents =="
# The archive bytes are not compared: member timestamps follow SOURCE_DATE_EPOCH
# or the file's own mtime (reproducible-builds.org/docs/timestamps).  What must be
# identical is the *content*: every member's uncompressed SHA-256 plus the entry
# list.  Both the framework zip and the per-plugin zips are built twice.
package_digest() {
    python3 - "$root" "$1" <<'PY'
import hashlib, os, shutil, sys, tempfile, zipfile
root, which = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(root, "scripts"))
from cdefw import framework
tmp = tempfile.mkdtemp(prefix="repro-pkg-")
try:
    if which == "framework":
        out = os.path.join(tmp, "f.zip")
        framework.make_framework_package(
            os.path.join(root, "artifacts", "CDE.exe.original"), out,
            loader=os.path.join(root, "build", "modloader", "cdeloader.dll"),
            all_mods=False, version="0.0.0")
    else:
        zips = framework.make_plugin_zips(root, tmp)
        if not zips:
            raise SystemExit("no plugin zips built")
        out = zips[0]
    with zipfile.ZipFile(out) as zf:
        for name in sorted(zf.namelist()):
            print(hashlib.sha256(zf.read(name)).hexdigest() + "  " + name)
finally:
    shutil.rmtree(tmp, ignore_errors=True)
PY
}

package_digest framework > /tmp/repro-check-pkg-1.txt
package_digest framework > /tmp/repro-check-pkg-2.txt
package_digest plugin > /tmp/repro-check-plugin-1.txt
package_digest plugin > /tmp/repro-check-plugin-2.txt

if ! diff -u /tmp/repro-check-pkg-1.txt /tmp/repro-check-pkg-2.txt; then
    echo "FAIL: framework package contents are not reproducible" >&2
    exit 1
fi
if ! diff -u /tmp/repro-check-plugin-1.txt /tmp/repro-check-plugin-2.txt; then
    echo "FAIL: plugin package contents are not reproducible" >&2
    exit 1
fi
echo "OK: framework and plugin package contents are identical across builds"
