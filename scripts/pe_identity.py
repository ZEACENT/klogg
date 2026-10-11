"""Read bounded Windows PE machine/magic identity without executing a binary."""

from __future__ import annotations

import pathlib
import struct

MAX_PE_HEADER_OFFSET = 64 * 1024


def architecture_from_pe(path: pathlib.Path) -> str:
    with path.open("rb") as program:
        dos = program.read(64)
        if len(dos) != 64 or dos[:2] != b"MZ":
            raise ValueError("PE DOS header invalid")
        offset = struct.unpack_from("<I", dos, 60)[0]
        if not 64 <= offset <= MAX_PE_HEADER_OFFSET:
            raise ValueError("PE header offset invalid")
        program.seek(offset)
        pe = program.read(26)
    if len(pe) != 26 or pe[:4] != b"PE\0\0" or struct.unpack_from("<H", pe, 20)[0] < 2:
        raise ValueError("PE COFF header invalid")
    identity = (struct.unpack_from("<H", pe, 4)[0], struct.unpack_from("<H", pe, 24)[0])
    if identity == (0x14C, 0x10B):
        return "x86"
    if identity == (0x8664, 0x20B):
        return "x64"
    raise ValueError("PE architecture or optional-header magic unsupported")
