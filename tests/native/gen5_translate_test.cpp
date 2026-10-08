// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// The pieces joined, on programs assembled here so the expected shape is
// known. What this test is for is whether they fit: a module comes out, it
// is a legal one, and the register traffic is what the bank promised
// rather than what a dispatcher would have paid.
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "gen5_translate.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

// Assembles guest words. The encodings are the ones the decoder reads, so
// the program goes through exactly the path a real shader does.
struct Program {
    std::map<std::uint32_t, std::uint32_t> words;

    // SOP2: opcode in bits 23..29, destination 16..22, sources in the low
    // two bytes.
    void sop2(std::uint32_t pc, std::uint32_t opcode, std::uint32_t dst,
              std::uint32_t src0, std::uint32_t src1) {
        words[pc] = 0x80000000u | ((opcode & 0x7Fu) << 23) |
            ((dst & 0x7Fu) << 16) | ((src1 & 0xFFu) << 8) | (src0 & 0xFFu);
    }
    void sopp(std::uint32_t pc, std::uint32_t opcode,
              std::int16_t immediate = 0) {
        words[pc] = 0xBF800000u | ((opcode & 0x7Fu) << 16) |
            static_cast<std::uint16_t>(immediate);
    }
    void endpgm(std::uint32_t pc) { sopp(pc, 0x01); }
    void branch_if(std::uint32_t pc, std::int16_t offset) {
        sopp(pc, 0x04, offset);
    }
    std::uint32_t operator()(std::uint32_t pc) const {
        const auto found = words.find(pc);
        return found == words.end() ? 0xBF800000u : found->second;
    }
};

static void check_module_well_formed(
    const std::vector<std::uint32_t>& words, const char* what) {
    using ps5spirv::Op;
    if (words.size() < 5 || words[0] != 0x07230203u) {
        std::printf("FAIL %s: not a SPIR-V module\n", what);
        ++failures;
        return;
    }
    std::size_t index = 5;
    std::set<std::uint32_t> labels;
    while (index < words.size()) {
        const auto length = words[index] >> 16;
        if (length == 0 || index + length > words.size()) {
            std::printf("FAIL %s: malformed instruction stream\n", what);
            ++failures;
            return;
        }
        if ((words[index] & 0xFFFFu) ==
            static_cast<std::uint32_t>(Op::Label)) {
            if (!labels.insert(words[index + 1]).second) {
                std::printf("FAIL %s: a label appears twice\n", what);
                ++failures;
            }
        }
        index += length;
    }
    if (index != words.size()) {
        std::printf("FAIL %s: the stream overruns its end\n", what);
        ++failures;
    }
}

int main() {
    using namespace ps5gen5;

    // These programs have no resources; nothing is readable.
    const auto no_memory = [](std::uint64_t, std::uint32_t&) {
        return false;
    };

    // A straight line of arithmetic: s2 = s0 + s1; s3 = s2 * s2; end.
    {
        Program p;
        p.sop2(0, 0x00, 2, 0, 1);   // SAddU32 s2, s0, s1
        p.sop2(1, 0x26, 3, 2, 2);   // SMulI32 s3, s2, s2
        p.endpgm(2);
        const auto result = translate_shader(0, p, {}, 0, no_memory);
        check(result.ok, "a straight line translates");
        check(result.blocks == 1, "it is one block");
        check(result.instructions_translated == 2,
              "both arithmetic instructions are translated");
        check(result.instructions_skipped == 0, "nothing is skipped");
        check_module_well_formed(result.words, "straight line");

        // s0 and s1 are loaded once each; s2 is written then read, so it is
        // never loaded; s2 and s3 are stored once each at the end. A
        // dispatcher would have paid a load and a store per instruction.
        check(result.register_stats.loads_emitted == 2,
              "two registers are loaded, and the one written is not");
        check(result.register_stats.stores_emitted == 2,
              "the two registers written are stored once each");
        check(result.register_stats.reads_served_from_cache == 2,
              "reading s2 twice costs nothing after it was written");
    }

    // A branch: the compare sets the condition the branch uses, and the
    // arms and the join all become blocks.
    {
        Program p;
        p.sop2(0, 0x00, 2, 0, 1);   // SAddU32 s2, s0, s1
        p.branch_if(1, 1);          // to 3
        p.sop2(2, 0x02, 3, 2, 2);   // SAddI32 s3, s2, s2
        p.sop2(3, 0x0E, 4, 2, 3);   // SAndB32 s4, s2, s3
        p.endpgm(4);
        const auto result = translate_shader(0, p, {}, 0, no_memory);
        check(result.ok, "a branch translates");
        check(result.blocks >= 3, "a branch makes at least three blocks");
        check_module_well_formed(result.words, "branch");
    }

    // A loop, which is what the dispatcher exists for and what structured
    // emission has to get right.
    {
        Program p;
        p.sop2(0, 0x00, 2, 0, 1);   // SAddU32 s2, s0, s1
        p.sop2(1, 0x02, 2, 2, 1);   // SAddI32 s2, s2, s1
        p.branch_if(2, -2);         // back to 1
        p.endpgm(3);
        const auto result = translate_shader(0, p, {}, 0, no_memory);
        check(result.ok, "a loop translates");
        check_module_well_formed(result.words, "loop");
        bool has_loop_merge = false;
        std::size_t index = 5;
        while (index < result.words.size()) {
            const auto length = result.words[index] >> 16;
            if (length == 0) {
                break;
            }
            if ((result.words[index] & 0xFFFFu) ==
                static_cast<std::uint32_t>(ps5spirv::Op::LoopMerge)) {
                has_loop_merge = true;
            }
            index += length;
        }
        check(has_loop_merge, "the loop is emitted as a loop");
    }

    // The saving, stated as the two numbers side by side. This is the
    // reason the port exists, so it is measured rather than described.
    {
        Program p;
        // Eight instructions over four registers, the shape of a real
        // block: read two, compute through them, write two.
        p.sop2(0, 0x00, 4, 0, 1);
        p.sop2(1, 0x02, 5, 4, 2);
        p.sop2(2, 0x0E, 4, 5, 3);
        p.sop2(3, 0x10, 5, 4, 5);
        p.sop2(4, 0x12, 4, 5, 4);
        p.sop2(5, 0x00, 5, 4, 5);
        p.sop2(6, 0x26, 4, 5, 4);
        p.endpgm(7);
        const auto result = translate_shader(0, p, {}, 0, no_memory);
        check(result.ok, "the measured block translates");
        const auto ours = result.register_stats.loads_emitted +
            result.register_stats.stores_emitted;
        const auto theirs = result.dispatcher_loads + result.dispatcher_stores;
        std::printf(
            "  block of %u instructions: %u memory operations against %u\n",
            result.instructions_translated, ours, theirs);
        check(ours < theirs / 2,
              "the bank costs less than half what a dispatcher would");
    }

    // VOP3 wears the same opcode names as VOP2 and VOP1 but is a
    // different shape: the destination is the low byte of the first word
    // and all three sources are in the second, nine bits each, any of them
    // free to name a scalar register or an inline constant. Reading it as
    // VOP2 takes the destination out of the opcode field and the sources
    // out of bits that are not operands, and nothing downstream complains
    // - the module still builds, against the wrong registers.
    {
        // v5 = v1 * v2 + s3, as VOP3 would encode it.
        const std::uint32_t word = (0x35u << 26) | (0x141u << 16) | 5u;
        const std::uint32_t extra = (3u << 18) | (258u << 9) | 257u;
        const auto operands = ps5gen5::vector_operands("VMadF32", word,
                                                       extra, 3);
        check(operands.destination == 5,
              "a VOP3 destination is the low byte of the first word");
        check(operands.source0 == 1 && operands.source0_is_vector,
              "the first source is a vector register from the second word");
        check(operands.source1 == 2 && operands.source1_is_vector,
              "the second source is nine bits above it");
        check(operands.has_source2 && operands.source2 == 3 &&
                  !operands.source2_is_vector,
              "the third source is named rather than taken from the "
              "destination, and can be scalar");
    }

    // VOP2 keeps its own shape: the destination high in the word, the
    // second source in its own field, and a third source that is the
    // destination read before it is written.
    {
        const std::uint32_t word = (0x2Cu << 25) | (7u << 17) | (2u << 9) |
            257u;
        const auto operands = ps5gen5::vector_operands("VMacF32", word, 0, 3);
        check(operands.destination == 7, "a VOP2 destination stays at 17");
        check(operands.source0 == 1 && operands.source0_is_vector,
              "a VOP2 first source is the nine bit encoding");
        check(operands.source1 == 2 && operands.source1_is_vector,
              "a VOP2 second source is always a vector register");
        check(!operands.has_source2,
              "a VOP2 accumulator is not a named source");
    }

    // The K forms are not accumulators: K is the literal after the word,
    // added by "ak" and multiplied by "mk". Taken from the intro's colour
    // conversion: v_fmaak_f32 v4, v15, v1, K.
    {
        const auto ak = ps5gen5::vector_operands(
            "VFmaAkF32", 0x5A08020Fu, 0x3DDA6B51u, 3);
        check(ak.has_source2 && !ak.source2_is_vector && ak.source2 == 255,
              "an ak form adds the literal");
        check(ak.source0 == 15 && !ak.source0_is_vector &&
                  ak.source1 == 1 && ak.source1_is_vector,
              "and multiplies its two named sources");
        const auto mk = ps5gen5::vector_operands(
            "VMadMkF32", (0x20u << 25) | (4u << 17) | (3u << 9) | 258u,
            0x3F800000u, 3);
        check(!mk.source1_is_vector && mk.source1 == 255,
              "an mk form multiplies by the literal");
        check(mk.has_source2 && mk.source2_is_vector && mk.source2 == 3,
              "and adds its vector source");
    }

    if (failures == 0) {
        std::printf("all translation checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
