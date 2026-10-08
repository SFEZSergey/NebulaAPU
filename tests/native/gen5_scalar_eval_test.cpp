// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// The scalar interpreter finds where a shader's resources are, so what
// matters is that it computes the same addresses the hardware would and
// that it knows when it does not know. A resolved address that is wrong is
// worse than one that is missing: the first binds the wrong memory, the
// second declines to bind.
#include <cstdio>
#include <map>
#include <vector>

#include "gen5_scalar_eval.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

struct Program {
    std::map<std::uint32_t, std::uint32_t> words;

    void sop2(std::uint32_t pc, std::uint32_t opcode, std::uint32_t dst,
              std::uint32_t src0, std::uint32_t src1) {
        words[pc] = 0x80000000u | ((opcode & 0x7Fu) << 23) |
            ((dst & 0x7Fu) << 16) | ((src1 & 0xFFu) << 8) | (src0 & 0xFFu);
    }
    void sop1(std::uint32_t pc, std::uint32_t opcode, std::uint32_t dst,
              std::uint32_t src0) {
        words[pc] = 0x80000000u | (0x7Du << 23) | ((dst & 0x7Fu) << 16) |
            ((opcode & 0xFFu) << 8) | (src0 & 0xFFu);
    }
    // SMRD: opcode in bits 22..26, destination 15..21, base pair 9..14,
    // immediate flag 8, offset 0..7. The fields abut, so packing one of
    // them in the wrong place quietly changes another - which is how the
    // evaluator's first attempt read its destination four registers along.
    void sload(std::uint32_t pc, std::uint32_t opcode, std::uint32_t dst,
               std::uint32_t base_pair, std::uint32_t offset) {
        words[pc] = 0xC0000000u | ((opcode & 0x1Fu) << 22) |
            ((dst & 0x7Fu) << 15) | ((base_pair & 0x3Fu) << 9) | (1u << 8) |
            (offset & 0xFFu);
    }
    // SOPC: opcode in bits 16..22, two sources, result to the condition
    // code and nowhere else. Bits 16..22 are a destination in SOP1 and
    // SOP2 but not here, which is what the evaluator used to get wrong.
    void sopc(std::uint32_t pc, std::uint32_t opcode, std::uint32_t src0,
              std::uint32_t src1) {
        words[pc] = 0x80000000u | (0x7Eu << 23) | ((opcode & 0x7Fu) << 16) |
            ((src1 & 0xFFu) << 8) | (src0 & 0xFFu);
    }
    void sopp(std::uint32_t pc, std::uint32_t opcode) {
        words[pc] = 0xBF800000u | ((opcode & 0x7Fu) << 16);
    }
    // MIMG is two words. The second holds the vector address at the
    // bottom, the vector data above it, then the resource at 16 and the
    // sampler at 21 - five bits each, naming a group of four registers.
    // Reading those two fields five bits high still resolves about half the
    // descriptors, because the sampler group often sits near the resource
    // group, which is why this needs a test rather than a glance.
    void image_sample(std::uint32_t pc, std::uint32_t srsrc_group,
                      std::uint32_t ssamp_group) {
        words[pc] = 0xF0000000u | (0x20u << 18) | (0xFu << 8);
        words[pc + 1] = ((ssamp_group & 0x1Fu) << 21) |
            ((srsrc_group & 0x1Fu) << 16);
    }
    void endpgm(std::uint32_t pc) {
        words[pc] = 0xBF800000u | (0x01u << 16);
    }
    void branch_if(std::uint32_t pc, std::int16_t offset) {
        words[pc] = 0xBF800000u | (0x04u << 16) |
            static_cast<std::uint16_t>(offset);
    }
    std::uint32_t operator()(std::uint32_t pc) const {
        const auto found = words.find(pc);
        return found == words.end() ? 0xBF800000u : found->second;
    }
};

int main() {
    using namespace ps5gen5;

    const auto no_memory = [](std::uint64_t, std::uint32_t&) {
        return false;
    };

    // User data arrives in the registers it is told to arrive in, and
    // nothing else is known.
    {
        Program p;
        p.endpgm(0);
        const auto result =
            evaluate_scalar(0, {0x11111111u, 0x22222222u}, 4, p, no_memory);
        check(result.initial.registers[4] == 0x11111111u &&
                  result.initial.known[4],
              "user data lands where it is told");
        check(result.initial.registers[5] == 0x22222222u,
              "and continues into the next register");
        check(!result.initial.known[7],
              "a register nobody wrote is not known");
    }

    // EXEC starts with every lane on. A shader reads it, and a zero there
    // would make every branch look taken with no lanes.
    {
        Program p;
        p.endpgm(0);
        const auto result = evaluate_scalar(0, {}, 0, p, no_memory);
        check(read_pair(result.initial, 126) == kWaveMask,
              "exec starts with every lane on");
        check(read_pair(result.initial, 106) == 0, "vcc starts clear");
    }

    // Arithmetic follows through, which is what makes an address knowable.
    {
        Program p;
        p.sop2(0, 0x00, 8, 4, 5);   // SAddU32 s8, s4, s5
        p.endpgm(1);
        const auto result =
            evaluate_scalar(0, {100, 200}, 4, p, no_memory);
        check(result.final_state.known[8] &&
                  result.final_state.registers[8] == 300,
              "an add of two known values is known");
    }

    // A value that came from nowhere stays unknown, and anything computed
    // from it is unknown too. This is the property that stops a descriptor
    // being resolved from a stale zero.
    {
        Program p;
        p.sop2(0, 0x00, 8, 60, 4);  // SAddU32 s8, s60, s4 - s60 unknown
        p.endpgm(1);
        const auto result = evaluate_scalar(0, {7}, 4, p, no_memory);
        check(!result.final_state.known[8],
              "a value computed from an unknown is unknown");
    }

    // The inline constants: 128 is zero and 129 upwards are the small
    // positive integers. Reading them as register indexes instead would
    // take whatever those registers held.
    {
        Program p;
        p.sop2(0, 0x00, 8, 128, 130);  // s8 = 0 + 2
        p.endpgm(1);
        const auto result = evaluate_scalar(0, {}, 0, p, no_memory);
        check(result.final_state.known[8] &&
                  result.final_state.registers[8] == 2,
              "the inline constants are constants");
    }

    // A scalar load resolves its address from a register pair and an
    // offset, and the words it reads become known.
    {
        Program p;
        p.sload(0, 0x02, 16, 2, 4);  // s16..19 = load [s4:s5 + 16]
        p.endpgm(1);
        std::map<std::uint64_t, std::uint32_t> memory{
            {0x1000 + 16, 0xAAAAAAAAu},
            {0x1000 + 20, 0xBBBBBBBBu},
        };
        const auto reader = [&](std::uint64_t address, std::uint32_t& out) {
            const auto found = memory.find(address);
            if (found == memory.end()) {
                return false;
            }
            out = found->second;
            return true;
        };
        // The base pair is named in units of two registers, so 2 means
        // s4:s5 - which is where the user data was put.
        const auto result = evaluate_scalar(0, {0x1000u, 0u}, 4, p, reader);
        check(result.loads.size() == 1, "the load is recorded");
        if (!result.loads.empty()) {
            check(result.loads[0].address_known,
                  "its address is known because the base pair is");
            check(result.loads[0].address == 0x1000 + 16,
                  "the address is the pair plus the offset in words");
            check(result.loads[0].dword_count == 4,
                  "a dwordx4 reads four words");
        }
        check(result.final_state.known[16] &&
                  result.final_state.registers[16] == 0xAAAAAAAAu,
              "what was read becomes known");
    }

    // A load whose base is unknown is recorded but not resolved, and must
    // not leave the destination looking known.
    {
        Program p;
        p.sload(0, 0x02, 16, 30, 0);  // base pair s60:s61, never written
        // 30 means s60:s61, which nothing wrote.
        p.endpgm(1);
        const auto result = evaluate_scalar(0, {}, 0, p, no_memory);
        check(result.loads.size() == 1 && !result.loads[0].address_known,
              "an unresolvable load says so");
        check(!result.final_state.known[16],
              "and leaves its destination unknown");
    }

    // A load from memory that cannot be read leaves the destination
    // unknown rather than zero.
    {
        Program p;
        p.sload(0, 0x02, 16, 2, 0);
        p.endpgm(1);
        const auto result = evaluate_scalar(0, {0x2000u, 0u}, 4, p, no_memory);
        check(result.loads.size() == 1 && result.loads[0].address_known,
              "the address is known even when the memory is not readable");
        check(!result.final_state.known[16],
              "but nothing was learned from it");
    }

    // Both sides of a branch are walked, because which is taken can depend
    // on the vector half. A resource behind either arm has to be found.
    {
        Program p;
        p.branch_if(0, 2);          // to 3
        p.sop2(1, 0x00, 8, 128, 130);
        p.endpgm(2);
        p.sload(3, 0x02, 20, 2, 0);
        p.endpgm(4);
        const auto result = evaluate_scalar(0, {0x3000u, 0u}, 4, p, no_memory);
        check(result.paths_walked >= 2, "both arms are walked");
        check(!result.loads.empty(),
              "a load behind a branch is still found");
    }

    // A loop must not be followed forever. The same program counter with
    // the same state is the same path.
    {
        Program p;
        p.sop2(0, 0x00, 8, 128, 129);
        p.branch_if(1, -2);   // back to 0
        p.endpgm(2);
        const auto result = evaluate_scalar(0, {}, 0, p, no_memory);
        check(!result.exhausted, "a loop terminates rather than exhausting");
        check(result.paths_merged > 0,
              "the repeat is recognised as one already walked");
    }

    // An instruction with no destination must not cost a register. SNop
    // and the compares put something in bits 16 to 22 that is not one, and
    // treating it as one is what made most loads unresolvable: the count
    // said SNop alone cost 86 registers and the compares another 376.
    {
        Program p;
        p.sop2(0, 0x00, 20, 128, 129);   // s20 = 0 + 1, so s20 is known
        p.sopc(1, 0x0B, 128, 129);       // SCmpLeU32, bits 16..22 are zero
        p.sopp(2, 0x00);                 // SNop, no destination at all
        p.endpgm(3);
        const auto result = evaluate_scalar(0, {}, 0, p, no_memory);
        check(result.final_state.known[20],
              "a compare and a nop leave other registers alone");
    }

    // SCselect picks on the condition code, so the walk needs the code a
    // compare leaves. Comparing 1 to 2 unsigned is less-or-equal, which
    // takes the first source.
    {
        Program p;
        p.sop2(0, 0x00, 20, 129, 128);   // s20 = 1
        p.sop2(1, 0x00, 21, 130, 128);   // s21 = 2
        p.sopc(2, 0x0B, 20, 21);         // SCmpLeU32 s20, s21 -> true
        p.sop2(3, 0x0A, 22, 20, 21);     // SCselectB32 s22 = scc ? s20 : s21
        p.endpgm(4);
        const auto result = evaluate_scalar(0, {}, 0, p, no_memory);
        check(result.final_state.known[22] &&
                  result.final_state.registers[22] == 1,
              "a select takes the arm the compare chose");
    }

    // The same select with the code unknown must give up rather than pick
    // an arm: a descriptor resolved out of the wrong one binds the wrong
    // memory, which is worse than binding none.
    {
        Program p;
        p.sop2(0, 0x00, 20, 129, 128);
        p.sop2(1, 0x00, 21, 130, 128);
        p.sopc(2, 0x0B, 20, 40);         // s40 is not known
        p.sop2(3, 0x0A, 22, 20, 21);
        p.endpgm(4);
        const auto result = evaluate_scalar(0, {}, 0, p, no_memory);
        check(!result.final_state.known[22],
              "a select on an unknown condition resolves to nothing");
    }

    // An operation between the compare and the select writes the condition
    // code itself, so the select must not read the compare's.
    {
        Program p;
        p.sop2(0, 0x00, 20, 129, 128);
        p.sop2(1, 0x00, 21, 130, 128);
        p.sopc(2, 0x0B, 20, 21);         // sets the code
        p.sop2(3, 0x0E, 23, 20, 21);     // SAndB32 overwrites it
        p.sop2(4, 0x0A, 22, 20, 21);     // SCselectB32
        p.endpgm(5);
        const auto result = evaluate_scalar(0, {}, 0, p, no_memory);
        check(!result.final_state.known[22],
              "the condition code does not survive an instruction that "
              "writes it");
    }

    // SBitset1 reads its own destination, so it is only knowable when that
    // register already is.
    {
        Program p;
        p.sop2(0, 0x00, 20, 128, 128);   // s20 = 0
        p.sop1(1, 0x1D, 20, 131);        // SBitset1B32 s20, bit 3
        p.endpgm(2);
        const auto result = evaluate_scalar(0, {}, 0, p, no_memory);
        check(result.final_state.known[20] &&
                  result.final_state.registers[20] == 8u,
              "bitset sets the bit its source names");
    }

    // Saveexec keeps the old mask, and that saved half is where descriptor
    // addresses are often found.
    {
        Program p;
        p.sop1(0, 0x37, 20, 128);        // SAndn1SaveexecB64 s[20:21]
        p.endpgm(1);
        const auto result = evaluate_scalar(0, {}, 0, p, no_memory);
        check(result.final_state.known[20] &&
                  result.final_state.known[21] &&
                  result.final_state.registers[20] == 0xFFFFFFFFu,
              "saveexec leaves the old mask where it can be read");
    }

    // A 64-bit shift, which the walk used to treat as destroying its
    // destination.
    {
        Program p;
        p.sop2(0, 0x00, 20, 129, 128);   // s20 = 1
        p.sop2(1, 0x00, 21, 128, 128);   // s21 = 0
        p.sop2(2, 0x1F, 22, 20, 160);    // SLshlB64 s[22:23] = s[20:21] << 32
        p.endpgm(3);
        const auto result = evaluate_scalar(0, {}, 0, p, no_memory);
        check(result.final_state.known[22] &&
                  result.final_state.known[23] &&
                  result.final_state.registers[22] == 0u &&
                  result.final_state.registers[23] == 1u,
              "a 64-bit shift crosses the register boundary");
    }

    // An image instruction names its descriptor by a group of four
    // registers, and the walk has to look in the right eight.
    {
        Program p;
        for (std::uint32_t index = 0; index < 8; ++index) {
            p.sop2(index, 0x00, 8 + index, 129, 128);   // s[8..15] = 1
        }
        p.image_sample(8, 2, 4);   // resource s8, sampler s16
        p.endpgm(10);
        const auto result = evaluate_scalar(0, {}, 0, p, no_memory);
        check(result.images.size() == 1, "the image instruction is seen");
        if (!result.images.empty()) {
            const auto& image = result.images.front();
            check(image.resource_register == 8,
                  "the resource group is read from bits 16 to 20");
            check(image.sampler_register == 16,
                  "the sampler group is read from bits 21 to 25");
            check(image.descriptor_known,
                  "a descriptor whose eight registers are known resolves");
        }
    }

    if (failures == 0) {
        std::printf("all scalar evaluation checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
