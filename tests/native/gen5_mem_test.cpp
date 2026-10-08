// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Nine families, and every one computes its length differently. That is
// where the danger is: a wrong name spoils one instruction, a wrong length
// spoils every instruction after it.
#include <cstdio>

#include "gen5_decoder_memory.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

int main() {
    using namespace ps5gen5;

    // VOP3: three nine-bit sources in the second word, any of which can be
    // the escape. The fields do not start on byte boundaries, so a test
    // that only ever sets source 0 would pass against a wrong shift.
    check(decode_vop3(0, 0x000000FFu, false).size_dwords == 3,
          "VOP3 source 0 escape gives three words");
    check(decode_vop3(0, 0xFFu << 9, false).size_dwords == 3,
          "VOP3 source 1 escape gives three words");
    check(decode_vop3(0, 0xFFu << 18, false).size_dwords == 3,
          "VOP3 source 2 escape gives three words");
    check(decode_vop3(0, 0x00000100u, false).size_dwords == 2,
          "a source that is not the escape gives two words");
    // 0x1FF is nine bits set, which is not 0xFF: the field is nine bits and
    // the comparison must not be made after a byte mask.
    check(decode_vop3(0, 0x000001FFu, false).size_dwords == 2,
          "a nine bit source of all ones is not the escape");

    // VOP3 has two tables behind one opcode field.
    check(decode_vop3(0x128u << 16, 0, true).name == "VAddCoCiU32",
          "the VOP3B table is reached when the flag is set");
    check(decode_vop3(0x128u << 16, 0, false).name != "VAddCoCiU32",
          "the same opcode means something else in the other table");

    // MTBUF and MUBUF look at the top byte of the second word instead,
    // which is a different place from VOP3's sources.
    check(decode_mubuf(0, 0xFFu << 24).size_dwords == 3,
          "MUBUF escape is the top byte of the second word");
    check(decode_mubuf(0, 0x000000FFu).size_dwords == 2,
          "MUBUF does not look where VOP3 looks");
    check(decode_mtbuf(0, 0xFFu << 24).size_dwords == 3,
          "MTBUF uses the same place as MUBUF");

    // MTBUF's opcode is three bits, so its table cannot exceed eight and
    // the mask has to be three bits or neighbouring instructions collide.
    check(decode_mtbuf(0x0u << 16, 0).name == decode_mtbuf(0x8u << 16, 0).name,
          "MTBUF opcode wraps at three bits");

    // SMRD is the only single word family, and its length needs both
    // fields: the immediate flag off and the offset byte at the escape.
    check(decode_smrd(0x000000FFu).size_dwords == 2,
          "SMRD with a literal offset is two words");
    check(decode_smrd(0x000001FFu).size_dwords == 1,
          "the immediate flag alone makes it one word again");
    check(decode_smrd(0x000000FEu).size_dwords == 1,
          "an offset that is not the escape is one word");

    // MIMG carries the top opcode bit in the bottom bit of the word and
    // its length in the two bits above that.
    check(decode_mimg(0).size_dwords == 2, "MIMG is at least two words");
    check(decode_mimg(0x6u).size_dwords == 5, "MIMG can reach five words");
    check(decode_mimg(0x2u).size_dwords == 3, "MIMG length is two bits wide");
    const auto low = decode_mimg(0x00u << 18);
    const auto high = decode_mimg((0x00u << 18) | 1u);
    check(low.name != high.name || low.name.empty(),
          "the bottom bit of the word selects a different opcode");

    // The always-two families stay two whatever the word holds.
    check(decode_ds(0xFFFFFFFFu).size_dwords == 2, "DS is always two words");
    check(decode_flat(0xFFFFFFFFu).size_dwords == 2,
          "FLAT is always two words");
    check(decode_smem(0xFFFFFFFFu).size_dwords == 2,
          "SMEM is always two words");

    if (failures == 0) {
        std::printf("all memory decoder checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
