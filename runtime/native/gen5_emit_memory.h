// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest buffers, declared the way the runtime expects to bind them.
//
// This part is not free to invent. The runtime builds its descriptor sets
// from the resource manifest, so a module that declares its buffers
// differently from the C# translator will not bind: same storage class,
// same decorations, same set and binding, one array of blocks rather than
// one binding each. The layout here is copied from DeclareBuffers with
// that in mind, and the test checks the decorations rather than trusting
// the copy.
//
// What this cannot do yet is decide which binding an instruction uses.
// That comes from the manifest, which the C# scalar evaluator produces by
// running the shader's scalar half ahead of time to resolve its
// descriptors - 2414 lines that have not been ported. So the binding is a
// parameter here, and naming it as a dependency is more useful than
// pretending the resource path is nearly done.

#ifndef PS5_GEN5_EMIT_MEMORY_H
#define PS5_GEN5_EMIT_MEMORY_H

#include <cstdint>

#include "spirv_builder.h"

namespace ps5gen5 {

// The ids of the buffer declaration, kept together because emitting a load
// needs all of them.
struct GuestBuffers {
    std::uint32_t variable = 0;
    std::uint32_t block_pointer = 0;
    std::uint32_t uint_pointer = 0;
    std::uint32_t uint_type = 0;
    bool declared = false;
};

// One binding holding an array of storage blocks, each an unbounded array
// of words. The decorations are what make it match: ArrayStride four on the
// words, Block on the struct, Offset zero on its member, set zero and
// binding zero on the variable.
inline GuestBuffers declare_guest_buffers(
    ps5spirv::ModuleBuilder& module,
    std::uint32_t uint_type,
    std::uint32_t buffer_count) {
    GuestBuffers buffers;
    if (buffer_count == 0) {
        return buffers;
    }
    buffers.uint_type = uint_type;

    const auto runtime_array = module.type_runtime_array(uint_type);
    module.add_decoration(
        runtime_array, ps5spirv::Decoration::ArrayStride, {4});

    const auto block = module.allocate_id();
    module.add_global(ps5spirv::Op::TypeStruct, {block, runtime_array});
    module.add_decoration(block, ps5spirv::Decoration::Block);
    module.add_member_decoration(
        block, 0, ps5spirv::Decoration::Offset, {0});

    const auto count = module.constant(uint_type, buffer_count);
    const auto descriptors = module.type_array(block, count);
    const auto descriptors_pointer = module.type_pointer(
        ps5spirv::StorageClass::StorageBuffer, descriptors);
    buffers.block_pointer = module.type_pointer(
        ps5spirv::StorageClass::StorageBuffer, block);
    buffers.uint_pointer = module.type_pointer(
        ps5spirv::StorageClass::StorageBuffer, uint_type);

    buffers.variable = module.allocate_id();
    module.add_global(
        ps5spirv::Op::Variable,
        {descriptors_pointer, buffers.variable,
         static_cast<std::uint32_t>(ps5spirv::StorageClass::StorageBuffer)});
    module.add_name(buffers.variable, "guestBuffers");
    module.add_decoration(
        buffers.variable, ps5spirv::Decoration::DescriptorSet, {0});
    module.add_decoration(
        buffers.variable, ps5spirv::Decoration::Binding, {0});
    buffers.declared = true;
    return buffers;
}

// A pointer to one word of one buffer. The chain is the binding, then the
// struct's only member, then the word - three indexes, because the
// declaration is an array of blocks each holding an array.
inline std::uint32_t buffer_word_pointer(
    ps5spirv::ModuleBuilder& module,
    const GuestBuffers& buffers,
    std::uint32_t binding,
    std::uint32_t dword_address) {
    const auto pointer = module.allocate_id();
    module.add_function_word(
        ps5spirv::Op::AccessChain,
        {buffers.uint_pointer, pointer, buffers.variable,
         module.constant(buffers.uint_type, binding),
         module.constant(buffers.uint_type, 0),
         dword_address});
    return pointer;
}

inline std::uint32_t emit_buffer_load(
    ps5spirv::ModuleBuilder& module,
    const GuestBuffers& buffers,
    std::uint32_t binding,
    std::uint32_t dword_address) {
    const auto pointer =
        buffer_word_pointer(module, buffers, binding, dword_address);
    const auto value = module.allocate_id();
    module.add_function_word(
        ps5spirv::Op::Load, {buffers.uint_type, value, pointer});
    return value;
}

inline void emit_buffer_store(
    ps5spirv::ModuleBuilder& module,
    const GuestBuffers& buffers,
    std::uint32_t binding,
    std::uint32_t dword_address,
    std::uint32_t value) {
    const auto pointer =
        buffer_word_pointer(module, buffers, binding, dword_address);
    module.add_function_word(ps5spirv::Op::Store, {pointer, value});
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_EMIT_MEMORY_H
