# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only

"""Emit the remaining instruction decoders from the C# they are ported from.

Nine families, 381 opcodes. Same reason as the vector generator: a name
transcribed wrongly among that many is found by a shader misbehaving, not
by reading. The length rules are not tables and are written out by hand in
the header, with tests, because each family has its own and getting one
wrong desynchronises the whole program rather than one instruction.

Opcodes the native decoder knows and the C# tables do not are listed in
NATIVE_ONLY below, so a regeneration keeps them. Before writing, the
generator checks that every opcode the current header decodes is still in
its output, and stops without writing if one would be lost: an entry added
to the header by hand belongs in NATIVE_ONLY first. --force writes anyway.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src', 'Ps5Recomp.ShaderCompiler', 'Gen5ShaderTranslator.cs')
OUT = os.path.join(ROOT, 'runtime', 'native', 'gen5_decoder_memory.h')

text = open(SRC, encoding='utf-8').read()


def body(method):
    start = text.index('private static bool %s(' % method)
    after = text.find('\n    private static ', start + 1)
    if after < 0:
        after = len(text)
    return text[start:after]


def pairs(source):
    return re.findall(r'0x([0-9A-Fa-f]+) => "([A-Za-z0-9]+)"', source)


# Per family: (comment, [(opcode, name), ...]) emitted after the ported
# entries. Opcodes in hex without the 0x, as pairs() returns them.
NATIVE_ONLY = {
    'vop3p': [(
        'Fused multiply-add on sources that are each a float or one half\n'
        'of a register as a half float; the result is a float, or a half\n'
        'written into one half of the destination.',
        [('20', 'VFmaMixF32'), ('21', 'VFmaMixloF16'),
         ('22', 'VFmaMixhiF16')],
    )],
}


def emit(found, indent='        ', family=None):
    lines = [
        '%scase 0x%s: decoded.name = "%s"; break;' % (indent, c.upper(), n)
        for c, n in found]
    ported = {int(c, 16) for c, _ in found}
    for comment, extra in NATIVE_ONLY.get(family, []):
        for c, n in extra:
            if int(c, 16) in ported:
                sys.exit('NATIVE_ONLY %s 0x%s is now in the C# table; '
                         'remove it from the list' % (family, c.upper()))
        lines += ['%s// %s' % (indent, l) for l in comment.split('\n')]
        lines += [
            '%scase 0x%s: decoded.name = "%s"; break;' % (indent, c.upper(), n)
            for c, n in extra]
    lines.append('%sdefault: break;' % indent)
    return '\n'.join(lines)


def decoded_cases(source):
    """(function, opcode, name) for every case of every decode_* function."""
    found = set()
    for fn, block in re.findall(
            r'inline DecodedOpcode (decode_\w+)\((.*?)\n}\n', source, re.S):
        branch = 0
        for line in block.split('\n'):
            if line.strip().startswith('if (is_vop3b)'):
                branch = 'b'
            elif line.strip() == 'return decoded;' and branch == 'b':
                branch = 'a'
            m = re.search(r'case 0x([0-9A-F]+): decoded.name = "(\w+)"', line)
            if m:
                found.add(('%s%s' % (fn, branch or ''), int(m.group(1), 16),
                           m.group(2)))
    return found


# VOP3 carries two tables chosen by a flag, split at the ':' of the
# conditional that picks between them.
vop3_body = body('DecodeVop3')
vop3_split = vop3_body.index(': opcode switch')
vop3_b = pairs(vop3_body[:vop3_split])
vop3_a = pairs(vop3_body[vop3_split:])

tables = {
    'vop3p': pairs(body('DecodeVop3p')),
    'ds': pairs(body('DecodeDs')),
    'mtbuf': pairs(body('DecodeMtbuf')),
    'mubuf': pairs(body('DecodeMubuf')),
    'flat': pairs(body('DecodeFlat')),
    'smrd': pairs(body('DecodeSmrd')),
    'smem': pairs(body('DecodeSmem')),
    'mimg': pairs(body('DecodeMimg')),
}

header = '''// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The three-operand and memory instruction families, ported from
// Gen5ShaderTranslator.cs.
//
// Fifth piece of the native shader path, and the last of the decoding. The
// opcode tables are emitted by tools/gen-memory-decoder.py; the lengths are
// written here because each family computes its own and a wrong length
// desynchronises every instruction after it rather than spoiling one.
// Opcodes the C# tables lack go into the generator's NATIVE_ONLY list, not
// straight into this file: the generator refuses to drop an opcode it
// cannot reproduce.
//
// The lengths, in the order they appear below:
//   VOP3 and VOP3P are two words, three when any of the three sources in
//     the second word reads the literal escape.
//   MTBUF and MUBUF are two words, three when the top byte of the second
//     word is the escape - a different place from VOP3's sources.
//   DS, FLAT and SMEM are always two.
//   SMRD is one, two when it names a literal offset rather than an
//     immediate one: the immediate flag has to be off AND the offset byte
//     has to be the escape, and testing either alone is wrong.
//   MIMG is two plus the two bits above the bottom of the word, so it
//     ranges from two to five.

#ifndef PS5_GEN5_DECODER_MEMORY_H
#define PS5_GEN5_DECODER_MEMORY_H

#include <cstdint>

#include "gen5_decoder_sop.h"

namespace ps5gen5 {

// The second word of a VOP3 holds three nine bit sources.
inline bool vop3_sources_have_literal(std::uint32_t extra) {
    return (extra & 0x1FFu) == 0xFFu ||
        ((extra >> 9) & 0x1FFu) == 0xFFu ||
        ((extra >> 18) & 0x1FFu) == 0xFFu;
}

// Which opcodes are read from VOP3's second table. It is a list rather than
// a range, and a decoder that always answers no fails on every carry-in add
// and subtract the title uses - three of the six shaders that would not
// decode stopped on 0x128 and 0x12A.
inline bool is_vop3b_opcode(std::uint32_t opcode) {
    switch (opcode) {
        case 0x128:
        case 0x129:
        case 0x12A:
        case 0x16D:
        case 0x16E:
        case 0x176:
        case 0x177:
        case 0x30F:
        case 0x310:
        case 0x319:
            return true;
        default:
            return false;
    }
}

inline DecodedOpcode decode_vop3(
    std::uint32_t word, std::uint32_t extra, bool is_vop3b) {
    const auto opcode = (word >> 16) & 0x3FFu;
    DecodedOpcode decoded;
    decoded.size_dwords = vop3_sources_have_literal(extra) ? 3u : 2u;
    if (is_vop3b) {
        switch (opcode) {
%s
        }
        return decoded;
    }
    switch (opcode) {
%s
    }
    return decoded;
}

inline DecodedOpcode decode_vop3p(std::uint32_t word, std::uint32_t extra) {
    const auto opcode = (word >> 16) & 0x7Fu;
    DecodedOpcode decoded;
    decoded.size_dwords = vop3_sources_have_literal(extra) ? 3u : 2u;
    switch (opcode) {
%s
    }
    return decoded;
}

inline DecodedOpcode decode_ds(std::uint32_t word) {
    const auto opcode = (word >> 18) & 0xFFu;
    DecodedOpcode decoded;
    decoded.size_dwords = 2;
    switch (opcode) {
%s
    }
    return decoded;
}

inline DecodedOpcode decode_mtbuf(std::uint32_t word, std::uint32_t extra) {
    const auto opcode = (word >> 16) & 0x7u;
    DecodedOpcode decoded;
    decoded.size_dwords = (extra >> 24) == 0xFFu ? 3u : 2u;
    switch (opcode) {
%s
    }
    return decoded;
}

inline DecodedOpcode decode_mubuf(std::uint32_t word, std::uint32_t extra) {
    const auto opcode = (word >> 18) & 0x7Fu;
    DecodedOpcode decoded;
    decoded.size_dwords = (extra >> 24) == 0xFFu ? 3u : 2u;
    switch (opcode) {
%s
    }
    return decoded;
}

inline DecodedOpcode decode_flat(std::uint32_t word) {
    const auto opcode = (word >> 18) & 0x7Fu;
    DecodedOpcode decoded;
    decoded.size_dwords = 2;
    switch (opcode) {
%s
    }
    return decoded;
}

// The only single word family here, and the only one whose length depends
// on two fields at once.
inline DecodedOpcode decode_smrd(std::uint32_t word) {
    const auto opcode = (word >> 22) & 0x1Fu;
    const auto offset = word & 0xFFu;
    const auto immediate_offset = ((word >> 8) & 1u) != 0u;
    DecodedOpcode decoded;
    decoded.size_dwords = (!immediate_offset && offset == 0xFFu) ? 2u : 1u;
    switch (opcode) {
%s
    }
    return decoded;
}

inline DecodedOpcode decode_smem(std::uint32_t word) {
    const auto opcode = (word >> 18) & 0xFFu;
    DecodedOpcode decoded;
    decoded.size_dwords = 2;
    switch (opcode) {
%s
    }
    return decoded;
}

// The opcode's top bit lives in the bottom bit of the word, and the length
// in the two bits above it.
inline DecodedOpcode decode_mimg(std::uint32_t word) {
    const auto opcode = ((word >> 18) & 0x7Fu) | ((word & 1u) << 7);
    DecodedOpcode decoded;
    decoded.size_dwords = 2 + ((word >> 1) & 0x3u);
    switch (opcode) {
%s
    }
    return decoded;
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_DECODER_MEMORY_H
''' % (emit(vop3_b, '            '), emit(vop3_a),
       emit(tables['vop3p'], family='vop3p'), emit(tables['ds']), emit(tables['mtbuf']),
       emit(tables['mubuf']), emit(tables['flat']), emit(tables['smrd']),
       emit(tables['smem']), emit(tables['mimg']))

if os.path.exists(OUT) and '--force' not in sys.argv[1:]:
    lost = sorted(decoded_cases(open(OUT, encoding='utf-8').read()) -
                  decoded_cases(header))
    if lost:
        for fn, opcode, name in lost:
            print('would lose %s 0x%X %s' % (fn, opcode, name))
        sys.exit('%s not written: add these to NATIVE_ONLY, or pass --force'
                 % os.path.basename(OUT))

open(OUT, 'w', encoding='utf-8', newline='\n').write(header)
counts = ' '.join('%s=%d' % (k, len(v)) for k, v in tables.items())
print('vop3b=%d vop3=%d %s' % (len(vop3_b), len(vop3_a), counts))
