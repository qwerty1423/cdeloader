"""Minimal PE32 (i386) reader/writer for the CDE patch toolchain.

Pure standard library on purpose: modders should not need to ``pip install``
anything. Only the structures the Windows loader consults for a new section
(import table, export table, section headers, directory entries) are modeled.

The image layout this module targets is the shipped ``CDE.exe``: PE32, image
base ``0x400000``, ``SectionAlignment`` 0x1000, ``FileAlignment`` 0x200. The
code is written generically but the happy path mirrors exactly what the
community patch does to the executable (see ``notes/PATCH-FORMAT.md``).
"""

from __future__ import annotations

import hashlib
import struct
from typing import List, Optional

# --- section characteristics -------------------------------------------------
SCN_CNT_CODE = 0x00000020
SCN_CNT_INITIALIZED_DATA = 0x00000040
SCN_CNT_UNINITIALIZED_DATA = 0x00000080
SCN_MEM_DISCARDABLE = 0x02000000
SCN_MEM_EXECUTE = 0x20000000
SCN_MEM_READ = 0x40000000
SCN_MEM_WRITE = 0x80000000

# --- file header characteristics --------------------------------------------
IMAGE_FILE_RELOCS_STRIPPED = 0x0001
IMAGE_FILE_EXECUTABLE_IMAGE = 0x0002
IMAGE_FILE_LARGE_ADDRESS_AWARE = 0x0020
IMAGE_FILE_32BIT_MACHINE = 0x0100

# --- data directory indices --------------------------------------------------
DIR_EXPORT = 0
DIR_IMPORT = 1
DIR_RESOURCE = 2
DIR_EXCEPTION = 3
DIR_SECURITY = 4
DIR_BASERELOC = 5
DIR_DEBUG = 6
DIR_ARCHITECTURE = 7
DIR_GLOBALPTR = 8
DIR_TLS = 9
DIR_LOAD_CONFIG = 10
DIR_BOUND_IMPORT = 11
DIR_IAT = 12
DIR_DELAY_IMPORT = 13
DIR_CLR = 14

DIRECTORY_NAMES = [
    "export", "import", "resource", "exception", "security", "basereloc",
    "debug", "architecture", "globalptr", "tls", "load_config", "bound_import",
    "iat", "delay_import", "clr", "reserved",
]

_SECTION_HEADER_SIZE = 40
_DOS_HEADER_PE_OFFSET = 0x3C
_PE32_OPTIONAL_MAGIC = 0x10B


def align_up(value: int, alignment: int) -> int:
    """Round ``value`` up to the next multiple of ``alignment``."""
    if alignment <= 1:
        return value
    return (value + alignment - 1) & ~(alignment - 1)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class PEError(Exception):
    """Raised when the input is not the PE32 image we can edit."""


class Section:
    """A section header plus its place in the section table."""

    __slots__ = (
        "name", "virtual_size", "virtual_address", "size_of_raw_data",
        "pointer_to_raw_data", "characteristics", "header_offset",
    )

    def __init__(self, name: str, virtual_size: int, virtual_address: int,
                 size_of_raw_data: int, pointer_to_raw_data: int,
                 characteristics: int, header_offset: int) -> None:
        self.name = name
        self.virtual_size = virtual_size
        self.virtual_address = virtual_address
        self.size_of_raw_data = size_of_raw_data
        self.pointer_to_raw_data = pointer_to_raw_data
        self.characteristics = characteristics
        self.header_offset = header_offset

    @property
    def raw_end(self) -> int:
        return self.pointer_to_raw_data + self.size_of_raw_data

    @property
    def virtual_end(self) -> int:
        return self.virtual_address + max(self.virtual_size, self.size_of_raw_data)

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return (
            "Section(%r va=0x%x vsz=0x%x raw=0x%x rawsz=0x%x chars=0x%x)"
            % (self.name, self.virtual_address, self.virtual_size,
               self.pointer_to_raw_data, self.size_of_raw_data,
               self.characteristics)
        )


class PE:
    """A mutable PE32 image."""

    def __init__(self, data: bytes) -> None:
        self.data = bytearray(data)
        self._parse()

    # -- parsing --------------------------------------------------------------
    def _parse(self) -> None:
        d = self.data
        if len(d) < 0x40 or d[:2] != b"MZ":
            raise PEError("not an MZ image")
        self.e_lfanew = struct.unpack_from("<I", d, _DOS_HEADER_PE_OFFSET)[0]
        if d[self.e_lfanew:self.e_lfanew + 4] != b"PE\0\0":
            raise PEError("missing PE signature")

        self.coff_offset = self.e_lfanew + 4
        (self.machine, self.number_of_sections, self.timestamp,
         self.pointer_to_symbol_table, self.number_of_symbols,
         self.optional_header_size, self.characteristics) = struct.unpack_from(
            "<HHIIIHH", d, self.coff_offset)

        self.opt_offset = self.coff_offset + 20
        magic = struct.unpack_from("<H", d, self.opt_offset)[0]
        if magic != _PE32_OPTIONAL_MAGIC:
            raise PEError("not a PE32 image (optional magic 0x%x)" % magic)

        self.address_of_entry_point = struct.unpack_from("<I", d, self.opt_offset + 16)[0]
        self.image_base = struct.unpack_from("<I", d, self.opt_offset + 28)[0]
        self.section_alignment = struct.unpack_from("<I", d, self.opt_offset + 32)[0]
        self.file_alignment = struct.unpack_from("<I", d, self.opt_offset + 36)[0]
        self.size_of_image = struct.unpack_from("<I", d, self.opt_offset + 56)[0]
        self.size_of_headers = struct.unpack_from("<I", d, self.opt_offset + 60)[0]
        self.checksum_offset = self.opt_offset + 64
        self.subsystem = struct.unpack_from("<H", d, self.opt_offset + 68)[0]
        self.dll_characteristics = struct.unpack_from("<H", d, self.opt_offset + 70)[0]
        self.number_of_rva_and_sizes = struct.unpack_from("<I", d, self.opt_offset + 92)[0]
        self.directory_offset = self.opt_offset + 96

        self.section_table_offset = self.opt_offset + self.optional_header_size
        self.sections: List[Section] = []
        for i in range(self.number_of_sections):
            off = self.section_table_offset + i * _SECTION_HEADER_SIZE
            name = d[off:off + 8].rstrip(b"\0").decode("latin1")
            vsz, va, rawsz, raw = struct.unpack_from("<IIII", d, off + 8)
            chars = struct.unpack_from("<I", d, off + 36)[0]
            self.sections.append(Section(name, vsz, va, rawsz, raw, chars, off))

    # -- header accessors -----------------------------------------------------
    def directory(self, index: int) -> tuple:
        if index >= self.number_of_rva_and_sizes:
            return (0, 0)
        return struct.unpack_from("<II", self.data, self.directory_offset + index * 8)

    def set_directory(self, index: int, rva: int, size: int) -> None:
        struct.pack_into("<II", self.data, self.directory_offset + index * 8, rva, size)

    def has_directory(self, index: int) -> bool:
        rva, size = self.directory(index)
        return bool(rva and size)

    # -- address mapping ------------------------------------------------------
    def rva_to_offset(self, rva: int) -> Optional[int]:
        for s in self.sections:
            if s.virtual_address <= rva < s.virtual_address + s.size_of_raw_data:
                return s.pointer_to_raw_data + (rva - s.virtual_address)
        return None

    def offset_to_rva(self, offset: int) -> Optional[int]:
        for s in self.sections:
            if s.pointer_to_raw_data <= offset < s.raw_end:
                return s.virtual_address + (offset - s.pointer_to_raw_data)
        return None

    def read_rva(self, rva: int, size: int) -> bytes:
        off = self.rva_to_offset(rva)
        if off is None:
            raise PEError("RVA 0x%x is not backed by file data" % rva)
        return bytes(self.data[off:off + size])

    def read_c_string_rva(self, rva: int) -> str:
        off = self.rva_to_offset(rva)
        if off is None:
            return ""
        end = self.data.find(b"\0", off)
        if end < 0:
            end = len(self.data)
        return bytes(self.data[off:end]).decode("latin1")

    # -- section management ---------------------------------------------------
    def next_section_rva(self) -> int:
        """The RVA the next appended section will be given."""
        end = 0
        for s in self.sections:
            end = max(end, s.virtual_end)
        return align_up(max(end, self.size_of_image), self.section_alignment)

    def add_section(self, name: str, content: bytes, characteristics: int,
                    virtual_size: Optional[int] = None,
                    virtual_address: Optional[int] = None) -> Section:
        """Append a new section and return it.

        The content is zero-padded up to ``FileAlignment``. The section is
        placed at the current end of the file and at the next free RVA, which
        is exactly how the community patch lays out its ``.cdei`` / ``.cdep``
        sections.
        """
        if len(self.sections) >= (self.size_of_headers - self.section_table_offset) // _SECTION_HEADER_SIZE:
            raise PEError("no room in the header for another section header")
        if len(name.encode("latin1")) > 8:
            raise PEError("section name %r is longer than 8 bytes" % name)

        content = bytes(content)
        raw_size = align_up(max(len(content), 1), self.file_alignment)
        raw_offset = align_up(len(self.data), self.file_alignment)
        rva = virtual_address if virtual_address is not None else self.next_section_rva()
        vsize = virtual_size if virtual_size is not None else len(content)

        header_off = self.section_table_offset + len(self.sections) * _SECTION_HEADER_SIZE
        hdr = bytearray(_SECTION_HEADER_SIZE)
        hdr[0:8] = name.encode("latin1").ljust(8, b"\0")
        struct.pack_into("<IIII", hdr, 8, vsize, rva, raw_size, raw_offset)
        struct.pack_into("<I", hdr, 36, characteristics)
        self.data[header_off:header_off + _SECTION_HEADER_SIZE] = hdr

        if raw_offset > len(self.data):
            self.data.extend(b"\0" * (raw_offset - len(self.data)))
        self.data[raw_offset:raw_offset + len(content)] = content
        self.data.extend(b"\0" * (raw_offset + raw_size - len(self.data)))

        self.number_of_sections += 1
        struct.pack_into("<H", self.data, self.coff_offset + 2, self.number_of_sections)
        self.size_of_image = align_up(max(self.size_of_image, rva + vsize),
                                      self.section_alignment)
        struct.pack_into("<I", self.data, self.opt_offset + 56, self.size_of_image)

        section = Section(name, vsize, rva, raw_size, raw_offset,
                          characteristics, header_off)
        self.sections.append(section)
        return section

    def set_large_address_aware(self, enabled: bool = True) -> None:
        if enabled:
            self.characteristics |= IMAGE_FILE_LARGE_ADDRESS_AWARE
        else:
            self.characteristics &= ~IMAGE_FILE_LARGE_ADDRESS_AWARE
        struct.pack_into("<H", self.data, self.coff_offset + 18, self.characteristics)

    # -- entry point / patching ----------------------------------------------
    def set_entry_point(self, va: int) -> None:
        struct.pack_into("<I", self.data, self.opt_offset + 16, va - self.image_base)

    def write_va(self, va: int, payload: bytes) -> None:
        off = self.rva_to_offset(va - self.image_base)
        if off is None:
            raise PEError("VA 0x%x is not backed by file data" % va)
        self.data[off:off + len(payload)] = payload

    def write_offset(self, offset: int, payload: bytes) -> None:
        self.data[offset:offset + len(payload)] = payload

    def to_bytes(self) -> bytes:
        return bytes(self.data)

    # -- diagnostics ----------------------------------------------------------
    def summary(self) -> dict:
        imports, exports = [], []
        try:
            imports = [d["dll"] for d in self.iter_imports()]
        except Exception:
            pass
        try:
            exports = [s["name"] for s in self.iter_exports()]
        except Exception:
            pass
        return {
            "machine": self.machine,
            "number_of_sections": self.number_of_sections,
            "timestamp": self.timestamp,
            "characteristics": self.characteristics,
            "image_base": self.image_base,
            "address_of_entry_point": self.address_of_entry_point,
            "section_alignment": self.section_alignment,
            "file_alignment": self.file_alignment,
            "size_of_image": self.size_of_image,
            "size_of_headers": self.size_of_headers,
            "sections": [
                {"name": s.name, "virtual_address": s.virtual_address,
                 "virtual_size": s.virtual_size,
                 "pointer_to_raw_data": s.pointer_to_raw_data,
                 "size_of_raw_data": s.size_of_raw_data,
                 "characteristics": s.characteristics}
                for s in self.sections
            ],
            "imports": imports,
            "exports": exports,
        }

    # -- import / export readers ---------------------------------------------
    def iter_imports(self):
        """Yield ``{"dll", "functions", "first_thunk_rva"}`` for each import."""
        imp_rva, imp_size = self.directory(DIR_IMPORT)
        if not imp_rva:
            return
        off = self.rva_to_offset(imp_rva)
        if off is None:
            return
        while True:
            oft, tds, fc, name_rva, ft = struct.unpack_from("<IIIII", self.data, off)
            if not (oft or tds or fc or name_rva or ft):
                break
            dll = self.read_c_string_rva(name_rva)
            funcs = []
            thunk_rva = oft or ft
            t = self.rva_to_offset(thunk_rva)
            if t is not None:
                while True:
                    value = struct.unpack_from("<I", self.data, t)[0]
                    if value == 0:
                        break
                    if value & 0x80000000:
                        funcs.append({"ordinal": value & 0xFFFF})
                    else:
                        name_off = self.rva_to_offset(value)
                        if name_off is None:
                            funcs.append({"name": None})
                        else:
                            hint = struct.unpack_from("<H", self.data, name_off)[0]
                            end = self.data.find(b"\0", name_off + 2)
                            funcs.append({
                                "name": bytes(self.data[name_off + 2:end]).decode("latin1"),
                                "hint": hint})
                    t += 4
            yield {"dll": dll, "functions": funcs, "first_thunk_rva": ft}
            off += 20

    def iter_exports(self):
        """Yield ``{"name", "rva", "ordinal"}`` for each exported symbol."""
        exp_rva, exp_size = self.directory(DIR_EXPORT)
        if not exp_rva:
            return
        off = self.rva_to_offset(exp_rva)
        if off is None:
            return
        (char, tds, major, minor, name_rva, base, num_funcs, num_names,
         addr_funcs, addr_names, addr_ordinals) = struct.unpack_from(
            "<IIHHIIIIIII", self.data, off)
        fo = self.rva_to_offset(addr_funcs)
        no = self.rva_to_offset(addr_names)
        oo = self.rva_to_offset(addr_ordinals)
        for i in range(num_names):
            name_ptr = struct.unpack_from("<I", self.data, no + i * 4)[0]
            ordinal_index = struct.unpack_from("<H", self.data, oo + i * 2)[0]
            func_rva = struct.unpack_from("<I", self.data, fo + ordinal_index * 4)[0]
            yield {"name": self.read_c_string_rva(name_ptr), "rva": func_rva,
                   "ordinal": base + ordinal_index}
