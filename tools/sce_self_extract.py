#!/usr/bin/env python3
# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only
"""Extract a loadable ELF from a decrypted PS5 SELF container.

PS5 SELF files store their segments in a table of their own, whose file
offsets do not match the offsets recorded in the embedded ELF program
headers. Disassemblers read the program headers, so the segments have to be
laid back out before the file is usable. This rebuilds the file the program
headers already describe: each segment is written at the p_offset its own
header names, so no header has to be rewritten and no two segments collide.

Only plain (uncompressed, unencrypted) segments are supported; encrypted
retail SELFs are rejected rather than silently mangled.
"""

import argparse
import struct
import sys

SELF_MAGIC = 0xEEF51454
SEGMENT_COMPRESSED = 0x2
ELF_MAGIC = b"\x7fELF"


def read_self_segments(data):
    magic, = struct.unpack_from("<I", data, 0)
    if magic != SELF_MAGIC:
        raise SystemExit(f"not a PS5 SELF: magic 0x{magic:08X}")
    count, = struct.unpack_from("<H", data, 0x18)
    segments = {}
    for index in range(count):
        flags, offset, encoded, decoded = struct.unpack_from(
            "<QQQQ", data, 0x20 + index * 0x20)
        if flags & SEGMENT_COMPRESSED:
            raise SystemExit(f"segment {index} is compressed; unsupported")
        if encoded != decoded:
            raise SystemExit(f"segment {index} looks encrypted; unsupported")
        # Segments come in pairs: a small table segment and the payload that
        # carries the program header's contents. Only the latter is wanted.
        if flags & 0x800:
            segments[(flags >> 20) & 0xFFF] = (offset, decoded)
    return 0x20 + count * 0x20, segments


def rebuild(data):
    """Return the ELF image described by a decrypted SELF's program headers."""
    elf_offset, segments = read_self_segments(data)
    if data[elf_offset:elf_offset + 4] != ELF_MAGIC:
        raise SystemExit(f"no ELF header at 0x{elf_offset:X}")

    phoff, = struct.unpack_from("<Q", data, elf_offset + 32)
    phentsize, phnum = struct.unpack_from("<HH", data, elf_offset + 54)

    headers = []
    for index in range(phnum):
        base = elf_offset + phoff + index * phentsize
        p_type, p_flags = struct.unpack_from("<II", data, base)
        p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align =             struct.unpack_from("<QQQQQQ", data, base + 8)
        headers.append([base, p_type, p_offset, p_vaddr, p_filesz])

    header_span = phoff + phnum * phentsize
    span = max([header_span] +
               [offset + size for _, _, offset, _, size in headers])
    image = bytearray(span)
    image[:header_span] = data[elf_offset:elf_offset + header_span]

    placed = []
    for index, (base, p_type, p_offset, p_vaddr, p_filesz) in             enumerate(headers):
        source = segments.get(index)
        if source is None or p_filesz == 0:
            continue
        offset, size = source
        size = min(size, p_filesz)
        image[p_offset:p_offset + size] = data[offset:offset + size]
        placed.append((index, p_type, p_offset, p_vaddr, size, offset))

    if not placed:
        raise SystemExit("no segments placed")

    # ET_SCE_DYNAMIC (0xFE18) confuses generic tools; present it as ET_DYN.
    e_type, = struct.unpack_from("<H", image, 16)
    if e_type == 0xFE18:
        struct.pack_into("<H", image, 16, 3)
    return image, placed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("destination")
    arguments = parser.parse_args()

    data = open(arguments.source, "rb").read()
    image, placed = rebuild(data)
    for index, p_type, p_offset, p_vaddr, size, source in placed:
        print(f"  ph{index}: type=0x{p_type:X} vaddr=0x{p_vaddr:X} "
              f"offset=0x{p_offset:X} size={size} "
              f"from sprx offset 0x{source:X}")
    open(arguments.destination, "wb").write(image)
    print(f"wrote {arguments.destination} ({len(image)} bytes, "
          f"{len(placed)} segments)")


if __name__ == "__main__":
    sys.exit(main())
