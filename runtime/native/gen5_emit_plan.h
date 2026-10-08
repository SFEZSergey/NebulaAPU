// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The order a shader's blocks are emitted in, and the regions they nest
// inside.
//
// SPIR-V does not take a control flow graph. It takes blocks in an order
// where a loop says up front where it merges and where it continues, and a
// branch says where its arms rejoin. Working that out is the whole
// difference between this translator and the C# one, which sidesteps it
// with a program counter dispatcher and pays for the sidestep with every
// register living in memory.
//
// This produces the plan and nothing else - no SPIR-V, no registers - so
// the hard part can be judged on its own. What makes a plan right is
// structural: every reachable block appears exactly once, every region
// that opens closes, and regions nest rather than overlap. Those are
// invariants a test can check on a graph whose shape is known, and they
// are what the emitter downstream relies on.

#ifndef PS5_GEN5_EMIT_PLAN_H
#define PS5_GEN5_EMIT_PLAN_H

#include <cstdint>
#include <vector>

#include "gen5_structure.h"

namespace ps5gen5 {

enum class PlanStepKind {
    Block,
    LoopBegin,
    LoopEnd,
    SelectionBegin,
    SelectionEnd,
};

struct PlanStep {
    PlanStepKind kind = PlanStepKind::Block;
    // For Block, the block emitted. For LoopBegin, its header. For
    // SelectionBegin, the block whose branch opens it.
    std::size_t block = kNoBlock;
    // Where control rejoins, which is what SPIR-V's merge instructions
    // name. kNoBlock when a region never rejoins - an arm that returns.
    std::size_t merge = kNoBlock;
    // LoopBegin only: the block the back edge comes from.
    std::size_t continue_target = kNoBlock;
};

struct EmitPlan {
    std::vector<PlanStep> steps;
    bool complete = false;
    // Set when a block would have been emitted twice, which means the
    // graph was not the shape this walk assumes. Better to say so than to
    // emit a module that is quietly wrong.
    bool duplicated_block = false;
};

// Which loop a block heads, if any.
inline const NaturalLoop* loop_headed_by(
    const Structure& structure, std::size_t block) {
    for (const auto& loop : structure.loops) {
        if (loop.header == block) {
            return &loop;
        }
    }
    return nullptr;
}

// Where a loop leaves to. A reducible loop can be left from more than one
// block, but SPIR-V needs one merge, so this takes the earliest target
// outside the body - which for the shapes a compiler emits is the only one.
inline std::size_t loop_exit(
    const CfgGraph& graph, const NaturalLoop& loop) {
    std::size_t exit = kNoBlock;
    for (const auto member : loop.body) {
        for (const auto succ : graph.successors[member]) {
            if (loop.body.count(succ) != 0) {
                continue;
            }
            if (exit == kNoBlock || succ < exit) {
                exit = succ;
            }
        }
    }
    return exit;
}

// Blocks are emitted in reverse post order, and the regions they belong to
// are opened and closed around them.
//
// The first version of this followed successors recursively, which works
// for the shapes drawn on a whiteboard and refused four of the title's 34
// shaders - the large ones, 159 to 179 blocks. A block with several
// predecessors that is not a merge point can be reached twice down such a
// walk, and emitting it twice would be worse than refusing.
//
// Reverse post order removes the possibility rather than guarding against
// it: every block is visited once because the order is a list. What is
// left is bookkeeping - a stack of the regions still open, closed when the
// block their merge names comes up.
class EmitPlanner {
public:
    EmitPlanner(const CfgGraph& graph, const Structure& structure)
        : graph_(graph), structure_(structure) {}

    EmitPlan run() {
        EmitPlan plan;
        if (graph_.size() == 0) {
            plan.complete = true;
            return plan;
        }
        const auto order = reverse_post_order(graph_);

        struct OpenRegion {
            std::size_t merge = kNoBlock;
            std::size_t opener = kNoBlock;
            bool is_loop = false;
        };
        std::vector<OpenRegion> open;
        std::vector<bool> emitted(graph_.size(), false);

        for (const auto node : order) {
            // Any region whose merge is this block ends before the block
            // itself: the merge belongs to the level outside.
            //
            // So does a selection whose opener does not dominate the block.
            // Reverse post order can bring up a block the outer construct
            // branches to directly before the inner construct's merge: the
            // title's pixel shaders do it, an outer branch jumping past an
            // inner one to a block that then falls into the shared exit.
            // Emitted inside the inner selection, that block branches to the
            // inner merge from outside the inner header's reach, the module
            // is not structured - spirv-val says the header does not
            // dominate its merge - and the driver's reconvergence goes wrong
            // badly enough to hang the GPU a few hundred frames into the
            // scene after the intro.
            while (!open.empty() &&
                   (open.back().merge == node ||
                    (!open.back().is_loop &&
                     !dominates(open.back().opener, node)))) {
                plan.steps.push_back(
                    PlanStep{open.back().is_loop ? PlanStepKind::LoopEnd
                                                 : PlanStepKind::SelectionEnd,
                             open.back().opener});
                open.pop_back();
            }

            const auto* loop = loop_headed_by(structure_, node);
            if (loop != nullptr) {
                const auto exit = loop_exit(graph_, *loop);
                plan.steps.push_back(
                    PlanStep{PlanStepKind::LoopBegin, node, exit,
                             loop->latch});
                open.push_back(OpenRegion{exit, node, true});
                // A header that branches two ways inside the loop is also
                // a selection. SPIR-V will not have the two in one block,
                // so the emitter splits it; the plan only has to say so.
                const auto& targets = graph_.successors[node];
                if (targets.size() >= 2) {
                    bool leaves = false;
                    for (const auto target : targets) {
                        leaves = leaves || target == exit || target == node;
                    }
                    const auto merge =
                        structure_.immediate_post_dominator[node];
                    if (!leaves && merge != exit &&
                        (merge == kNoBlock || loop->body.count(merge) != 0)) {
                        plan.steps.push_back(PlanStep{
                            PlanStepKind::SelectionBegin, node, merge});
                        open.push_back(OpenRegion{merge, node, false});
                    }
                }
            } else if (graph_.successors[node].size() >= 2) {
                const auto merge = structure_.immediate_post_dominator[node];
                // A branch whose arms never rejoin - one of them runs to an
                // s_endpgm of its own, like the kill-everything tail a
                // pixel shader keeps after its main one - still opens a
                // selection: SPIR-V wants a merge on every conditional
                // branch that is not a loop's. The emitter gives it one
                // that nothing reaches, which is what compilers do for an
                // early return. Leaving it out was "Selection must be
                // structured" in a dozen of the title's modules.
                plan.steps.push_back(
                    PlanStep{PlanStepKind::SelectionBegin, node, merge});
                open.push_back(OpenRegion{merge, node, false});
            }

            if (emitted[node]) {
                plan.duplicated_block = true;
            }
            emitted[node] = true;
            plan.steps.push_back(PlanStep{PlanStepKind::Block, node});
        }

        // A loop whose exit is unreachable, or a selection whose merge was
        // never visited, leaves its region open; close them in order.
        while (!open.empty()) {
            plan.steps.push_back(
                PlanStep{open.back().is_loop ? PlanStepKind::LoopEnd
                                             : PlanStepKind::SelectionEnd,
                         open.back().opener});
            open.pop_back();
        }

        plan.complete = !plan.duplicated_block;
        return plan;
    }

private:
    bool dominates(std::size_t dominator, std::size_t block) const {
        for (std::size_t steps = 0; steps <= graph_.size(); ++steps) {
            if (block == dominator) {
                return true;
            }
            if (block >= structure_.immediate_dominator.size()) {
                return false;
            }
            const auto up = structure_.immediate_dominator[block];
            if (up == block || up == kNoBlock) {
                return false;
            }
            block = up;
        }
        return false;
    }

    const CfgGraph& graph_;
    const Structure& structure_;
};

inline EmitPlan plan_emission(
    const CfgGraph& graph, const Structure& structure) {
    return EmitPlanner(graph, structure).run();
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_EMIT_PLAN_H
