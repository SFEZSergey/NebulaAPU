// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Checks the scalar decoders on the things that decide behaviour: which
// family a word belongs to, and how long the instruction is. A wrong length
// does not produce a wrong instruction, it produces a wrong program - the
// walk lands inside the next instruction and never recovers.
#include <cstdio>

#include "gen5_decoder_sop.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

// Builds a word with the SOP2-position opcode field set, which is what the
// dispatch reads.
static std::uint32_t sop_word(std::uint32_t opcode7, std::uint32_t low = 0) {
    return ((opcode7 & 0x7Fu) << 23) | low;
}

int main() {
    using namespace ps5gen5;

    // Family dispatch, including the boundaries. 0x60 is the first SOPK and
    // 0x5F the last SOP2; 0x7D, 0x7E and 0x7F are claimed before the SOPK
    // range can take them.
    check(decode_sop(sop_word(0x7D, (0x03u << 8))).name == "SMovB32",
          "0x7D dispatches to SOP1");
    check(decode_sop(sop_word(0x7E, 0)).name == "SCmpEqI32",
          "0x7E dispatches to SOPC");
    check(decode_sop(sop_word(0x7F, (0x01u << 16))).name == "SEndpgm",
          "0x7F dispatches to SOPP");
    check(decode_sop(sop_word(0x60)).name == "SMovkI32",
          "0x60 is the first SOPK");
    check(decode_sop(sop_word(0x02)).name == "SAddI32",
          "below 0x60 is SOP2");
    check(decode_sop(sop_word(0x5F)).name.empty(),
          "0x5F is SOP2 and has no opcode there");

    // Lengths. The literal escape is 0xFF in a source field, and SOP2 and
    // SOPC take it in either source.
    check(decode_sop1(sop_word(0x7D, (0x03u << 8) | 0x02u)).size_dwords == 1,
          "SOP1 without a literal is one word");
    check(decode_sop1(sop_word(0x7D, (0x03u << 8) | 0xFFu)).size_dwords == 2,
          "SOP1 with a literal source is two words");
    check(decode_sop2(sop_word(0x00, 0x0102u)).size_dwords == 1,
          "SOP2 without a literal is one word");
    check(decode_sop2(sop_word(0x00, 0x00FFu)).size_dwords == 2,
          "SOP2 with a literal in source 0 is two words");
    check(decode_sop2(sop_word(0x00, 0xFF02u)).size_dwords == 2,
          "SOP2 with a literal in source 1 is two words");
    check(decode_sopc(sop_word(0x7E, 0x00FFu)).size_dwords == 2,
          "SOPC takes the literal escape too");
    check(decode_sopp(sop_word(0x7F, (0x0Cu << 16) | 0xFFu)).size_dwords == 1,
          "SOPP is one word whatever the low bits say");
    check(decode_sopk(sop_word(0x60, 0xFFFFu)).size_dwords == 1,
          "SOPK is one word, its immediate is in the instruction");

    // SOPK's opcode is biased, so the bias has to be applied before the
    // lookup or every SOPK decodes as the wrong instruction.
    check(decode_sop(sop_word(0x60 + 0x0F)).name == "SAddkI32",
          "SOPK opcodes are biased by 0x60");
    check(decode_sop(sop_word(0x60 + 0x17)).name == "SWaitcntVscnt",
          "the last SOPK decodes");

    // Unknown opcodes report rather than inventing a name, and still carry
    // a length so a caller that skips them stays aligned.
    const auto unknown = decode_sop2(sop_word(0x3F, 0x00FFu));
    check(!unknown.ok(), "an unknown SOP2 opcode is not ok");
    check(unknown.size_dwords == 2,
          "an unknown opcode still reports its length");

    // Names are views onto literals: decoding must not allocate, and the
    // same word must give the same pointer twice.
    check(decode_sop(sop_word(0x02)).name.data() ==
              decode_sop(sop_word(0x02)).name.data(),
          "names are stable views, not fresh strings");

    if (failures == 0) {
        std::printf("all SOP decoder checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
