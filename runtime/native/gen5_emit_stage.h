// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// What makes a vertex or pixel shader different from a compute one: the
// values that cross between stages.
//
// GCN says nothing about inputs and outputs as declarations. A vertex
// shader ends by exporting its position and its parameters, a pixel shader
// begins by interpolating those parameters and ends by exporting a colour,
// and all of it is instructions rather than a signature. SPIR-V wants the
// signature, so this builds one out of the exports and interpolations as
// they are met - an export to parameter three declares an output at
// location three, and an interpolation of attribute three in a pixel
// shader declares the input that matches it.
//
// The locations are the attribute numbers the hardware uses, unchanged.
// Renumbering them would be free here and wrong: the two stages are
// translated separately and only agree because both take their numbers
// from the same place.

#ifndef PS5_GEN5_EMIT_STAGE_H
#define PS5_GEN5_EMIT_STAGE_H

#include <cstdint>
#include <cstdlib>
#include <map>
#include <vector>

#include "spirv_builder.h"

namespace ps5gen5 {

enum class StageKind {
    Compute,
    Vertex,
    Pixel,
};

// Export targets, as the instruction encodes them.
constexpr std::uint32_t kExportColourFirst = 0;
constexpr std::uint32_t kExportColourLast = 7;
constexpr std::uint32_t kExportDepth = 8;
constexpr std::uint32_t kExportPositionFirst = 12;
constexpr std::uint32_t kExportPositionLast = 15;
constexpr std::uint32_t kExportParameterFirst = 32;

struct StageIo {
    StageKind stage = StageKind::Compute;
    std::uint32_t float_type = 0;
    std::uint32_t uint_type = 0;
    std::uint32_t vector4_type = 0;
    std::uint32_t output_pointer = 0;
    std::uint32_t input_pointer = 0;
    // The built-in position a vertex shader writes, and the locations
    // everything else uses.
    std::uint32_t position = 0;
    // The depth a pixel shader writes, which is a built-in of its own
    // rather than one of the colour targets.
    std::uint32_t depth = 0;
    std::map<std::uint32_t, std::uint32_t> outputs;
    std::map<std::uint32_t, std::uint32_t> inputs;
    // Every variable declared here, for the entry point's interface - which
    // from SPIR-V 1.4 has to name all of them.
    std::vector<std::uint32_t> interface_ids;
    // A diagnostic: the vertex stage passes the position it exports on to
    // the pixel stage unchanged and unblended, and the pixel stage writes
    // it as its colour - so a dump of the target shows the clip-space
    // positions the vertex shader actually produced.
    bool debug_position = false;
};

constexpr std::uint32_t kDebugPositionLocation = 31;
constexpr std::uint32_t kDecorationFlat = 14;

inline StageIo make_stage_io(
    ps5spirv::ModuleBuilder& module,
    StageKind stage,
    std::uint32_t float_type,
    std::uint32_t uint_type) {
    StageIo io;
    io.stage = stage;
    io.float_type = float_type;
    io.uint_type = uint_type;
    io.vector4_type = module.type_vector(float_type, 4);
    io.output_pointer = module.type_pointer(
        ps5spirv::StorageClass::Output, io.vector4_type);
    io.input_pointer = module.type_pointer(
        ps5spirv::StorageClass::Input, io.vector4_type);
    return io;
}

// The position a vertex shader writes. It is a built-in rather than a
// location: the rasteriser reads it by name, and a module that declares it
// as location zero instead has a vertex shader that emits nothing.
//
// And it is a member of a block, not a variable of its own. A lone
// position output is valid SPIR-V, but paired with a pixel stage that has
// inputs of its own the driver linked the two wrongly: the draw ran and
// wrote nothing, while the same vertex stage with an input-free pixel stage
// drew. Declared the way every shader compiler declares it - one member,
// decorated as the position, in a block - the pairing holds.
//
// Returns a pointer to the member, made where it is used, since an access
// chain belongs in the function rather than among the globals.
inline std::uint32_t stage_position(
    ps5spirv::ModuleBuilder& module, StageIo& io) {
    if (io.position == 0) {
        const auto block = module.allocate_id();
        module.add_global(ps5spirv::Op::TypeStruct, {block, io.vector4_type});
        module.add_decoration(block, ps5spirv::Decoration::Block, {});
        module.add_member_decoration(
            block, 0, ps5spirv::Decoration::BuiltIn,
            {static_cast<std::uint32_t>(ps5spirv::BuiltIn::Position)});
        const auto pointer = module.type_pointer(
            ps5spirv::StorageClass::Output, block);
        io.position = module.allocate_id();
        module.add_global(
            ps5spirv::Op::Variable,
            {pointer, io.position,
             static_cast<std::uint32_t>(ps5spirv::StorageClass::Output)});
        module.add_name(io.position, "per_vertex");
        io.interface_ids.push_back(io.position);
    }
    const auto member = module.allocate_id();
    module.add_function_word(
        ps5spirv::Op::AccessChain,
        {io.output_pointer, member, io.position,
         module.constant(io.uint_type, 0)});
    return member;
}

// The depth a pixel shader writes. Exporting to the depth target and
// declaring nothing leaves the shader's depth on the floor, which is not
// a colour being wrong - it is a surface sorting against the wrong value.
inline std::uint32_t stage_depth(
    ps5spirv::ModuleBuilder& module, StageIo& io) {
    if (io.depth != 0) {
        return io.depth;
    }
    const auto pointer = module.type_pointer(
        ps5spirv::StorageClass::Output, io.float_type);
    io.depth = module.allocate_id();
    module.add_global(
        ps5spirv::Op::Variable,
        {pointer, io.depth,
         static_cast<std::uint32_t>(ps5spirv::StorageClass::Output)});
    module.add_decoration(
        io.depth, ps5spirv::Decoration::BuiltIn,
        {static_cast<std::uint32_t>(ps5spirv::BuiltIn::FragDepth)});
    module.add_name(io.depth, "depth");
    io.interface_ids.push_back(io.depth);
    return io.depth;
}

inline std::uint32_t stage_output(
    ps5spirv::ModuleBuilder& module,
    StageIo& io,
    std::uint32_t location) {
    const auto found = io.outputs.find(location);
    if (found != io.outputs.end()) {
        return found->second;
    }
    const auto variable = module.allocate_id();
    module.add_global(
        ps5spirv::Op::Variable,
        {io.output_pointer, variable,
         static_cast<std::uint32_t>(ps5spirv::StorageClass::Output)});
    module.add_decoration(
        variable, ps5spirv::Decoration::Location, {location});
    io.outputs[location] = variable;
    io.interface_ids.push_back(variable);
    return variable;
}

inline std::uint32_t stage_input(
    ps5spirv::ModuleBuilder& module,
    StageIo& io,
    std::uint32_t location) {
    const auto found = io.inputs.find(location);
    if (found != io.inputs.end()) {
        return found->second;
    }
    const auto variable = module.allocate_id();
    module.add_global(
        ps5spirv::Op::Variable,
        {io.input_pointer, variable,
         static_cast<std::uint32_t>(ps5spirv::StorageClass::Input)});
    module.add_decoration(
        variable, ps5spirv::Decoration::Location, {location});
    io.inputs[location] = variable;
    io.interface_ids.push_back(variable);
    return variable;
}

// A built-in that arrives as a plain integer rather than a vector: which
// vertex this is, which instance, and the like.
inline std::uint32_t stage_scalar_builtin(
    ps5spirv::ModuleBuilder& module,
    StageIo& io,
    ps5spirv::BuiltIn which) {
    const auto pointer = module.type_pointer(
        ps5spirv::StorageClass::Input, io.uint_type);
    const auto variable = module.allocate_id();
    module.add_global(
        ps5spirv::Op::Variable,
        {pointer, variable,
         static_cast<std::uint32_t>(ps5spirv::StorageClass::Input)});
    module.add_decoration(
        variable, ps5spirv::Decoration::BuiltIn,
        {static_cast<std::uint32_t>(which)});
    io.interface_ids.push_back(variable);
    return variable;
}

// A built-in input that is a vector of four floats - the pixel's own
// position, which a pixel shader is handed in registers on the PS5.
inline std::uint32_t stage_vector_builtin(
    ps5spirv::ModuleBuilder& module,
    StageIo& io,
    ps5spirv::BuiltIn which) {
    const auto variable = module.allocate_id();
    module.add_global(
        ps5spirv::Op::Variable,
        {io.input_pointer, variable,
         static_cast<std::uint32_t>(ps5spirv::StorageClass::Input)});
    module.add_decoration(
        variable, ps5spirv::Decoration::BuiltIn,
        {static_cast<std::uint32_t>(which)});
    io.interface_ids.push_back(variable);
    return variable;
}

// An export writes up to four components, and which of them it writes is a
// mask rather than a count - a shader may export only x and w. The
// components it does not write keep whatever the variable held, which for
// a fresh one is undefined, so this writes the whole vector and fills the
// gaps with zero rather than leaving them.
inline void emit_export(
    ps5spirv::ModuleBuilder& module,
    StageIo& io,
    std::uint32_t target,
    std::uint32_t enabled,
    const std::uint32_t components[4]) {
    std::uint32_t variable = 0;
    if (target >= kExportPositionFirst && target <= kExportPositionLast) {
        if (io.stage != StageKind::Vertex) {
            return;
        }
        variable = stage_position(module, io);
        if (io.debug_position && io.outputs.count(kDebugPositionLocation) == 0) {
            const auto debug =
                stage_output(module, io, kDebugPositionLocation);
            module.add_decoration(
                debug, static_cast<ps5spirv::Decoration>(kDecorationFlat),
                {});
        }
    } else if (target >= kExportParameterFirst) {
        if (io.stage != StageKind::Vertex) {
            return;
        }
        // A diagnostic: PS5RT_NATIVE_DEBUG_NO_PARAMS drops every parameter
        // a vertex stage exports, to tell a stage pair that fails through
        // its parameters from one that fails some other way.
        if (std::getenv("PS5RT_NATIVE_DEBUG_NO_PARAMS") != nullptr) {
            return;
        }
        variable = stage_output(module, io, target - kExportParameterFirst);
        // And PS5RT_NATIVE_DEBUG_CONST_PARAMS writes 0.5 into every
        // parameter instead of what the shader computed.
        if (std::getenv("PS5RT_NATIVE_DEBUG_CONST_PARAMS") != nullptr) {
            const auto half = module.constant(io.float_type, 0x3F000000u);
            const auto value = module.allocate_id();
            module.add_function_word(
                ps5spirv::Op::CompositeConstruct,
                {io.vector4_type, value, half, half, half, half});
            module.add_function_word(ps5spirv::Op::Store, {variable, value});
            return;
        }
    } else if (target <= kExportColourLast) {
        if (io.stage != StageKind::Pixel) {
            return;
        }
        variable = stage_output(module, io, target);
    } else if (target == kExportDepth) {
        if (io.stage != StageKind::Pixel) {
            return;
        }
        // Depth is one value, in the first component, and is stored
        // directly rather than through a vector.
        const auto variable = stage_depth(module, io);
        if ((enabled & 1u) != 0 && components[0] != 0) {
            module.add_function_word(
                ps5spirv::Op::Store, {variable, components[0]});
        }
        return;
    } else {
        // The null target, which has no variable here.
        return;
    }

    const auto zero = module.constant(io.float_type, 0);
    std::vector<std::uint32_t> operands{io.vector4_type, 0};
    const auto value = module.allocate_id();
    operands[1] = value;
    for (std::uint32_t index = 0; index < 4; ++index) {
        operands.push_back(
            (enabled & (1u << index)) != 0 && components[index] != 0
                ? components[index]
                : zero);
    }
    module.add_function_word(
        ps5spirv::Op::CompositeConstruct, operands);
    if (io.debug_position && io.stage == StageKind::Pixel && target == 0) {
        // The colour is the position the vertex stage passed on.
        auto input = io.inputs.find(kDebugPositionLocation);
        std::uint32_t source = 0;
        if (input == io.inputs.end()) {
            source = stage_input(module, io, kDebugPositionLocation);
            module.add_decoration(
                source, static_cast<ps5spirv::Decoration>(kDecorationFlat),
                {});
        } else {
            source = input->second;
        }
        const auto loaded = module.allocate_id();
        module.add_function_word(
            ps5spirv::Op::Load, {io.vector4_type, loaded, source});
        module.add_function_word(ps5spirv::Op::Store, {variable, loaded});
        return;
    }
    module.add_function_word(ps5spirv::Op::Store, {variable, value});
    if (io.debug_position && io.stage == StageKind::Vertex &&
        target >= kExportPositionFirst && target <= kExportPositionLast) {
        module.add_function_word(
            ps5spirv::Op::Store,
            {io.outputs[kDebugPositionLocation], value});
    }
}

// An interpolated attribute. The hardware does this in two instructions
// that build the value from barycentric weights; the interpolation itself
// belongs to the rasteriser in SPIR-V, so what is left is reading the
// component the pair was building.
inline std::uint32_t emit_interpolate(
    ps5spirv::ModuleBuilder& module,
    StageIo& io,
    std::uint32_t attribute,
    std::uint32_t channel) {
    const auto variable = stage_input(module, io, attribute);
    const auto pointer_type = module.type_pointer(
        ps5spirv::StorageClass::Input, io.float_type);
    const auto pointer = module.allocate_id();
    module.add_function_word(
        ps5spirv::Op::AccessChain,
        {pointer_type, pointer, variable,
         module.constant(io.uint_type, channel & 3u)});
    const auto value = module.allocate_id();
    module.add_function_word(
        ps5spirv::Op::Load, {io.float_type, value, pointer});
    return value;
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_EMIT_STAGE_H
