"""PE section injection: the machinery the community patch uses.

Two sections are appended to the executable:

``.cdei``  a *relocated* import table.  The original import descriptor array is
           copied verbatim, a descriptor for the helper DLL is appended, and the
           new ILT/IAT/hint-name/DLL-name blobs live at the end of the section.
           ``DataDirectory[IMPORT]`` is repointed there.  Because the loader
           reads imports from the new table, the helper DLL is loaded (and its
           ``DllMain`` runs) before the game's entry point.

``.cdep``  an export directory.  The community patch exports two DWORD globals
           (``NvOptimusEnablement`` and ``AmdPowerXpressRequestHighPerformance``)
           which is the standard way to ask a laptop GPU driver for the
           high-performance adapter.  Function RVAs (``kind="rva"``) are also
           supported so a patch can export a thunk that the helper DLL calls.

The exact byte layout reproduced here is verified against the shipped
``CDE-complete.delta.json`` / ``CDE-fixes-only.delta.json`` (see
``tests/test_toolchain.py``).
"""

from __future__ import annotations

import struct
from typing import Dict, Iterable, List, Sequence, Tuple

from .pe import (
    DIR_EXPORT, DIR_IMPORT, PE,
    SCN_CNT_INITIALIZED_DATA, SCN_MEM_READ, SCN_MEM_WRITE,
)

_CDEI_CHARACTERISTICS = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE
_CDEP_CHARACTERISTICS = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ


class InjectionError(Exception):
    pass


def _read_import_descriptors(pe: PE) -> List[bytes]:
    """Return the raw 20-byte import descriptors, excluding the terminator."""
    imp_rva, _ = pe.directory(DIR_IMPORT)
    if not imp_rva:
        return []
    off = pe.rva_to_offset(imp_rva)
    if off is None:
        raise InjectionError("import directory RVA 0x%x has no file data" % imp_rva)
    descriptors = []
    while True:
        raw = bytes(pe.data[off:off + 20])
        oft, tds, fc, name, ft = struct.unpack("<IIIII", raw)
        if not (oft or tds or fc or name or ft):
            break
        descriptors.append(raw)
        off += 20
    return descriptors


def build_import_section(pe: PE, new_imports: Sequence[Tuple[str, Sequence[str]]],
                         section_rva: int) -> Tuple[bytes, int, List[int]]:
    """Build the ``.cdei`` section content.

    ``new_imports`` is a sequence of ``(dll_name, [function, ...])``.  Returns
    ``(content, descriptor_array_size, iat_rvas)`` where ``iat_rvas`` are the
    section RVAs of each new function's IAT slot — the addresses an injected
    helper can read to find the real function pointers (and which the loader
    patches on startup).
    """
    original = _read_import_descriptors(pe)

    buf = bytearray()
    buf += b"".join(original)

    descriptor_slots: List[int] = []
    for _dll, _funcs in new_imports:
        descriptor_slots.append(len(buf))
        buf += b"\0" * 20
    descriptor_array_end = len(buf) + 20  # + terminator
    buf += b"\0" * 20

    iat_rvas: List[int] = []
    for slot, (dll, funcs) in zip(descriptor_slots, new_imports):
        funcs = list(funcs)

        ilt_off = len(buf)
        buf += b"\0" * (4 * (len(funcs) + 1))
        iat_off = len(buf)
        buf += b"\0" * (4 * (len(funcs) + 1))

        name_blob_rvas = []
        for fn in funcs:
            name_blob_rvas.append(section_rva + len(buf))
            buf += struct.pack("<H", 0) + fn.encode("latin1") + b"\0"

        dll_off = len(buf)
        buf += dll.encode("latin1") + b"\0"

        for i, hn_rva in enumerate(name_blob_rvas):
            struct.pack_into("<I", buf, ilt_off + i * 4, hn_rva)
            struct.pack_into("<I", buf, iat_off + i * 4, hn_rva)
        # terminator entries are already zero

        struct.pack_into(
            "<IIIII", buf, slot,
            section_rva + ilt_off, 0, 0, section_rva + dll_off, section_rva + iat_off)
        iat_rvas.extend(section_rva + iat_off + i * 4 for i in range(len(funcs)))

    return bytes(buf), descriptor_array_end, iat_rvas


def build_export_section(pe: PE, symbols: Sequence[Dict],
                         section_rva: int, module_name: str) -> Tuple[bytes, int]:
    """Build the ``.cdep`` section content.

    Each symbol is ``{"name": str, "kind": "data_u32"|"rva", "value": int}``.
    ``data_u32`` allocates a 4-byte global in the section; ``rva`` points at an
    existing address (a VA).  Returns ``(content, directory_size)``.
    """
    n = len(symbols)
    if n == 0:
        raise InjectionError("cannot build an export section with no symbols")

    dir_off = 0
    funcs_off = dir_off + 40
    names_off = funcs_off + n * 4
    ordinals_off = names_off + n * 4
    data_off = ordinals_off + n * 2

    buf = bytearray(data_off)

    data_rvas: Dict[int, int] = {}
    for i, sym in enumerate(symbols):
        kind = sym.get("kind", "data_u32")
        if kind == "data_u32":
            data_rvas[i] = section_rva + len(buf)
            buf += struct.pack("<I", sym.get("value", 0) & 0xFFFFFFFF)
        elif kind in ("rva", "va"):
            data_rvas[i] = sym["value"] - pe.image_base
        else:
            raise InjectionError("unknown export kind %r" % kind)

    module_off = len(buf)
    buf += module_name.encode("latin1") + b"\0"

    name_offsets: List[int] = []
    for sym in symbols:
        name_offsets.append(len(buf))
        buf += sym["name"].encode("latin1") + b"\0"

    struct.pack_into("<IIHHIIIIIII", buf, dir_off,
                     0, 0, 0, 0,                      # characteristics, timestamp, major, minor
                     section_rva + module_off,        # Name
                     1,                               # Base
                     n, n,                            # NumberOfFunctions, NumberOfNames
                     section_rva + funcs_off,
                     section_rva + names_off,
                     section_rva + ordinals_off)

    for i in range(n):
        struct.pack_into("<I", buf, funcs_off + i * 4, data_rvas[i])
        struct.pack_into("<I", buf, names_off + i * 4, section_rva + name_offsets[i])
        struct.pack_into("<H", buf, ordinals_off + i * 2, i)

    return bytes(buf), len(buf)


def inject(pe: PE, *, imports: Sequence[Tuple[str, Sequence[str]]] = (),
           exports: Sequence[Dict] = (), module_name: str = "CDE.exe",
           large_address_aware: bool = False) -> Dict:
    """Append the ``.cdei`` / ``.cdep`` sections to ``pe`` in place.

    Returns a small report (section RVAs and IAT RVAs) for the caller.
    """
    report = {"iat_rvas": [], "sections": {}}

    if large_address_aware:
        pe.set_large_address_aware(True)

    if imports:
        rva = pe.next_section_rva()
        content, desc_size, iat_rvas = build_import_section(pe, imports, rva)
        sec = pe.add_section(".cdei", content, _CDEI_CHARACTERISTICS,
                             virtual_address=rva)
        pe.set_directory(DIR_IMPORT, sec.virtual_address, desc_size)
        report["iat_rvas"] = iat_rvas
        report["sections"][".cdei"] = sec.virtual_address

    if exports:
        rva = pe.next_section_rva()
        content, size = build_export_section(pe, exports, rva, module_name)
        sec = pe.add_section(".cdep", content, _CDEP_CHARACTERISTICS,
                             virtual_address=rva)
        pe.set_directory(DIR_EXPORT, sec.virtual_address, size)
        report["sections"][".cdep"] = sec.virtual_address

    return report
