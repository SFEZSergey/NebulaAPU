// Copyright (C) 2026 SharpEmu Emulator Project
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
            case 0x128: decoded.name = "VAddCoCiU32"; break;
            case 0x129: decoded.name = "VSubCoCiU32"; break;
            case 0x12A: decoded.name = "VSubrevCoCiU32"; break;
            case 0x30F: decoded.name = "VAddCoU32"; break;
            case 0x310: decoded.name = "VSubCoU32"; break;
            case 0x319: decoded.name = "VSubrevCoU32"; break;
            case 0x176: decoded.name = "VMadU64U32"; break;
            default: break;
        }
        return decoded;
    }
    switch (opcode) {
        case 0x101: decoded.name = "VCndmaskB32"; break;
        case 0x103: decoded.name = "VAddF32"; break;
        case 0x104: decoded.name = "VSubF32"; break;
        case 0x108: decoded.name = "VMulF32"; break;
        case 0x10F: decoded.name = "VMinF32"; break;
        case 0x110: decoded.name = "VMaxF32"; break;
        case 0x11F: decoded.name = "VMacF32"; break;
        case 0x12B: decoded.name = "VFmacF32"; break;
        case 0x12F: decoded.name = "VCvtPkrtzF16F32"; break;
        case 0x141: decoded.name = "VMadF32"; break;
        case 0x143: decoded.name = "VMadU32U24"; break;
        case 0x144: decoded.name = "VCubeidF32"; break;
        case 0x145: decoded.name = "VCubescF32"; break;
        case 0x146: decoded.name = "VCubetcF32"; break;
        case 0x147: decoded.name = "VCubemaF32"; break;
        case 0x149: decoded.name = "VBfeI32"; break;
        case 0x14A: decoded.name = "VBfiB32"; break;
        case 0x14B: decoded.name = "VFmaF32"; break;
        case 0x151: decoded.name = "VMin3F32"; break;
        case 0x152: decoded.name = "VMin3I32"; break;
        case 0x153: decoded.name = "VMin3U32"; break;
        case 0x154: decoded.name = "VMax3F32"; break;
        case 0x155: decoded.name = "VMax3I32"; break;
        case 0x156: decoded.name = "VMax3U32"; break;
        case 0x157: decoded.name = "VMed3F32"; break;
        case 0x158: decoded.name = "VMed3I32"; break;
        case 0x159: decoded.name = "VMed3U32"; break;
        case 0x15A: decoded.name = "VSadU8"; break;
        case 0x15B: decoded.name = "VSadHiU8"; break;
        case 0x15C: decoded.name = "VSadU16"; break;
        case 0x15D: decoded.name = "VSadU32"; break;
        case 0x15E: decoded.name = "VCvtPkU8F32"; break;
        case 0x148: decoded.name = "VBfeU32"; break;
        case 0x169: decoded.name = "VMulLoU32"; break;
        case 0x16A: decoded.name = "VMulHiU32"; break;
        case 0x16B: decoded.name = "VMulLoI32"; break;
        case 0x16C: decoded.name = "VMulHiI32"; break;
        case 0x0E0: decoded.name = "VCmpFU64"; break;
        case 0x0E1: decoded.name = "VCmpLtU64"; break;
        case 0x0E2: decoded.name = "VCmpEqU64"; break;
        case 0x0E3: decoded.name = "VCmpLeU64"; break;
        case 0x0E4: decoded.name = "VCmpGtU64"; break;
        case 0x0E5: decoded.name = "VCmpNeU64"; break;
        case 0x0E6: decoded.name = "VCmpGeU64"; break;
        case 0x0E7: decoded.name = "VCmpTU64"; break;
        case 0x0F0: decoded.name = "VCmpxFU64"; break;
        case 0x0F1: decoded.name = "VCmpxLtU64"; break;
        case 0x0F2: decoded.name = "VCmpxEqU64"; break;
        case 0x0F3: decoded.name = "VCmpxLeU64"; break;
        case 0x0F4: decoded.name = "VCmpxGtU64"; break;
        case 0x0F5: decoded.name = "VCmpxNeU64"; break;
        case 0x0F6: decoded.name = "VCmpxGeU64"; break;
        case 0x0F7: decoded.name = "VCmpxTU64"; break;
        case 0x360: decoded.name = "VReadlaneB32"; break;
        case 0x361: decoded.name = "VWritelaneB32"; break;
        case 0x362: decoded.name = "VLdexpF32"; break;
        case 0x363: decoded.name = "VBfmB32"; break;
        case 0x364: decoded.name = "VBcntU32B32"; break;
        case 0x365: decoded.name = "VMbcntLoU32B32"; break;
        case 0x366: decoded.name = "VMbcntHiU32B32"; break;
        case 0x368: decoded.name = "VCvtPknormI16F32"; break;
        case 0x369: decoded.name = "VCvtPknormU16F32"; break;
        case 0x373: decoded.name = "VMadU32U16"; break;
        case 0x346: decoded.name = "VLshlAddU32"; break;
        case 0x347: decoded.name = "VAddLshlU32"; break;
        case 0x36D: decoded.name = "VAdd3U32"; break;
        case 0x36F: decoded.name = "VLshlOrU32"; break;
        case 0x371: decoded.name = "VAndOrB32"; break;
        case 0x372: decoded.name = "VOr3U32"; break;
        case 0x377: decoded.name = "VPermlane16B32"; break;
        case 0x378: decoded.name = "VPermlanex16B32"; break;
        default: break;
    }
    return decoded;
}

inline DecodedOpcode decode_vop3p(std::uint32_t word, std::uint32_t extra) {
    const auto opcode = (word >> 16) & 0x7Fu;
    DecodedOpcode decoded;
    decoded.size_dwords = vop3_sources_have_literal(extra) ? 3u : 2u;
    switch (opcode) {
        case 0x0E: decoded.name = "VPkFmaF16"; break;
        case 0x0F: decoded.name = "VPkAddF16"; break;
        case 0x10: decoded.name = "VPkMulF16"; break;
        case 0x11: decoded.name = "VPkMinF16"; break;
        case 0x12: decoded.name = "VPkMaxF16"; break;
        // Fused multiply-add on sources that are each a float or one half
        // of a register as a half float; the result is a float, or a half
        // written into one half of the destination.
        case 0x20: decoded.name = "VFmaMixF32"; break;
        case 0x21: decoded.name = "VFmaMixloF16"; break;
        case 0x22: decoded.name = "VFmaMixhiF16"; break;
        default: break;
    }
    return decoded;
}

inline DecodedOpcode decode_ds(std::uint32_t word) {
    const auto opcode = (word >> 18) & 0xFFu;
    DecodedOpcode decoded;
    decoded.size_dwords = 2;
    switch (opcode) {
        case 0x00: decoded.name = "DsAddU32"; break;
        case 0x01: decoded.name = "DsSubU32"; break;
        case 0x03: decoded.name = "DsIncU32"; break;
        case 0x04: decoded.name = "DsDecU32"; break;
        case 0x05: decoded.name = "DsMinI32"; break;
        case 0x06: decoded.name = "DsMaxI32"; break;
        case 0x07: decoded.name = "DsMinU32"; break;
        case 0x08: decoded.name = "DsMaxU32"; break;
        case 0x09: decoded.name = "DsAndB32"; break;
        case 0x0A: decoded.name = "DsOrB32"; break;
        case 0x0B: decoded.name = "DsXorB32"; break;
        case 0x0D: decoded.name = "DsWriteB32"; break;
        case 0x0E: decoded.name = "DsWrite2B32"; break;
        case 0x0F: decoded.name = "DsWrite2St64B32"; break;
        case 0x10: decoded.name = "DsCmpstB32"; break;
        case 0x20: decoded.name = "DsAddRtnU32"; break;
        case 0x21: decoded.name = "DsSubRtnU32"; break;
        case 0x23: decoded.name = "DsIncRtnU32"; break;
        case 0x24: decoded.name = "DsDecRtnU32"; break;
        case 0x25: decoded.name = "DsMinRtnI32"; break;
        case 0x26: decoded.name = "DsMaxRtnI32"; break;
        case 0x27: decoded.name = "DsMinRtnU32"; break;
        case 0x28: decoded.name = "DsMaxRtnU32"; break;
        case 0x29: decoded.name = "DsAndRtnB32"; break;
        case 0x2A: decoded.name = "DsOrRtnB32"; break;
        case 0x2B: decoded.name = "DsXorRtnB32"; break;
        case 0x2D: decoded.name = "DsWrxchgRtnB32"; break;
        case 0x30: decoded.name = "DsCmpstRtnB32"; break;
        case 0x35: decoded.name = "DsSwizzleB32"; break;
        case 0x36: decoded.name = "DsReadB32"; break;
        case 0x37: decoded.name = "DsRead2B32"; break;
        case 0x38: decoded.name = "DsRead2St64B32"; break;
        case 0x3E: decoded.name = "DsAppend"; break;
        case 0x4D: decoded.name = "DsWriteB64"; break;
        case 0x76: decoded.name = "DsReadB64"; break;
        case 0xDE: decoded.name = "DsWriteB96"; break;
        case 0xDF: decoded.name = "DsWriteB128"; break;
        case 0xFE: decoded.name = "DsReadB96"; break;
        case 0xFF: decoded.name = "DsReadB128"; break;
        default: break;
    }
    return decoded;
}

inline DecodedOpcode decode_mtbuf(std::uint32_t word, std::uint32_t extra) {
    const auto opcode = (word >> 16) & 0x7u;
    DecodedOpcode decoded;
    decoded.size_dwords = (extra >> 24) == 0xFFu ? 3u : 2u;
    switch (opcode) {
        case 0x00: decoded.name = "TBufferLoadFormatX"; break;
        case 0x01: decoded.name = "TBufferLoadFormatXy"; break;
        case 0x02: decoded.name = "TBufferLoadFormatXyz"; break;
        case 0x03: decoded.name = "TBufferLoadFormatXyzw"; break;
        case 0x04: decoded.name = "TBufferStoreFormatX"; break;
        case 0x05: decoded.name = "TBufferStoreFormatXy"; break;
        case 0x06: decoded.name = "TBufferStoreFormatXyz"; break;
        case 0x07: decoded.name = "TBufferStoreFormatXyzw"; break;
        default: break;
    }
    return decoded;
}

inline DecodedOpcode decode_mubuf(std::uint32_t word, std::uint32_t extra) {
    const auto opcode = (word >> 18) & 0x7Fu;
    DecodedOpcode decoded;
    decoded.size_dwords = (extra >> 24) == 0xFFu ? 3u : 2u;
    switch (opcode) {
        case 0x00: decoded.name = "BufferLoadFormatX"; break;
        case 0x01: decoded.name = "BufferLoadFormatXy"; break;
        case 0x02: decoded.name = "BufferLoadFormatXyz"; break;
        case 0x03: decoded.name = "BufferLoadFormatXyzw"; break;
        case 0x04: decoded.name = "BufferStoreFormatX"; break;
        case 0x05: decoded.name = "BufferStoreFormatXy"; break;
        case 0x06: decoded.name = "BufferStoreFormatXyz"; break;
        case 0x07: decoded.name = "BufferStoreFormatXyzw"; break;
        case 0x08: decoded.name = "BufferLoadUbyte"; break;
        case 0x09: decoded.name = "BufferLoadSbyte"; break;
        case 0x0A: decoded.name = "BufferLoadUshort"; break;
        case 0x0B: decoded.name = "BufferLoadSshort"; break;
        case 0x0C: decoded.name = "BufferLoadDword"; break;
        case 0x0D: decoded.name = "BufferLoadDwordx2"; break;
        case 0x0E: decoded.name = "BufferLoadDwordx4"; break;
        case 0x0F: decoded.name = "BufferLoadDwordx3"; break;
        case 0x18: decoded.name = "BufferStoreByte"; break;
        case 0x19: decoded.name = "BufferStoreByteD16Hi"; break;
        case 0x1A: decoded.name = "BufferStoreShort"; break;
        case 0x1B: decoded.name = "BufferStoreShortD16Hi"; break;
        case 0x1C: decoded.name = "BufferStoreDword"; break;
        case 0x1D: decoded.name = "BufferStoreDwordx2"; break;
        case 0x1E: decoded.name = "BufferStoreDwordx4"; break;
        case 0x1F: decoded.name = "BufferStoreDwordx3"; break;
        case 0x20: decoded.name = "BufferLoadUbyteD16"; break;
        case 0x21: decoded.name = "BufferLoadUbyteD16Hi"; break;
        case 0x22: decoded.name = "BufferLoadSbyteD16"; break;
        case 0x23: decoded.name = "BufferLoadSbyteD16Hi"; break;
        case 0x24: decoded.name = "BufferLoadShortD16"; break;
        case 0x25: decoded.name = "BufferLoadShortD16Hi"; break;
        case 0x30: decoded.name = "BufferAtomicSwap"; break;
        case 0x31: decoded.name = "BufferAtomicCmpswap"; break;
        case 0x32: decoded.name = "BufferAtomicAdd"; break;
        case 0x33: decoded.name = "BufferAtomicSub"; break;
        case 0x35: decoded.name = "BufferAtomicSmin"; break;
        case 0x36: decoded.name = "BufferAtomicUmin"; break;
        case 0x37: decoded.name = "BufferAtomicSmax"; break;
        case 0x38: decoded.name = "BufferAtomicUmax"; break;
        case 0x39: decoded.name = "BufferAtomicAnd"; break;
        case 0x3A: decoded.name = "BufferAtomicOr"; break;
        case 0x3B: decoded.name = "BufferAtomicXor"; break;
        case 0x3C: decoded.name = "BufferAtomicInc"; break;
        case 0x3D: decoded.name = "BufferAtomicDec"; break;
        default: break;
    }
    return decoded;
}

inline DecodedOpcode decode_flat(std::uint32_t word) {
    const auto opcode = (word >> 18) & 0x7Fu;
    DecodedOpcode decoded;
    decoded.size_dwords = 2;
    switch (opcode) {
        case 0x08: decoded.name = "GlobalLoadUbyte"; break;
        case 0x09: decoded.name = "GlobalLoadSbyte"; break;
        case 0x0A: decoded.name = "GlobalLoadUshort"; break;
        case 0x0B: decoded.name = "GlobalLoadSshort"; break;
        case 0x0C: decoded.name = "GlobalLoadDword"; break;
        case 0x0D: decoded.name = "GlobalLoadDwordx2"; break;
        case 0x0E: decoded.name = "GlobalLoadDwordx4"; break;
        case 0x0F: decoded.name = "GlobalLoadDwordx3"; break;
        case 0x18: decoded.name = "GlobalStoreByte"; break;
        case 0x19: decoded.name = "GlobalStoreByteD16Hi"; break;
        case 0x1A: decoded.name = "GlobalStoreShort"; break;
        case 0x1B: decoded.name = "GlobalStoreShortD16Hi"; break;
        case 0x1C: decoded.name = "GlobalStoreDword"; break;
        case 0x1D: decoded.name = "GlobalStoreDwordx2"; break;
        case 0x1E: decoded.name = "GlobalStoreDwordx4"; break;
        case 0x1F: decoded.name = "GlobalStoreDwordx3"; break;
        case 0x20: decoded.name = "GlobalLoadUbyteD16"; break;
        case 0x21: decoded.name = "GlobalLoadUbyteD16Hi"; break;
        case 0x22: decoded.name = "GlobalLoadSbyteD16"; break;
        case 0x23: decoded.name = "GlobalLoadSbyteD16Hi"; break;
        case 0x24: decoded.name = "GlobalLoadShortD16"; break;
        case 0x25: decoded.name = "GlobalLoadShortD16Hi"; break;
        case 0x32: decoded.name = "GlobalAtomicAdd"; break;
        case 0x38: decoded.name = "GlobalAtomicUMax"; break;
        default: break;
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
        case 0x00: decoded.name = "SLoadDword"; break;
        case 0x01: decoded.name = "SLoadDwordx2"; break;
        case 0x02: decoded.name = "SLoadDwordx4"; break;
        case 0x03: decoded.name = "SLoadDwordx8"; break;
        case 0x04: decoded.name = "SLoadDwordx16"; break;
        case 0x08: decoded.name = "SBufferLoadDword"; break;
        case 0x09: decoded.name = "SBufferLoadDwordx2"; break;
        case 0x0A: decoded.name = "SBufferLoadDwordx4"; break;
        case 0x0B: decoded.name = "SBufferLoadDwordx8"; break;
        case 0x0C: decoded.name = "SBufferLoadDwordx16"; break;
        default: break;
    }
    return decoded;
}

inline DecodedOpcode decode_smem(std::uint32_t word) {
    const auto opcode = (word >> 18) & 0xFFu;
    DecodedOpcode decoded;
    decoded.size_dwords = 2;
    switch (opcode) {
        case 0x00: decoded.name = "SLoadDword"; break;
        case 0x01: decoded.name = "SLoadDwordx2"; break;
        case 0x02: decoded.name = "SLoadDwordx4"; break;
        case 0x03: decoded.name = "SLoadDwordx8"; break;
        case 0x04: decoded.name = "SLoadDwordx16"; break;
        case 0x08: decoded.name = "SBufferLoadDword"; break;
        case 0x09: decoded.name = "SBufferLoadDwordx2"; break;
        case 0x0A: decoded.name = "SBufferLoadDwordx4"; break;
        case 0x0B: decoded.name = "SBufferLoadDwordx8"; break;
        case 0x0C: decoded.name = "SBufferLoadDwordx16"; break;
        default: break;
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
        case 0x00: decoded.name = "ImageLoad"; break;
        case 0x01: decoded.name = "ImageLoadMip"; break;
        case 0x08: decoded.name = "ImageStore"; break;
        case 0x09: decoded.name = "ImageStoreMip"; break;
        case 0x0E: decoded.name = "ImageGetResinfo"; break;
        case 0x0F: decoded.name = "ImageAtomicSwap"; break;
        case 0x10: decoded.name = "ImageAtomicCmpswap"; break;
        case 0x11: decoded.name = "ImageAtomicAdd"; break;
        case 0x12: decoded.name = "ImageAtomicSub"; break;
        case 0x14: decoded.name = "ImageAtomicSmin"; break;
        case 0x15: decoded.name = "ImageAtomicUmin"; break;
        case 0x16: decoded.name = "ImageAtomicSmax"; break;
        case 0x17: decoded.name = "ImageAtomicUmax"; break;
        case 0x18: decoded.name = "ImageAtomicAnd"; break;
        case 0x19: decoded.name = "ImageAtomicOr"; break;
        case 0x1A: decoded.name = "ImageAtomicXor"; break;
        case 0x1B: decoded.name = "ImageAtomicInc"; break;
        case 0x1C: decoded.name = "ImageAtomicDec"; break;
        case 0x20: decoded.name = "ImageSample"; break;
        case 0x22: decoded.name = "ImageSampleD"; break;
        case 0x24: decoded.name = "ImageSampleL"; break;
        case 0x25: decoded.name = "ImageSampleB"; break;
        case 0x27: decoded.name = "ImageSampleLz"; break;
        case 0x2F: decoded.name = "ImageSampleCLz"; break;
        case 0x30: decoded.name = "ImageSampleO"; break;
        case 0x34: decoded.name = "ImageSampleLO"; break;
        case 0x37: decoded.name = "ImageSampleLzO"; break;
        case 0x40: decoded.name = "ImageGather4"; break;
        case 0x47: decoded.name = "ImageGather4Lz"; break;
        case 0x48: decoded.name = "ImageGather4C"; break;
        case 0x4E: decoded.name = "ImageGather4CBCl"; break;
        case 0x57: decoded.name = "ImageGather4LzO"; break;
        case 0x5F: decoded.name = "ImageGather4CLzO"; break;
        case 0xE6: decoded.name = "ImageBvhIntersectRay"; break;
        default: break;
    }
    return decoded;
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_DECODER_MEMORY_H
