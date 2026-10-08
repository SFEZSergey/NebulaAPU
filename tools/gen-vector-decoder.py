# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only

"""Emit the vector instruction decoders from the C# they are ported from.

The opcode tables are long and flat, and transcribing them by hand invites
exactly the error that is hardest to find later: one wrong name in one of
two hundred cases, in a shader that is only reached sometimes. Reading the
source and emitting from it removes the possibility.
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src', 'Ps5Recomp.ShaderCompiler', 'Gen5ShaderTranslator.cs')
OUT = os.path.join(ROOT, 'runtime', 'native', 'gen5_decoder_vop.h')

text = open(SRC, encoding='utf-8').read()


def table(method):
    """The opcode->name pairs inside one decoder method."""
    start = text.index('private static bool %s(' % method)
    body = text[start:text.index('return FinishDecode', start)]
    return re.findall(r'0x([0-9A-Fa-f]+) => "([A-Za-z0-9]+)"', body)


def emit_switch(pairs, indent='        '):
    lines = []
    for code, name in pairs:
        lines.append('%scase 0x%s: decoded.name = "%s"; break;'
                     % (indent, code.upper(), name))
    lines.append('%sdefault: break;' % indent)
    return '\n'.join(lines)


vop1 = table('DecodeVop1')
vop2 = table('DecodeVop2')
vopc = table('DecodeVopc')

header = '''// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The vector instruction families, ported from Gen5ShaderTranslator.cs.
//
// Fourth piece of the native shader path. The opcode tables are emitted
// from the C# rather than transcribed, because one wrong name among two
// hundred flat cases is the kind of error that only shows up in a shader
// the title reaches sometimes.
//
// The literal escape is wider here than in the scalar families: a vector
// source is nine bits, and 0xE9, 0xEA, 0xF9, 0xFA and 0xFF all introduce a
// following word. VOP2 adds four opcodes that are always two words because
// they carry an inline constant of their own.

#ifndef PS5_GEN5_DECODER_VOP_H
#define PS5_GEN5_DECODER_VOP_H

#include <cstdint>

#include "gen5_decoder_sop.h"

namespace ps5gen5 {

// A nine bit vector source reading any of these is followed by a literal.
inline bool vector_source_has_literal(std::uint32_t src0) {
    return src0 == 0xE9u || src0 == 0xEAu || src0 == 0xF9u ||
        src0 == 0xFAu || src0 == 0xFFu;
}

inline DecodedOpcode decode_vop1(std::uint32_t word) {
    const auto opcode = (word >> 9) & 0xFFu;
    const auto src0 = word & 0x1FFu;
    DecodedOpcode decoded;
    decoded.size_dwords = vector_source_has_literal(src0) ? 2u : 1u;
    switch (opcode) {
%s
    }
    return decoded;
}

inline DecodedOpcode decode_vopc(std::uint32_t word) {
    const auto opcode = (word >> 17) & 0xFFu;
    const auto src0 = word & 0x1FFu;
    DecodedOpcode decoded;
    decoded.size_dwords = vector_source_has_literal(src0) ? 2u : 1u;
    switch (opcode) {
%s
    }
    return decoded;
}

// VOP2 hands two of its opcodes to the other two families: 0x3E is the
// whole of VOPC and 0x3F the whole of VOP1, which is how three encodings
// share one prefix. The four opcodes listed below carry an inline constant
// and are two words whatever their source says.
inline DecodedOpcode decode_vop2(std::uint32_t word) {
    const auto opcode = (word >> 25) & 0x3Fu;
    if (opcode == 0x3E) {
        return decode_vopc(word);
    }
    if (opcode == 0x3F) {
        return decode_vop1(word);
    }
    const auto src0 = word & 0x1FFu;
    const auto inline_constant =
        opcode == 0x20 || opcode == 0x21 || opcode == 0x2C || opcode == 0x2D;
    DecodedOpcode decoded;
    decoded.size_dwords =
        (inline_constant || vector_source_has_literal(src0)) ? 2u : 1u;
    switch (opcode) {
%s
    }
    return decoded;
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_DECODER_VOP_H
''' % (emit_switch(vop1), emit_switch(vopc), emit_switch(vop2))

open(OUT, 'w', encoding='utf-8', newline='\n').write(header)
print('vop1=%d vopc=%d vop2=%d entries' % (len(vop1), len(vopc), len(vop2)))
