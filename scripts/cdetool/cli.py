"""Command-line entry point: ``python3 -m cdetool``.

Subcommands
-----------
``info``    headers, hashes, sections, imports and exports of a PE
``symbols`` resolve/search PDB-derived symbols
``diff``    build a delta from two executables
``build``   build a delta from a patch spec
``apply``   apply a delta to an executable (with backups, like the installer)
``revert``  restore an executable from its ``.original`` backup
``verify``  check that an executable matches a delta's source or target hash
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from typing import Optional

from . import __version__, inject
from .delta import DeltaError, apply_delta, create_delta, load_delta, save_delta
from .pe import DIRECTORY_NAMES, PE, PEError, sha256
from .package import make_package
from .spec import SpecError, build_from_spec, load_spec
from .symbols import Symbols


def _read(path: str) -> bytes:
    with open(path, "rb") as fh:
        return fh.read()


def _write(path: str, data: bytes) -> None:
    with open(path, "wb") as fh:
        fh.write(data)


def _backup_path(exe: str) -> str:
    return exe + ".original"


# --------------------------------------------------------------------------
def cmd_info(args) -> int:
    data = _read(args.exe)
    pe = PE(data)
    print("file:        %s" % os.path.abspath(args.exe))
    print("size:        %d bytes" % len(data))
    print("sha256:      %s" % sha256(data))
    print("machine:     0x%04x  sections: %d  timestamp: %d"
          % (pe.machine, pe.number_of_sections, pe.timestamp))
    print("image base:  0x%08x  entry: 0x%08x" % (pe.image_base, pe.address_of_entry_point))
    print("align:       section 0x%x / file 0x%x" % (pe.section_alignment, pe.file_alignment))
    print("size image:  0x%08x  headers: 0x%08x" % (pe.size_of_image, pe.size_of_headers))
    print("characteristics: 0x%04x%s"
          % (pe.characteristics,
             "  (LARGE_ADDRESS_AWARE)" if pe.characteristics & 0x20 else ""))
    print("sections:")
    for s in pe.sections:
        print("  %-8s va 0x%08x vsz 0x%08x raw 0x%08x rawsz 0x%08x chars 0x%08x"
              % (s.name, s.virtual_address, s.virtual_size,
                 s.pointer_to_raw_data, s.size_of_raw_data, s.characteristics))
    print("data directories:")
    for i in range(pe.number_of_rva_and_sizes):
        rva, size = pe.directory(i)
        if rva or size:
            name = DIRECTORY_NAMES[i] if i < len(DIRECTORY_NAMES) else str(i)
            print("  %-12s rva 0x%08x size 0x%08x" % (name, rva, size))
    imports = list(pe.iter_imports())
    if imports:
        print("imports:")
        for entry in imports:
            print("  %s (%d)" % (entry["dll"], len(entry["functions"])))
    exports = list(pe.iter_exports())
    if exports:
        print("exports:")
        for entry in exports:
            print("  %-40s rva 0x%08x ordinal %d"
                  % (entry["name"], entry["rva"], entry["ordinal"]))
    return 0


def cmd_symbols(args) -> int:
    symbols = Symbols.load(args.table)
    if len(symbols) == 0:
        print("no symbol table found (run scripts/pdb-functions.py first)", file=sys.stderr)
        return 2
    if args.query:
        va = symbols.resolve(args.query)
        if va is None:
            print("not found: %s" % args.query, file=sys.stderr)
            return 1
        print("0x%08x  %s" % (va, symbols.name_of(va)))
        return 0
    for row in symbols.search(args.search, args.limit):
        print("0x%08x  %s" % (int(row["va"], 0), row["demangled"] or row["mangled"]))
    return 0


def cmd_diff(args) -> int:
    source = _read(args.source)
    target = _read(args.target)
    delta = create_delta(source, target, variant=args.variant, version=args.version)
    save_delta(delta, args.output)
    print("wrote %s (%d changes, %d appended bytes, target %s)"
          % (args.output, len(delta["changes"]), len(delta["append"]) // 2,
             delta["target_sha256"]))
    return 0


def cmd_build(args) -> int:
    spec = load_spec(args.spec)
    source = _read(args.source)
    symbols = Symbols.load(args.table)
    delta = build_from_spec(spec, source, symbols=symbols)
    save_delta(delta, args.output)
    print("wrote %s (%d changes, %d appended bytes, target %s)"
          % (args.output, len(delta["changes"]), len(delta["append"]) // 2,
             delta["target_sha256"]))
    return 0


def cmd_apply(args) -> int:
    delta = load_delta(args.delta)
    exe = args.exe
    if not os.path.isfile(exe):
        print("error: %s does not exist" % exe, file=sys.stderr)
        return 2
    current = _read(exe)
    backup = args.backup or _backup_path(exe)
    have = sha256(current)

    if have == delta["target_sha256"] and not args.force:
        print("already patched (%s)" % delta.get("version", "?"))
        return 0

    if have == delta["source_sha256"]:
        source = current
        if not os.path.isfile(backup):
            shutil.copy2(exe, backup)
            print("backed up original to %s" % backup)
    elif os.path.isfile(backup) and sha256(_read(backup)) == delta["source_sha256"]:
        source = _read(backup)
        print("patching from %s" % backup)
    else:
        print("error: %s is neither the expected original (%s) nor the target; "
              "refusing to patch" % (exe, delta["source_sha256"]), file=sys.stderr)
        return 1

    patched = apply_delta(source, delta)
    out = args.out or exe
    tmp = out + ".new"
    _write(tmp, patched)
    os.replace(tmp, out)
    print("wrote %s (%s, %s)" % (out, delta.get("version", "?"),
                                 delta.get("variant", "")))
    return 0


def cmd_revert(args) -> int:
    exe = args.exe
    backup = args.backup or _backup_path(exe)
    if not os.path.isfile(backup):
        print("error: no backup at %s" % backup, file=sys.stderr)
        return 1
    shutil.copy2(backup, exe)
    print("restored %s from %s" % (exe, backup))
    return 0


def cmd_verify(args) -> int:
    delta = load_delta(args.delta)
    data = _read(args.exe)
    have = sha256(data)
    if have == delta["source_sha256"]:
        print("source (%s)" % have)
        return 0
    if have == delta["target_sha256"]:
        print("target (%s)" % have)
        return 0
    print("unknown (%s)" % have, file=sys.stderr)
    return 1


def _resolve_new_import(spec: str):
    dll, _, funcs = spec.partition(":")
    fns = [f for f in funcs.split(",") if f]
    return (dll, fns)


def cmd_inject(args) -> int:
    """One-shot PE injection without a spec file (convenience)."""
    pe = PE(_read(args.exe))
    imports = [_resolve_new_import(i) for i in args.import_dll]
    exports = []
    for e in args.export:
        name, _, value = e.partition("=")
        exports.append({"name": name, "kind": "data_u32", "value": int(value, 0)})
    report = inject.inject(pe, imports=imports, exports=exports,
                           module_name=args.module_name,
                           large_address_aware=args.large_address_aware)
    _write(args.output, pe.to_bytes())
    print("wrote %s" % args.output)
    if report["sections"]:
        for name, rva in report["sections"].items():
            print("  %s at RVA 0x%08x" % (name, rva))
    for rva in report["iat_rvas"]:
        print("  IAT slot RVA 0x%08x (VA 0x%08x)" % (rva, pe.image_base + rva))
    return 0


# --------------------------------------------------------------------------
def cmd_package(args) -> int:
    out = make_package(args.delta, args.output, name=args.name,
                       version=args.version, variant=args.variant,
                       game=args.game, dll=args.dll, ini=args.ini)
    print("wrote %s" % out)
    return 0


# --------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="cdetool",
                                description="Children of a Dead Earth patch toolchain")
    p.add_argument("--version", action="version", version="cdetool " + __version__)
    sub = p.add_subparsers(dest="command", required=True)

    q = sub.add_parser("info", help="inspect a PE image")
    q.add_argument("exe")
    q.set_defaults(func=cmd_info)

    q = sub.add_parser("symbols", help="resolve/search PDB-derived symbols")
    q.add_argument("query", nargs="?")
    q.add_argument("--search", default=".", help="regex when no exact query")
    q.add_argument("--limit", type=int, default=40)
    q.add_argument("--table", default=None)
    q.set_defaults(func=cmd_symbols)

    q = sub.add_parser("diff", help="build a delta from two executables")
    q.add_argument("source")
    q.add_argument("target")
    q.add_argument("-o", "--output", required=True)
    q.add_argument("--variant")
    q.add_argument("--version", dest="version")
    q.set_defaults(func=cmd_diff)

    q = sub.add_parser("build", help="build a delta from a patch spec")
    q.add_argument("spec")
    q.add_argument("-s", "--source", required=True)
    q.add_argument("-o", "--output", required=True)
    q.add_argument("--table", default=None)
    q.set_defaults(func=cmd_build)

    q = sub.add_parser("apply", help="apply a delta to an executable")
    q.add_argument("exe")
    q.add_argument("delta")
    q.add_argument("-o", "--out", default=None)
    q.add_argument("--backup", default=None)
    q.add_argument("--force", action="store_true")
    q.set_defaults(func=cmd_apply)

    q = sub.add_parser("revert", help="restore an executable from its backup")
    q.add_argument("exe")
    q.add_argument("--backup", default=None)
    q.set_defaults(func=cmd_revert)

    q = sub.add_parser("verify", help="check an executable against a delta")
    q.add_argument("exe")
    q.add_argument("delta")
    q.set_defaults(func=cmd_verify)

    q = sub.add_parser("inject", help="append .cdei/.cdep sections (no delta)")
    q.add_argument("exe")
    q.add_argument("-o", "--output", required=True)
    q.add_argument("--import", dest="import_dll", action="append", default=[],
                   metavar="DLL:FN,FN")
    q.add_argument("--export", action="append", default=[], metavar="NAME=VALUE")
    q.add_argument("--module-name", default="CDE.exe")
    q.add_argument("--large-address-aware", action="store_true")
    q.set_defaults(func=cmd_inject)

    q = sub.add_parser("package", help="bundle a delta into a distributable zip")
    q.add_argument("delta")
    q.add_argument("-o", "--output", required=True)
    q.add_argument("--name", required=True)
    q.add_argument("--version", required=True)
    q.add_argument("--variant", default=None)
    q.add_argument("--game", default="Children of a Dead Earth")
    q.add_argument("--dll", default=None, help="helper DLL to include")
    q.add_argument("--ini", default=None, help="settings ini to include")
    q.set_defaults(func=cmd_package)

    return p


def main(argv: Optional[list] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (PEError, DeltaError, SpecError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
