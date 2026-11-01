"""The ``*.delta.json`` format used by the CDE Community Patch.

A delta is a self-contained description of how to turn one executable into
another, with enough hashes to refuse to patch the wrong file:

``source_sha256`` / ``source_size``
    Identity of the unmodified executable the patch expects.
``target_sha256`` / ``target_size``
    Identity of the result.
``changes``
    Sparse list of ``{"offset": int, "bytes": "<hex>"}`` over the first
    ``source_size`` bytes.  Consecutive differing bytes are coalesced into one
    entry.
``append``
    Hex of every byte added past ``source_size`` (the raw data of the injected
    sections).  Zero padding is included verbatim.

``variant`` / ``version`` are optional metadata.

The installer applies a delta by copying ``source``, overwriting each change,
and pasting ``append`` at ``source_size``; it then checks ``target_sha256``.
This module is the authority for both creating and applying deltas.
"""

from __future__ import annotations

import json
from typing import Optional

from .pe import sha256

_FORMAT = "cde-delta/1"


class DeltaError(Exception):
    pass


def _coalesce_changes(source: bytes, target: bytes, limit: int):
    changes = []
    i = 0
    while i < limit:
        if source[i] == target[i]:
            i += 1
            continue
        j = i
        while j < limit and source[j] != target[j]:
            j += 1
        changes.append({"offset": i, "bytes": target[i:j].hex()})
        i = j
    return changes


def create_delta(source: bytes, target: bytes, *, variant: Optional[str] = None,
                 version: Optional[str] = None,
                 extra: Optional[dict] = None) -> dict:
    """Build a delta that turns ``source`` into ``target``."""
    if len(target) < len(source):
        raise DeltaError("target is shorter than source; the format only appends")
    limit = len(source)
    delta = {
        "format": _FORMAT,
        "source_sha256": sha256(source),
        "source_size": len(source),
        "target_sha256": sha256(target),
        "target_size": len(target),
        "changes": _coalesce_changes(source, target, limit),
        "append": target[limit:].hex(),
    }
    if variant is not None:
        delta["variant"] = variant
    if version is not None:
        delta["version"] = version
    if extra:
        delta.update(extra)
    # A delta we just made must round-trip.
    apply_delta(source, delta)
    return delta


def apply_delta(source: bytes, delta: dict, *, verify_source: bool = True) -> bytes:
    """Apply ``delta`` to ``source`` and return the patched bytes.

    Raises :class:`DeltaError` if the source hash does not match (when
    ``verify_source``) or if the result does not match ``target_sha256``.
    """
    src_size = int(delta["source_size"])
    tgt_size = int(delta["target_size"])
    if verify_source and sha256(source) != delta["source_sha256"]:
        raise DeltaError("source does not match source_sha256 (wrong game build?)")

    patched = bytearray(tgt_size)
    patched[:src_size] = source[:src_size]
    for change in delta["changes"]:
        offset = int(change["offset"])
        blob = bytes.fromhex(change["bytes"])
        if offset < 0 or offset + len(blob) > tgt_size:
            raise DeltaError("change at offset %d is out of range" % offset)
        patched[offset:offset + len(blob)] = blob
    tail = bytes.fromhex(delta.get("append", ""))
    if tail:
        if src_size + len(tail) > tgt_size:
            raise DeltaError("append overruns target_size")
        patched[src_size:src_size + len(tail)] = tail

    result = bytes(patched)
    if delta.get("target_sha256") and sha256(result) != delta["target_sha256"]:
        raise DeltaError("patched result does not match target_sha256")
    return result


def load_delta(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def save_delta(delta: dict, path: str) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(delta, fh, indent=1)
        fh.write("\n")
