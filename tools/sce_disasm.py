#!/usr/bin/env python3
# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only
"""Locate and disassemble one exported function of a rebuilt SCE library.

`sce_self_extract.py` turns a .sprx into an ELF that carries program headers
but no section headers, so objdump alone reports an empty file. This maps a
virtual address back to a file offset, and resolves NIDs through the dynamic
symbol table, so a single export can be dumped by name.

Answers questions that belong to the producer side of AGC - which register a
builder writes, how a packet is laid out - directly from Sony's own code,
rather than by inferring the layout from another emulator's guess. Nothing it
reads is written into the repository: it prints to stdout for reading, and the
input is the user's own game dump.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile


def read_program_headers(data):
    program_header_offset = struct.unpack_from("<Q", data, 0x20)[0]
    entry_size, count = struct.unpack_from("<HH", data, 0x36)
    segments = []
    for index in range(count):
        base = program_header_offset + index * entry_size
        segment_type = struct.unpack_from("<I", data, base)[0]
        offset, virtual, _physical, file_size, _memory_size = struct.unpack_from(
            "<QQQQQ", data, base + 8)
        if segment_type == 1 and file_size != 0:
            segments.append((virtual, offset, file_size))
        # PT_DYNAMIC
        if segment_type == 2:
            segments.append((virtual, offset, file_size))
    return segments


def read_bytes(segments, virtual_address, length):
    for virtual, offset, file_size in segments:
        if virtual <= virtual_address < virtual + file_size:
            start = offset + (virtual_address - virtual)
            return data_cache[start:start + length]
    return b""


def read_dynamic_symbols(data, segments):
    """Returns [(name, value, size)] from the ELF dynamic symbol table."""
    program_header_offset = struct.unpack_from("<Q", data, 0x20)[0]
    entry_size, count = struct.unpack_from("<HH", data, 0x36)
    dynamic = None
    for index in range(count):
        base = program_header_offset + index * entry_size
        segment_type = struct.unpack_from("<I", data, base)[0]
        offset, virtual, _physical, file_size, _memory_size = struct.unpack_from(
            "<QQQQQ", data, base + 8)
        if segment_type == 2:
            dynamic = (offset, file_size)
    if dynamic is None:
        return []

    symbol_table = string_table = None
    symbol_entry_size = 24
    offset, size = dynamic
    for position in range(offset, offset + size, 16):
        tag, value = struct.unpack_from("<QQ", data, position)
        if tag == 0:
            break
        if tag == 5:  # DT_STRTAB
            string_table = value
        elif tag == 6:  # DT_SYMTAB
            symbol_table = value
        elif tag == 11:  # DT_SYMENT
            symbol_entry_size = value
    if symbol_table is None or string_table is None:
        return []

    symbols = []
    position = symbol_table
    while True:
        entry = read_bytes(segments, position, symbol_entry_size)
        if len(entry) < symbol_entry_size:
            break
        name_offset, _info, _other, _shndx, value, size = struct.unpack_from(
            "<IBBHQQ", entry, 0)
        if name_offset == 0 and value == 0 and size == 0 and symbols:
            break
        raw = read_bytes(segments, string_table + name_offset, 128)
        name = raw.split(b"\x00", 1)[0].decode("ascii", "replace")
        if name:
            symbols.append((name, value, size))
        position += symbol_entry_size
        if len(symbols) > 4096:
            break
    return symbols


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", help="ELF produced by sce_self_extract.py")
    parser.add_argument(
        "target",
        nargs="?",
        help="NID, or an address such as 0x2960; omit to list exports")
    parser.add_argument(
        "--length",
        type=lambda value: int(value, 0),
        default=0,
        help="bytes to disassemble when the symbol size is unknown")
    parser.add_argument("--objdump", default="objdump")
    arguments = parser.parse_args()

    global data_cache
    with open(arguments.library, "rb") as handle:
        data_cache = handle.read()
    segments = read_program_headers(data_cache)
    symbols = read_dynamic_symbols(data_cache, segments)

    if arguments.target is None:
        for name, value, size in sorted(symbols, key=lambda entry: entry[1]):
            if value != 0:
                print(f"0x{value:08X} size={size:<6} {name}")
        print(f"# {len(symbols)} symbols", file=sys.stderr)
        return 0

    address = None
    length = arguments.length
    if arguments.target.startswith("0x"):
        address = int(arguments.target, 16)
    else:
        for name, value, size in symbols:
            if name.split("#", 1)[0] == arguments.target:
                address, length = value, (length or size)
                break
    if address is None:
        print(f"not found: {arguments.target}", file=sys.stderr)
        return 1
    if length == 0:
        length = 0x80

    body = read_bytes(segments, address, length)
    if not body:
        print(f"no segment covers 0x{address:X}", file=sys.stderr)
        return 1

    with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as handle:
        handle.write(body)
        temporary = handle.name
    try:
        subprocess.run(
            [
                arguments.objdump,
                "-D",
                "-b", "binary",
                "-m", "i386:x86-64",
                "-M", "intel",
                f"--adjust-vma=0x{address:X}",
                temporary,
            ],
            check=True)
    finally:
        os.unlink(temporary)
    return 0


data_cache = b""

if __name__ == "__main__":
    sys.exit(main())
