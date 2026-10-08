# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only

# Lists the vector registers a dumped shader reads before anything in it
# writes them - the ones the hardware is expected to have filled in. Works on
# the output of gen5-disasm, a straight-line approximation: it follows the
# program in order and ignores branches, which is enough to see the inputs a
# pixel shader takes at its top.
#
#   gen5-disasm stage1_X.code | python tools/gen5-live-in.py
import re
import sys

written = set()
live_in = {}
for line in sys.stdin:
    parts = line.split()
    if len(parts) < 3:
        continue
    pc, name, words = parts[0], parts[1], [int(w, 16) for w in parts[2:]]
    word = words[0]
    reads, writes = [], []
    top6 = word >> 26
    if (word & 0x80000000) == 0:  # VOP2
        src0 = word & 0x1FF
        if src0 >= 256:
            reads.append(src0 - 256)
        reads.append((word >> 9) & 0xFF)
        writes.append((word >> 17) & 0xFF)
    elif (word >> 25) == 0x3F:  # VOP1
        src0 = word & 0x1FF
        if src0 >= 256:
            reads.append(src0 - 256)
        writes.append((word >> 17) & 0xFF)
    elif (word >> 25) == 0x3E:  # VOPC
        src0 = word & 0x1FF
        if src0 >= 256:
            reads.append(src0 - 256)
        reads.append((word >> 9) & 0xFF)
    elif top6 in (0x35, 0x33) and len(words) > 1:  # VOP3 / VOP3P
        extra = words[1]
        for shift in (0, 9, 18):
            src = (extra >> shift) & 0x1FF
            if src >= 256:
                reads.append(src - 256)
        writes.append(word & 0xFF)
    elif top6 == 0x32:  # VINTRP
        writes.append((word >> 18) & 0xFF)
        reads.append(word & 0xFF)
    elif top6 == 0x3E and len(words) > 1:  # EXP
        for index in range(4):
            reads.append((words[1] >> (index * 8)) & 0xFF)
    elif top6 in (0x38, 0x3A, 0x3C) and len(words) > 1:  # MUBUF/MTBUF/MIMG
        reads.append(words[1] & 0xFF)
        if 'Store' in name:
            reads.append((words[1] >> 8) & 0xFF)
        else:
            writes.append((words[1] >> 8) & 0xFF)
    for reg in reads:
        if reg not in written and reg not in live_in:
            live_in[reg] = (pc, name)
    for reg in writes:
        written.add(reg)

for reg in sorted(live_in):
    pc, name = live_in[reg]
    print('v%d first read at %s by %s' % (reg, pc, name))
