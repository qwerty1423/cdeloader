"""Bundle a delta (and optional helper DLL / ini) into a distributable patch.

Produces a zip with self-contained ``installer.py`` plus ``install``/``uninstall``
wrappers for Windows and POSIX, the ``*.delta.json``, and the helper files.  The
installer is standalone (Python 3 standard library only) so the package can be
handed to a player without this repository.
"""

from __future__ import annotations

import json
import os
import shutil
import tempfile
import time
import zipfile
from typing import List, Optional, Tuple

from .delta import load_delta

TEMPLATES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "templates")


class PackageError(Exception):
    pass


# Archive member timestamps.  The reproducible-builds convention is
# SOURCE_DATE_EPOCH when the environment sets it, and the file's own mtime
# otherwise; forcing the 1980 ZIP epoch is not required and some tools dislike
# it (see reproducible-builds.org/docs/timestamps).  What has to be byte-exact is
# the compiled output, which scripts/repro-check.sh compares separately; the
# package check compares archive *contents*, not archive bytes.
def _zip_time(src: str):
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch:
        try:
            return time.localtime(int(epoch))[:6]
        except (ValueError, OSError):
            pass
    try:
        return time.localtime(os.stat(src).st_mtime)[:6]
    except OSError:
        return (1980, 1, 1, 0, 0, 0)


def _write_zip(out_zip: str, files: List[Tuple[str, str]]) -> None:
    """Write ``(source_path, archive_path)`` pairs in sorted archive order.

    Members carry a normalized Unix mode and no host-specific metadata; the
    timestamp is SOURCE_DATE_EPOCH when set, else the file's own mtime.
    """
    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED) as zf:
        for src, arc in sorted(files, key=lambda item: item[1]):
            mode = 0o755 if arc.endswith((".sh", ".py")) else 0o644
            info = zipfile.ZipInfo(arc, date_time=_zip_time(src))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3  # Unix, so external_attr is a mode
            info.external_attr = mode << 16
            with open(src, "rb") as fh:
                zf.writestr(info, fh.read())


def _render(template: str, values: dict) -> str:
    with open(os.path.join(TEMPLATES, template), "r", encoding="utf-8") as fh:
        text = fh.read()
    for key, value in values.items():
        text = text.replace("@@%s@@" % key, str(value))
    return text


def make_package(delta_path: str, out_zip: str, *, name: str, version: str,
                 variant: Optional[str] = None, game: str = "Children of a Dead Earth",
                 dll: Optional[str] = None, ini: Optional[str] = None,
                 extra_files: Optional[List[Tuple[str, str]]] = None,
                 zip_only_files: Optional[List[Tuple[str, str]]] = None,
                 hashes: Optional[dict] = None) -> str:
    """Bundle a delta + optional helper files.

    ``extra_files`` is a list of ``(source_path, dest_rel_path)`` copied into the
    package and installed next to ``CDE.exe`` (e.g. a symbol table, or a
    ``mods/<mod>/plugin.dll`` tree for the framework package).
    ``zip_only_files`` are copied into the package but not installed (e.g. the
    compat deltas used by the installer).
    """
    delta = load_delta(delta_path)
    variant = variant or delta.get("variant") or "custom"
    delta_name = "CDE-%s.delta.json" % variant
    extra_files = list(extra_files or [])
    zip_only_files = list(zip_only_files or [])

    values = {
        "HASHES": ("CDE.exe (source) SHA-256: @@SOURCE_SHA@@\n"
                   "CDE.exe (patched) SHA-256: @@TARGET_SHA@@"),
        "NAME": name,
        "VERSION": version,
        "VARIANT": variant,
        "GAME": game,
        "DELTA": delta_name,
        "DLL": os.path.basename(dll) if dll else "",
        "INI": os.path.basename(ini) if ini else "",
        "SOURCE_SHA": delta["source_sha256"],
        "TARGET_SHA": delta["target_sha256"],
        "EXTRA_FILES": json.dumps([dest for _, dest in extra_files]),
        "EXTRA_PS": ",".join("'%s'" % dest for _, dest in extra_files),
    }

    # The hash block is rendered from the delta by default; a package can pass
    # its own (the framework lists accepted game builds and the runtime it
    # replaces).  Resolve the nested placeholders before the override.
    values["HASHES"] = (values["HASHES"]
                        .replace("@@SOURCE_SHA@@", values["SOURCE_SHA"])
                        .replace("@@TARGET_SHA@@", values["TARGET_SHA"]))
    if hashes:
        values.update(hashes)

    tmp = tempfile.mkdtemp(prefix="cdepkg-")
    try:
        files = {
            "installer.py": _render("installer.py", values),
            "install.sh": _render("install.sh", values),
            "install.ps1": _render("install.ps1", values),
            "install.cmd": _render("install.cmd", values),
            "install-delta.cmd": _render("install-delta.cmd", values),
            "install-delta.sh": _render("install-delta.sh", values),
            "uninstall.sh": _render("uninstall.sh", values),
            "uninstall.cmd": _render("uninstall.cmd", values),
            "README.txt": _render("README.txt", values),
        }
        for name_, text in files.items():
            path = os.path.join(tmp, name_)
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
            if name_.endswith(".sh"):
                os.chmod(path, 0o755)

        shutil.copyfile(delta_path, os.path.join(tmp, delta_name))
        if dll:
            shutil.copyfile(dll, os.path.join(tmp, os.path.basename(dll)))
        if ini:
            shutil.copyfile(ini, os.path.join(tmp, os.path.basename(ini)))
        for src, dest in extra_files:
            dst = os.path.join(tmp, dest.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst) or tmp, exist_ok=True)
            shutil.copyfile(src, dst)
        for src, dest in zip_only_files:
            dst = os.path.join(tmp, dest.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst) or tmp, exist_ok=True)
            shutil.copyfile(src, dst)

        out_zip = os.path.abspath(out_zip)
        members = []
        for root, _dirs, names in os.walk(tmp):
            for fn in names:
                full = os.path.join(root, fn)
                arc = os.path.relpath(full, tmp).replace(os.sep, "/")
                members.append((full, arc))
        _write_zip(out_zip, members)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return out_zip
