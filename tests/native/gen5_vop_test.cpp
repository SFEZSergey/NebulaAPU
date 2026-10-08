// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// The vector families share one prefix and hand opcodes to each other, so
// the dispatch is the thing worth testing, along with the two separate
// reasons a vector instruction is two words rather than one.
#include <cstdio>

#include "gen5_decoder_vop.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

int main() {
    using namespace ps5gen5;

    // VOP2 gives 0x3E to VOPC and 0x3F to VOP1. Those are not VOP2 opcodes
    // with odd names, they are other encodings sharing the prefix, and a
    // decoder that treats them as VOP2 returns nothing for every compare
    // and every move in the program.
    const auto to_vopc = (0x3Eu << 25) | (0x02u << 17);
    check(decode_vop2(to_vopc).name == "VCmpEqF32",
          "VOP2 opcode 0x3E is the whole of VOPC");
    const auto to_vop1 = (0x3Fu << 25) | (0x01u << 9);
    check(decode_vop2(to_vop1).name == "VMovB32",
          "VOP2 opcode 0x3F is the whole of VOP1");

    // Ordinary VOP2 still decodes.
    check(decode_vop2((0x03u << 25)).name == "VAddF32",
          "VOP2 decodes its own opcodes");

    // Two reasons for a second word, and they are independent. The source
    // escape is wider than the scalar families' single 0xFF.
    check(decode_vop1((0x01u << 9) | 0x02u).size_dwords == 1,
          "an ordinary vector source is one word");
    for (const std::uint32_t escape : {0xE9u, 0xEAu, 0xF9u, 0xFAu, 0xFFu}) {
        check(decode_vop1((0x01u << 9) | escape).size_dwords == 2,
              "every vector literal escape gives two words");
    }
    check(decode_vop1((0x01u << 9) | 0xE8u).size_dwords == 1,
          "a source next to an escape is not an escape");

    // The inline-constant opcodes are two words whatever the source says.
    for (const std::uint32_t opcode : {0x20u, 0x21u, 0x2Cu, 0x2Du}) {
        check(decode_vop2((opcode << 25) | 0x02u).size_dwords == 2,
              "inline constant opcodes are always two words");
    }
    check(decode_vop2((0x1Fu << 25) | 0x02u).size_dwords == 1,
          "a neighbouring opcode is not an inline constant one");

    // VOPC's table is wide and sparse: the integer compares sit at 0x80 and
    // the exec-writing forms 0x10 above each float and integer block.
    check(decode_vopc((0x00u << 17)).name == "VCmpFF32",
          "VOPC starts at the float compares");
    check(decode_vopc((0x10u << 17)).name == "VCmpxFF32",
          "the exec-writing float compares are 0x10 higher");
    check(decode_vopc((0x80u << 17)).name == "VCmpFI32",
          "the integer compares start at 0x80");
    check(decode_vopc((0xD7u << 17)).name == "VCmpxTU32",
          "the last VOPC entry decodes");
    check(decode_vopc((0x40u << 17)).name.empty(),
          "a gap in the table is a gap, not a neighbour's name");

    if (failures == 0) {
        std::printf("all VOP decoder checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
