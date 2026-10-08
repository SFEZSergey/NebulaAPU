// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// What makes an emission plan right is structural, so that is what is
// checked: every reachable block once, every region closed, regions nested
// rather than overlapping. Those are the things the SPIR-V emitter will
// rely on, and a plan that breaks them produces a module a driver rejects
// or, worse, accepts and runs wrongly.
#include <cstdio>
#include <set>
#include <vector>

#include "gen5_emit_plan.h"

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

// Every block emitted once, regions balanced, and no region closed by the
// wrong opener.
static void check_well_formed(
    const ps5gen5::EmitPlan& plan, std::size_t expected_blocks,
    const char* what) {
    using namespace ps5gen5;
    std::set<std::size_t> seen;
    std::vector<PlanStep> open;
    bool balanced = true;
    for (const auto& step : plan.steps) {
        switch (step.kind) {
            case PlanStepKind::Block:
                if (!seen.insert(step.block).second) {
                    std::printf("FAIL %s: block %zu emitted twice\n",
                                what, step.block);
                    ++failures;
                }
                break;
            case PlanStepKind::LoopBegin:
            case PlanStepKind::SelectionBegin:
                open.push_back(step);
                break;
            case PlanStepKind::LoopEnd:
            case PlanStepKind::SelectionEnd: {
                if (open.empty()) {
                    balanced = false;
                    break;
                }
                const auto opener = open.back();
                open.pop_back();
                const auto wants_loop = step.kind == PlanStepKind::LoopEnd;
                const auto was_loop =
                    opener.kind == PlanStepKind::LoopBegin;
                if (wants_loop != was_loop || opener.block != step.block) {
                    balanced = false;
                }
                break;
            }
        }
    }
    if (!open.empty()) {
        balanced = false;
    }
    check(balanced, what);
    if (seen.size() != expected_blocks) {
        std::printf("FAIL %s: %zu blocks emitted, %zu expected\n",
                    what, seen.size(), expected_blocks);
        ++failures;
    }
}

int main() {
    using namespace ps5gen5;

    // A chain emits in order and opens nothing.
    {
        const auto graph = make(3, {{0, 1}, {1, 2}});
        const auto plan = plan_emission(graph, analyse_structure(graph));
        check(plan.complete, "a chain plans completely");
        check_well_formed(plan, 3, "chain");
        check(plan.steps.size() == 3, "a chain is three steps and no regions");
    }

    // A diamond opens one selection, emits both arms inside it, and closes
    // before the join - the join belongs to the enclosing level.
    {
        const auto graph = make(4, {{0, 1}, {0, 2}, {1, 3}, {2, 3}});
        const auto plan = plan_emission(graph, analyse_structure(graph));
        check(plan.complete, "a diamond plans completely");
        check_well_formed(plan, 4, "diamond");
        check(plan.steps.front().kind == PlanStepKind::SelectionBegin &&
                  plan.steps.front().merge == 3,
              "the selection names the join as its merge");
        check(plan.steps.back().kind == PlanStepKind::Block &&
                  plan.steps.back().block == 3,
              "the join is emitted after the selection closes");
    }

    // A loop names its latch as the continue target and the block outside
    // as the merge, and the back edge is not walked as an ordinary edge.
    {
        const auto graph = make(4, {{0, 1}, {1, 2}, {2, 1}, {2, 3}});
        const auto plan = plan_emission(graph, analyse_structure(graph));
        check(plan.complete, "a loop plans completely");
        check_well_formed(plan, 4, "loop");
        bool found = false;
        for (const auto& step : plan.steps) {
            if (step.kind != PlanStepKind::LoopBegin) {
                continue;
            }
            found = true;
            check(step.block == 1, "the loop begins at its header");
            check(step.continue_target == 2, "the latch is the continue");
            check(step.merge == 3, "the block outside is the merge");
        }
        check(found, "a loop is planned as a loop");
    }

    // Nested loops nest their regions rather than interleaving them. The
    // outer loop leaves to 5, because a program that never ends has no
    // post-dominators and so no merge points - see the exitless case below.
    {
        const auto graph = make(
            6, {{0, 1}, {1, 2}, {2, 3}, {3, 2}, {3, 4}, {4, 1}, {4, 5}});
        const auto plan = plan_emission(graph, analyse_structure(graph));
        check(plan.complete, "nested loops plan completely");
        check_well_formed(plan, 6, "nested loops");
        int depth = 0;
        int deepest = 0;
        for (const auto& step : plan.steps) {
            if (step.kind == PlanStepKind::LoopBegin) {
                deepest = ++depth > deepest ? depth : deepest;
            } else if (step.kind == PlanStepKind::LoopEnd) {
                --depth;
            }
        }
        check(deepest == 2, "the inner loop is planned inside the outer");
    }

    // A branch whose arms both end the program has no merge, and the plan
    // has to survive that rather than walk into a block that is not there.
    {
        const auto graph = make(3, {{0, 1}, {0, 2}});
        const auto plan = plan_emission(graph, analyse_structure(graph));
        check(plan.complete, "a branch with no join still plans");
        check_well_formed(plan, 3, "two exits");
    }

    // A graph with no way out is not a program a shader could be - one
    // always ends - but the planner should not fall over on it. The
    // recursive walk used to refuse it by reaching a block twice; reverse
    // post order cannot do that, because the order is a list. The loops
    // have no exit to merge at, so their regions stay open to the end, and
    // the emitter answers that by inventing a merge block that returns.
    {
        const auto graph = make(
            5, {{0, 1}, {1, 2}, {2, 3}, {3, 2}, {3, 4}, {4, 1}});
        const auto plan = plan_emission(graph, analyse_structure(graph));
        check(plan.complete, "a graph with no exit still plans");
        check_well_formed(plan, 5, "no exit");
    }

    if (failures == 0) {
        std::printf("all emit plan checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
