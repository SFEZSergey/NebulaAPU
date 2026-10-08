// Copyright (C) 2026 SharpEmu Emulator Project
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
        case 0x00: decoded.name = "VNop"; break;
        case 0x01: decoded.name = "VMovB32"; break;
        case 0x02: decoded.name = "VReadfirstlaneB32"; break;
        case 0x05: decoded.name = "VCvtF32I32"; break;
        case 0x06: decoded.name = "VCvtF32U32"; break;
        case 0x07: decoded.name = "VCvtU32F32"; break;
        case 0x08: decoded.name = "VCvtI32F32"; break;
        case 0x0A: decoded.name = "VCvtF16F32"; break;
        case 0x0B: decoded.name = "VCvtF32F16"; break;
        case 0x0C: decoded.name = "VCvtRpiI32F32"; break;
        case 0x0D: decoded.name = "VCvtFlrI32F32"; break;
        case 0x0E: decoded.name = "VCvtOffF32I4"; break;
        case 0x11: decoded.name = "VCvtF32Ubyte0"; break;
        case 0x12: decoded.name = "VCvtF32Ubyte1"; break;
        case 0x13: decoded.name = "VCvtF32Ubyte2"; break;
        case 0x14: decoded.name = "VCvtF32Ubyte3"; break;
        case 0x20: decoded.name = "VFractF32"; break;
        case 0x21: decoded.name = "VTruncF32"; break;
        case 0x22: decoded.name = "VCeilF32"; break;
        case 0x23: decoded.name = "VRndneF32"; break;
        case 0x24: decoded.name = "VFloorF32"; break;
        case 0x25: decoded.name = "VExpF32"; break;
        case 0x27: decoded.name = "VLogF32"; break;
        case 0x2A: decoded.name = "VRcpF32"; break;
        case 0x2B: decoded.name = "VRcpIflagF32"; break;
        case 0x2E: decoded.name = "VRsqF32"; break;
        case 0x33: decoded.name = "VSqrtF32"; break;
        case 0x35: decoded.name = "VSinF32"; break;
        case 0x36: decoded.name = "VCosF32"; break;
        case 0x37: decoded.name = "VNotB32"; break;
        case 0x38: decoded.name = "VBfrevB32"; break;
        case 0x3A: decoded.name = "VFfblB32"; break;
        case 0x42: decoded.name = "VMovreldB32"; break;
        case 0x43: decoded.name = "VMovrelsB32"; break;
        case 0x44: decoded.name = "VMovrelsdB32"; break;
        default: break;
    }
    return decoded;
}

inline DecodedOpcode decode_vopc(std::uint32_t word) {
    const auto opcode = (word >> 17) & 0xFFu;
    const auto src0 = word & 0x1FFu;
    DecodedOpcode decoded;
    decoded.size_dwords = vector_source_has_literal(src0) ? 2u : 1u;
    switch (opcode) {
        case 0x00: decoded.name = "VCmpFF32"; break;
        case 0x01: decoded.name = "VCmpLtF32"; break;
        case 0x02: decoded.name = "VCmpEqF32"; break;
        case 0x03: decoded.name = "VCmpLeF32"; break;
        case 0x04: decoded.name = "VCmpGtF32"; break;
        case 0x05: decoded.name = "VCmpLgF32"; break;
        case 0x06: decoded.name = "VCmpGeF32"; break;
        case 0x07: decoded.name = "VCmpOF32"; break;
        case 0x08: decoded.name = "VCmpUF32"; break;
        case 0x09: decoded.name = "VCmpNgeF32"; break;
        case 0x0A: decoded.name = "VCmpNlgF32"; break;
        case 0x0B: decoded.name = "VCmpNgtF32"; break;
        case 0x0C: decoded.name = "VCmpNleF32"; break;
        case 0x0D: decoded.name = "VCmpNeqF32"; break;
        case 0x0E: decoded.name = "VCmpNltF32"; break;
        case 0x0F: decoded.name = "VCmpTruF32"; break;
        case 0x10: decoded.name = "VCmpxFF32"; break;
        case 0x11: decoded.name = "VCmpxLtF32"; break;
        case 0x12: decoded.name = "VCmpxEqF32"; break;
        case 0x13: decoded.name = "VCmpxLeF32"; break;
        case 0x14: decoded.name = "VCmpxGtF32"; break;
        case 0x15: decoded.name = "VCmpxLgF32"; break;
        case 0x16: decoded.name = "VCmpxGeF32"; break;
        case 0x17: decoded.name = "VCmpxOF32"; break;
        case 0x18: decoded.name = "VCmpxUF32"; break;
        case 0x19: decoded.name = "VCmpxNgeF32"; break;
        case 0x1A: decoded.name = "VCmpxNlgF32"; break;
        case 0x1B: decoded.name = "VCmpxNgtF32"; break;
        case 0x1C: decoded.name = "VCmpxNleF32"; break;
        case 0x1D: decoded.name = "VCmpxNeqF32"; break;
        case 0x1E: decoded.name = "VCmpxNltF32"; break;
        case 0x1F: decoded.name = "VCmpxTruF32"; break;
        case 0x80: decoded.name = "VCmpFI32"; break;
        case 0x81: decoded.name = "VCmpLtI32"; break;
        case 0x82: decoded.name = "VCmpEqI32"; break;
        case 0x83: decoded.name = "VCmpLeI32"; break;
        case 0x84: decoded.name = "VCmpGtI32"; break;
        case 0x85: decoded.name = "VCmpNeI32"; break;
        case 0x86: decoded.name = "VCmpGeI32"; break;
        case 0x87: decoded.name = "VCmpTI32"; break;
        case 0x88: decoded.name = "VCmpClassF32"; break;
        case 0x90: decoded.name = "VCmpxFI32"; break;
        case 0x91: decoded.name = "VCmpxLtI32"; break;
        case 0x92: decoded.name = "VCmpxEqI32"; break;
        case 0x93: decoded.name = "VCmpxLeI32"; break;
        case 0x94: decoded.name = "VCmpxGtI32"; break;
        case 0x95: decoded.name = "VCmpxNeI32"; break;
        case 0x96: decoded.name = "VCmpxGeI32"; break;
        case 0x97: decoded.name = "VCmpxTI32"; break;
        case 0xC0: decoded.name = "VCmpFU32"; break;
        case 0xC1: decoded.name = "VCmpLtU32"; break;
        case 0xC2: decoded.name = "VCmpEqU32"; break;
        case 0xC3: decoded.name = "VCmpLeU32"; break;
        case 0xC4: decoded.name = "VCmpGtU32"; break;
        case 0xC5: decoded.name = "VCmpNeU32"; break;
        case 0xC6: decoded.name = "VCmpGeU32"; break;
        case 0xC7: decoded.name = "VCmpTU32"; break;
        case 0xD0: decoded.name = "VCmpxFU32"; break;
        case 0xD1: decoded.name = "VCmpxLtU32"; break;
        case 0xD2: decoded.name = "VCmpxEqU32"; break;
        case 0xD3: decoded.name = "VCmpxLeU32"; break;
        case 0xD4: decoded.name = "VCmpxGtU32"; break;
        case 0xD5: decoded.name = "VCmpxNeU32"; break;
        case 0xD6: decoded.name = "VCmpxGeU32"; break;
        case 0xD7: decoded.name = "VCmpxTU32"; break;
        default: break;
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
        case 0x01: decoded.name = "VCndmaskB32"; break;
        case 0x02: decoded.name = "VDot2cF32F16"; break;
        case 0x03: decoded.name = "VAddF32"; break;
        case 0x04: decoded.name = "VSubF32"; break;
        case 0x05: decoded.name = "VSubrevF32"; break;
        case 0x08: decoded.name = "VMulF32"; break;
        case 0x0B: decoded.name = "VMulU32U24"; break;
        case 0x0C: decoded.name = "VMulHiU32U24"; break;
        case 0x0F: decoded.name = "VMinF32"; break;
        case 0x10: decoded.name = "VMaxF32"; break;
        case 0x11: decoded.name = "VMinI32"; break;
        case 0x12: decoded.name = "VMaxI32"; break;
        case 0x13: decoded.name = "VMinU32"; break;
        case 0x14: decoded.name = "VMaxU32"; break;
        case 0x15: decoded.name = "VLshrB32"; break;
        case 0x16: decoded.name = "VLshrrevB32"; break;
        case 0x17: decoded.name = "VAshrI32"; break;
        case 0x18: decoded.name = "VAshrrevI32"; break;
        case 0x19: decoded.name = "VLshlB32"; break;
        case 0x1A: decoded.name = "VLshlrevB32"; break;
        case 0x1B: decoded.name = "VAndB32"; break;
        case 0x1C: decoded.name = "VOrB32"; break;
        case 0x1D: decoded.name = "VXorB32"; break;
        case 0x1E: decoded.name = "VXnorB32"; break;
        case 0x1F: decoded.name = "VMacF32"; break;
        case 0x20: decoded.name = "VMadMkF32"; break;
        case 0x21: decoded.name = "VMadAkF32"; break;
        case 0x22: decoded.name = "VBcntU32B32"; break;
        case 0x23: decoded.name = "VMbcntLoU32B32"; break;
        case 0x24: decoded.name = "VMbcntHiU32B32"; break;
        case 0x25: decoded.name = "VAddI32"; break;
        case 0x26: decoded.name = "VSubI32"; break;
        case 0x27: decoded.name = "VSubrevI32"; break;
        case 0x28: decoded.name = "VAddcU32"; break;
        case 0x29: decoded.name = "VSubbU32"; break;
        case 0x2A: decoded.name = "VSubbrevU32"; break;
        case 0x2B: decoded.name = "VFmacF32"; break;
        case 0x2C: decoded.name = "VFmaMkF32"; break;
        case 0x2D: decoded.name = "VFmaAkF32"; break;
        case 0x2F: decoded.name = "VCvtPkrtzF16F32"; break;
        case 0x30: decoded.name = "VCvtPkU16U32"; break;
        case 0x31: decoded.name = "VCvtPkI16I32"; break;
        default: break;
    }
    return decoded;
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_DECODER_VOP_H
