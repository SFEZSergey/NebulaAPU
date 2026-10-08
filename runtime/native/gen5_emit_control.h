// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Turns an emission plan into SPIR-V control flow.
//
// Everything hard about this was decided by the planner: which blocks come
// in which order, which region each is inside, where each region merges.
// What is left is the shape SPIR-V demands - a label per block, a merge
// instruction before the terminator of any block that opens a region, and
// a terminator on every block, with nothing after it until the next label.
//
// The block bodies are the caller's: it is handed a block and emits the
// instructions in it, which is where registers and arithmetic will go when
// they are ported. This file only ever emits the skeleton, so it can be
// finished and tested before any of that exists.

#ifndef PS5_GEN5_EMIT_CONTROL_H
#define PS5_GEN5_EMIT_CONTROL_H

#include <cstdint>
#include <cstdlib>
#include <set>
#include <utility>
#include <vector>

#include "gen5_emit_plan.h"
#include "spirv_builder.h"

namespace ps5gen5 {

// What a block's terminator should be. The caller decides, because whether
// a branch is conditional and on what depends on the instruction that ends
// the block, which this file does not decode.
struct BlockTerminator {
    // Zero when the block's branch is unconditional or it has no successor.
    std::uint32_t condition = 0;
};

// Labels for everything the plan names, allocated up front because a merge
// instruction has to name a label that appears later in the module.
struct ControlLabels {
    std::vector<std::uint32_t> block;
    // A loop needs two labels of its own beyond its blocks': one for the
    // continue target that the back edge lands on, one for the block after
    // the loop. The continue target is a block of its own even when the
    // latch is an ordinary block, because SPIR-V wants somewhere to put the
    // back edge that is not the latch's terminator.
    std::vector<std::uint32_t> loop_continue;
    std::vector<std::uint32_t> loop_merge;
    std::vector<std::uint32_t> selection_merge;
    // The invocation's loop budget, when any loop was emitted: a Private
    // variable the entry point has to list.
    std::uint32_t loop_budget = 0;
};

// How many back edges one invocation may take across all its loops before
// every loop leaves at its next one. A translated loop whose exit depends
// on something the translation got wrong - a waterfall loop that never
// matches its own lane, a count read from the wrong buffer - otherwise
// runs until the driver resets the GPU and takes the process's device
// with it. A million was not enough: at 4K a million iterations a pixel
// is still seconds of GPU, and the Team Asobi logo scene lost the device
// with it; at 16384 it ran and loading was unaffected.
// PS5RT_LOOP_BUDGET sets it; 0 turns it off.
inline std::uint32_t loop_budget_limit() {
    static const std::uint32_t limit = [] {
        const auto* value = std::getenv("PS5RT_LOOP_BUDGET");
        return value == nullptr
            ? 16384u
            : static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0));
    }();
    return limit;
}

inline ControlLabels allocate_control_labels(
    ps5spirv::ModuleBuilder& module, const CfgGraph& graph) {
    ControlLabels labels;
    labels.block.resize(graph.size());
    labels.loop_continue.assign(graph.size(), 0);
    labels.loop_merge.assign(graph.size(), 0);
    labels.selection_merge.assign(graph.size(), 0);
    for (std::size_t index = 0; index < graph.size(); ++index) {
        labels.block[index] = module.allocate_id();
    }
    return labels;
}

// Emits the control flow of a planned function body. `emit_block` is called
// with a block index and returns its terminator's condition, or zero for an
// unconditional one.
template <typename EmitBlock>
bool emit_control_flow(
    ps5spirv::ModuleBuilder& module,
    const CfgGraph& graph,
    const EmitPlan& plan,
    ControlLabels& labels,
    const EmitBlock& emit_block) {
    if (!plan.complete) {
        return false;
    }
    const auto label_of = [&](std::size_t block) {
        return block < labels.block.size() ? labels.block[block] : 0u;
    };

    // The first block needs a label before anything, and every region that
    // opens has to be closed in the reverse order it opened.
    std::vector<PlanStep> open;
    bool started = false;
    std::vector<bool> loop_merge_is_synthetic_(graph.size(), false);

    // A block may be the merge block of exactly one construct. Nested
    // branches that leave through the same exit all have that exit as
    // their immediate post dominator, so naming it directly makes them all
    // claim it - which a driver is entitled to do anything with, and one
    // does: it faults inside pipeline creation, taking the process with
    // it, on a module that claims one block 54 times.
    //
    // The second and later claimants get a merge block of their own: an
    // empty block that branches to the real exit. For that to be the merge
    // it claims to be, the arms inside the construct have to leave through
    // it rather than past it, so while the construct is open every branch
    // to the real exit is retargeted at it. They nest, so the innermost
    // override is the one that applies.
    std::set<std::size_t> claimed_merges;
    struct MergeOverride {
        std::size_t block = kNoBlock;
        std::uint32_t label = 0;
    };
    std::vector<MergeOverride> overrides;
    std::vector<std::uint32_t> open_synthetic;
    const auto main_exit = main_exit_block(graph);

    const auto target_label_of = [&](std::size_t block) {
        for (auto entry = overrides.rbegin(); entry != overrides.rend();
             ++entry) {
            if (entry->block == block) {
                return entry->label;
            }
        }
        return label_of(block);
    };
    // Claims a merge, returning a synthetic label when the block was
    // already claimed and zero when it was not.
    const auto claim_merge = [&](std::size_t block) -> std::uint32_t {
        if (block == kNoBlock) {
            return 0;
        }
        if (claimed_merges.insert(block).second) {
            return 0;
        }
        const auto label = module.allocate_id();
        overrides.push_back(MergeOverride{block, label});
        return label;
    };

    for (std::size_t index = 0; index < plan.steps.size(); ++index) {
        const auto& step = plan.steps[index];
        switch (step.kind) {
            case PlanStepKind::LoopBegin: {
                if (labels.loop_continue[step.block] == 0) {
                    labels.loop_continue[step.block] = module.allocate_id();
                }
                auto loop_synthetic = std::uint32_t{0};
                if (labels.loop_merge[step.block] == 0) {
                    if (step.merge != kNoBlock) {
                        loop_synthetic = claim_merge(step.merge);
                        labels.loop_merge[step.block] = loop_synthetic != 0
                            ? loop_synthetic
                            : label_of(step.merge);
                    } else {
                        labels.loop_merge[step.block] = module.allocate_id();
                        loop_merge_is_synthetic_[step.block] = true;
                    }
                }
                // No branch to the header is emitted here. The block
                // before the loop already ends with one - that is what
                // makes it the predecessor - and a second puts two
                // terminators in a row. When the header is the entry
                // block there is nothing before it to branch from at all.
                open.push_back(step);
                open_synthetic.push_back(loop_synthetic);
                // Every branch back to the header from inside the loop goes
                // through the continue target: SPIR-V allows one back edge,
                // and a loop continued from two places - the title has
                // pixel shaders that do - had two.
                overrides.push_back(MergeOverride{
                    step.block, labels.loop_continue[step.block]});
                break;
            }
            case PlanStepKind::SelectionBegin:
                open.push_back(step);
                // No merge: an unreachable block of its own, emitted when
                // the selection closes. Not an override - nothing branches
                // to it.
                open_synthetic.push_back(
                    step.merge == kNoBlock ? module.allocate_id()
                                           : claim_merge(step.merge));
                break;
            case PlanStepKind::Block: {
                module.add_function_word(
                    ps5spirv::Op::Label, {label_of(step.block)});
                started = true;
                const auto condition = emit_block(step.block);

                // A block that opens a region says so here, between its
                // instructions and its terminator, which is the only place
                // SPIR-V allows it.
                const auto* opener = open.empty() ? nullptr : &open.back();
                // A loop header that is a selection too: the loop's merge
                // stays in the header, which then branches on to a block of
                // its own that carries the selection.
                if (opener != nullptr && opener->block == step.block &&
                    opener->kind == PlanStepKind::SelectionBegin &&
                    open.size() >= 2 &&
                    open[open.size() - 2].kind == PlanStepKind::LoopBegin &&
                    open[open.size() - 2].block == step.block) {
                    module.add_function_word(
                        ps5spirv::Op::LoopMerge,
                        {labels.loop_merge[step.block],
                         labels.loop_continue[step.block],
                         0});
                    const auto split = module.allocate_id();
                    module.add_function_word(ps5spirv::Op::Branch, {split});
                    module.add_function_word(ps5spirv::Op::Label, {split});
                }
                if (opener != nullptr && opener->block == step.block) {
                    if (opener->kind == PlanStepKind::LoopBegin) {
                        module.add_function_word(
                            ps5spirv::Op::LoopMerge,
                            {labels.loop_merge[step.block],
                             labels.loop_continue[step.block],
                             0});
                    } else {
                        const auto synthetic = open_synthetic.empty()
                            ? std::uint32_t{0}
                            : open_synthetic.back();
                        if (labels.selection_merge[step.block] == 0) {
                            labels.selection_merge[step.block] =
                                synthetic != 0 ? synthetic
                                               : label_of(opener->merge);
                        }
                        module.add_function_word(
                            ps5spirv::Op::SelectionMerge,
                            {labels.selection_merge[step.block], 0});
                    }
                }

                const auto& successors = graph.successors[step.block];
                // A block that only ends the program and is reached from
                // several places - the kill-everything tail a pixel shader
                // keeps after its main s_endpgm - belongs to none of the
                // constructs that jump to it, and a branch out of a
                // construct to it is not a structured exit. Each such
                // branch gets a copy of it instead, emitted right here,
                // inside the construct the branch is in.
                std::vector<std::pair<std::uint32_t, std::size_t>> copies;
                const auto branch_label = [&](std::size_t target) {
                    if (copied_exit_block(graph, target, main_exit)) {
                        const auto copy = module.allocate_id();
                        copies.emplace_back(copy, target);
                        return copy;
                    }
                    return target_label_of(target);
                };
                if (successors.empty()) {
                    module.add_function_word(ps5spirv::Op::Return, {});
                } else if (successors.size() == 1) {
                    module.add_function_word(
                        ps5spirv::Op::Branch,
                        {branch_label(successors[0])});
                } else {
                    const auto first = branch_label(successors[0]);
                    const auto second = branch_label(successors[1]);
                    module.add_function_word(
                        ps5spirv::Op::BranchConditional,
                        {condition, first, second});
                }
                for (const auto& [copy, target] : copies) {
                    module.add_function_word(ps5spirv::Op::Label, {copy});
                    (void)emit_block(target);
                    module.add_function_word(ps5spirv::Op::Return, {});
                }
                break;
            }
            case PlanStepKind::LoopEnd: {
                // The continue target carries the back edge and is this
                // file's own block, so it is emitted here.
                module.add_function_word(
                    ps5spirv::Op::Label,
                    {labels.loop_continue[step.block]});
                if (loop_budget_limit() != 0) {
                    const auto uint_type = module.type_int(32, false);
                    const auto bool_type = module.type_bool();
                    if (labels.loop_budget == 0) {
                        const auto pointer = module.type_pointer(
                            ps5spirv::StorageClass::Private, uint_type);
                        labels.loop_budget = module.allocate_id();
                        module.add_global(
                            ps5spirv::Op::Variable,
                            {pointer, labels.loop_budget,
                             static_cast<std::uint32_t>(
                                 ps5spirv::StorageClass::Private),
                             module.constant(uint_type, 0)});
                        module.add_name(labels.loop_budget, "loop_budget");
                    }
                    const auto spent = module.allocate_id();
                    module.add_function_word(
                        ps5spirv::Op::Load,
                        {uint_type, spent, labels.loop_budget});
                    const auto next = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(128u),
                        {uint_type, next, spent,
                         module.constant(uint_type, 1)});
                    module.add_function_word(
                        ps5spirv::Op::Store, {labels.loop_budget, next});
                    const auto within = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(176u),
                        {bool_type, within, next,
                         module.constant(uint_type, loop_budget_limit())});
                    module.add_function_word(
                        ps5spirv::Op::BranchConditional,
                        {within, label_of(step.block),
                         labels.loop_merge[step.block]});
                } else {
                    module.add_function_word(
                        ps5spirv::Op::Branch, {label_of(step.block)});
                }
                // The merge is not. When the loop leaves to a real block,
                // the plan emits that block next and it brings its own
                // label; emitting one here as well gives the module two
                // labels for one block and instructions after a
                // terminator. Only a loop that leaves nowhere needs a
                // merge block invented, and then it is an exit.
                const auto* opener = &step;
                (void)opener;
                if (loop_merge_is_synthetic_[step.block]) {
                    module.add_function_word(
                        ps5spirv::Op::Label,
                        {labels.loop_merge[step.block]});
                    module.add_function_word(ps5spirv::Op::Return, {});
                }
                // The back edge override goes first: it was pushed last.
                overrides.pop_back();
                const auto loop_exit =
                    open.empty() ? kNoBlock : open.back().merge;
                const auto loop_synthetic_merge = open_synthetic.empty()
                    ? std::uint32_t{0}
                    : open_synthetic.back();
                if (!open.empty()) {
                    open.pop_back();
                }
                if (!open_synthetic.empty()) {
                    open_synthetic.pop_back();
                }
                if (loop_synthetic_merge != 0) {
                    // Popped first, so the branch out resolves to whatever
                    // the enclosing construct made of the same exit.
                    overrides.pop_back();
                    module.add_function_word(
                        ps5spirv::Op::Label, {loop_synthetic_merge});
                    module.add_function_word(
                        ps5spirv::Op::Branch, {target_label_of(loop_exit)});
                }
                break;
            }
            case PlanStepKind::SelectionEnd: {
                const auto exit = open.empty() ? kNoBlock : open.back().merge;
                const auto synthetic = open_synthetic.empty()
                    ? std::uint32_t{0}
                    : open_synthetic.back();
                if (!open.empty()) {
                    open.pop_back();
                }
                if (!open_synthetic.empty()) {
                    open_synthetic.pop_back();
                }
                if (synthetic != 0 && exit == kNoBlock) {
                    module.add_function_word(
                        ps5spirv::Op::Label, {synthetic});
                    module.add_function_word(ps5spirv::Op::Unreachable, {});
                } else if (synthetic != 0) {
                    overrides.pop_back();
                    module.add_function_word(
                        ps5spirv::Op::Label, {synthetic});
                    module.add_function_word(
                        ps5spirv::Op::Branch, {target_label_of(exit)});
                }
                break;
            }
        }
    }
    if (!started) {
        return false;
    }
    // A function's last block still needs a terminator, and a plan that
    // ends inside a region would leave one open.
    return open.empty();
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_EMIT_CONTROL_H
