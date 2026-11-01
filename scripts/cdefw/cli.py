"""Command-line entry point: ``python3 -m cdefw``.

Subcommands
-----------
``sdk``           generate the loader's ``cdeloader.symbols`` from the PDB
``new-plugin``    scaffold a plugin project
``new-data-mod``  scaffold a data-mod folder (mods/<id>/Data/)
``install``       build the loader-injection delta and lay out a dist folder
``package``       build the delta and bundle a distributable zip
``info``          report the framework layout and tool availability
"""

from __future__ import annotations

import argparse
import hashlib
import os
import time
import shutil
import sys
from typing import List, Optional, Tuple

from . import __version__
from . import framework
from . import sdk as sdk_mod
from .scaffold import scaffold, scaffold_data_mod


def cmd_sdk(args) -> int:
    try:
        sha, count = sdk_mod.build(
            tsv_path=args.table, out_path=args.output, exe_path=args.exe,
            pdb_path=args.pdb, repo_root=args.repo)
    except FileNotFoundError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    print("wrote %s (%d symbols, exe sha256 %s)" % (args.output, count, sha or "?"))
    return 0


def cmd_new_plugin(args) -> int:
    target = scaffold(args.name, args.out, mod_id=args.id, author=args.author)
    print("created plugin project at %s" % target)
    print("  build: (cd %s && ./build.sh)" % target)
    return 0


def cmd_new_data_mod(args) -> int:
    target = scaffold_data_mod(args.name, args.out, mod_id=args.id,
                               author=args.author, files=args.file)
    print("created data mod at %s" % target)
    print("  drop the game's data files under %s/Data/ at their relative paths" % target)
    print("  enable it with 'redirect = 1' in the game's cdeloader.ini")
    return 0


def _resolve(root: str, args) -> Tuple[str, str, str]:
    exe = args.exe or os.path.join(root, "artifacts", "CDE.exe.original")
    loader = args.loader or os.path.join(root, "build", "modloader", "cdeloader.dll")
    # build/modloader/cdeloader.symbols is overwritten by the wine test host;
    # prefer the packaged (real-game) table when it exists.
    symbols = args.symbols
    if not symbols:
        packaged = os.path.join(root, "build", "framework", "dist", "cdeloader.symbols")
        symbols = packaged if os.path.isfile(packaged) else os.path.join(
            root, "build", "modloader", "cdeloader.symbols")
    return exe, loader, symbols


def _mod_files(root: str, args) -> List[Tuple[str, str]]:
    """Extra files to ship under the game dir.  ``--mod src[=rel]`` overrides the
    default (the built sample plugin, if present)."""
    files: List[Tuple[str, str]] = []
    for spec in (args.mod or []):
        if "=" in spec:
            src, rel = spec.split("=", 1)
        else:
            src = spec
            rel = "mods/%s/%s" % (os.path.basename(os.path.dirname(spec)), os.path.basename(spec))
        files.append((os.path.abspath(src), rel))
    if not files:
        sample = os.path.join(root, "build", "modloader", "mods", "sample", "plugin.dll")
        if os.path.isfile(sample):
            files.append((sample, "mods/sample/plugin.dll"))
    return files


def _imports_include(pe, dll_name: str) -> bool:
    """True when ``pe``'s import table already names ``dll_name``."""
    import struct

    from cdetool.inject import _read_import_descriptors

    for raw in _read_import_descriptors(pe):
        _oft, _tds, _fc, name_rva, _ft = struct.unpack("<IIIII", raw)
        if not name_rva:
            continue
        off = pe.rva_to_offset(name_rva)
        if off is None:
            continue
        end = pe.data.find(b"\0", off)
        if end > off and pe.data[off:end].decode("latin1").lower() == dll_name.lower():
            return True
    return False


def _require(exe: str, loader: str) -> Optional[int]:
    if not os.path.isfile(exe):
        print("error: executable not found: %s" % exe, file=sys.stderr)
        return 2
    if not os.path.isfile(loader):
        print("error: loader not found: %s (run `make modloader`)" % loader, file=sys.stderr)
        return 2
    return None


def _install_proxy(args, root):
    """Proxy mode: copy the loader + vcruntime140 forwarder; never touch the exe."""
    from cdefw import proxy as proxy_mod

    loader = args.loader or os.path.join(root, "build", "modloader", "cdeloader.dll")
    if not os.path.isfile(loader):
        print("error: loader not built: %s" % loader, file=sys.stderr)
        return 2
    if not args.gamedir:
        print("error: --proxy needs --gamedir <game folder>", file=sys.stderr)
        return 2

    exe = args.exe or os.path.join(root, "artifacts", "CDE.exe.original")
    symbols = args.symbols or os.path.join(root, "build", "modloader",
                                          "cdeloader.symbols")
    if not os.path.isfile(symbols) or os.path.getsize(symbols) < 4096:
        symbols = os.path.join(root, "build", "modloader", "cdeloader.symbols")
    proxy_dll = proxy_mod.default_proxy_dll(root)
    if not os.path.isfile(proxy_dll):
        print("error: proxy DLL not built; run scripts/build-proxy.sh", file=sys.stderr)
        return 2

    if args.restore_runtime:
        try:
            report = proxy_mod.restore(args.gamedir)
        except proxy_mod.ProxyError as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 2
        print("restored %s" % report["restored"])
        return 0

    import tempfile
    ini_path = None
    if framework.DEFAULT_INI:
        fd, ini_path = tempfile.mkstemp(prefix="cdeloader-", suffix=".ini")
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(framework.DEFAULT_INI)

    try:
        report = proxy_mod.install(
            args.gamedir, proxy_dll=proxy_dll, loader=loader,
            symbols=symbols if os.path.isfile(symbols) else None,
            ini=ini_path, stock_exe=exe if os.path.isfile(exe) else None,
            mod_files=_mod_files(root, args))
    except proxy_mod.ProxyError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    print("proxy mode installed into %s (CDE.exe untouched)" % args.gamedir)
    for kind, src, dst in report["actions"]:
        print("  %-7s %s -> %s" % (kind, os.path.basename(src), dst))
    print("launch the game normally; undo with: cdefw install --proxy "
          "--restore-runtime --gamedir %s" % args.gamedir)
    return 0


def cmd_install(args) -> int:
    root = sdk_mod.find_repo_root()
    if (getattr(args, "proxy", False)
            or getattr(args, "restore_runtime", False)):
        return _install_proxy(args, root)
    exe, loader, symbols = _resolve(root, args)
    rc = _require(exe, loader)
    if rc is not None:
        return rc

    out = args.out or os.path.join(root, "build", "framework")
    os.makedirs(out, exist_ok=True)
    delta_path = os.path.join(out, "cdeloader.delta.json")
    delta = framework.build_delta(exe, delta_path, version=args.version)
    dist = framework.assemble_dist(
        os.path.join(out, "dist"), loader=loader,
        symbols=symbols if os.path.isfile(symbols) else None,
        delta_path=delta_path, mod_files=_mod_files(root, args))

    print("built %s" % delta_path)
    print("  source sha256 %s" % delta["source_sha256"])
    print("  target sha256 %s" % delta["target_sha256"])
    print("layout at %s (cdeloader.dll + symbols + ini + mods/)" % dist)

    if args.gamedir:
        from cdetool.delta import apply_delta, load_delta
        game_exe = os.path.join(args.gamedir, "CDE.exe")
        if not os.path.isfile(game_exe):
            print("error: no CDE.exe in %s" % args.gamedir, file=sys.stderr)
            return 2
        with open(game_exe, "rb") as fh:
            source = fh.read()

        if args.compat:
            # Stack our import on top of whatever is already injected (e.g. the
            # community patch's cdepatch.dll).  inject() rebuilds the import
            # table from the current descriptors, so there is no stock-hash
            # assumption; existing imports are preserved.
            from cdetool.pe import PE
            from cdetool.inject import inject as inject_sections
            pe = PE(source)
            backup = game_exe + ".before-cdeloader"
            if _imports_include(pe, "cdeloader.dll"):
                print("%s already imports cdeloader.dll; leaving the exe alone" % game_exe)
            else:
                report = inject_sections(
                    pe, imports=[("cdeloader.dll", ["CdeModInit"])],
                    large_address_aware=True)
                target = pe.to_bytes()
                if not os.path.exists(backup):
                    shutil.copyfile(game_exe, backup)
                with open(game_exe, "wb") as fh:
                    fh.write(target)
                print("stacked cdeloader onto %s (backup: %s)" % (game_exe, backup))
                for name, rva in report["sections"].items():
                    print("  %s at RVA 0x%08x" % (name, rva))
            # Keep the stock exe next to the game for symbol verification.  Only
            # copy it when the delta source really is the stock build and the
            # game has no stock backup yet.
            with open(exe, "rb") as fh:
                stock_bytes = fh.read()
            if (hashlib.sha256(stock_bytes).hexdigest() == delta["source_sha256"]
                    and not os.path.exists(game_exe + ".original")):
                shutil.copyfile(exe, game_exe + ".original")
                print("  wrote stock backup %s (for symbol verification)"
                      % (game_exe + ".original"))
        else:
            target = apply_delta(source, load_delta(delta_path), verify_source=True)
            backup = game_exe + ".original"
            if not os.path.exists(backup):
                shutil.copyfile(game_exe, backup)
            with open(game_exe, "wb") as fh:
                fh.write(target)
        for rel in ("cdeloader.dll", "cdeloader.symbols", "cdeloader.ini"):
            src = os.path.join(dist, rel)
            if os.path.isfile(src):
                shutil.copyfile(src, os.path.join(args.gamedir, rel))
        for src, rel in _mod_files(root, args):
            dst = os.path.join(args.gamedir, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst) or args.gamedir, exist_ok=True)
            shutil.copyfile(src, dst)
        print("installed into %s (backup: %s)" % (args.gamedir, backup))
    return 0


def cmd_package(args) -> int:
    root = sdk_mod.find_repo_root()
    exe, loader, symbols = _resolve(root, args)
    rc = _require(exe, loader)
    if rc is not None:
        return rc

    out_zip = args.zip or os.path.join(
        root, "build", "framework", "cdeloader-%s.zip" % args.version)
    os.makedirs(os.path.dirname(out_zip), exist_ok=True)
    # The framework package is the loader, and only the loader. Plugins travel in the modpack, the
    # per-plugin zips and the combined installer; a framework zip that also drops five plugins into
    # someone's game folder is not what its name says it is.
    framework.make_framework_package(
        exe, out_zip, loader=loader,
        symbols=symbols if os.path.isfile(symbols) else None,
        mod_files=_mod_files(root, args), all_mods=not args.no_mods and args.with_mods,
        version=args.version)
    print("wrote %s" % out_zip)

    # The modpack is a convenience bundle whose contents change independently of
    # the framework version, so its name carries a timestamp instead of a number
    # that would imply a version bump.
    stamp = int(time.time())
    modpack = args.modpack or os.path.join(
        os.path.dirname(out_zip), "cdeloader-modpack-%d.zip" % stamp)
    framework.make_modpack_zip(root, modpack, args.version)
    print("wrote %s" % modpack)

    # Source tree as its own download.
    if not args.no_source:
        src_zip = os.path.join(os.path.dirname(out_zip),
                               "cdeloader-source-%s.zip" % args.version)
        framework.make_source_zip(root, src_zip, args.version)
    if not args.no_source_installer:
        variant_zip = os.path.join(os.path.dirname(out_zip),
                                   "cdeloader-source-installer-%s.zip" % args.version)
        framework.make_source_installer(root, variant_zip, args.version)
        print("wrote %s" % src_zip)

    # A single installer download: framework plus every plugin plus the install script.
    if not args.no_installer:
        installer = os.path.join(os.path.dirname(out_zip),
                                 "cdeloader-installer-%s.zip" % args.version)
        framework.make_installer_package(
            root, installer, loader=loader,
            symbols=symbols if os.path.isfile(symbols) else None,
            version=args.version)
        print("wrote %s" % installer)

    # One zip per plugin, each named after that plugin's own CDE_MOD version, so
    # updating one mod never renumbers or repackages the others.
    if args.each_plugin:
        for path in framework.make_plugin_zips(root, os.path.dirname(out_zip)):
            print("wrote %s" % path)
    return 0


def cmd_info(args) -> int:
    root = sdk_mod.find_repo_root()
    print("cdefw %s" % __version__)
    print("repo root:  %s" % root)
    print("loader src: %s" % os.path.join(root, "src", "modloader"))
    tsv = sdk_mod.find_functions_tsv(root)
    print("symbols tsv: %s" % (tsv or "(missing — run scripts/pdb-functions.py)"))
    for tool in ("i686-w64-mingw32-g++", "wine", "i686-w64-mingw32-nm"):
        from shutil import which
        print("  %-26s %s" % (tool, which(tool) or "(not found)"))
    return 0


def _add_build_args(q) -> None:
    q.add_argument("--exe", default=None,
                   help="stock executable (default: artifacts/CDE.exe.original)")
    q.add_argument("--loader", default=None,
                   help="cdeloader.dll (default: build/modloader/cdeloader.dll)")
    q.add_argument("--symbols", default=None,
                   help="cdeloader.symbols (default: build/modloader/cdeloader.symbols)")
    q.add_argument("--version", default="0.1.0")
    q.add_argument("--mod", action="append", default=None,
                   help="extra file to ship: SRC[=REL] (repeatable)")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="cdefw",
                                description="CDE mod framework tooling (docs/10)")
    p.add_argument("--version", action="version", version="cdefw " + __version__)
    sub = p.add_subparsers(dest="command", required=True)

    q = sub.add_parser("sdk", help="generate cdeloader.symbols from the PDB")
    q.add_argument("-o", "--output", default="cdeloader.symbols")
    q.add_argument("--table", default=None,
                   help="functions.tsv (default: decomp/symbols/functions.tsv)")
    q.add_argument("--exe", default=None,
                   help="executable whose sha256 is recorded (default: artifacts/CDE.exe.original)")
    q.add_argument("--pdb", default=None, help="PDB to build a table from if the tsv is missing")
    q.add_argument("--repo", default=None, help="repo root (default: auto-detect)")
    q.set_defaults(func=cmd_sdk)

    q = sub.add_parser("new-plugin", help="scaffold a plugin project")
    q.add_argument("name")
    q.add_argument("--out", default="plugins")
    q.add_argument("--id", default=None, help="reverse-DNS mod id")
    q.add_argument("--author", default="you")
    q.set_defaults(func=cmd_new_plugin)

    q = sub.add_parser("new-data-mod", help="scaffold a data-mod folder")
    q.add_argument("name")
    q.add_argument("--out", default="mods", help="output folder (default: mods)")
    q.add_argument("--id", default=None, help="reverse-DNS mod id")
    q.add_argument("--author", default="you")
    q.add_argument("--file", action="append", default=None,
                   help="relative data path to pre-create (repeatable)")
    q.set_defaults(func=cmd_new_data_mod)

    q = sub.add_parser("install", help="build the loader delta and a dist folder")
    _add_build_args(q)
    q.add_argument("--out", default=None, help="output folder (default: build/framework)")
    q.add_argument("--gamedir", default=None,
                   help="also apply to this game folder (backs up CDE.exe)")
    q.add_argument("--compat", action="store_true",
                   help="stack the loader onto an already-patched CDE.exe (e.g. the "
                        "CDE Community Patch) instead of applying the stock delta")
    q.add_argument("--proxy", action="store_true",
                   help="proxy mode: install a vcruntime140 forwarder instead of "
                        "patching CDE.exe (needs --gamedir; no delta is built)")
    q.add_argument("--restore-runtime", action="store_true",
                   help="undo proxy mode: restore the original vcruntime140.dll")
    q.set_defaults(func=cmd_install)

    q = sub.add_parser("package", help="build the delta and a distributable zip")
    _add_build_args(q)
    q.add_argument("--zip", default=None, help="output zip path")
    q.add_argument("--modpack", default=None,
                   help="output modpack zip (default: cdeloader-modpack-<version>.zip "
                        "next to --zip)")
    q.add_argument("--no-source-installer", action="store_true",
                   help="skip the build-from-source installer package")
    q.add_argument("--no-source", action="store_true",
                   help="skip the source zip")
    q.add_argument("--no-installer", action="store_true",
                   help="skip the combined installer zip (framework + all plugins + install.cmd)")
    q.add_argument("--each-plugin", action="store_true",
                   help="also write one zip per plugin (cdeloader-mod-<folder>-<version>.zip)")
    q.add_argument("--with-mods", action="store_true",
                   help="also install the built plugins into the framework package")
    q.add_argument("--no-mods", action="store_true",
                   help="do not install the mod set into the framework package "
                        "(only mods/sample then)")
    q.set_defaults(func=cmd_package)

    q = sub.add_parser("info", help="report framework layout and tool availability")
    q.set_defaults(func=cmd_info)

    return p


def main(argv: Optional[list] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
