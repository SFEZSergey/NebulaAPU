// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// What crosses between stages. GCN has no signature: a vertex shader
// exports its position and parameters as instructions and a pixel shader
// interpolates them as instructions, so the SPIR-V signature has to be
// built out of what those instructions name. Getting a number wrong here
// does not fail - it produces a module that compiles and reads the wrong
// attribute, so the numbers are what these checks are about.
#include <cstdio>
#include <vector>

#include "gen5_emit_stage.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

// Finds every OpDecorate of a kind in a built module, as pairs of the id
// decorated and the value.
static std::vector<std::pair<std::uint32_t, std::uint32_t>> decorations_of(
    const std::vector<std::uint32_t>& words, std::uint32_t decoration) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> found;
    std::size_t at = 5;
    while (at < words.size()) {
        const auto length = words[at] >> 16;
        const auto opcode = words[at] & 0xFFFF;
        if (length == 0 || at + length > words.size()) {
            break;
        }
        if (opcode == 71 && length >= 4 && words[at + 2] == decoration) {
            found.push_back({words[at + 1], words[at + 3]});
        }
        at += length;
    }
    return found;
}

static std::uint32_t count_of(
    const std::vector<std::uint32_t>& words, std::uint32_t opcode) {
    std::uint32_t count = 0;
    std::size_t at = 5;
    while (at < words.size()) {
        const auto length = words[at] >> 16;
        if (length == 0 || at + length > words.size()) {
            break;
        }
        if ((words[at] & 0xFFFF) == opcode) {
            ++count;
        }
        at += length;
    }
    return count;
}

int main() {
    using namespace ps5gen5;

    // A vertex shader's position is a built-in, not a location. A module
    // that declares it as location zero has a vertex shader whose output
    // the rasteriser never reads.
    {
        ps5spirv::ModuleBuilder module;
        const auto float_type = module.type_float(32);
        const auto uint_type = module.type_int(32, false);
        auto io = make_stage_io(
            module, StageKind::Vertex, float_type, uint_type);
        const auto one = module.constant(float_type, 0x3F800000u);
        const std::uint32_t components[4] = {one, one, one, one};
        emit_export(module, io, kExportPositionFirst, 0xF, components);
        emit_export(module, io, kExportParameterFirst + 1, 0x3, components);
        const auto words = module.build();

        // The position is the member of a block, the way every shader
        // compiler declares it; a lone variable decorated as the position
        // is linked wrongly against a pixel stage that has inputs.
        std::uint32_t position_members = 0;
        for (std::size_t at = 5; at < words.size();) {
            const auto length = words[at] >> 16;
            if (length == 0) {
                break;
            }
            if ((words[at] & 0xFFFF) == 72 && length >= 5 &&
                words[at + 3] ==
                    static_cast<std::uint32_t>(
                        ps5spirv::Decoration::BuiltIn) &&
                words[at + 4] ==
                    static_cast<std::uint32_t>(
                        ps5spirv::BuiltIn::Position)) {
                ++position_members;
            }
            at += length;
        }
        check(position_members == 1 &&
                  decorations_of(
                      words, static_cast<std::uint32_t>(
                                 ps5spirv::Decoration::BuiltIn)).empty(),
              "an export to a position target declares the position "
              "built-in as the member of a block");
        const auto locations = decorations_of(
            words, static_cast<std::uint32_t>(
                       ps5spirv::Decoration::Location));
        check(locations.size() == 1 && locations[0].second == 1,
              "an export to parameter one declares location one, not the "
              "target number");
        check(count_of(words, 62) == 2, "both exports store");
    }

    // The same target numbers mean something else in a pixel shader, and
    // the wrong one there is a colour written to a parameter.
    {
        ps5spirv::ModuleBuilder module;
        const auto float_type = module.type_float(32);
        const auto uint_type = module.type_int(32, false);
        auto io = make_stage_io(
            module, StageKind::Pixel, float_type, uint_type);
        const auto zero = module.constant(float_type, 0);
        const std::uint32_t components[4] = {zero, zero, zero, zero};
        emit_export(module, io, 0, 0xF, components);
        // A position export in a pixel shader is not one; it must declare
        // nothing rather than a second output.
        emit_export(module, io, kExportPositionFirst, 0xF, components);
        const auto words = module.build();

        const auto locations = decorations_of(
            words, static_cast<std::uint32_t>(
                       ps5spirv::Decoration::Location));
        check(locations.size() == 1 && locations[0].second == 0,
              "a colour export declares one output at its own target");
        check(decorations_of(
                  words, static_cast<std::uint32_t>(
                             ps5spirv::Decoration::BuiltIn)).empty(),
              "a pixel shader declares no position");
        check(count_of(words, 62) == 1, "only the colour export stores");
    }

    // An interpolated attribute becomes an input at the attribute's own
    // number, and the two instructions that build one value read one
    // input rather than two.
    {
        ps5spirv::ModuleBuilder module;
        const auto float_type = module.type_float(32);
        const auto uint_type = module.type_int(32, false);
        auto io = make_stage_io(
            module, StageKind::Pixel, float_type, uint_type);
        emit_interpolate(module, io, 3, 0);
        emit_interpolate(module, io, 3, 1);
        const auto words = module.build();

        const auto locations = decorations_of(
            words, static_cast<std::uint32_t>(
                       ps5spirv::Decoration::Location));
        check(locations.size() == 1 && locations[0].second == 3,
              "two channels of one attribute declare one input at its "
              "number");
        check(io.interface_ids.size() == 1,
              "and name it once in the interface");
    }

    // Depth is a built-in of its own, not one of the colour targets. A
    // pixel shader that exports depth and declares none leaves its depth
    // on the floor, which is not a colour being wrong - it is a surface
    // sorting against the value the rasteriser interpolated instead of
    // the one the shader computed.
    {
        ps5spirv::ModuleBuilder module;
        const auto float_type = module.type_float(32);
        const auto uint_type = module.type_int(32, false);
        auto io = make_stage_io(
            module, StageKind::Pixel, float_type, uint_type);
        const auto half = module.constant(float_type, 0x3F000000u);
        const std::uint32_t components[4] = {half, 0, 0, 0};
        emit_export(module, io, kExportDepth, 0x1, components);
        const auto words = module.build();

        const auto built_ins = decorations_of(
            words, static_cast<std::uint32_t>(
                       ps5spirv::Decoration::BuiltIn));
        check(built_ins.size() == 1 &&
                  built_ins[0].second ==
                      static_cast<std::uint32_t>(
                          ps5spirv::BuiltIn::FragDepth),
              "a depth export declares the depth built-in");
        check(decorations_of(
                  words, static_cast<std::uint32_t>(
                             ps5spirv::Decoration::Location)).empty(),
              "and no location");
        check(count_of(words, 62) == 1,
              "depth is stored as one value, not as a vector");
    }
    {
        ps5spirv::ModuleBuilder module;
        const auto float_type = module.type_float(32);
        const auto uint_type = module.type_int(32, false);
        auto io = make_stage_io(
            module, StageKind::Vertex, float_type, uint_type);
        const auto half = module.constant(float_type, 0x3F000000u);
        const std::uint32_t components[4] = {half, 0, 0, 0};
        emit_export(module, io, kExportDepth, 0x1, components);
        const auto words = module.build();
        check(count_of(words, 62) == 0,
              "a vertex shader exporting to the depth target writes "
              "nothing");
    }

    if (failures == 0) {
        std::printf("all stage checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
