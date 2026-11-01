"""Build the framework distribution: the delta, the dist folder and the zip.

Phase D (docs/10-mod-framework.md §7).  Reuses cdetool's verified spec/inject/
delta machinery to produce a patch that injects the loader import, then bundles
the loader, its symbol table and the mods tree into a distributable package.
"""

from __future__ import annotations

import json
import re
import os
import shutil
import tempfile
import textwrap
from typing import List, Optional, Tuple

from cdetool.delta import apply_delta, create_delta, load_delta, save_delta
from cdetool.inject import inject as inject_sections
from cdetool.package import make_package
from cdetool.pe import PE, sha256
from cdetool.spec import build_from_spec
from cdetool.symbols import Symbols

def _read_template(name: str) -> str:
    """Read one of the text templates shipped next to this module."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "templates", name)
    with open(path, encoding="utf-8") as fh:
        return fh.read()


DEFAULT_INI = """\
; cdeloader.ini — CDE mod loader settings (docs/10-mod-framework.md)
[loader]
enabled = 1
log_level = 2
; redirect = 1 serves files from mods/<mod>/Data/ in place of the game's copies
redirect = 0
; overlay = 1 hooks wglSwapBuffers and shows the in-game ImGui menu (F2/Insert)
overlay = 1
; console = 1 starts the separate log console; F3 toggles it at runtime
console = 1
; [disabled]
; org.example.some-mod = 1
"""


def build_delta(exe_path: str, out_delta: str, *, loader_name: str = "cdeloader.dll",
                version: str = "0.1.0", name: str = "CDE Mod Framework") -> dict:
    """Inject an import of ``<loader_name>!CdeModInit`` and write a delta."""
    with open(exe_path, "rb") as fh:
        source = fh.read()
    spec = {
        "format": "cde-patch/1",
        "name": name,
        "version": version,
        "variant": "framework",
        "module_name": "CDE.exe",
        "large_address_aware": True,
        "source": {"sha256": sha256(source), "size": len(source)},
        "imports": [{"dll": loader_name, "functions": ["CdeModInit"]}],
    }
    delta = build_from_spec(spec, source, symbols=Symbols.load())
    save_delta(delta, out_delta)
    return delta


def _copy_if_different(src: str, dst: str) -> None:
    """Copy unless src and dst are the same path (in-place installs hit this)."""
    if os.path.abspath(src) == os.path.abspath(dst):
        return
    shutil.copyfile(src, dst)


def assemble_dist(out_dir: str, *, loader: str, symbols: Optional[str] = None,
                  delta_path: Optional[str] = None,
                  mod_files: Optional[List[Tuple[str, str]]] = None) -> str:
    """Lay out an unpacked install folder (useful for local testing)."""
    os.makedirs(out_dir, exist_ok=True)
    _copy_if_different(loader, os.path.join(out_dir, "cdeloader.dll"))
    if symbols and os.path.isfile(symbols):
        _copy_if_different(symbols, os.path.join(out_dir, "cdeloader.symbols"))
    ini = os.path.join(out_dir, "cdeloader.ini")
    if not os.path.exists(ini):
        with open(ini, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(DEFAULT_INI)
    if delta_path and os.path.isfile(delta_path):
        _copy_if_different(delta_path, os.path.join(out_dir, "cdeloader.delta.json"))
    for src, rel in (mod_files or []):
        dst = os.path.join(out_dir, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst) or out_dir, exist_ok=True)
        shutil.copyfile(src, dst)
    return out_dir


def _write_compat_deltas(exe_path: str, out_dir: str, version: str) -> List[str]:
    """Build ``community variant -> community + cdeloader`` deltas.

    The packaged installer picks one by the current ``CDE.exe`` hash, so a game
    with the CDE Community Patch is stacked automatically (cdepatch.dll is
    preserved).  Requires the community specs in ``patches/``.
    """
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    with open(exe_path, "rb") as fh:
        stock = fh.read()
    symbols = Symbols.load()
    written: List[str] = []
    for variant in ("complete", "fixes-only"):
        spec_path = os.path.join(root, "patches", "cde-community-%s.spec.json" % variant)
        if not os.path.isfile(spec_path):
            continue
        with open(spec_path, "r", encoding="utf-8") as fh:
            spec = json.load(fh)
        community = apply_delta(stock, build_from_spec(spec, stock, symbols=symbols))
        pe = PE(community)
        inject_sections(pe, imports=[("cdeloader.dll", ["CdeModInit"])])
        combined = pe.to_bytes()
        delta = create_delta(community, combined, variant="compat-" + variant,
                             version=version)
        path = os.path.join(out_dir, "cdeloader-compat-%s.delta.json" % variant)
        save_delta(delta, path)
        written.append(path)
    return written


FRAMEWORK_README = _read_template("framework-readme.md")
SOURCE_NOTE = """\
CDE Mod Framework — source and documentation
=============================================

The full source of the framework is in `source/`.

  source/src/modloader/      loader, SDK headers, overlay, sample plugins
  source/scripts/            cdetool, cdefw, turbopump_calc.py, tests
  source/docs/               framework, modding, installation, parity, turbopump
  source/notes/              findings, parity, turboelectric investigation
  source/patches/            community-patch specs (reproduce the deltas)
  source/third_party/imgui/  vendored Dear ImGui (MIT)

Build (Linux cross-compile):
  cd source
  scripts/build-modloader.sh      # needs i686-w64-mingw32-g++

The build is path-independent: extracting this zip anywhere and running the
script reproduces cdeloader.dll and the plugin DLLs byte-for-byte with the same
toolchain version (this release was built with i686-w64-mingw32-g++ 16.2.0).
Verify with:

  sha256sum build/modloader/cdeloader.dll   # must equal the cdeloader.dll
                                            # delivered next to this file
  scripts/repro-check.sh                    # builds twice + packages twice

Install:
  python3 install.py <game folder>   # the packaged installer

License:
  Our code and docs: Unlicense (public domain).
  Dear ImGui: MIT (source/third_party/imgui/LICENSE.txt).
  The game and its assets are not included and remain their owners' property.

No game binaries, PDB, cdepatch.dll, or extracted game data are included.
"""


# The agent environment memo is not part of the shipped source (it is a
# workspace note: local paths, share IP, remote/key path).  Nothing else is
# filtered or rewritten; the source ships as it is in the repo.
ZIP_EXCLUDE = {"notes/ENVIRONMENT.md"}


def _source_files(root: str, prefix: str = "source/") -> List[Tuple[str, str]]:
    """Repo source, docs and notes, under ``prefix`` ("" for a archive root)."""
    out: List[Tuple[str, str]] = []
    skip = {"__pycache__", ".venv", "build", "artifacts", "decomp", ".git"}
    for rel in ("src", "scripts", "docs", "notes", "patches", "third_party",
                "examples"):
        base = os.path.join(root, rel)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in skip]
            for fn in filenames:
                if fn.endswith((".pyc", ".pyo")):
                    continue
                full = os.path.join(dirpath, fn)
                relpath = os.path.relpath(full, root).replace(os.sep, "/")
                if relpath in ZIP_EXCLUDE:
                    continue
                out.append((full, prefix + relpath))
    for fn in ("LICENSE", "README.md", "PLAN.md"):
        full = os.path.join(root, fn)
        if os.path.isfile(full):
            out.append((full, prefix + fn))
    return out


PROXY_README = """Proxy mode (no change to CDE.exe)
=====================================

1. Close the game.
2. In the game folder, rename vcruntime140.dll to vcruntime140_orig.dll.
3. Copy proxy/vcruntime140.dll from this package into the game folder as
   vcruntime140.dll.
4. Copy cdeloader.dll, cdeloader.symbols and cdeloader.ini next to CDE.exe
   (this package's installer already does that for delta mode).
5. Start the game normally.

Undo: delete the proxy vcruntime140.dll and rename vcruntime140_orig.dll back to
vcruntime140.dll.

CDE.exe is never modified, so Steam "verify integrity" cannot revert it, and
ReShade (opengl32.dll) or the community patch (steam_api.dll) keep their slots.
If the exe also has the delta import, the loader still initialises once: Windows
loads a DLL only once per process.
"""


OPTIONAL_SPECS: List[Tuple[str, str, str]] = [
    ("laser-m2", "org.cdere.laser-m2",
     "[mod]\n# lower the M^2 beam-quality floor (notes/M22-M2-CAP.md)\n"
     "enabled = 1\n"
     "# 1 = physical limit (M^2 >= 1, diffraction limited; default)\n"
     "# 3 = vanilla limit (M^2 >= 3)\n"
     "limit = 1\n"
     "log = 0\n"),
    ("laser-jitter", "org.cdere.laser-jitter",
     "[mod]\n# bound the turret aim-point wander (notes/M24-LASER-JITTER.md)\n"
     "enabled = 1\n"
     "absolute = 1\n"
     "arcsec = 0.5\n"
     "scale = 0.1\n"
     "log = 0\n"),
    ("node-editor", "org.cdere.node-editor",
     "[mod]\n# manoeuvre-node editor window (notes/M19-NODES.md)\n"
     "enabled = 1\n"
     "step_kms = 0.001\n"
     "log = 0\n"),
    ("auto-intercept", "org.cdere.auto-intercept",
     "[mod]\n# flyby at any range, nothing else (notes/M25-AUTO-INTERCEPT-RANGE.md)\n"
     "enabled = 1\n"),
    ("laser-cap", "org.cdere.laser-cap",
     "[mod]\n# remove the armour photon-damage clamp (1.0 depth cap)\n"
     "enabled = 1\n"),
]


def _optional_mods(root: str, tmp: str) -> List[Tuple[str, str]]:
    """Plugins shipped for opt-in installation under optional/ in the zip."""
    out: List[Tuple[str, str]] = []
    optional = os.path.join(root, "build", "modloader")
    specs = OPTIONAL_SPECS
    for folder, mod_id, ini_text in specs:
        dll = os.path.join(optional, folder, "plugin.dll")
        if not os.path.isfile(dll):
            continue
        out.append((dll, "optional/%s/%s.dll" % (folder, folder)))
        path = os.path.join(tmp, "%s.ini" % mod_id)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(ini_text)
        out.append((path, "optional/%s/%s.ini" % (folder, mod_id)))
    if out:
        readme = os.path.join(tmp, "optional-README.txt")
        with open(readme, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(OPTIONAL_README)
        out.append((readme, "optional/README.txt"))
    return out


OPTIONAL_README = """Optional plugins
================

These are not installed by the default installers.  To use one:

1. Copy its folder into the game's mods/ directory, e.g.
       mods/laser-cap/laser-cap.dll
2. Copy the .ini into mods/config/ (create the folder if needed), e.g.
       mods/config/org.cdere.laser-cap.ini
3. Edit the ini, then start the game.  F2 (or Insert) opens the loader menu
   (the game stays playable), F3 the log console; a faulting plugin is isolated
   and disabled with an error in cdeloader.log.

node-editor      Shows the selected fleet's manoeuvre nodes and lets a burn be
                 nudged through the game's own burn path.  See
                 notes/M19-NODES.md.

long-range-intercept  Lifts the delta-v truncation on planned burns and lets you
                 pick how soon the intercept should happen; stock behaviour comes
                 back with enabled = 0.  See notes/LONG-RANGE-INTERCEPT-PLAN.md.

auto-intercept   Offers flyby at any range and changes nothing else: every fleet
                 is listed, the flyby row is kept, and the intercept/join row only
                 appears for fleets in engagement range, as stock.
                 See notes/M25-AUTO-INTERCEPT-RANGE.md.

laser-cap        Removes the 1.0 armour photon-ablation depth clamp, live.
                 See docs/17-mod-catalog.md.

laser-jitter     Bounds turret pointing jitter to a real pointing accuracy:
                 absolute mode (default) caps the angular error in arcseconds,
                 relative mode scales the shipped offset, and the panel shows
                 what the stock jitter was.
                 See notes/M24-LASER-JITTER.md.

laser-m2         Lowers the beam-quality limit from M^2 = 3 to M^2 = 1, live.
                 See notes/M22-M2-CAP.md.
"""


# vcruntime140.dll as shipped in the retail game (renamed to _orig in proxy mode).
# Observed on the Steam install used for verification; the installer accepts any
# copy but reports this hash for reference.
RUNTIME140_SHA256 = "1b372f064eacb455a0351863706e6326ca31b08e779a70de5de986b5be8069a1"


def _sha256_file(path: str) -> str:
    import hashlib
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()



def _hash_or_dash(path: str) -> str:
    try:
        return _sha256_file(path)
    except OSError:
        return "-"


def _hashes_for_readme(loader: str, proxy: str) -> dict:
    return {"@@PROXY_SHA@@": _hash_or_dash(proxy),
            "@@LOADER_SHA@@": _hash_or_dash(loader)}

def _fill_hashes(text: str, loader: str, proxy: str) -> str:
    for key, value in _hashes_for_readme(loader, proxy).items():
        text = text.replace(key, value)
    return text

def _hash_block(exe_path: str, delta_path: str, compat: List[str],
                proxy_dll: Optional[str]) -> str:
    """The README hash section: accepted game builds + the runtime we replace."""
    import json

    with open(delta_path, "r", encoding="utf-8") as fh:
        fram = json.load(fh)
    lines = ["CDE.exe builds this package accepts (it refuses anything else):"]
    lines.append("  %s  stock / Steam" % fram["source_sha256"])
    labels = {"compat-complete": "CDE Community Patch (complete)",
              "compat-fixes-only": "CDE Community Patch (fixes-only)"}
    targets = [(fram["target_sha256"], "stock + the framework delta")]
    for path in compat:
        with open(path, "r", encoding="utf-8") as fh:
            d = json.load(fh)
        label = labels.get(d.get("variant", ""), "CDE Community Patch")
        lines.append("  %s  %s" % (d["source_sha256"], label))
        targets.append((d["target_sha256"], label + " + framework"))
    lines.append("")
    lines.append("Proxy mode never touches CDE.exe.  Only install-delta.* writes a")
    lines.append("patched exe, and then it is one of:")
    for sha, what in targets:
        lines.append("  %s  %s" % (sha, what))
    lines.append("")
    lines.append("vcruntime140.dll replaced by proxy mode:")
    lines.append("  %s  original runtime (renamed to vcruntime140_orig.dll)"
                 % RUNTIME140_SHA256)
    if proxy_dll and os.path.isfile(proxy_dll):
        lines.append("  %s  our forwarder (installed as vcruntime140.dll)"
                     % _sha256_file(proxy_dll))
    return "\n".join(lines)


MODPACK_README = _read_template("modpack-readme.md")
_CDE_MOD_RE = re.compile('CDE_MOD\\(\\s*"([^"]+)"\\s*,\\s*"([^"]+)"\\s*,\\s*"([^"]+)"')

PLUGIN_SUMMARIES = {
    "org.cdere.auto-intercept":
        "Flyby at any range",
    "org.cdere.node-editor":
        "Manoeuvre node editor",
    "org.cdere.laser-cap":
        "Laser ablation cap removal",
    "org.cdere.laser-jitter":
        "Laser jitter reduction",
    "org.cdere.laser-m2":
        "Laser M^2 limit removal",
}


def plugin_versions(root: str) -> Dict[str, Tuple[str, str, str]]:
    """mod_id -> (folder, display name, version) for every plugin source."""
    out: Dict[str, Tuple[str, str, str]] = {}
    sample = os.path.join(root, "src", "modloader", "sample")
    if not os.path.isdir(sample):
        return out
    for name in sorted(os.listdir(sample)):
        if not name.endswith(".cpp"):
            continue
        try:
            with open(os.path.join(sample, name), "r", encoding="utf-8") as fh:
                m = _CDE_MOD_RE.search(fh.read())
        except OSError:
            continue
        if not m:
            continue
        mod_id, display, version = m.group(1), m.group(2), m.group(3)
        folder = name[: -len(".cpp")].replace("_", "-")
        out[mod_id] = (folder, display, version)
    return out


PLUGIN_README = _read_template("plugin-readme.md")
def _plugin_settings_block(ini_text: str) -> str:
    """Turn a default ini into a plain list of keys for the README."""
    keys = []
    for line in ini_text.splitlines():
        line = line.strip()
        if not line or line.startswith("[") or line.startswith("#"):
            continue
        if "=" in line:
            key, value = line.split("=", 1)
            keys.append("        %-12s %s" % (key.strip(), value.strip()))
    return "\n".join(keys) if keys else "        (no settings)"


def make_plugin_zip(root: str, out_zip: str, mod_id: str, *, folder: str,
                    display: str, version: str, ini_text: str,
                    summary: str = "") -> str:
    """Bundle one plugin as a drag-and-drop zip for the game folder."""
    from cdetool.package import _write_zip

    dll = os.path.join(root, "build", "modloader", folder, "plugin.dll")
    if not os.path.isfile(dll):
        raise RuntimeError("plugin not built: %s" % dll)
    tmp = tempfile.mkdtemp(prefix="cdefw-plugin-")
    try:
        ini = os.path.join(tmp, "%s.ini" % mod_id)
        with open(ini, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(ini_text)
        readme = os.path.join(tmp, "README.md")
        with open(readme, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(PLUGIN_README
                     .replace("@@DISPLAY@@", display)
                     .replace("@@VERSION@@", version)
                     .replace("@@LEN@@", "=" * (len(display) + len(version) + 1))
                     .replace("@@SUMMARY@@", summary or display)
                     .replace("@@SETTINGS@@", _plugin_settings_block(ini_text))
                     .replace("@@FOLDER@@", folder)
                     .replace("@@MODID@@", mod_id))
        entries = [(dll, "mods/%s/%s.dll" % (folder, folder)),
                   (ini, "mods/config/%s.ini" % mod_id),
                   (readme, "README.md")]
        license_path = os.path.join(root, "LICENSE")
        if os.path.isfile(license_path):
            entries.append((license_path, "LICENSE"))
        _write_zip(out_zip, entries)
        return out_zip
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def make_plugin_zips(root: str, out_dir: str) -> List[str]:
    """One zip per shipped plugin, each named after its own version."""
    versions = plugin_versions(root)
    written: List[str] = []
    for folder, mod_id, ini_text in OPTIONAL_SPECS:
        info = versions.get(mod_id)
        if not info:
            continue
        _folder, display, version = info
        out = os.path.join(out_dir, "cdeloader-mod-%s-%s.zip" % (folder, version))
        written.append(make_plugin_zip(
            root, out, mod_id, folder=folder, display=display, version=version,
            ini_text=ini_text,
            summary=PLUGIN_SUMMARIES.get(mod_id, "").replace("\\n", "\n")))
    return written





# --- installer package -----------------------------------------------------
#
# A single download for people who just want the thing working: the framework
# files, every plugin, and a batch script that puts them where they belong.  The
# script is deliberately plain: it checks for CDE.exe, keeps a backup of the
# runtime it replaces, and never overwrites an existing cdeloader.ini.

INSTALLER_README = _read_template("installer-readme.md")


# --- what a source release contains ---------------------------------------
#
# A release is not a copy of the working tree.  The working tree carries research
# notes that refer to one machine (paths, a file share on a private network), agent
# scaffolding and generated caches.  This list is the boundary: the code, the tools
# and the reference documentation go out; notes, session state and anything
# derived from a local machine stay in the repository.

RELEASE_DIRS = ("src", "scripts", "examples", "patches", "third_party")
RELEASE_FILES = ("LICENSE",)
RELEASE_SKIP_DIRS = {"__pycache__", ".git", ".venv", "build", "decomp",
                     "artifacts", "screenshots", ".agents"}

# Nothing ships that only makes sense beside the private research corpus: those tools read
# notes/ and decomp/, neither of which is in the archive, so in a public tree they would be files
# without a purpose.
RELEASE_SKIP_FILES = {
    "scripts/check-notes.py", "scripts/check-offsets.py", "scripts/audit-addresses.py",
    "scripts/dump-enums.py", "scripts/dump-hierarchy.py", "scripts/dump-struct.py",
    "scripts/find-callers.py", "scripts/read-function.py", "scripts/gen-community-specs.py",
    "scripts/typefields.py", "scripts/turbopump_calc.py", "scripts/pdb_guid.py",
    "scripts/pdb-dump.sh", "scripts/ghidra-headless.sh", "scripts/fetch-artifacts.sh",
    "scripts/verify-artifacts.sh", "scripts/env-check.sh",
    "scripts/dev-log.sh", "scripts/dev-serve.sh", "scripts/dev-package.sh",
    # The update plugin is a development tool: it is in no release package, so its source and its
    # LAN address have no business in the public tree either.
    "src/modloader/sample/update.cpp",
    # Documentation that is not code and not a licence: the archive carries README.md and LICENSE
    # and nothing else of that kind.
    "examples/README.md", "examples/data-only/README.md",
    "scripts/cdetool/templates/README.txt", "scripts/requirements-analysis.txt",
    "third_party/imgui/VENDOR.md",
    "scripts/tests/test_entry_addresses.py", "scripts/tests/test_check_offsets.py",
    "scripts/tests/test_dump_struct.py", "scripts/tests/test_find_callers.py",
    "scripts/tests/test_read_function.py", "scripts/tests/test_abi_tables.py",
    "scripts/tests/test_dump_enums.py", "scripts/tests/test_notes_structure.py",
}
RELEASE_SKIP_SUFFIX = (".pyc", ".pyo", ".log", ".dmp", ".zip", ".exe", ".pdb",
                       ".dll", ".tar", ".gz")

# Host-specific text is replaced so the archive does not describe a private
# machine.  Order matters: longer strings first.
RELEASE_SCRUB = (
    # private ranges, as patterns, so no address is written down here
    (r"https?://(?:192\.168|10|172\.(?:1[6-9]|2[0-9]|3[01]))\.[0-9.]+(?::\d+)?/?",
     "<host>/"),
    (r"/home/[A-Za-z0-9_-]+/(?:Projects|games|export)[^\s\"'`)]*", "<path>"),
    (r"/home/[A-Za-z0-9_-]+", "<home>"),
    (r"[A-Z]:\\\\[A-Za-z0-9_ .-]+", "<game folder>"),
)

# Only these extensions get the scrub; a rewritten binary would be worse than a
# leaked path.
RELEASE_TEXT_SUFFIX = (".md", ".py", ".sh", ".cpp", ".hpp", ".h", ".c", ".ini",
                       ".txt", ".json", ".yml", ".yaml", ".def", ".tsv", "")


def _scrub_text(source: str, dest: str, replacements) -> None:
    try:
        with open(source, "r", encoding="utf-8", errors="surrogateescape") as fh:
            text = fh.read()
    except OSError:
        shutil.copyfile(source, dest)
        return
    for pattern, new in replacements:
        text = re.sub(pattern, new, text)
    with open(dest, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as fh:
        fh.write(text)


def _release_source_files(root: str, tmp: str) -> List[Tuple[str, str]]:
    """Files for the public source zip, scrubbed of host-specific text."""
    out: List[Tuple[str, str]] = []
    count = 0
    for rel in RELEASE_DIRS:
        base = os.path.join(root, rel)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in RELEASE_SKIP_DIRS]
            for name in filenames:
                if name.endswith(RELEASE_SKIP_SUFFIX):
                    continue
                full = os.path.join(dirpath, name)
                relpath = os.path.relpath(full, root).replace(os.sep, "/")
                if relpath in RELEASE_SKIP_FILES:
                    continue
                if relpath.endswith(RELEASE_TEXT_SUFFIX):
                    count += 1
                    scrubbed = os.path.join(tmp, "s%d" % count)
                    _scrub_text(full, scrubbed, RELEASE_SCRUB)
                    out.append((scrubbed, relpath))
                else:
                    out.append((full, relpath))
    for rel in RELEASE_FILES:
        full = os.path.join(root, rel)
        if not os.path.isfile(full):
            continue
        count += 1
        scrubbed = os.path.join(tmp, "r%d" % count)
        _scrub_text(full, scrubbed, RELEASE_SCRUB)
        out.append((scrubbed, rel))
    return out


def make_source_zip(root: str, out_zip: str, version: str) -> str:
    """The source tree as its own download, laid out like the repository."""
    from cdetool.package import _write_zip

    tmp = tempfile.mkdtemp(prefix="cdefw-source-")
    try:
        entries = _release_source_files(root, tmp)
        readme = os.path.join(tmp, "README.md")
        with open(readme, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(SOURCE_ZIP_README.replace("@@VERSION@@", version))
        entries.append((readme, "README.md"))
        _write_zip(out_zip, entries)
        return out_zip
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


SOURCE_ZIP_README = _read_template("source-readme.md")
SOURCE_INSTALLER_README = _read_template("source-installer-readme.md")


def make_source_installer(root: str, out_zip: str, version: str) -> str:
    """Source, build scripts and installer - no compiled binaries at all.

    The point of this package is that every DLL is built on the machine that will run it, from source
    the reader can inspect. Nothing is downloaded; the game folder supplies CDE.exe and CDE.pdb.
    """
    from cdetool.package import _write_zip

    tmp = tempfile.mkdtemp(prefix="cdefw-sourceinstaller-")
    try:
        # The repository's own README describes the project; this package's README describes the
        # build. Only one of them can be README.md at the archive root.
        entries: List[Tuple[str, str]] = [(src, arc) for src, arc in _release_source_files(root, tmp)
                                          if arc != "README.md"]
        builder = os.path.join(root, "scripts", "build-and-install.sh")
        if not os.path.isfile(builder):
            raise RuntimeError("build-and-install.sh missing: %s" % builder)
        entries.append((builder, "build-and-install.sh"))
        script_src = os.path.join(root, "scripts", "cdefw", "templates", "install.py")
        if not os.path.isfile(script_src):
            raise RuntimeError("installer template missing: %s" % script_src)
        with open(script_src, encoding="utf-8") as fh:
            script_text = fh.read()
        # The forwarder does not exist yet: build-and-install.sh fills this in once it has built one.
        script = os.path.join(tmp, "install.py")
        with open(script, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(script_text)
        entries.append((script, "install.py"))
        readme = os.path.join(tmp, "README.md")
        with open(readme, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(SOURCE_INSTALLER_README.replace("@@VERSION@@", version))
        entries.append((readme, "README.md"))
        _write_zip(out_zip, entries)
        return out_zip
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def make_installer_package(root: str, out_zip: str, *, loader: str,
                           symbols: Optional[str] = None,
                           version: str = "0.1.0") -> str:
    """Framework, every plugin and the install script, as one download."""
    from cdetool.package import _write_zip

    tmp = tempfile.mkdtemp(prefix="cdefw-installer-")
    try:
        entries: List[Tuple[str, str]] = [(loader, "cdeloader.dll")]
        if symbols and os.path.isfile(symbols):
            entries.append((symbols, "cdeloader.symbols"))
        ini = os.path.join(tmp, "cdeloader.ini")
        with open(ini, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(DEFAULT_INI)
        entries.append((ini, "cdeloader.ini"))
        proxy_dll = os.path.join(root, "build", "proxy", "vcruntime140.dll")
        if os.path.isfile(proxy_dll):
            entries.append((proxy_dll, "vcruntime140.dll"))
        license_path = os.path.join(root, "LICENSE")
        if os.path.isfile(license_path):
            entries.append((license_path, "LICENSE"))
        entries.extend(_all_mod_files(root, tmp))
        # The install script verifies what it copies and writes the result into the
        # game folder for the loader to check on every start.
        manifest_lines = []
        for src_path, arc in sorted(entries):
            if arc.startswith("mods/") and arc.endswith(".dll"):
                manifest_lines.append("%s  %s" % (_sha256_file(src_path), arc))
        if manifest_lines:
            manifest = os.path.join(tmp, "MODS.sha256")
            with open(manifest, "w", encoding="utf-8", newline="\n") as fh:
                fh.write("\n".join(manifest_lines) + "\n")
            entries.append((manifest, "MODS.sha256"))
        script_src = os.path.join(root, "scripts", "cdefw", "templates", "install.py")
        if not os.path.isfile(script_src):
            raise RuntimeError("installer template missing: %s" % script_src)
        with open(script_src, encoding="utf-8") as fh:
            script_text = fh.read()
        script_text = script_text.replace("@@FORWARDER_SHA@@",
                                          _hash_or_dash(proxy_dll))
        script = os.path.join(tmp, "install.py")
        with open(script, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(script_text)
        entries.append((script, "install.py"))
        readme = os.path.join(tmp, "README.md")
        with open(readme, "w", encoding="utf-8", newline="\n") as fh:
            rows = []
            versions = plugin_versions(root)
            for folder, mod_id, _ini in OPTIONAL_SPECS:
                info = versions.get(mod_id)
                if info:
                    rows.append("  %-22s %s" % (folder, info[2]))
            fh.write(_fill_hashes(INSTALLER_README.replace("@@VERSION@@", version)
                                  .replace("@@MODLIST@@", "\n".join(rows)),
                                  loader, proxy_dll))
        entries.append((readme, "README.md"))
        _write_zip(out_zip, entries)
        return out_zip
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def make_modpack_zip(root: str, out_zip: str, version: str) -> str:
    """Bundle every built plugin as one drag-and-drop zip."""
    from cdetool.package import _write_zip

    tmp = tempfile.mkdtemp(prefix="cdefw-modpack-")
    try:
        raw = _optional_mods(root, tmp)
        entries: List[Tuple[str, str]] = []
        rows: List[str] = []
        for src, dst in raw:
            if dst == "optional/README.txt":
                continue
            if not dst.startswith("optional/"):
                continue
            rel = dst[len("optional/"):]
            folder = rel.split("/")[0]
            if dst.endswith(".dll"):
                # The build always calls the artefact plugin.dll; the archive name is the plugin's
                # folder, which is what the loader, the readmes and the installer all use.
                entries.append((src, "mods/%s/%s.dll" % (folder, folder)))
            elif dst.endswith(".ini"):
                entries.append((src, "mods/config/%s" % os.path.basename(rel)))
        versions = plugin_versions(root)
        for folder, mod_id, _ini in OPTIONAL_SPECS:
            info = versions.get(mod_id)
            if not info:
                continue
            summary = PLUGIN_SUMMARIES.get(mod_id, "").replace("\n", " ")
            rows.append("- **%s** %s: %s" % (folder, info[2], summary))
        if not entries:
            raise RuntimeError("no optional plugins built")
        readme = os.path.join(tmp, "README.md")
        with open(readme, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(MODPACK_README.replace("@@VERSION@@", version)
                     .replace("@@MODLIST@@", "\n".join(rows)))
        entries.append((readme, "README.md"))
        license_path = os.path.join(root, "LICENSE")
        if os.path.isfile(license_path):
            entries.append((license_path, "LICENSE"))
        _write_zip(out_zip, entries)
        return out_zip
    finally:
        shutil.rmtree(tmp, ignore_errors=True)



def _all_mod_files(root: str, tmp: str) -> List[Tuple[str, str]]:
    """Every built mod, installed into the game dir (not optional/).

    The 2026-10-19 user request was one command that drops the whole set into
    the game folder for testing, so the framework package installs all of them
    with their default inis; the separate modpack zip still exists for people
    who want to pick and choose.
    """
    out: List[Tuple[str, str]] = []
    built = os.path.join(root, "build", "modloader")
    for folder, mod_id, ini_text in OPTIONAL_SPECS:
        dll = os.path.join(built, folder, "plugin.dll")
        if not os.path.isfile(dll):
            continue
        out.append((dll, "mods/%s/%s.dll" % (folder, folder)))
        path = os.path.join(tmp, "%s.ini" % mod_id)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(ini_text)
        out.append((path, "mods/config/%s.ini" % mod_id))
    return out




def make_framework_package(exe_path: str, out_zip: str, *, loader: str,
                           symbols: Optional[str] = None,
                           mod_files: Optional[List[Tuple[str, str]]] = None,
                           all_mods: bool = True,
                           version: str = "0.1.0",
                           name: str = "CDE Mod Framework") -> str:
    """Bundle the loader, its settings and the SDK as a drag-and-drop zip.

    The archive is laid out as the game folder expects it, so installing is
    "rename the runtime, then extract this over the game folder".  No executable
    patching: the loader arrives as `cdeloader.dll` next to a forwarding
    `vcruntime140.dll`.
    """
    from cdetool.package import _write_zip

    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    tmp = tempfile.mkdtemp(prefix="cdefw-")
    try:
        ini = os.path.join(tmp, "cdeloader.ini")
        with open(ini, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(DEFAULT_INI)
        entries: List[Tuple[str, str]] = [(loader, "cdeloader.dll")]
        if symbols and os.path.isfile(symbols):
            entries.append((symbols, "cdeloader.symbols"))
        entries.append((ini, "cdeloader.ini"))

        proxy_dll = os.path.join(root, "build", "proxy", "vcruntime140.dll")
        if os.path.isfile(proxy_dll):
            entries.append((proxy_dll, "vcruntime140.dll"))

        license_path = os.path.join(root, "LICENSE")
        if os.path.isfile(license_path):
            entries.append((license_path, "LICENSE"))

        if all_mods:
            entries.extend(_all_mod_files(root, tmp))
        # Callers may supply their own (source_path, archive_path) pairs, which is how
        # the all-mods package and the tests add plugins.
        entries.extend(mod_files or [])

        # The README carries what used to be a separate SOURCE.txt.
        readme = os.path.join(tmp, "README.md")
        with open(readme, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(_fill_hashes(FRAMEWORK_README.replace("@@VERSION@@", version),
                                  loader, proxy_dll))
        entries.append((readme, "README.md"))
        _write_zip(out_zip, entries)
        return out_zip
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _all_mod_files(root: str, tmp: str) -> List[Tuple[str, str]]:
    """Every built mod, installed into the game dir (not optional/).

    The 2026-10-19 user request was one command that drops the whole set into
    the game folder for testing, so the framework package installs all of them
    with their default inis; the separate modpack zip still exists for people
    who want to pick and choose.
    """
    out: List[Tuple[str, str]] = []
    built = os.path.join(root, "build", "modloader")
    for folder, mod_id, ini_text in OPTIONAL_SPECS:
        dll = os.path.join(built, folder, "plugin.dll")
        if not os.path.isfile(dll):
            continue
        out.append((dll, "mods/%s/%s.dll" % (folder, folder)))
        path = os.path.join(tmp, "%s.ini" % mod_id)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(ini_text)
        out.append((path, "mods/config/%s.ini" % mod_id))
    return out



