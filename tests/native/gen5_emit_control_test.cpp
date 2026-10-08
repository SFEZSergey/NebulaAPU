// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// The control flow skeleton, checked by walking the words that come out.
// SPIR-V's rules here are unforgiving and a driver's only reply is a
// rejection with no line number, so the things that make a module legal
// are checked directly: a label starts every block, a terminator ends it,
// nothing follows a terminator until the next label, and a merge sits
// immediately before the terminator of the block that opens a region.
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "gen5_emit_control.h"

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

static bool is_terminator(std::uint32_t opcode) {
    using ps5spirv::Op;
    return opcode == static_cast<std::uint32_t>(Op::Branch) ||
        opcode == static_cast<std::uint32_t>(Op::BranchConditional) ||
        opcode == static_cast<std::uint32_t>(Op::Return) ||
        opcode == static_cast<std::uint32_t>(Op::Switch);
}

// Walks the function words and checks the block rules.
static void check_blocks(
    const std::vector<std::uint32_t>& words, const char* what) {
    using ps5spirv::Op;
    bool in_block = false;
    bool after_terminator = false;
    std::uint32_t previous = 0;
    std::size_t index = 5;
    std::set<std::uint32_t> labels;
    while (index < words.size()) {
        const auto length = words[index] >> 16;
        const auto opcode = words[index] & 0xFFFFu;
        if (length == 0) {
            std::printf("FAIL %s: zero length instruction\n", what);
            ++failures;
            return;
        }
        if (opcode == static_cast<std::uint32_t>(Op::Label)) {
            if (in_block && !after_terminator) {
                std::printf("FAIL %s: a block had no terminator\n", what);
                ++failures;
            }
            if (!labels.insert(words[index + 1]).second) {
                std::printf("FAIL %s: label emitted twice\n", what);
                ++failures;
            }
            in_block = true;
            after_terminator = false;
        } else if (opcode == static_cast<std::uint32_t>(Op::FunctionEnd)) {
            // A function ends after its last terminator, which is not an
            // instruction inside a block.
            if (in_block && !after_terminator) {
                std::printf(
                    "FAIL %s: the last block had no terminator\n", what);
                ++failures;
            }
            in_block = false;
        } else if (in_block) {
            if (after_terminator) {
                std::printf("FAIL %s: instruction after a terminator\n", what);
                ++failures;
                return;
            }
            if (is_terminator(opcode)) {
                after_terminator = true;
                // A merge, if there was one, must be the instruction
                // immediately before this.
                (void)previous;
            }
        }
        previous = opcode;
        index += length;
    }
    if (in_block && !after_terminator) {
        std::printf("FAIL %s: the last block had no terminator\n", what);
        ++failures;
    }
}

// Every merge instruction must be followed immediately by a terminator.
static void check_merges(
    const std::vector<std::uint32_t>& words, const char* what) {
    using ps5spirv::Op;
    std::size_t index = 5;
    bool pending_merge = false;
    while (index < words.size()) {
        const auto length = words[index] >> 16;
        const auto opcode = words[index] & 0xFFFFu;
        if (length == 0) {
            return;
        }
        if (pending_merge && !is_terminator(opcode)) {
            std::printf("FAIL %s: a merge was not followed by a terminator\n",
                        what);
            ++failures;
            return;
        }
        pending_merge =
            opcode == static_cast<std::uint32_t>(Op::LoopMerge) ||
            opcode == static_cast<std::uint32_t>(Op::SelectionMerge);
        index += length;
    }
}

static std::vector<std::uint32_t> emit(
    const ps5gen5::CfgGraph& graph, bool& ok) {
    using namespace ps5gen5;
    ps5spirv::ModuleBuilder module;
    module.add_capability(ps5spirv::Capability::Shader);
    module.set_logical_glsl450_memory_model();
    const auto void_type = module.type_void();
    const auto bool_type = module.type_bool();
    const auto function_type = module.type_function(void_type);
    const auto entry = module.allocate_id();
    module.add_entry_point(
        ps5spirv::ExecutionModel::GLCompute, entry, "main", {});
    module.add_execution_mode(
        entry, ps5spirv::ExecutionMode::LocalSize, {1, 1, 1});
    module.add_function_word(
        ps5spirv::Op::Function, {void_type, entry, 0, function_type});

    const auto structure = analyse_structure(graph);
    const auto plan = plan_emission(graph, structure);
    auto labels = allocate_control_labels(module, graph);
    // A condition value the branches can use; its definition does not
    // matter to the shape being tested.
    const auto condition = module.constant(bool_type, 1);
    ok = emit_control_flow(
        module, graph, plan, labels,
        [&](std::size_t) { return condition; });
    module.add_function_word(ps5spirv::Op::FunctionEnd, {});
    return module.build();
}

int main() {
    using namespace ps5gen5;

    {
        bool ok = false;
        const auto words = emit(make(3, {{0, 1}, {1, 2}}), ok);
        check(ok, "a chain emits");
        check_blocks(words, "chain");
        check_merges(words, "chain");
    }
    {
        bool ok = false;
        const auto words =
            emit(make(4, {{0, 1}, {0, 2}, {1, 3}, {2, 3}}), ok);
        check(ok, "a diamond emits");
        check_blocks(words, "diamond");
        check_merges(words, "diamond");
    }
    {
        bool ok = false;
        const auto words =
            emit(make(4, {{0, 1}, {1, 2}, {2, 1}, {2, 3}}), ok);
        check(ok, "a loop emits");
        check_blocks(words, "loop");
        check_merges(words, "loop");
        // A loop must carry a merge instruction; without it the module is
        // a graph rather than structured control flow.
        bool found = false;
        std::size_t index = 5;
        while (index < words.size()) {
            const auto length = words[index] >> 16;
            if (length == 0) {
                break;
            }
            if ((words[index] & 0xFFFFu) ==
                static_cast<std::uint32_t>(ps5spirv::Op::LoopMerge)) {
                found = true;
            }
            index += length;
        }
        check(found, "a loop emits OpLoopMerge");
    }
    {
        bool ok = false;
        const auto words = emit(
            make(6,
                 {{0, 1}, {1, 2}, {2, 3}, {3, 2}, {3, 4}, {4, 1}, {4, 5}}),
            ok);
        check(ok, "nested loops emit");
        check_blocks(words, "nested loops");
        check_merges(words, "nested loops");
    }

    // A block may be the merge block of exactly one construct. Two
    // branches nested inside one another leave through the same block, and
    // that block is the immediate post dominator of both - so naming it
    // directly makes both claim it. A driver is entitled to do anything
    // with such a module, and one faults inside pipeline creation and
    // takes the process with it.
    {
        bool ok = false;
        // Block 0 branches to 1 and 5; block 1 branches to 2 and 3; both
        // arms reach 4, which reaches 5. The inner selection merges at 4
        // and the outer at 5, but make the inner arms leave to 5 as well
        // and the two constructs collide.
        const auto words = emit(
            make(6,
                 {{0, 1}, {0, 5}, {1, 2}, {1, 3}, {2, 5}, {3, 5}, {5, 4}}),
            ok);
        check(ok, "nested branches leaving through one block emit");
        std::map<std::uint32_t, std::uint32_t> claimed;
        std::size_t index = 5;
        std::uint32_t current = 0;
        bool collided = false;
        while (index < words.size()) {
            const auto length = words[index] >> 16;
            const auto opcode = words[index] & 0xFFFF;
            if (length == 0 || index + length > words.size()) {
                break;
            }
            if (opcode == 248) {
                current = words[index + 1];
            } else if (opcode == 246 || opcode == 247) {
                const auto merge = words[index + 1];
                if (claimed.count(merge) != 0 && claimed[merge] != current) {
                    collided = true;
                }
                claimed[merge] = current;
            }
            index += length;
        }
        check(!collided,
              "no block is named as the merge of two constructs");
    }

    if (failures == 0) {
        std::printf("all emit control checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
