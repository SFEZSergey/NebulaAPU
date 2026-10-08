#!/usr/bin/env python3
# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only
"""Recover AGC's default GPU register state from Sony's own libSceAgc.

The library builds its default context and shader register state from static
tables, one set per pipeline variant. Each builder loads three table pointers
and three counts in a fixed instruction sequence, which is what this matches;
the tables themselves are arrays of (register offset, value) pairs.

The three tables are context, shader and uconfig, in that order. The
third one is uconfig and not a second context table: its offsets reach
0x41F, which does not fit the 1024-slot context space at all, and the
offsets Mesa's gfx103 database can name resolve to GDS_OA_ADDRESS,
GE_CNTL, GE_STEREO_CNTL, VGT_PRIMITIVE_TYPE and TA_CS_BC_BASE_ADDR
only when read as uconfig. All ten variants agree that exactly one
uconfig register has a non-zero default.

Only facts are emitted - a register offset and the value the hardware is left
in - never any of Sony's code. The generated header is written outside the
repository, because the input is the user's own game dump.
"""

import argparse
import re
import struct
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sce_self_extract

# lea rsi / lea rcx / lea r9 (the three tables), mov edx / mov r8d / movq
# (%rsp) (their three counts), then the call that consumes them.
BUILDER = re.compile(
    rb'\x48\x8d\x35(.{4})\x48\x8d\x0d(.{4})\x4c\x8d\x0d(.{4})'
    rb'\xba(.{4})\x41\xb8(.{4})\x48\xc7\x04\x24(.{4})\xe8', re.S)
CONTEXT_SPACE = 0x28000
MINIMUM_FULL_TABLE = 100


def segments(image):
    phoff, = struct.unpack_from('<Q', image, 32)
    phentsize, phnum = struct.unpack_from('<HH', image, 54)
    out = []
    for index in range(phnum):
        base = phoff + index * phentsize
        p_type, p_flags = struct.unpack_from('<II', image, base)
        p_offset, p_vaddr, _, p_filesz, _, _ = struct.unpack_from(
            '<QQQQQQ', image, base + 8)
        if p_type == 1 and p_filesz:
            out.append((p_flags, p_offset, p_vaddr, p_filesz))
    return out


def reader(image):
    loaded = segments(image)

    def read(vaddr, count):
        for _, offset, base, size in loaded:
            if base <= vaddr < base + size:
                start = vaddr - base + offset
                return [struct.unpack_from('<II', image, start + i * 8)
                        for i in range(count)]
        raise SystemExit(f'vaddr 0x{vaddr:X} is outside every segment')
    return read


def relocations(image):
    """R_X86_64_RELATIVE addends, by the address they are written to."""
    phoff, = struct.unpack_from('<Q', image, 32)
    phentsize, phnum = struct.unpack_from('<HH', image, 54)
    dynamic = None
    for index in range(phnum):
        base = phoff + index * phentsize
        p_type, = struct.unpack_from('<I', image, base)
        p_offset, _, _, p_filesz, _, _ = struct.unpack_from(
            '<QQQQQQ', image, base + 8)
        if p_type == 2:
            dynamic = (p_offset, p_filesz)
    if dynamic is None:
        return {}
    tags = {}
    offset, size = dynamic
    for index in range(size // 16):
        tag, value = struct.unpack_from('<QQ', image, offset + index * 16)
        tags.setdefault(tag, value)
    if 7 not in tags or 8 not in tags:
        return {}
    table = _to_offset(image, tags[7])
    out = {}
    for index in range(tags[8] // 24):
        where, _info, addend = struct.unpack_from(
            '<QQq', image, table + index * 24)
        out[where] = addend
    return out


def _to_offset(image, vaddr):
    for _, offset, base, size in segments(image):
        if base <= vaddr < base + size:
            return offset + (vaddr - base)
    raise SystemExit(f'vaddr 0x{vaddr:X} is outside every segment')


def read_groups(image, descriptor):
    """The (hash, id) table the guest searches, from the descriptor.

    The guest calls sceAgcGetRegisterDefaults2 once, reads the group count
    at +0x38 and the table at +0x30, and walks it twelve bytes at a time
    comparing a 32-bit hash. A zero count sends it down a path that writes
    -1 into every slot it was going to cache an id in, which is what our
    empty descriptor has been doing.
    """
    fixups = relocations(image)
    pointer = fixups.get(descriptor + 0x30)
    if pointer is None:
        return []
    count, = struct.unpack_from(
        '<I', image, _to_offset(image, descriptor + 0x38))
    start = _to_offset(image, pointer)
    return [struct.unpack_from('<II', image, start + index * 12)
            for index in range(count)]


def read_space_arrays(image, descriptor, tables):
    """The pointer arrays the guest indexes, as indices into the tables.

    A group id encodes a space in its low two bits and an index in the
    next eight. The guest reads the descriptor's pointer for that space,
    indexes it, and dereferences once - so each slot holds the address of
    one (offset, value) pair in the space's default table. Stored here as
    the pair's index, which is the same fact without the addresses.
    """
    fixups = relocations(image)
    out = []
    for space, (table, count) in enumerate(tables):
        array = fixups.get(descriptor + 8 * space)
        slots = []
        index = 0
        while array is not None:
            target = fixups.get(array + index * 8)
            # The arrays sit end to end, so a slot that points outside this
            # space's table is the first slot of the next array.
            if target is None or not table <= target < table + count * 8:
                break
            slots.append((target - table) // 8)
            index += 1
        out.append(slots)
    return out


def find_tables(image):
    text = next(s for s in segments(image) if s[0] & 1)
    _, offset, vaddr, size = text
    body = image[offset:offset + size]
    read = reader(image)

    found = {}
    for match in BUILDER.finditer(body):
        base = vaddr + match.start()
        pointers = [base + 7 * (i + 1) + struct.unpack('<i', match.group(i + 1))[0]
                    for i in range(3)]
        counts = [struct.unpack('<I', match.group(i))[0] for i in (4, 5, 6)]
        if counts[0] < MINIMUM_FULL_TABLE:
            continue                      # a small patch table, not a full set
        found.setdefault(
            pointers[0],
            (counts[0], pointers[1], counts[1], pointers[2], counts[2]))
        _builder_bases.add(base)
        _builder_by_base[base] = pointers[0]
        # The builder ends `call <patch>; lea rax,[rip+X]; ret`, and that
        # lea names the descriptor sceAgcGetRegisterDefaults2 hands the
        # guest. The guest reads two fields of it: a group table and a
        # count.
        tail = offset + match.end() + 4
        if image[tail:tail + 3] == bytes((0x48, 0x8D, 0x05)):
            _descriptor_by_base[base] = (
                vaddr + match.end() + 11 +
                struct.unpack('<i', image[tail + 3:tail + 7])[0])

    if not found:
        raise SystemExit('no default-state builders found')
    return found, read


# sceAgcGetRegisterDefaults2 switches on the version it is handed. Versions
# up to twelve go through a jump table; anything above falls through to a
# branch of its own, which asks sceKernelIsTrinityMode - PS5 Pro - and calls
# one of two builders:
#
#     call   sceKernelIsTrinityMode
#     lea    rdi,[rbp-0x3c]
#     test   al,al
#     je     <base console builder>
#     call   <Trinity builder>
#
# Taking the first call in that window picks the Trinity table, which is not
# the one this title is given on the console it ships for.
DISPATCH_FALLBACK = re.compile(rb'\x83\xfb\x0c\x0f\x87(.{4})', re.S)
CALL = re.compile(rb'\xe8(.{4})', re.S)
TRINITY_BRANCH = re.compile(rb'\x84\xc0\x74(.)', re.S)


def find_version_builder(image, version, trinity=False):
    """Returns the builder address the given version dispatches to."""
    if version <= 12:
        return None                      # jump-table versions are not walked
    text = next(s for s in segments(image) if s[0] & 1)
    _, offset, vaddr, size = text
    body = image[offset:offset + size]
    match = DISPATCH_FALLBACK.search(body)
    if match is None:
        return None
    fallback = (vaddr + match.end() +
                struct.unpack('<i', match.group(1))[0])
    window_start = fallback - vaddr

    def builder_at(address):
        # The builders matched above start a few bytes before their entry,
        # at the first lea of the table triple.
        for candidate in range(address, address + 0x20):
            if candidate in _builder_bases:
                return candidate
        return None

    def first_builder(start, span=0x40):
        window = body[start - vaddr:start - vaddr + span]
        for call in CALL.finditer(window):
            target = (start + call.end() +
                      struct.unpack('<i', call.group(1))[0])
            found = builder_at(target)
            if found is not None:
                return found
        return None

    if not trinity:
        branch = TRINITY_BRANCH.search(body, window_start,
                                       window_start + 0x40)
        if branch is not None:
            taken = (vaddr + branch.end() +
                     struct.unpack('<b', branch.group(1))[0])
            found = first_builder(taken, 0x10)
            if found is not None:
                return found
    return first_builder(fallback)


_builder_bases = set()
_descriptor_by_base = {}
_builder_by_base = {}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('library', help='libSceAgc.sprx from the game dump')
    parser.add_argument('header', help='C++ header to generate')
    parser.add_argument(
        '--version',
        type=int,
        default=13,
        help='the defaults version the guest asks for (default 13)')
    parser.add_argument(
        '--trinity',
        action='store_true',
        help='take the PS5 Pro table rather than the base console one')
    arguments = parser.parse_args()

    image, _ = sce_self_extract.rebuild(open(arguments.library, 'rb').read())
    found, read = find_tables(image)

    variants = {}
    raw = {}
    for pointer, entry in found.items():
        count, sh_pointer, sh_count, uc_pointer, uc_count = entry
        # The slot arrays index the tables as they are written, duplicates
        # and all, so the raw rows are kept beside the deduplicated maps.
        rows = (read(pointer, count),
                read(sh_pointer, sh_count),
                read(uc_pointer, uc_count))
        raw[pointer] = rows
        variants[pointer] = tuple(dict(table) for table in rows)

    # Picking the fullest table was a guess, and the wrong one: AGC keeps a
    # separate default set per SDK generation, and the guest asks for one by
    # number. This title asks for version 13, whose set is smaller than the
    # largest present. Selecting by size therefore seeded a different
    # generation's defaults than the game was given on console.
    builder = find_version_builder(image, arguments.version,
                                   arguments.trinity)
    requested = _builder_by_base.get(builder)
    if requested is not None and requested in variants:
        chosen = requested
    else:
        print(f'warning: no builder found for version {arguments.version};'
              ' falling back to the fullest table')
        builder = None
        chosen = max(variants, key=lambda p: len(variants[p][0]))
    context, shader, uconfig = variants[chosen]
    # The descriptor has to come from the builder that was selected, not
    # from any builder that installs the same tables: several of them share
    # a table and return descriptors that differ, down to the width of the
    # group entries.
    descriptor = _descriptor_by_base.get(builder)
    groups = read_groups(image, descriptor) if descriptor else []
    slot_tables = ((chosen, found[chosen][0]),
                   (found[chosen][1], found[chosen][2]),
                   (found[chosen][3], found[chosen][4]))
    slots = (read_space_arrays(image, descriptor, slot_tables)
             if descriptor else [[], [], []])
    full = raw[chosen]
    disputed = sorted(
        offset for offset in context
        if any(other[0].get(offset, context[offset]) != context[offset]
               for other in variants.values()))

    # A register whose default is zero is indistinguishable from one that was
    # never written, and the decoder treats absence as meaningful, so only the
    # non-zero defaults are seeded.
    rows = sorted((o, v) for o, v in context.items() if v)
    sh_rows = sorted((o, v) for o, v in shader.items() if v)
    uc_rows = sorted((o, v) for o, v in uconfig.items() if v)

    with open(arguments.header, 'w', encoding='utf-8', newline='\n') as out:
        out.write('// Generated by tools/agc_extract_defaults.py.'
                  ' Do not edit.\n')
        out.write(f'// Source: {arguments.library}\n')
        out.write(f'// {len(variants)} pipeline variants were found; the'
                  f' one this title asks for, version {arguments.version}, is used'
                  f' ({len(context)} context registers).\n')
        out.write(f'// {len(disputed)} of them disagree between variants: ' +
                  ', '.join(f'0x{o:03X}' for o in disputed) + '\n')
        out.write('#pragma once\n#include <cstdint>\n\n'
                  'namespace agc_defaults {\n\n'
                  'struct RegisterDefault {\n'
                  '    std::uint32_t offset;\n'
                  '    std::uint32_t value;\n'
                  '};\n\n')
        for name, table in (('kContext', rows),
                            ('kShader', sh_rows),
                            ('kUconfig', uc_rows)):
            out.write(f'inline constexpr RegisterDefault {name}[] = {{\n')
            for offset, value in table:
                out.write(f'    {{0x{offset:03X}u, 0x{value:08X}u}},\n')
            out.write('};\n\n')
        # The guest reads none of the filtered tables above: it asks for
        # a group by hash, and the descriptor answers with a pointer into
        # the table as Sony wrote it. Those go out whole.
        for name, table in (('kContextFull', full[0]),
                            ('kShaderFull', full[1]),
                            ('kUconfigFull', full[2])):
            out.write(f'inline constexpr RegisterDefault {name}[] = {{\n')
            for offset, value in table:
                out.write(f'    {{0x{offset:03X}u, 0x{value:08X}u}},\n')
            out.write('};\n\n')
        for space, entries in enumerate(slots):
            out.write(f'inline constexpr std::uint16_t kSlots{space}[] = {{\n')
            for index in entries:
                out.write(f'    {index}u,\n')
            out.write('};\n\n')
        out.write('struct RegisterGroup {\n'
                  '    std::uint32_t hash;\n'
                  '    std::uint32_t id;\n'
                  '};\n\n'
                  'inline constexpr RegisterGroup kGroups[] = {\n')
        for group_hash, identifier in groups:
            out.write(f'    {{0x{group_hash:08X}u, 0x{identifier:08X}u}},\n')
        out.write('};\n\n')
        out.write('}  // namespace agc_defaults\n')

    print(f'{len(groups)} register groups')
    print(f'{len(variants)} variants, chose the one with {len(context)} '
          f'context registers')
    print(f'{len(rows)} non-zero context defaults, {len(sh_rows)} shader, '
          f'{len(uc_rows)} uconfig')
    print(f'wrote {arguments.header}')


if __name__ == '__main__':
    sys.exit(main())
