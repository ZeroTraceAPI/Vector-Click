#!/usr/bin/env python3
"""Normalize the PE / COFF timestamp for deterministic Zig rebuilds."""

from __future__ import annotations

import os
from pathlib import Path
import struct
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: normalize-pe.py <windows-executable>", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    try:
        data = bytearray(path.read_bytes())
    except OSError as error:
        print(f"Vector Click Zig build: cannot read {path}: {error}", file=sys.stderr)
        return 1

    if len(data) < 0x40 or data[0:2] != b"MZ":
        print(f"Vector Click Zig build: {path} is not an MZ executable", file=sys.stderr)
        return 1

    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 12 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        print(f"Vector Click Zig build: {path} has no valid PE header", file=sys.stderr)
        return 1

    try:
        epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "1784246400"), 10)
    except ValueError:
        print("Vector Click Zig build: SOURCE_DATE_EPOCH must be an integer", file=sys.stderr)
        return 2
    if not 0 <= epoch <= 0xFFFFFFFF:
        print("Vector Click Zig build: SOURCE_DATE_EPOCH is outside PE range", file=sys.stderr)
        return 2

    # IMAGE_FILE_HEADER.TimeDateStamp begins four bytes after the PE signature
    # and four bytes into the COFF header. The PE checksum is already zero in
    # this unsigned build, so no checksum repair is required.
    struct.pack_into("<I", data, pe_offset + 8, epoch)
    try:
        path.write_bytes(data)
    except OSError as error:
        print(f"Vector Click Zig build: cannot update {path}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
