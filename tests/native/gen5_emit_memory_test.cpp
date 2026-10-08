// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// The buffer declaration has to match what the runtime binds, so the test
// reads the decorations back out of the module rather than trusting that
// the code was copied correctly. A module that declares its buffers
// differently does not fail visibly - it fails as a shader reading
// somebody else's memory.
#include <cstdio>
#include <vector>

#include "gen5_emit_memory.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

// Finds a decoration on a target and returns its first operand, or a
// sentinel when the decoration is absent.
static std::uint32_t decoration_of(
    const std::vector<std::uint32_t>& words,
    std::uint32_t target,
    std::uint32_t decoration) {
    std::size_t index = 5;
    while (index < words.size()) {
        const auto length = words[index] >> 16;
        if (length == 0) {
            break;
        }
        if ((words[index] & 0xFFFFu) ==
                static_cast<std::uint32_t>(ps5spirv::Op::Decorate) &&
            length >= 3 && words[index + 1] == target &&
            words[index + 2] == decoration) {
            return length >= 4 ? words[index + 3] : 1u;
        }
        index += length;
    }
    return 0xFFFFFFFFu;
}

static bool has_opcode(
    const std::vector<std::uint32_t>& words, std::uint32_t opcode) {
    std::size_t index = 5;
    while (index < words.size()) {
        const auto length = words[index] >> 16;
        if (length == 0) {
            break;
        }
        if ((words[index] & 0xFFFFu) == opcode) {
            return true;
        }
        index += length;
    }
    return false;
}

int main() {
    using namespace ps5gen5;
    using ps5spirv::Decoration;
    using ps5spirv::Op;

    // No buffers means no declaration at all, rather than an empty array
    // that the runtime would try to bind.
    {
        ps5spirv::ModuleBuilder module;
        const auto uint_type = module.type_int(32, false);
        const auto buffers = declare_guest_buffers(module, uint_type, 0);
        check(!buffers.declared, "no buffers declares nothing");
        check(!has_opcode(module.build(),
                          static_cast<std::uint32_t>(Op::TypeRuntimeArray)),
              "and emits no runtime array");
    }

    // The decorations the runtime binds against.
    {
        ps5spirv::ModuleBuilder module;
        const auto uint_type = module.type_int(32, false);
        const auto buffers = declare_guest_buffers(module, uint_type, 8);
        check(buffers.declared, "eight buffers are declared");
        const auto words = module.build();
        check(decoration_of(words, buffers.variable,
                            static_cast<std::uint32_t>(
                                Decoration::DescriptorSet)) == 0,
              "the buffers are in descriptor set zero");
        check(decoration_of(words, buffers.variable,
                            static_cast<std::uint32_t>(Decoration::Binding)) ==
                  0,
              "at binding zero");
        check(has_opcode(words,
                         static_cast<std::uint32_t>(Op::TypeRuntimeArray)),
              "the words are an unbounded array");
        // The stride has to be four: a guest buffer is addressed in words,
        // and a wrong stride reads the right buffer at the wrong place.
        bool stride_is_four = false;
        std::size_t index = 5;
        while (index < words.size()) {
            const auto length = words[index] >> 16;
            if (length == 0) {
                break;
            }
            if ((words[index] & 0xFFFFu) ==
                    static_cast<std::uint32_t>(Op::Decorate) &&
                length >= 4 &&
                words[index + 2] ==
                    static_cast<std::uint32_t>(Decoration::ArrayStride) &&
                words[index + 3] == 4) {
                stride_is_four = true;
            }
            index += length;
        }
        check(stride_is_four, "the array stride is four bytes");
    }

    // A load walks three indexes: the binding, the struct's member, the
    // word. Two would reach the block instead of the word inside it.
    {
        ps5spirv::ModuleBuilder module;
        const auto uint_type = module.type_int(32, false);
        const auto buffers = declare_guest_buffers(module, uint_type, 4);
        const auto address = module.constant(uint_type, 17);
        const auto value = emit_buffer_load(module, buffers, 2, address);
        check(value != 0, "a load produces a value");
        const auto words = module.build();
        bool chain_has_three_indexes = false;
        std::size_t index = 5;
        while (index < words.size()) {
            const auto length = words[index] >> 16;
            if (length == 0) {
                break;
            }
            if ((words[index] & 0xFFFFu) ==
                static_cast<std::uint32_t>(Op::AccessChain)) {
                // result type, result, base, then the indexes
                chain_has_three_indexes = length == 7;
            }
            index += length;
        }
        check(chain_has_three_indexes,
              "the access chain names binding, member and word");
    }

    // A store writes through the same chain.
    {
        ps5spirv::ModuleBuilder module;
        const auto uint_type = module.type_int(32, false);
        const auto buffers = declare_guest_buffers(module, uint_type, 4);
        const auto address = module.constant(uint_type, 3);
        const auto value = module.constant(uint_type, 42);
        emit_buffer_store(module, buffers, 1, address, value);
        check(has_opcode(module.build(),
                         static_cast<std::uint32_t>(Op::Store)),
              "a store emits a store");
    }

    if (failures == 0) {
        std::printf("all buffer declaration checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
