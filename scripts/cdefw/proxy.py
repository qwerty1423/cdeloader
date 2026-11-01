"""Proxy-mode install: load the mod loader without touching CDE.exe.

The framework normally injects an import of ``cdeloader.dll`` into the exe.  The
proxy path instead ships a local ``vcruntime140.dll`` whose exports all forward
to ``vcruntime140_orig.dll`` (the real runtime, renamed once) and whose DllMain
loads the loader.  ``CDE.exe`` stays byte-identical, so Steam's "verify
integrity" cannot revert it and ReShade / the community patch keep their slots.

Verified on the retail game (notes/FINDINGS.md F60).
"""

from __future__ import annotations

import os
import shutil
from typing import List, Optional, Tuple

PROXY_NAME = "vcruntime140.dll"
ORIGINAL_NAME = "vcruntime140_orig.dll"
MARKER = b"vcruntime140_orig"


class ProxyError(Exception):
    pass


def default_proxy_dll(repo_root: str) -> str:
    return os.path.join(repo_root, "build", "proxy", PROXY_NAME)



def is_proxy(path: str) -> bool:
    """True when *path* is our forwarding proxy (not the real runtime)."""
    try:
        with open(path, "rb") as fh:
            return MARKER in fh.read(4096 * 1024)
    except OSError:
        return False


def install(game_dir: str, *, proxy_dll: str, loader: str,
            symbols: Optional[str] = None, ini: Optional[str] = None,
            stock_exe: Optional[str] = None,
            mod_files: Optional[List[Tuple[str, str]]] = None) -> dict:
    """Install proxy mode into *game_dir*; never modifies CDE.exe.

    Returns a report with the file operations, so the CLI/tests can print or
    assert them.  Idempotent: a second run leaves the renamed original alone.
    """
    if not os.path.isdir(game_dir):
        raise ProxyError("not a directory: %s" % game_dir)
    if not os.path.isfile(proxy_dll):
        raise ProxyError("proxy DLL not built: %s (run scripts/build-proxy.sh)"
                         % proxy_dll)
    if not os.path.isfile(loader):
        raise ProxyError("loader not built: %s" % loader)

    target = os.path.join(game_dir, PROXY_NAME)
    original = os.path.join(game_dir, ORIGINAL_NAME)
    actions = []

    if os.path.isfile(target) and not is_proxy(target):
        if os.path.isfile(original):
            raise ProxyError(
                "%s exists and is not our proxy, and %s already exists; "
                "refusing to guess which runtime to keep"
                % (target, original))
        os.replace(target, original)
        actions.append(("renamed", target, original))

    shutil.copyfile(proxy_dll, target)
    actions.append(("copied", proxy_dll, target))

    actions += _copy_runtime_files(game_dir, loader, symbols, ini, stock_exe,
                                   mod_files)
    return {"game_dir": game_dir, "actions": actions}


def _copy_runtime_files(game_dir: str, loader: str, symbols: Optional[str],
                        ini: Optional[str], stock_exe: Optional[str],
                        mod_files: Optional[List[Tuple[str, str]]]) -> list:
    actions = []
    copies = [("cdeloader.dll", loader)]
    if symbols and os.path.isfile(symbols):
        copies.append(("cdeloader.symbols", symbols))
    else:
        copies.append(("cdeloader.symbols",
                       os.path.join(os.path.dirname(loader), "cdeloader.symbols")))
    if ini and os.path.isfile(ini):
        copies.append(("cdeloader.ini", ini))
    else:
        copies.append(("cdeloader.ini",
                       os.path.join(os.path.dirname(loader), "cdeloader.ini")))
    for rel, src in copies:
        if not os.path.isfile(src):
            continue
        dst = os.path.join(game_dir, rel)
        shutil.copyfile(src, dst)
        actions.append(("copied", src, dst))

    # A community-patched exe differs from the stock hash; the loader verifies
    # game VAs against CDE.exe.original, so keep a stock copy beside it.
    stock_target = os.path.join(game_dir, "CDE.exe.original")
    if stock_exe and os.path.isfile(stock_exe) and not os.path.exists(stock_target):
        shutil.copyfile(stock_exe, stock_target)
        actions.append(("copied", stock_exe, stock_target))

    for src, rel in (mod_files or []):
        dst = os.path.join(game_dir, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst) or game_dir, exist_ok=True)
        shutil.copyfile(src, dst)
        actions.append(("copied", src, dst))
    return actions


def restore(game_dir: str) -> dict:
    """Undo proxy mode: put the renamed runtime back and drop our proxy."""
    target = os.path.join(game_dir, PROXY_NAME)
    original = os.path.join(game_dir, ORIGINAL_NAME)
    if not os.path.isfile(original):
        raise ProxyError("no %s in %s; proxy mode was not installed"
                         % (ORIGINAL_NAME, game_dir))
    os.replace(original, target)
    return {"game_dir": game_dir, "restored": target}
