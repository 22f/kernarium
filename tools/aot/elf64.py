# SPDX-License-Identifier: GPL-2.0-or-later
"""Minimal ELF64 little-endian reader for the AOT rewriter.

It reads the program headers, the section names, and the function ranges of
.eh_frame, which is how a stripped Android library says where its functions
are. Nothing else is interpreted.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import dataclasses
import hashlib
import struct

PT_LOAD = 1
PF_X = 1
PF_W = 2
SHT_NOTE = 7


class ElfError(Exception):
    pass


@dataclasses.dataclass(frozen=True)
class Segment:
    vaddr: int
    offset: int
    filesz: int
    memsz: int
    flags: int


@dataclasses.dataclass(frozen=True)
class Function:
    start: int
    end: int


class Elf:
    def __init__(self, data: bytes) -> None:
        self.data = data
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            raise ElfError("not a little-endian ELF64 file")
        (self.e_type, self.e_machine, _, _, self.phoff, self.shoff, _,
         _, self.phentsize, self.phnum, self.shentsize, self.shnum,
         self.shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
        if self.e_machine != 183:
            raise ElfError("not an AArch64 ELF file")
        self.segments = []
        for index in range(self.phnum):
            p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = (
                struct.unpack_from("<IIQQQQQQ", data,
                                   self.phoff + index * self.phentsize))
            if p_type == PT_LOAD:
                self.segments.append(Segment(p_vaddr, p_offset, p_filesz,
                                             p_memsz, p_flags))
        self.sections = self._read_sections()

    def _read_sections(self) -> dict[str, tuple[int, int, int]]:
        sections: dict[str, tuple[int, int, int]] = {}
        if self.shoff == 0 or self.shnum == 0:
            return sections
        raw = []
        for index in range(self.shnum):
            (name, sh_type, _, addr, offset, size, _, _, _, _) = (
                struct.unpack_from("<IIQQQQIIQQ", self.data,
                                   self.shoff + index * self.shentsize))
            raw.append((name, sh_type, addr, offset, size))
        names_offset = raw[self.shstrndx][3]
        for name, sh_type, addr, offset, size in raw:
            end = self.data.index(b"\0", names_offset + name)
            label = self.data[names_offset + name:end].decode()
            sections[label] = (addr, offset, size)
        return sections

    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.data).hexdigest()

    @property
    def build_id(self) -> bytes:
        if ".note.gnu.build-id" not in self.sections:
            return b""
        _, offset, size = self.sections[".note.gnu.build-id"]
        namesz, descsz, _ = struct.unpack_from("<III", self.data, offset)
        start = offset + 12 + ((namesz + 3) & ~3)
        return bytes(self.data[start:start + descsz])

    def executable_segment(self) -> Segment:
        candidates = [s for s in self.segments if s.flags & PF_X]
        if len(candidates) != 1:
            raise ElfError("expected exactly one executable PT_LOAD")
        return candidates[0]

    def read_vaddr(self, vaddr: int, size: int) -> bytes:
        for segment in self.segments:
            if segment.vaddr <= vaddr and vaddr + size <= (
                    segment.vaddr + segment.filesz):
                start = segment.offset + vaddr - segment.vaddr
                return bytes(self.data[start:start + size])
        raise ElfError(f"address {vaddr:#x} is not file-backed")

    def functions(self) -> list[Function]:
        """The function ranges the unwind tables describe, merged and sorted."""
        if ".eh_frame" not in self.sections:
            raise ElfError("no .eh_frame: functions cannot be located")
        addr, offset, size = self.sections[".eh_frame"]
        data = self.data
        cies: dict[int, int] = {}
        found: list[tuple[int, int]] = []
        position = offset
        end = offset + size
        while position + 4 <= end:
            (length,) = struct.unpack_from("<I", data, position)
            if length == 0:
                break
            body = position + 4
            (cie_id,) = struct.unpack_from("<I", data, body)
            if cie_id == 0:
                cies[position] = self._read_cie(body + 4, position + 4 + length)
            else:
                cie_position = body - cie_id
                encoding = cies.get(cie_position)
                if encoding is None:
                    raise ElfError("FDE names an unknown CIE")
                field = body + 4
                begin = self._read_encoded(field, encoding, addr - offset)
                range_ = self._read_encoded(field + 4, encoding & 0x0f, 0)
                if range_ > 0:
                    found.append((begin, begin + range_))
            position += 4 + length
        found.sort()
        merged: list[Function] = []
        for begin, finish in found:
            if merged and begin <= merged[-1].end:
                last = merged.pop()
                merged.append(Function(last.start, max(last.end, finish)))
            else:
                merged.append(Function(begin, finish))
        return merged

    def _read_cie(self, position: int, end: int) -> int:
        data = self.data
        version = data[position]
        position += 1
        stop = data.index(b"\0", position)
        augmentation = data[position:stop].decode()
        position = stop + 1
        for _ in range(2):  # code and data alignment factors
            position = _skip_leb(data, position)
        position = _skip_leb(data, position) if version >= 3 else position + 1
        encoding = 0
        if augmentation.startswith("z"):
            position = _skip_leb(data, position)
            for letter in augmentation[1:]:
                if letter == "R":
                    encoding = data[position]
                    position += 1
                elif letter == "L":
                    position += 1
                elif letter == "P":
                    personality = data[position]
                    position += 1 + _encoded_size(personality)
                elif letter in "SB":
                    pass
                else:
                    raise ElfError(f"unknown CIE augmentation {letter!r}")
        return encoding

    def _read_encoded(self, position: int, encoding: int, section_bias: int
                      ) -> int:
        fmt = encoding & 0x0f
        if fmt == 0x0b:
            (value,) = struct.unpack_from("<i", self.data, position)
        elif fmt == 0x03:
            (value,) = struct.unpack_from("<I", self.data, position)
        elif fmt == 0x0c:
            (value,) = struct.unpack_from("<q", self.data, position)
        elif fmt == 0x04:
            (value,) = struct.unpack_from("<Q", self.data, position)
        else:
            raise ElfError(f"unsupported pointer encoding {encoding:#x}")
        if encoding & 0x70 == 0x10:  # pc-relative
            value += position + section_bias
        return value & 0xffffffffffffffff


def _skip_leb(data: bytes, position: int) -> int:
    while data[position] & 0x80:
        position += 1
    return position + 1


def _encoded_size(encoding: int) -> int:
    fmt = encoding & 0x0f
    return {0x03: 4, 0x0b: 4, 0x04: 8, 0x0c: 8}.get(fmt, 0)
