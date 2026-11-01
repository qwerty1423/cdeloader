"""Patch project specs: the authoring front end.

A *spec* is a small JSON file that describes a patch in terms of intent, not
raw file offsets.  ``cdetool build`` turns a spec plus the unmodified
executable into a ``*.delta.json``.

Example::

    {
      "format": "cde-patch/1",
      "name": "CDE Community Patch",
      "version": "0.1.0-test5",
      "variant": "fixes-only",
      "module_name": "CDE.exe",
      "large_address_aware": true,
      "imports": [{"dll": "cdepatch.dll", "functions": ["CdeInit"]}],
      "exports": [
        {"name": "AmdPowerXpressRequestHighPerformance", "kind": "data_u32", "value": 1},
        {"name": "NvOptimusEnablement", "kind": "data_u32", "value": 1}
      ],
      "byte_patches": [
        {"va": "0x00504dd6", "bytes": "6690", "expect": "0f1f440000",
         "comment": "disable the per-frame list rebuild"}
      ]
    }

Each byte patch may address the image by ``offset`` (file offset), ``va``
(virtual address) or ``symbol`` (+ optional ``offset``).  ``expect`` is an
optional hex string that must match the current bytes, which catches a spec
aimed at the wrong game build.  Instead of ``bytes`` you may use ``nop`` (a
count of 0x90 bytes) or ``int3`` (a count of 0xCC bytes).
"""

from __future__ import annotations

import json
import struct
from typing import Dict, Optional

from . import inject
from .delta import create_delta
from .pe import PE, PEError, sha256
from .symbols import Symbols

SPEC_FORMAT = "cde-patch/1"


class SpecError(Exception):
    pass


def _as_int(value, what: str) -> int:
    try:
        return int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError):
        raise SpecError("cannot read %s as an integer: %r" % (what, value))


def _patch_payload(patch: dict) -> bytes:
    keys = [k for k in ("bytes", "nop", "int3") if k in patch]
    if len(keys) != 1:
        raise SpecError("a byte_patch needs exactly one of bytes/nop/int3")
    key = keys[0]
    if key == "bytes":
        try:
            return bytes.fromhex(patch["bytes"])
        except ValueError:
            raise SpecError("bytes is not valid hex: %r" % patch["bytes"])
    if key == "nop":
        return b"\x90" * _as_int(patch["nop"], "nop")
    return b"\xcc" * _as_int(patch["int3"], "int3")


def _patch_offset(pe: PE, patch: dict, symbols: Symbols) -> int:
    if "symbol" in patch:
        if len(symbols) == 0:
            raise SpecError("symbol patch needs decomp/symbols/functions.tsv")
        va = symbols.resolve(patch["symbol"])
        if va is None:
            raise SpecError("symbol not found: %r" % patch["symbol"])
        va += _as_int(patch.get("offset", 0), "offset")
        off = pe.rva_to_offset(va - pe.image_base)
        if off is None:
            raise SpecError("symbol VA 0x%x is not backed by file data" % va)
        return off
    if "va" in patch:
        va = _as_int(patch["va"], "va")
        off = pe.rva_to_offset(va - pe.image_base)
        if off is None:
            raise SpecError("VA 0x%x is not backed by file data" % va)
        return off
    if "offset" in patch:
        return _as_int(patch["offset"], "offset")
    raise SpecError("a byte_patch needs offset, va or symbol")


def build_from_spec(spec: dict, source: bytes, *,
                    symbols: Optional[Symbols] = None) -> dict:
    """Build a delta from a patch spec and the unmodified executable."""
    if symbols is None:
        symbols = Symbols.load()
    fmt = spec.get("format", SPEC_FORMAT)
    if fmt != SPEC_FORMAT:
        raise SpecError("unsupported spec format %r" % fmt)

    expected = spec.get("source") or {}
    if expected.get("sha256") and sha256(source) != expected["sha256"]:
        raise SpecError("source executable does not match spec source.sha256")
    if expected.get("size") and len(source) != int(expected["size"]):
        raise SpecError("source executable size does not match spec source.size")

    pe = PE(source)

    imports = [(entry["dll"], entry.get("functions", []))
               for entry in spec.get("imports", [])]
    exports = list(spec.get("exports", []))
    inject.inject(
        pe,
        imports=imports,
        exports=exports,
        module_name=spec.get("module_name", "CDE.exe"),
        large_address_aware=bool(spec.get("large_address_aware", False)),
    )

    for patch in spec.get("byte_patches", []):
        offset = _patch_offset(pe, patch, symbols)
        payload = _patch_payload(patch)
        if "expect" in patch:
            want = bytes.fromhex(patch["expect"])
            have = bytes(pe.data[offset:offset + len(want)])
            if have != want:
                raise SpecError(
                    "expect mismatch at offset 0x%x (%s): wanted %s, found %s"
                    % (offset, patch.get("comment", ""), want.hex(), have.hex()))
        pe.data[offset:offset + len(payload)] = payload

    return create_delta(
        source, pe.to_bytes(),
        variant=spec.get("variant"),
        version=spec.get("version"),
        extra={"name": spec["name"]} if spec.get("name") else None,
    )


def load_spec(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)
