// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The structure of a shader's control flow: which blocks dominate which,
// which edges close loops, what a loop contains, and where a branch's two
// arms come together again.
//
// This is what the native translator needs and the C# one does without.
// That one emits a program counter dispatcher - a loop around a switch,
// one case per block - which requires no analysis at all and costs every
// register a place in memory, because a value cannot stay in SSA across a
// switch and a back edge. 40% of a 129517 instruction shader is the traffic
// that costs.
//
// SPIR-V wants structured control flow: a loop says where it merges and
// where it continues, a branch says where its arms rejoin. Every one of
// this title's 34 decodable shaders has a reducible graph, so those
// answers exist for all of them, and this computes them.
//
// Nothing here is novel. Dominators by the iterative algorithm, back edges
// by dominance, natural loops by walking predecessors from the back edge,
// merge points by post-dominance. They are written out rather than pulled
// in because the runtime links nothing.

#ifndef PS5_GEN5_STRUCTURE_H
#define PS5_GEN5_STRUCTURE_H

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

#include "gen5_cfg.h"

namespace ps5gen5 {

constexpr std::size_t kNoBlock = static_cast<std::size_t>(-1);

// A loop, named by the block every path into it must pass through.
struct NaturalLoop {
    std::size_t header = kNoBlock;
    std::size_t latch = kNoBlock;
    std::set<std::size_t> body;
};

struct Structure {
    // immediate_dominator[b] is the last block every path from the entry to
    // b must pass through, other than b. The entry's is itself.
    std::vector<std::size_t> immediate_dominator;
    // The mirror image, computed on the reversed graph: the first block
    // every path from b to an exit must pass through. This is where a
    // branch's arms rejoin, which is what a selection has to name.
    std::vector<std::size_t> immediate_post_dominator;
    std::vector<NaturalLoop> loops;
    // depth[b] is how many loops contain b, so a translation knows which
    // merges it is nested inside.
    std::vector<std::uint32_t> loop_depth;
};

// Reverse post order of a depth first walk, which is the order the
// dominator iteration wants: a block's predecessors are visited before it
// wherever the graph allows.
inline std::vector<std::size_t> reverse_post_order(const CfgGraph& graph) {
    std::vector<std::size_t> order;
    std::vector<bool> seen(graph.size(), false);
    // An explicit stack, because a shader with thousands of blocks would
    // otherwise recurse as deep as it has blocks.
    std::vector<std::pair<std::size_t, std::size_t>> stack;
    if (graph.size() != 0) {
        stack.push_back({0, 0});
        seen[0] = true;
    }
    while (!stack.empty()) {
        auto& [node, next] = stack.back();
        if (next < graph.successors[node].size()) {
            const auto child = graph.successors[node][next];
            ++next;
            if (!seen[child]) {
                seen[child] = true;
                stack.push_back({child, 0});
            }
            continue;
        }
        order.push_back(node);
        stack.pop_back();
    }
    std::reverse(order.begin(), order.end());
    return order;
}

// Cooper, Harvey and Kennedy's iterative formulation: walk the blocks in
// reverse post order, intersecting the dominators of each block's
// predecessors, until nothing changes.
inline std::vector<std::size_t> compute_dominators(
    const CfgGraph& graph,
    const std::vector<std::vector<std::size_t>>& preds,
    const std::vector<std::size_t>& order) {
    std::vector<std::size_t> idom(graph.size(), kNoBlock);
    std::vector<std::size_t> position(graph.size(), 0);
    for (std::size_t index = 0; index < order.size(); ++index) {
        position[order[index]] = index;
    }
    if (graph.size() == 0) {
        return idom;
    }
    idom[order.front()] = order.front();

    const auto intersect = [&](std::size_t left, std::size_t right) {
        while (left != right) {
            while (position[left] > position[right]) {
                left = idom[left];
            }
            while (position[right] > position[left]) {
                right = idom[right];
            }
        }
        return left;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto node : order) {
            if (node == order.front()) {
                continue;
            }
            std::size_t candidate = kNoBlock;
            for (const auto pred : preds[node]) {
                if (idom[pred] == kNoBlock) {
                    continue;
                }
                candidate = candidate == kNoBlock
                    ? pred
                    : intersect(pred, candidate);
            }
            if (candidate != kNoBlock && idom[node] != candidate) {
                idom[node] = candidate;
                changed = true;
            }
        }
    }
    return idom;
}

inline bool dominates(
    const std::vector<std::size_t>& idom,
    std::size_t dominator,
    std::size_t node) {
    while (node != kNoBlock) {
        if (node == dominator) {
            return true;
        }
        const auto next = idom[node];
        if (next == node) {
            return false;
        }
        node = next;
    }
    return false;
}

// A back edge runs to a block that dominates its source, and the loop it
// closes is everything that can reach the source without leaving through
// the header.
inline NaturalLoop natural_loop(
    const CfgGraph& graph,
    std::size_t header,
    std::size_t latch) {
    NaturalLoop loop;
    loop.header = header;
    loop.latch = latch;
    loop.body.insert(header);
    std::vector<std::size_t> pending;
    if (latch != header) {
        loop.body.insert(latch);
        pending.push_back(latch);
    }
    while (!pending.empty()) {
        const auto node = pending.back();
        pending.pop_back();
        for (const auto pred : graph.predecessors[node]) {
            if (loop.body.insert(pred).second) {
                pending.push_back(pred);
            }
        }
    }
    return loop;
}

// The exit the program falls out of: of the blocks that end it, the
// first in the code. Compilers put the others - a pixel shader's
// kill-everything tail - after the main s_endpgm.
inline std::size_t main_exit_block(const CfgGraph& graph) {
    std::size_t main = kNoBlock;
    for (std::size_t block = 0; block < graph.size(); ++block) {
        if (!graph.successors[block].empty()) {
            continue;
        }
        if (main == kNoBlock ||
            graph.blocks[block].start_pc < graph.blocks[main].start_pc) {
            main = block;
        }
    }
    return main;
}

// An exit other than the main one that several places branch to. Each
// branch to it is emitted as a copy of it returning in place, since it
// belongs to none of the constructs that reach it; so it is no join for
// the structure either.
inline bool copied_exit_block(
    const CfgGraph& graph, std::size_t block, std::size_t main_exit) {
    return block < graph.size() && block != main_exit &&
        graph.successors[block].empty() &&
        graph.predecessors[block].size() > 1;
}

inline Structure analyse_structure(const CfgGraph& graph) {
    Structure structure;
    if (graph.size() == 0) {
        return structure;
    }
    const auto order = reverse_post_order(graph);
    structure.immediate_dominator =
        compute_dominators(graph, graph.predecessors, order);

    // Post-dominators are dominators of the reversed graph. A graph with
    // several exits needs them joined first, or blocks on different exit
    // paths have no common post-dominator at all.
    CfgGraph reversed;
    reversed.blocks.resize(graph.size() + 1);
    reversed.successors.assign(graph.size() + 1, {});
    reversed.predecessors.assign(graph.size() + 1, {});
    const auto sink = graph.size();
    // A branch to an exit block that several places reach is emitted as a
    // copy of that block, returning in place (see emit_control_flow), so
    // it is an early return rather than a path that rejoins anything, and
    // it is left out here. Counted in, a pixel shader's kill tail made
    // every branch that could reach it lose its merge.
    const auto main_exit = main_exit_block(graph);
    const auto copied_exit = [&](std::size_t block) {
        return copied_exit_block(graph, block, main_exit);
    };
    for (std::size_t node = 0; node < graph.size(); ++node) {
        bool continues = false;
        for (const auto succ : graph.successors[node]) {
            if (copied_exit(succ)) {
                continue;
            }
            continues = true;
            reversed.successors[succ].push_back(node);
            reversed.predecessors[node].push_back(succ);
        }
        if (!continues) {
            reversed.successors[sink].push_back(node);
            reversed.predecessors[node].push_back(sink);
        }
    }
    // The reversed walk has to start at the joined exit, which the reverse
    // post order finds only if it is block zero; rotate by hand instead.
    std::vector<std::size_t> reversed_order;
    {
        std::vector<bool> seen(reversed.size(), false);
        std::vector<std::pair<std::size_t, std::size_t>> stack{{sink, 0}};
        seen[sink] = true;
        while (!stack.empty()) {
            auto& [node, next] = stack.back();
            if (next < reversed.successors[node].size()) {
                const auto child = reversed.successors[node][next];
                ++next;
                if (!seen[child]) {
                    seen[child] = true;
                    stack.push_back({child, 0});
                }
                continue;
            }
            reversed_order.push_back(node);
            stack.pop_back();
        }
        std::reverse(reversed_order.begin(), reversed_order.end());
    }
    const auto ipdom =
        compute_dominators(reversed, reversed.predecessors, reversed_order);
    structure.immediate_post_dominator.assign(graph.size(), kNoBlock);
    for (std::size_t node = 0; node < graph.size(); ++node) {
        const auto found = ipdom[node];
        structure.immediate_post_dominator[node] =
            found == sink ? kNoBlock : found;
    }

    structure.loop_depth.assign(graph.size(), 0);
    for (std::size_t node = 0; node < graph.size(); ++node) {
        for (const auto succ : graph.successors[node]) {
            if (!dominates(structure.immediate_dominator, succ, node)) {
                continue;
            }
            auto loop = natural_loop(graph, succ, node);
            for (const auto member : loop.body) {
                ++structure.loop_depth[member];
            }
            structure.loops.push_back(std::move(loop));
        }
    }
    return structure;
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_STRUCTURE_H
