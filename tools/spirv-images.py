# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only

# Prints how a SPIR-V module declares and uses its images: every OpTypeImage,
# OpTypeSampledImage and OpTypeSampler, the variables of those types, and each
# sampling instruction with its operands. For looking at a module when the
# SDK's disassembler is not at hand.
#
#   python tools/spirv-images.py state-977-ps.spv
import struct
import sys

OPS = {25: 'TypeImage', 26: 'TypeSampler', 27: 'TypeSampledImage',
       32: 'TypePointer', 59: 'Variable', 86: 'SampledImage',
       87: 'ImageSampleImplicitLod', 88: 'ImageSampleExplicitLod',
       98: 'ImageRead', 100: 'Image', 61: 'Load', 22: 'TypeFloat',
       21: 'TypeInt', 23: 'TypeVector'}


def main(path):
    data = open(path, 'rb').read()
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    at = 5
    while at < len(words):
        length = words[at] >> 16
        opcode = words[at] & 0xFFFF
        if length == 0:
            break
        if opcode in (25, 26, 27, 86, 87, 88, 98, 100):
            print(OPS[opcode], list(words[at + 1:at + length]))
        at += length


main(sys.argv[1])
