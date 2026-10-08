#!/usr/bin/env python3
# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only
"""Recover export names of a rebuilt SCE library from its NIDs.

A .sprx exports functions by NID - the first eight bytes of
SHA1(name + salt), little-endian, in Sony's base64 alphabet - so the symbol
table carries no readable names. The hash is one way, but the name space is
not: every emulator that implements an AGC entry point spells the name in its
own source. Hashing those spellings and matching the result against the
library's NIDs names the exports without guessing at any of them - a match is
a proof, since a collision on sixty-four bits does not happen by accident.

That is what makes tools/sce_disasm.py usable on more than the handful of
entry points whose NIDs were already written down: a named export can be
disassembled to answer a question about the register or packet it produces.

Names are read from source trees given on the command line. Nothing is
written into the repository - the output is a listing.
"""

import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys

SALT = bytes.fromhex('518D64A635DED8C1E6B039B1C3E55230')
ALPHABET = ('ABCDEFGHIJKLMNOPQRSTUVWXYZ'
            'abcdefghijklmnopqrstuvwxyz'
            '0123456789+-')
NAME = re.compile(rb'\bsce[A-Z][A-Za-z0-9_]{3,}')
SYMBOL = re.compile(r'(0x[0-9A-Fa-f]+) size=(\d+)\s+([A-Za-z0-9+-]{11})#')
SOURCE_SUFFIXES = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.cs',
                   '.py', '.txt', '.md', '.json')


def nid(name):
    """Sony's NID: eleven base64 characters over a truncated SHA1."""
    digest = hashlib.sha1(name.encode('utf-8') + SALT).digest()
    value = struct.unpack('<Q', digest[:8])[0]
    head = ''.join(ALPHABET[(value >> (58 - 6 * index)) & 0x3F]
                   for index in range(10))
    # Sixty-four bits do not divide into six, so the last character carries
    # the four bits that are left, shifted up into place.
    return head + ALPHABET[(value << 2) & 0x3F]


def collect_names(roots, prefix):
    names = set()
    for root in roots:
        if os.path.isfile(root):
            files = [root]
        else:
            files = []
            for directory, _, entries in os.walk(root):
                if '.git' in directory:
                    continue
                files += [os.path.join(directory, entry) for entry in entries
                          if entry.lower().endswith(SOURCE_SUFFIXES)]
        for path in files:
            try:
                with open(path, 'rb') as handle:
                    body = handle.read()
            except OSError:
                continue
            for match in NAME.finditer(body):
                name = match.group(0).decode('ascii')
                if name.startswith(prefix):
                    names.add(name)
    return sorted(names)


def read_exports(library):
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        'sce_disasm.py')
    listing = subprocess.run([sys.executable, tool, library],
                             capture_output=True, text=True, check=True)
    exports = {}
    for line in listing.stdout.splitlines():
        match = SYMBOL.match(line)
        if match:
            exports[match.group(3)] = (int(match.group(1), 16),
                                       int(match.group(2)))
    return exports


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('library', help='ELF rebuilt by sce_self_extract.py')
    parser.add_argument('sources', nargs='+',
                        help='files or trees to read candidate names from')
    parser.add_argument('--prefix', default='sce',
                        help='only consider names with this prefix')
    parser.add_argument('--unmatched', action='store_true',
                        help='also list the NIDs no candidate name matched')
    arguments = parser.parse_args()

    exports = read_exports(arguments.library)
    candidates = collect_names(arguments.sources, arguments.prefix)
    table = {nid(name): name for name in candidates}

    named = {key: value for key, value in exports.items() if key in table}
    for key, (address, size) in sorted(named.items(),
                                       key=lambda item: item[1][0]):
        print(f'0x{address:08X} size={size:<6} {key} {table[key]}')
    if arguments.unmatched:
        print('# unmatched')
        for key, (address, size) in sorted(exports.items(),
                                           key=lambda item: item[1][0]):
            if key not in table:
                print(f'0x{address:08X} size={size:<6} {key}')
    print(f'# {len(exports)} exports, {len(candidates)} candidate names, '
          f'{len(named)} named')


if __name__ == '__main__':
    main()
