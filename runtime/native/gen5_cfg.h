// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The control flow graph of a guest shader, and whether it is reducible.
//
// The translation this is built for has a decision to make. The C# one
// emits a program counter dispatcher - a loop around a switch, one case per
// basic block - because GCN control flow is arbitrary and SPIR-V requires
// structure. That shape forces every register into memory, which is where
// 40% of a 129517 instruction shader goes.
//
// A reducible graph can be emitted as structured control flow directly,
// with registers as SSA values. An irreducible one cannot without either a
// general structurizer or node duplication. Which of the two the title
// actually produces decides how much work the native translator needs, and
// this measures it rather than assuming: shaders come from a compiler
// working on structured source, so most graphs should reduce, but "should"
// is not a number.
//
// Reducibility is tested by T1-T2 collapsing. Remove self loops; merge any
// node that has exactly one predecessor into that predecessor; repeat. A
// graph is reducible exactly when this leaves a single node.

#ifndef PS5_GEN5_CFG_H
#define PS5_GEN5_CFG_H

#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "gen5_decoder_memory.h"
#include "gen5_decoder_vop.h"

namespace ps5gen5 {

struct CfgBlock {
    std::uint32_t start_pc = 0;
    std::vector<std::uint32_t> successors;
};

// The same graph with the program counters resolved to indexes, which is
// the form every analysis after this one wants. Entry is always block 0
// because the leaders are collected in program counter order and the entry
// is the lowest.
struct CfgGraph {
    std::vector<CfgBlock> blocks;
    std::vector<std::vector<std::size_t>> successors;
    std::vector<std::vector<std::size_t>> predecessors;

    std::size_t size() const { return blocks.size(); }
};

struct CfgSummary {
    bool decoded = false;
    bool reducible = false;
    std::uint32_t instructions = 0;
    std::uint32_t blocks = 0;
    std::uint32_t edges = 0;
    std::uint32_t back_edges = 0;
    std::uint32_t words = 0;
    // Where a decode stopped making sense, if it did, and the word that
     // stopped it - which is the only part that says what is missing.
    std::uint32_t failed_pc = 0;
    std::uint32_t failed_word = 0;
};

// The scalar branch instructions, by the name the decoder gives them. A
// conditional branch falls through as well as jumping; an unconditional one
// does not; the program ends at SEndpgm.
inline bool is_conditional_branch(std::string_view name) {
    return name == "SCbranchScc0" || name == "SCbranchScc1" ||
        name == "SCbranchVccz" || name == "SCbranchVccnz" ||
        name == "SCbranchExecz" || name == "SCbranchExecnz";
}

inline bool is_unconditional_branch(std::string_view name) {
    return name == "SBranch";
}

inline bool is_program_end(std::string_view name) {
    return name == "SEndpgm";
}

// A branch's target is the next instruction plus a signed count of words
// taken from the low half of the word.
inline std::uint32_t branch_target(std::uint32_t pc, std::uint32_t word) {
    const auto simm = static_cast<std::int16_t>(word & 0xFFFFu);
    return static_cast<std::uint32_t>(
        static_cast<std::int64_t>(pc) + 1 + simm);
}

// One more family the CFG needs and the decoder headers do not carry,
// because it is three opcodes and a fixed length.
inline DecodedOpcode decode_vintrp(std::uint32_t word) {
    const auto opcode = (word >> 16) & 0x3u;
    DecodedOpcode decoded;
    decoded.size_dwords = 1;
    switch (opcode) {
        case 0x00: decoded.name = "VInterpP1F32"; break;
        case 0x01: decoded.name = "VInterpP2F32"; break;
        case 0x02: decoded.name = "VInterpMovF32"; break;
        default: break;
    }
    return decoded;
}

// Decodes one instruction far enough to know its length and whether it
// changes control. Reads the words it needs through the caller's accessor,
// because a shader lives in guest memory.
//
// The order of these tests is the decoding. The guards overlap - SMRD's
// prefix lies inside SOP's, VOP3P's inside the range the final switch
// covers - so a test moved earlier or later silently reads instructions as
// the wrong family. An earlier version of this invented the masks rather
// than porting them, and 2578 of 2632 shaders failed to decode.
template <typename ReadWord>
DecodedOpcode decode_one(
    std::uint32_t pc, const ReadWord& read, std::uint32_t& size_dwords) {
    const auto word = read(pc);
    DecodedOpcode decoded;
    if ((word & 0x80000000u) == 0u) {
        decoded = decode_vop2(word);
    } else if ((word & 0xF8000000u) == 0xC0000000u) {
        decoded = decode_smrd(word);
    } else if ((word & 0xC0000000u) == 0x80000000u) {
        decoded = decode_sop(word);
    } else if ((word & 0xFF800000u) == 0xCC000000u) {
        decoded = decode_vop3p(word, read(pc + 1));
    } else {
        switch (word >> 26) {
            case 0x32: decoded = decode_vintrp(word); break;
            case 0x33: decoded = decode_smem(word); break;
            case 0x34:
            case 0x35:
                decoded = decode_vop3(
                    word,
                    read(pc + 1),
                    is_vop3b_opcode((word >> 16) & 0x3FFu));
                break;
            case 0x36: decoded = decode_ds(word); break;
            case 0x37: decoded = decode_flat(word); break;
            case 0x38: decoded = decode_mubuf(word, read(pc + 1)); break;
            case 0x3A: decoded = decode_mtbuf(word, read(pc + 1)); break;
            case 0x3C: decoded = decode_mimg(word); break;
            case 0x3D: decoded = decode_smem(word); break;
            case 0x3E:
                // Export carries no opcode table: the word is the whole
                // instruction and the next word its operands.
                decoded.name = "Exp";
                decoded.size_dwords = 2;
                break;
            case 0x3F:
                // The C# falls back to a raw two word form here rather
                // than a table, so anything in this range decodes.
                decoded.name = "Vop3pRaw";
                decoded.size_dwords = 2;
                break;
            default: break;
        }
    }
    size_dwords = decoded.size_dwords;
    return decoded;
}

// Walks a program from its entry, splitting it into basic blocks and
// joining them by their successors. Stops at the first instruction it
// cannot decode, and says where.
template <typename ReadWord>
CfgSummary build_cfg(
    std::uint32_t entry_pc,
    const ReadWord& read,
    std::uint32_t word_limit = 1u << 20,
    CfgGraph* graph_out = nullptr) {
    CfgSummary summary;

    // First pass: follow the program, note where instructions begin, what
    // ends a block and what a block's successors are.
    std::map<std::uint32_t, std::vector<std::uint32_t>> terminators;
    std::set<std::uint32_t> leaders{entry_pc};
    std::set<std::uint32_t> visited;
    std::vector<std::uint32_t> pending{entry_pc};
    while (!pending.empty()) {
        auto pc = pending.back();
        pending.pop_back();
        if (!visited.insert(pc).second) {
            continue;
        }
        while (pc < word_limit) {
            std::uint32_t size = 0;
            const auto decoded = decode_one(pc, read, size);
            if (!decoded.ok() || size == 0) {
                summary.failed_pc = pc;
                summary.failed_word = read(pc);
                return summary;
            }
            ++summary.instructions;
            summary.words += size;
            const auto next = pc + size;
            if (is_program_end(decoded.name)) {
                terminators[pc] = {};
                break;
            }
            if (is_unconditional_branch(decoded.name)) {
                const auto target = branch_target(pc, read(pc));
                terminators[pc] = {target};
                leaders.insert(target);
                if (visited.find(target) == visited.end()) {
                    pending.push_back(target);
                }
                break;
            }
            if (is_conditional_branch(decoded.name)) {
                const auto target = branch_target(pc, read(pc));
                terminators[pc] = {target, next};
                leaders.insert(target);
                leaders.insert(next);
                if (visited.find(target) == visited.end()) {
                    pending.push_back(target);
                }
                if (visited.find(next) == visited.end()) {
                    pending.push_back(next);
                }
                break;
            }
            pc = next;
            if (leaders.count(pc) != 0) {
                // Someone branches here, so the block ends and falls
                // through into the one that starts here.
                terminators[pc - size] = {pc};
                break;
            }
        }
    }
    summary.decoded = true;

    // Second pass: a block runs from a leader to the terminator that ends
    // it, and its successors are that terminator's.
    std::map<std::uint32_t, std::size_t> index_of;
    std::vector<CfgBlock> blocks;
    for (const auto leader : leaders) {
        index_of[leader] = blocks.size();
        blocks.push_back(CfgBlock{leader, {}});
    }
    for (auto& block : blocks) {
        auto pc = block.start_pc;
        while (true) {
            const auto terminator = terminators.find(pc);
            if (terminator != terminators.end()) {
                block.successors = terminator->second;
                break;
            }
            std::uint32_t size = 0;
            const auto decoded = decode_one(pc, read, size);
            if (!decoded.ok() || size == 0) {
                break;
            }
            pc += size;
            if (index_of.count(pc) != 0) {
                block.successors = {pc};
                break;
            }
        }
    }
    summary.blocks = static_cast<std::uint32_t>(blocks.size());

    // T1-T2 collapsing over the successor lists, kept as sets of indexes so
    // merging is cheap and duplicate edges cannot accumulate.
    std::vector<std::set<std::size_t>> succ(blocks.size());
    std::vector<std::set<std::size_t>> pred(blocks.size());
    if (graph_out != nullptr) {
        graph_out->blocks = blocks;
        graph_out->successors.assign(blocks.size(), {});
        graph_out->predecessors.assign(blocks.size(), {});
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            for (const auto target : blocks[index].successors) {
                const auto found = index_of.find(target);
                if (found == index_of.end()) {
                    continue;
                }
                graph_out->successors[index].push_back(found->second);
                graph_out->predecessors[found->second].push_back(index);
            }
        }
    }
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        for (const auto target : blocks[index].successors) {
            const auto found = index_of.find(target);
            if (found == index_of.end()) {
                continue;
            }
            succ[index].insert(found->second);
            pred[found->second].insert(index);
            ++summary.edges;
            if (target <= blocks[index].start_pc) {
                ++summary.back_edges;
            }
        }
    }

    std::vector<bool> alive(blocks.size(), true);
    auto remaining = blocks.size();
    bool changed = true;
    while (changed && remaining > 1) {
        changed = false;
        for (std::size_t node = 0; node < blocks.size(); ++node) {
            if (!alive[node]) {
                continue;
            }
            // T1: a self loop carries no information about reducibility.
            if (succ[node].erase(node) != 0) {
                pred[node].erase(node);
                changed = true;
            }
        }
        for (std::size_t node = 0; node < blocks.size(); ++node) {
            if (!alive[node] || pred[node].size() != 1) {
                continue;
            }
            const auto parent = *pred[node].begin();
            if (parent == node) {
                continue;
            }
            // T2: fold a single-entry node into the node that enters it.
            succ[parent].erase(node);
            for (const auto target : succ[node]) {
                if (target == node) {
                    continue;
                }
                succ[parent].insert(target);
                pred[target].erase(node);
                pred[target].insert(parent);
            }
            succ[node].clear();
            pred[node].clear();
            alive[node] = false;
            --remaining;
            changed = true;
        }
    }
    summary.reducible = remaining <= 1;
    return summary;
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_CFG_H
