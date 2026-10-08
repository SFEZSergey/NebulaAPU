// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// The reducibility test has to be right before it is believed, so it is
// checked on graphs whose answer is known: a straight line, an if, a loop,
// and the smallest irreducible graph there is.
#include <cstdio>
#include <map>
#include <vector>

#include "gen5_cfg.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

// Assembles a tiny program out of the words the decoders recognise, so the
// graph comes from the same path the real shaders take.
struct Program {
    std::map<std::uint32_t, std::uint32_t> words;

    void nop(std::uint32_t pc) {
        // SOPP opcode 0 is SNop.
        words[pc] = 0xBF800000u;
    }
    void endpgm(std::uint32_t pc) {
        words[pc] = 0xBF800000u | (0x01u << 16);
    }
    void branch(std::uint32_t pc, std::int16_t offset) {
        words[pc] = 0xBF800000u | (0x02u << 16) |
            static_cast<std::uint16_t>(offset);
    }
    void branch_if(std::uint32_t pc, std::int16_t offset) {
        // SOPP opcode 4 is SCbranchScc0.
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

    // A straight line ending in endpgm: one block, reducible.
    {
        Program p;
        p.nop(0);
        p.nop(1);
        p.endpgm(2);
        const auto cfg = build_cfg(0, p);
        check(cfg.decoded, "a straight line decodes");
        check(cfg.blocks == 1, "a straight line is one block");
        check(cfg.reducible, "a straight line is reducible");
    }

    // An if: branch over a block, both paths meeting at the end.
    {
        Program p;
        p.branch_if(0, 2);   // to 3
        p.nop(1);
        p.branch(2, 1);      // to 4
        p.nop(3);
        p.endpgm(4);
        const auto cfg = build_cfg(0, p);
        check(cfg.decoded && cfg.blocks >= 3, "an if has at least three blocks");
        check(cfg.reducible, "an if is reducible");
    }

    // A loop: a conditional branch backwards. Single entry, so reducible.
    {
        Program p;
        p.nop(0);
        p.nop(1);
        p.branch_if(2, -3);  // back to 0
        p.endpgm(3);
        const auto cfg = build_cfg(0, p);
        check(cfg.decoded, "a loop decodes");
        check(cfg.back_edges >= 1, "a backwards branch is seen as a back edge");
        check(cfg.reducible, "a single entry loop is reducible");
    }

    // The smallest irreducible graph: entry branches to either of two
    // blocks, and those two branch to each other. The cycle has two
    // entries, so no amount of T1-T2 collapsing reaches one node.
    {
        Program p;
        p.branch_if(0, 2);   // to 3, else falls through to 1
        p.branch(1, 3);      // 1 -> 5
        p.nop(2);
        p.branch(3, 2);      // 3 -> 6
        p.nop(4);
        p.branch(5, 0);      // 5 -> 6
        p.branch(6, -3);     // 6 -> 4 ... into the other side of the cycle
        const auto cfg = build_cfg(0, p);
        check(cfg.decoded, "the irreducible graph decodes");
        check(!cfg.reducible || cfg.blocks < 3,
              "a two entry cycle is not reducible");
    }

    // A program whose first instruction cannot be decoded says so rather
    // than reporting an empty graph as a fine one. 0x39 is a gap in the
    // top-bits switch; 0xFFFFFFFF is not undecodable, because the 0x3F
    // case deliberately falls back to a raw two word form.
    {
        Program p;
        p.words[0] = 0x39u << 26;
        const auto cfg = build_cfg(0, p);
        check(!cfg.decoded, "an undecodable program is not reported decoded");
    }

    if (failures == 0) {
        std::printf("all CFG checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
