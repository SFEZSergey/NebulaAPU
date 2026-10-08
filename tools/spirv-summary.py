# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only

# A short look inside a SPIR-V module without the SDK: the entry point and
# its interface, every variable with its storage class and decorations, and
# how often the instructions that matter for a pixel shader's output appear.
#
#   python tools/spirv-summary.py state-977-ps.spv
import struct
import sys
from collections import Counter

STORAGE = {0: 'UniformConstant', 1: 'Input', 2: 'Uniform', 3: 'Output',
           4: 'Workgroup', 6: 'Private', 7: 'Function', 12: 'StorageBuffer'}
NAMES = {62: 'Store', 61: 'Load', 252: 'Kill', 4416: 'TerminateInvocation',
         87: 'ImageSampleImplicitLod', 88: 'ImageSampleExplicitLod',
         98: 'ImageRead', 99: 'ImageWrite', 247: 'Branch', 250: 'BranchConditional',
         253: 'Return', 12: 'ExtInst'}


def main(path):
    words = struct.unpack('<%dI' % (len(open(path, 'rb').read()) // 4),
                          open(path, 'rb').read())
    at = 5
    decorations = {}
    variables = []
    counts = Counter()
    while at < len(words):
        length = words[at] >> 16
        opcode = words[at] & 0xFFFF
        if length == 0:
            break
        operands = words[at + 1:at + length]
        if opcode == 15:  # EntryPoint
            name_words = operands[2:]
            raw = b''.join(struct.pack('<I', w) for w in name_words)
            name_end = raw.index(b'\x00')
            skip = name_end // 4 + 1
            print('EntryPoint model=%d function=%d name=%s interface=%s' % (
                operands[0], operands[1], raw[:name_end].decode(),
                list(name_words[skip:])))
        elif opcode == 16:  # ExecutionMode
            print('ExecutionMode', list(operands))
        elif opcode == 71:  # Decorate
            decorations.setdefault(operands[0], []).append(tuple(operands[1:]))
        elif opcode == 59 and len(operands) >= 3:  # Variable
            variables.append((operands[1], operands[2]))
        counts[NAMES.get(opcode, opcode)] += 1
        at += length
    for variable, storage in variables:
        if storage in (1, 3, 0, 2, 12):
            print('var %d %s %s' % (variable, STORAGE.get(storage, storage),
                                    decorations.get(variable, [])))
    for name in ('Store', 'Kill', 'TerminateInvocation', 'ImageSampleImplicitLod',
                 'ImageSampleExplicitLod', 'ImageRead', 'BranchConditional'):
        print('%s x%d' % (name, counts.get(name, 0)))


main(sys.argv[1])
