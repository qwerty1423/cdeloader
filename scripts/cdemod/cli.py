"""Command-line entry point for the mod tooling: ``python3 -m cdemod``.

    cdemod lint MODDIR [MODDIR ...] [--game GAMEDIR] [--save SAVEDIR]
    cdemod new NAME [--out DIR]
    cdemod parse FILE            # dump the parsed entries (debugging)
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import List, Optional, Tuple

from . import datum as dm
from .lint import format_report, index_directory, lint_mods
from .scaffold import create_mod

GAME_DATA_SUBDIR = os.path.join("Resources", "Data")


def _stock_index(game: Optional[str]):
    if not game:
        return None
    root = os.path.join(game, "Resources")
    index, problems = index_directory(root, "stock", "Data")
    if not index.entries:
        print("warning: no stock data found under %s" % root, file=sys.stderr)
    return index


def _local_mod_dirs(save: Optional[str]) -> List[Tuple[str, str]]:
    out = []
    if save:
        mods = os.path.join(save, "Mods")
        if os.path.isdir(mods):
            for name in sorted(os.listdir(mods)):
                path = os.path.join(mods, name)
                if os.path.isdir(path):
                    out.append((path, "local:" + name))
    return out


def _workshop_mod_dirs(workshop: Optional[str]) -> List[Tuple[str, str]]:
    out = []
    if workshop and os.path.isdir(workshop):
        for item in sorted(os.listdir(workshop)):
            path = os.path.join(workshop, item)
            if os.path.isdir(path):
                out.append((path, "workshop:" + item))
    return out


def cmd_lint(args) -> int:
    stock = _stock_index(args.game)
    mods: List[Tuple[str, str]] = []
    for d in args.mods:
        mods.append((d, os.path.basename(os.path.normpath(d))))
    mods += _local_mod_dirs(args.save)
    mods += _workshop_mod_dirs(args.workshop)
    if not mods:
        print("no mod folders to lint", file=sys.stderr)
        return 2
    problems = lint_mods(mods, stock)
    report = format_report(problems)
    sys.stdout.write(report)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as fh:
            fh.write(report)
        print("report written to %s" % args.report)
    return 1 if any(p.severity == "error" for p in problems) else 0


def cmd_new(args) -> int:
    root = create_mod(args.name, args.out, example=not args.bare)
    print("created %s" % root)
    return 0


def cmd_parse(args) -> int:
    datums, problems = dm.parse_file(args.file)
    for d in datums:
        print("%s %s (%d properties)" % (d.type, d.name, len(d.properties)))
        for p in d.properties:
            print("    %s\t%s" % ("\t" * p.indent + p.key, "\t".join(p.values)))
    for p in problems:
        print(p, file=sys.stderr)
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="cdemod", description="CDE mod tooling")
    sub = p.add_subparsers(dest="command", required=True)

    q = sub.add_parser("lint", help="validate mod folders")
    q.add_argument("mods", nargs="*")
    q.add_argument("--game", default=os.environ.get("CDE_GAME"))
    q.add_argument("--save", default=None,
                   help="save data folder containing Mods/ (%%APPDATA%%\\CDE)")
    q.add_argument("--workshop", default=None,
                   help="workshop content folder (…/content/476530)")
    q.add_argument("--report", default=None)
    q.set_defaults(func=cmd_lint)

    q = sub.add_parser("new", help="scaffold a new mod")
    q.add_argument("name")
    q.add_argument("--out", default=".")
    q.add_argument("--bare", action="store_true", help="no example data file")
    q.set_defaults(func=cmd_new)

    q = sub.add_parser("parse", help="dump parsed entries of a file")
    q.add_argument("file")
    q.set_defaults(func=cmd_parse)

    return p


def main(argv: Optional[list] = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
