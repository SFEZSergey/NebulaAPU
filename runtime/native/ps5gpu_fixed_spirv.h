// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <spirv-headers/spirv.hpp>

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace ps5gpu::fixed_spirv {

inline void emit(
    std::vector<std::uint32_t>& words,
    spv::Op op,
    std::initializer_list<std::uint32_t> operands = {}) {
    words.push_back(
        (static_cast<std::uint32_t>(operands.size() + 1) << 16) |
        static_cast<std::uint32_t>(op));
    words.insert(words.end(), operands.begin(), operands.end());
}

inline void emit_entry_point(
    std::vector<std::uint32_t>& words,
    spv::ExecutionModel model,
    std::uint32_t function,
    std::initializer_list<std::uint32_t> interfaces) {
    words.push_back(
        (static_cast<std::uint32_t>(5 + interfaces.size()) << 16) |
        static_cast<std::uint32_t>(spv::OpEntryPoint));
    words.push_back(static_cast<std::uint32_t>(model));
    words.push_back(function);
    words.push_back(0x6E69616Du); // "main"
    words.push_back(0);
    words.insert(words.end(), interfaces.begin(), interfaces.end());
}

inline std::vector<std::uint32_t> finish(
    std::vector<std::uint32_t> body,
    std::uint32_t bound) {
    std::vector<std::uint32_t> module = {
        spv::MagicNumber,
        0x00010000u,
        0,
        bound,
        0,
    };
    module.insert(module.end(), body.begin(), body.end());
    return module;
}

inline std::vector<std::uint32_t> solid_color_fragment(
    std::uint32_t red_bits,
    std::uint32_t green_bits,
    std::uint32_t blue_bits,
    std::uint32_t alpha_bits) {
    constexpr std::uint32_t void_type = 1;
    constexpr std::uint32_t float_type = 2;
    constexpr std::uint32_t vec4_type = 3;
    constexpr std::uint32_t output_pointer = 4;
    constexpr std::uint32_t function_type = 5;
    constexpr std::uint32_t red = 6;
    constexpr std::uint32_t green = 7;
    constexpr std::uint32_t blue = 8;
    constexpr std::uint32_t alpha = 9;
    constexpr std::uint32_t color = 10;
    constexpr std::uint32_t output = 11;
    constexpr std::uint32_t main_function = 12;
    constexpr std::uint32_t label = 13;

    std::vector<std::uint32_t> words;
    emit(words, spv::OpCapability, {spv::CapabilityShader});
    emit(
        words,
        spv::OpMemoryModel,
        {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    emit_entry_point(
        words,
        spv::ExecutionModelFragment,
        main_function,
        {output});
    emit(
        words,
        spv::OpExecutionMode,
        {main_function, spv::ExecutionModeOriginUpperLeft});
    emit(
        words,
        spv::OpDecorate,
        {output, spv::DecorationLocation, 0});
    emit(words, spv::OpTypeVoid, {void_type});
    emit(words, spv::OpTypeFloat, {float_type, 32});
    emit(words, spv::OpTypeVector, {vec4_type, float_type, 4});
    emit(
        words,
        spv::OpTypePointer,
        {output_pointer, spv::StorageClassOutput, vec4_type});
    emit(words, spv::OpTypeFunction, {function_type, void_type});
    emit(words, spv::OpConstant, {float_type, red, red_bits});
    emit(words, spv::OpConstant, {float_type, green, green_bits});
    emit(words, spv::OpConstant, {float_type, blue, blue_bits});
    emit(words, spv::OpConstant, {float_type, alpha, alpha_bits});
    emit(
        words,
        spv::OpConstantComposite,
        {vec4_type, color, red, green, blue, alpha});
    emit(
        words,
        spv::OpVariable,
        {output_pointer, output, spv::StorageClassOutput});
    emit(
        words,
        spv::OpFunction,
        {void_type, main_function, spv::FunctionControlMaskNone,
         function_type});
    emit(words, spv::OpLabel, {label});
    emit(words, spv::OpStore, {output, color});
    emit(words, spv::OpReturn);
    emit(words, spv::OpFunctionEnd);
    return finish(std::move(words), 14);
}

inline std::vector<std::uint32_t> solid_green_fragment() {
    return solid_color_fragment(
        0x00000000u,
        0x3F800000u,
        0x00000000u,
        0x3F800000u);
}

inline std::vector<std::uint32_t> solid_white_fragment() {
    return solid_color_fragment(
        0x3F800000u,
        0x3F800000u,
        0x3F800000u,
        0x3F800000u);
}

inline std::vector<std::uint32_t> solid_green_compute_storage(
    std::uint32_t descriptor_binding) {
    constexpr std::uint32_t void_type = 1;
    constexpr std::uint32_t float_type = 2;
    constexpr std::uint32_t int_type = 3;
    constexpr std::uint32_t ivec2_type = 4;
    constexpr std::uint32_t vec4_type = 5;
    constexpr std::uint32_t image_type = 6;
    constexpr std::uint32_t image_pointer = 7;
    constexpr std::uint32_t function_type = 8;
    constexpr std::uint32_t int_zero = 9;
    constexpr std::uint32_t coordinate = 10;
    constexpr std::uint32_t float_zero = 11;
    constexpr std::uint32_t float_one = 12;
    constexpr std::uint32_t color = 13;
    constexpr std::uint32_t image = 14;
    constexpr std::uint32_t main_function = 15;
    constexpr std::uint32_t label = 16;
    constexpr std::uint32_t image_value = 17;

    std::vector<std::uint32_t> words;
    emit(words, spv::OpCapability, {spv::CapabilityShader});
    emit(
        words,
        spv::OpCapability,
        {spv::CapabilityStorageImageExtendedFormats});
    emit(
        words,
        spv::OpMemoryModel,
        {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    emit_entry_point(
        words,
        spv::ExecutionModelGLCompute,
        main_function,
        {image});
    emit(
        words,
        spv::OpExecutionMode,
        {main_function, spv::ExecutionModeLocalSize, 8, 8, 1});
    emit(
        words,
        spv::OpDecorate,
        {image, spv::DecorationDescriptorSet, 0});
    emit(
        words,
        spv::OpDecorate,
        {image, spv::DecorationBinding, descriptor_binding});
    emit(words, spv::OpTypeVoid, {void_type});
    emit(words, spv::OpTypeFloat, {float_type, 32});
    emit(words, spv::OpTypeInt, {int_type, 32, 1});
    emit(words, spv::OpTypeVector, {ivec2_type, int_type, 2});
    emit(words, spv::OpTypeVector, {vec4_type, float_type, 4});
    emit(
        words,
        spv::OpTypeImage,
        {image_type, float_type, spv::Dim2D, 0, 0, 0, 2,
         spv::ImageFormatRgba16f});
    emit(
        words,
        spv::OpTypePointer,
        {image_pointer, spv::StorageClassUniformConstant, image_type});
    emit(words, spv::OpTypeFunction, {function_type, void_type});
    emit(words, spv::OpConstant, {int_type, int_zero, 0});
    emit(
        words,
        spv::OpConstantComposite,
        {ivec2_type, coordinate, int_zero, int_zero});
    emit(words, spv::OpConstant, {float_type, float_zero, 0x00000000u});
    emit(words, spv::OpConstant, {float_type, float_one, 0x3F800000u});
    emit(
        words,
        spv::OpConstantComposite,
        {vec4_type, color, float_zero, float_one, float_zero, float_one});
    emit(
        words,
        spv::OpVariable,
        {image_pointer, image, spv::StorageClassUniformConstant});
    emit(
        words,
        spv::OpFunction,
        {void_type, main_function, spv::FunctionControlMaskNone,
         function_type});
    emit(words, spv::OpLabel, {label});
    emit(words, spv::OpLoad, {image_type, image_value, image});
    emit(words, spv::OpImageWrite, {image_value, coordinate, color});
    emit(words, spv::OpReturn);
    emit(words, spv::OpFunctionEnd);
    return finish(std::move(words), 18);
}

inline std::vector<std::uint32_t> fullscreen_vertex() {
    constexpr std::uint32_t void_type = 1;
    constexpr std::uint32_t uint_type = 2;
    constexpr std::uint32_t float_type = 3;
    constexpr std::uint32_t vec4_type = 4;
    constexpr std::uint32_t input_uint_pointer = 5;
    constexpr std::uint32_t output_vec4_pointer = 6;
    constexpr std::uint32_t function_type = 7;
    constexpr std::uint32_t vertex_index = 8;
    constexpr std::uint32_t position = 9;
    constexpr std::uint32_t uint_one = 10;
    constexpr std::uint32_t uint_two = 11;
    constexpr std::uint32_t float_zero = 12;
    constexpr std::uint32_t float_one = 13;
    constexpr std::uint32_t float_two = 14;
    constexpr std::uint32_t main_function = 15;
    constexpr std::uint32_t label = 16;
    constexpr std::uint32_t index_value = 17;
    constexpr std::uint32_t shifted = 18;
    constexpr std::uint32_t x_bits = 19;
    constexpr std::uint32_t y_bits = 20;
    constexpr std::uint32_t x = 21;
    constexpr std::uint32_t y = 22;
    constexpr std::uint32_t x_scaled = 23;
    constexpr std::uint32_t x_position = 24;
    constexpr std::uint32_t y_scaled = 25;
    constexpr std::uint32_t y_position = 26;
    constexpr std::uint32_t position_value = 27;

    std::vector<std::uint32_t> words;
    emit(words, spv::OpCapability, {spv::CapabilityShader});
    emit(
        words,
        spv::OpMemoryModel,
        {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    emit_entry_point(
        words,
        spv::ExecutionModelVertex,
        main_function,
        {vertex_index, position});
    emit(
        words,
        spv::OpDecorate,
        {vertex_index, spv::DecorationBuiltIn, spv::BuiltInVertexIndex});
    emit(
        words,
        spv::OpDecorate,
        {position, spv::DecorationBuiltIn, spv::BuiltInPosition});
    emit(words, spv::OpTypeVoid, {void_type});
    emit(words, spv::OpTypeInt, {uint_type, 32, 0});
    emit(words, spv::OpTypeFloat, {float_type, 32});
    emit(words, spv::OpTypeVector, {vec4_type, float_type, 4});
    emit(
        words,
        spv::OpTypePointer,
        {input_uint_pointer, spv::StorageClassInput, uint_type});
    emit(
        words,
        spv::OpTypePointer,
        {output_vec4_pointer, spv::StorageClassOutput, vec4_type});
    emit(words, spv::OpTypeFunction, {function_type, void_type});
    emit(words, spv::OpConstant, {uint_type, uint_one, 1});
    emit(words, spv::OpConstant, {uint_type, uint_two, 2});
    emit(words, spv::OpConstant, {float_type, float_zero, 0x00000000u});
    emit(words, spv::OpConstant, {float_type, float_one, 0x3F800000u});
    emit(words, spv::OpConstant, {float_type, float_two, 0x40000000u});
    emit(
        words,
        spv::OpVariable,
        {input_uint_pointer, vertex_index, spv::StorageClassInput});
    emit(
        words,
        spv::OpVariable,
        {output_vec4_pointer, position, spv::StorageClassOutput});
    emit(
        words,
        spv::OpFunction,
        {void_type, main_function, spv::FunctionControlMaskNone,
         function_type});
    emit(words, spv::OpLabel, {label});
    emit(words, spv::OpLoad, {uint_type, index_value, vertex_index});
    emit(
        words,
        spv::OpShiftLeftLogical,
        {uint_type, shifted, index_value, uint_one});
    emit(
        words,
        spv::OpBitwiseAnd,
        {uint_type, x_bits, shifted, uint_two});
    emit(
        words,
        spv::OpBitwiseAnd,
        {uint_type, y_bits, index_value, uint_two});
    emit(words, spv::OpConvertUToF, {float_type, x, x_bits});
    emit(words, spv::OpConvertUToF, {float_type, y, y_bits});
    emit(words, spv::OpFMul, {float_type, x_scaled, x, float_two});
    emit(
        words,
        spv::OpFSub,
        {float_type, x_position, x_scaled, float_one});
    emit(words, spv::OpFMul, {float_type, y_scaled, y, float_two});
    emit(
        words,
        spv::OpFSub,
        {float_type, y_position, y_scaled, float_one});
    emit(
        words,
        spv::OpCompositeConstruct,
        {vec4_type, position_value, x_position, y_position, float_zero,
         float_one});
    emit(words, spv::OpStore, {position, position_value});
    emit(words, spv::OpReturn);
    emit(words, spv::OpFunctionEnd);
    return finish(std::move(words), 28);
}

inline std::vector<std::uint32_t> fullscreen_vertex_uv() {
    constexpr std::uint32_t void_type = 1;
    constexpr std::uint32_t uint_type = 2;
    constexpr std::uint32_t float_type = 3;
    constexpr std::uint32_t vec2_type = 4;
    constexpr std::uint32_t vec4_type = 5;
    constexpr std::uint32_t input_uint_pointer = 6;
    constexpr std::uint32_t output_vec2_pointer = 7;
    constexpr std::uint32_t output_vec4_pointer = 8;
    constexpr std::uint32_t function_type = 9;
    constexpr std::uint32_t vertex_index = 10;
    constexpr std::uint32_t position = 11;
    constexpr std::uint32_t uv = 12;
    constexpr std::uint32_t uint_one = 13;
    constexpr std::uint32_t uint_two = 14;
    constexpr std::uint32_t float_zero = 15;
    constexpr std::uint32_t float_one = 16;
    constexpr std::uint32_t float_two = 17;
    constexpr std::uint32_t main_function = 18;
    constexpr std::uint32_t label = 19;
    constexpr std::uint32_t index_value = 20;
    constexpr std::uint32_t shifted = 21;
    constexpr std::uint32_t x_bits = 22;
    constexpr std::uint32_t y_bits = 23;
    constexpr std::uint32_t x = 24;
    constexpr std::uint32_t y = 25;
    constexpr std::uint32_t x_scaled = 26;
    constexpr std::uint32_t x_position = 27;
    constexpr std::uint32_t y_scaled = 28;
    constexpr std::uint32_t y_position = 29;
    constexpr std::uint32_t position_value = 30;
    constexpr std::uint32_t uv_value = 31;

    std::vector<std::uint32_t> words;
    emit(words, spv::OpCapability, {spv::CapabilityShader});
    emit(
        words,
        spv::OpMemoryModel,
        {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    emit_entry_point(
        words,
        spv::ExecutionModelVertex,
        main_function,
        {vertex_index, position, uv});
    emit(
        words,
        spv::OpDecorate,
        {vertex_index, spv::DecorationBuiltIn, spv::BuiltInVertexIndex});
    emit(
        words,
        spv::OpDecorate,
        {position, spv::DecorationBuiltIn, spv::BuiltInPosition});
    emit(words, spv::OpDecorate, {uv, spv::DecorationLocation, 0});
    emit(words, spv::OpTypeVoid, {void_type});
    emit(words, spv::OpTypeInt, {uint_type, 32, 0});
    emit(words, spv::OpTypeFloat, {float_type, 32});
    emit(words, spv::OpTypeVector, {vec2_type, float_type, 2});
    emit(words, spv::OpTypeVector, {vec4_type, float_type, 4});
    emit(
        words,
        spv::OpTypePointer,
        {input_uint_pointer, spv::StorageClassInput, uint_type});
    emit(
        words,
        spv::OpTypePointer,
        {output_vec2_pointer, spv::StorageClassOutput, vec2_type});
    emit(
        words,
        spv::OpTypePointer,
        {output_vec4_pointer, spv::StorageClassOutput, vec4_type});
    emit(words, spv::OpTypeFunction, {function_type, void_type});
    emit(words, spv::OpConstant, {uint_type, uint_one, 1});
    emit(words, spv::OpConstant, {uint_type, uint_two, 2});
    emit(words, spv::OpConstant, {float_type, float_zero, 0x00000000u});
    emit(words, spv::OpConstant, {float_type, float_one, 0x3F800000u});
    emit(words, spv::OpConstant, {float_type, float_two, 0x40000000u});
    emit(
        words,
        spv::OpVariable,
        {input_uint_pointer, vertex_index, spv::StorageClassInput});
    emit(
        words,
        spv::OpVariable,
        {output_vec4_pointer, position, spv::StorageClassOutput});
    emit(
        words,
        spv::OpVariable,
        {output_vec2_pointer, uv, spv::StorageClassOutput});
    emit(
        words,
        spv::OpFunction,
        {void_type, main_function, spv::FunctionControlMaskNone,
         function_type});
    emit(words, spv::OpLabel, {label});
    emit(words, spv::OpLoad, {uint_type, index_value, vertex_index});
    emit(
        words,
        spv::OpShiftLeftLogical,
        {uint_type, shifted, index_value, uint_one});
    emit(
        words,
        spv::OpBitwiseAnd,
        {uint_type, x_bits, shifted, uint_two});
    emit(
        words,
        spv::OpBitwiseAnd,
        {uint_type, y_bits, index_value, uint_two});
    emit(words, spv::OpConvertUToF, {float_type, x, x_bits});
    emit(words, spv::OpConvertUToF, {float_type, y, y_bits});
    emit(words, spv::OpFMul, {float_type, x_scaled, x, float_two});
    emit(
        words,
        spv::OpFSub,
        {float_type, x_position, x_scaled, float_one});
    emit(words, spv::OpFMul, {float_type, y_scaled, y, float_two});
    emit(
        words,
        spv::OpFSub,
        {float_type, y_position, y_scaled, float_one});
    emit(
        words,
        spv::OpCompositeConstruct,
        {vec4_type, position_value, x_position, y_position, float_zero,
         float_one});
    emit(
        words,
        spv::OpCompositeConstruct,
        {vec2_type, uv_value, x, y});
    emit(words, spv::OpStore, {position, position_value});
    emit(words, spv::OpStore, {uv, uv_value});
    emit(words, spv::OpReturn);
    emit(words, spv::OpFunctionEnd);
    return finish(std::move(words), 32);
}

inline std::vector<std::uint32_t> fullscreen_vertex_vec4_attribute() {
    constexpr std::uint32_t void_type = 1;
    constexpr std::uint32_t uint_type = 2;
    constexpr std::uint32_t float_type = 3;
    constexpr std::uint32_t vec4_type = 4;
    constexpr std::uint32_t input_uint_pointer = 5;
    constexpr std::uint32_t output_vec4_pointer = 6;
    constexpr std::uint32_t function_type = 7;
    constexpr std::uint32_t vertex_index = 8;
    constexpr std::uint32_t position = 9;
    constexpr std::uint32_t attribute = 10;
    constexpr std::uint32_t uint_one = 11;
    constexpr std::uint32_t uint_two = 12;
    constexpr std::uint32_t float_zero = 13;
    constexpr std::uint32_t float_one = 14;
    constexpr std::uint32_t float_two = 15;
    constexpr std::uint32_t main_function = 16;
    constexpr std::uint32_t label = 17;
    constexpr std::uint32_t index_value = 18;
    constexpr std::uint32_t shifted = 19;
    constexpr std::uint32_t x_bits = 20;
    constexpr std::uint32_t y_bits = 21;
    constexpr std::uint32_t x = 22;
    constexpr std::uint32_t y = 23;
    constexpr std::uint32_t x_scaled = 24;
    constexpr std::uint32_t x_position = 25;
    constexpr std::uint32_t y_scaled = 26;
    constexpr std::uint32_t y_position = 27;
    constexpr std::uint32_t position_value = 28;
    constexpr std::uint32_t attribute_value = 29;

    std::vector<std::uint32_t> words;
    emit(words, spv::OpCapability, {spv::CapabilityShader});
    emit(
        words,
        spv::OpMemoryModel,
        {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    emit_entry_point(
        words,
        spv::ExecutionModelVertex,
        main_function,
        {vertex_index, position, attribute});
    emit(
        words,
        spv::OpDecorate,
        {vertex_index, spv::DecorationBuiltIn, spv::BuiltInVertexIndex});
    emit(
        words,
        spv::OpDecorate,
        {position, spv::DecorationBuiltIn, spv::BuiltInPosition});
    emit(
        words,
        spv::OpDecorate,
        {attribute, spv::DecorationLocation, 0});
    emit(
        words,
        spv::OpDecorate,
        {attribute, spv::DecorationNoPerspective});
    emit(words, spv::OpTypeVoid, {void_type});
    emit(words, spv::OpTypeInt, {uint_type, 32, 0});
    emit(words, spv::OpTypeFloat, {float_type, 32});
    emit(words, spv::OpTypeVector, {vec4_type, float_type, 4});
    emit(
        words,
        spv::OpTypePointer,
        {input_uint_pointer, spv::StorageClassInput, uint_type});
    emit(
        words,
        spv::OpTypePointer,
        {output_vec4_pointer, spv::StorageClassOutput, vec4_type});
    emit(words, spv::OpTypeFunction, {function_type, void_type});
    emit(words, spv::OpConstant, {uint_type, uint_one, 1});
    emit(words, spv::OpConstant, {uint_type, uint_two, 2});
    emit(words, spv::OpConstant, {float_type, float_zero, 0x00000000u});
    emit(words, spv::OpConstant, {float_type, float_one, 0x3F800000u});
    emit(words, spv::OpConstant, {float_type, float_two, 0x40000000u});
    emit(
        words,
        spv::OpVariable,
        {input_uint_pointer, vertex_index, spv::StorageClassInput});
    emit(
        words,
        spv::OpVariable,
        {output_vec4_pointer, position, spv::StorageClassOutput});
    emit(
        words,
        spv::OpVariable,
        {output_vec4_pointer, attribute, spv::StorageClassOutput});
    emit(
        words,
        spv::OpFunction,
        {void_type, main_function, spv::FunctionControlMaskNone,
         function_type});
    emit(words, spv::OpLabel, {label});
    emit(words, spv::OpLoad, {uint_type, index_value, vertex_index});
    emit(
        words,
        spv::OpShiftLeftLogical,
        {uint_type, shifted, index_value, uint_one});
    emit(
        words,
        spv::OpBitwiseAnd,
        {uint_type, x_bits, shifted, uint_two});
    emit(
        words,
        spv::OpBitwiseAnd,
        {uint_type, y_bits, index_value, uint_two});
    emit(words, spv::OpConvertUToF, {float_type, x, x_bits});
    emit(words, spv::OpConvertUToF, {float_type, y, y_bits});
    emit(words, spv::OpFMul, {float_type, x_scaled, x, float_two});
    emit(
        words,
        spv::OpFSub,
        {float_type, x_position, x_scaled, float_one});
    emit(words, spv::OpFMul, {float_type, y_scaled, y, float_two});
    emit(
        words,
        spv::OpFSub,
        {float_type, y_position, y_scaled, float_one});
    emit(
        words,
        spv::OpCompositeConstruct,
        {vec4_type, position_value, x_position, y_position, float_zero,
         float_one});
    emit(
        words,
        spv::OpCompositeConstruct,
        {vec4_type, attribute_value, x, y, float_zero, float_one});
    emit(words, spv::OpStore, {position, position_value});
    emit(words, spv::OpStore, {attribute, attribute_value});
    emit(words, spv::OpReturn);
    emit(words, spv::OpFunctionEnd);
    return finish(std::move(words), 30);
}

inline std::vector<std::uint32_t> sampled_texture_fragment(
    std::uint32_t descriptor_binding = 0) {
    constexpr std::uint32_t void_type = 1;
    constexpr std::uint32_t float_type = 2;
    constexpr std::uint32_t vec2_type = 3;
    constexpr std::uint32_t vec4_type = 4;
    constexpr std::uint32_t image_type = 5;
    constexpr std::uint32_t sampled_image_type = 6;
    constexpr std::uint32_t input_pointer = 7;
    constexpr std::uint32_t output_pointer = 8;
    constexpr std::uint32_t texture_pointer = 9;
    constexpr std::uint32_t function_type = 10;
    constexpr std::uint32_t input_uv = 11;
    constexpr std::uint32_t output_color = 12;
    constexpr std::uint32_t texture = 13;
    constexpr std::uint32_t main_function = 14;
    constexpr std::uint32_t label = 15;
    constexpr std::uint32_t uv_value = 16;
    constexpr std::uint32_t texture_value = 17;
    constexpr std::uint32_t sampled_color = 18;

    std::vector<std::uint32_t> words;
    emit(words, spv::OpCapability, {spv::CapabilityShader});
    emit(
        words,
        spv::OpMemoryModel,
        {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    emit_entry_point(
        words,
        spv::ExecutionModelFragment,
        main_function,
        {input_uv, output_color});
    emit(
        words,
        spv::OpExecutionMode,
        {main_function, spv::ExecutionModeOriginUpperLeft});
    emit(
        words,
        spv::OpDecorate,
        {input_uv, spv::DecorationLocation, 0});
    emit(
        words,
        spv::OpDecorate,
        {output_color, spv::DecorationLocation, 0});
    emit(
        words,
        spv::OpDecorate,
        {texture, spv::DecorationDescriptorSet, 0});
    emit(
        words,
        spv::OpDecorate,
        {texture, spv::DecorationBinding, descriptor_binding});
    emit(words, spv::OpTypeVoid, {void_type});
    emit(words, spv::OpTypeFloat, {float_type, 32});
    emit(words, spv::OpTypeVector, {vec2_type, float_type, 2});
    emit(words, spv::OpTypeVector, {vec4_type, float_type, 4});
    emit(
        words,
        spv::OpTypeImage,
        {image_type, float_type, spv::Dim2D, 0, 0, 0, 1,
         spv::ImageFormatUnknown});
    emit(
        words,
        spv::OpTypeSampledImage,
        {sampled_image_type, image_type});
    emit(
        words,
        spv::OpTypePointer,
        {input_pointer, spv::StorageClassInput, vec2_type});
    emit(
        words,
        spv::OpTypePointer,
        {output_pointer, spv::StorageClassOutput, vec4_type});
    emit(
        words,
        spv::OpTypePointer,
        {texture_pointer, spv::StorageClassUniformConstant,
         sampled_image_type});
    emit(words, spv::OpTypeFunction, {function_type, void_type});
    emit(
        words,
        spv::OpVariable,
        {input_pointer, input_uv, spv::StorageClassInput});
    emit(
        words,
        spv::OpVariable,
        {output_pointer, output_color, spv::StorageClassOutput});
    emit(
        words,
        spv::OpVariable,
        {texture_pointer, texture, spv::StorageClassUniformConstant});
    emit(
        words,
        spv::OpFunction,
        {void_type, main_function, spv::FunctionControlMaskNone,
         function_type});
    emit(words, spv::OpLabel, {label});
    emit(words, spv::OpLoad, {vec2_type, uv_value, input_uv});
    emit(
        words,
        spv::OpLoad,
        {sampled_image_type, texture_value, texture});
    emit(
        words,
        spv::OpImageSampleImplicitLod,
        {vec4_type, sampled_color, texture_value, uv_value});
    emit(words, spv::OpStore, {output_color, sampled_color});
    emit(words, spv::OpReturn);
    emit(words, spv::OpFunctionEnd);
    return finish(std::move(words), 19);
}

} // namespace ps5gpu::fixed_spirv
