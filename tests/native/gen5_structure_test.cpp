// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Dominators, loops and merge points, checked on graphs built by hand so
// the expected answer is known rather than assumed. These feed straight
// into what the translator emits: get a merge point wrong and the SPIR-V
// is rejected or, worse, accepted and wrong.
#include <cstdio>

#include "gen5_structure.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

static ps5gen5::CfgGraph make(
    std::size_t count,
    const std::vector<std::pair<std::size_t, std::size_t>>& edges) {
    ps5gen5::CfgGraph graph;
    graph.blocks.resize(count);
    graph.successors.assign(count, {});
    graph.predecessors.assign(count, {});
    for (const auto& [from, to] : edges) {
        graph.successors[from].push_back(to);
        graph.predecessors[to].push_back(from);
    }
    return graph;
}

int main() {
    using namespace ps5gen5;

    // A chain: 0 -> 1 -> 2. Each dominates the next, and each is
    // post-dominated by the next.
    {
        const auto graph = make(3, {{0, 1}, {1, 2}});
        const auto s = analyse_structure(graph);
        check(s.immediate_dominator[1] == 0 && s.immediate_dominator[2] == 1,
              "a chain dominates forward");
        check(s.immediate_post_dominator[0] == 1 &&
                  s.immediate_post_dominator[1] == 2,
              "a chain post-dominates backward");
        check(s.loops.empty(), "a chain has no loops");
    }

    // A diamond: 0 branches to 1 and 2, both reach 3. Block 3 is where the
    // arms rejoin, which is exactly what a selection merge has to name -
    // and 0 dominates all of them while 1 and 2 dominate nothing.
    {
        const auto graph = make(4, {{0, 1}, {0, 2}, {1, 3}, {2, 3}});
        const auto s = analyse_structure(graph);
        check(s.immediate_dominator[3] == 0,
              "the join is dominated by the branch, not by either arm");
        check(s.immediate_post_dominator[0] == 3,
              "the branch's merge point is the join");
        check(s.loops.empty(), "a diamond has no loops");
    }

    // A loop: 0 -> 1 -> 2 -> 1, with 2 also leaving to 3. The back edge is
    // 2 -> 1, the header is 1, and the body is {1, 2}.
    {
        const auto graph = make(4, {{0, 1}, {1, 2}, {2, 1}, {2, 3}});
        const auto s = analyse_structure(graph);
        check(s.loops.size() == 1, "one back edge is one loop");
        if (s.loops.size() == 1) {
            check(s.loops[0].header == 1, "the header is the branch target");
            check(s.loops[0].latch == 2, "the latch is the branch source");
            check(s.loops[0].body.count(1) != 0 &&
                      s.loops[0].body.count(2) != 0 &&
                      s.loops[0].body.count(3) == 0,
                  "the body is what reaches the latch, and no more");
        }
        check(s.loop_depth[2] == 1 && s.loop_depth[3] == 0,
              "depth counts the loops a block is inside");
    }

    // Nested loops: an outer 1..4 and an inner 2..3. The inner blocks are
    // inside both, which is what a translation needs to know to nest its
    // merges correctly.
    {
        const auto graph = make(
            5, {{0, 1}, {1, 2}, {2, 3}, {3, 2}, {3, 4}, {4, 1}});
        const auto s = analyse_structure(graph);
        check(s.loops.size() == 2, "two back edges are two loops");
        check(s.loop_depth[2] == 2 && s.loop_depth[3] == 2,
              "the inner blocks are inside both loops");
        check(s.loop_depth[1] == 1 && s.loop_depth[4] == 1,
              "the outer blocks are inside one");
    }

    // A self loop is a loop whose header and latch are the same block, and
    // must not be missed just because there is no separate latch.
    {
        const auto graph = make(3, {{0, 1}, {1, 1}, {1, 2}});
        const auto s = analyse_structure(graph);
        check(s.loops.size() == 1 && s.loops[0].header == 1 &&
                  s.loops[0].latch == 1,
              "a self loop is found");
    }

    // Two exits. Without joining them the blocks on different exit paths
    // would have no common post-dominator and a merge point would come out
    // as nonsense rather than as absent.
    {
        const auto graph = make(3, {{0, 1}, {0, 2}});
        const auto s = analyse_structure(graph);
        check(s.immediate_post_dominator[0] == kNoBlock,
              "a branch whose arms never rejoin has no merge point");
    }

    if (failures == 0) {
        std::printf("all structure checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
