// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unpacking a typed buffer read.
//
// Where every component of an element is a whole word, a typed read is an
// ordinary read: the bits in memory are the bits the shader wants. Where
// they are narrower - four bytes in a word, two halves, ten bits and ten
// and ten and two - the bits have to be taken apart and turned into the
// numbers the format says they are, and reading them as they lie puts the
// bit pattern of four bytes into one float.
//
// What a component becomes depends on both halves of the format. The
// layout says where its bits are; the number format says whether they are
// a fraction of the largest value the field holds, a signed fraction, a
// plain integer, or a half float.

#ifndef PS5_GEN5_EMIT_TYPED_H
#define PS5_GEN5_EMIT_TYPED_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gen5_buffer_format.h"
#include "gen5_emit_alu.h"
#include "spirv_builder.h"

namespace ps5gen5 {

// Where one component sits: which of the loaded words, how far up it, and
// how wide it is.
struct TypedComponent {
    std::uint32_t word = 0;
    std::uint32_t offset = 0;
    std::uint32_t width = 0;
};

// The layout of an element, or an empty list when the format is one this
// does not take apart.
inline std::vector<TypedComponent> typed_components_of(
    const BufferFormat& format) {
    const auto data = static_cast<BufferDataFormat>(format.data);
    std::vector<TypedComponent> parts;
    const auto even = [&](std::uint32_t count, std::uint32_t width) {
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto bit = index * width;
            parts.push_back(
                TypedComponent{bit / 32u, bit % 32u, width});
        }
    };
    switch (data) {
        case BufferDataFormat::Format8: even(1, 8); break;
        case BufferDataFormat::Format8_8: even(2, 8); break;
        case BufferDataFormat::Format8_8_8_8: even(4, 8); break;
        case BufferDataFormat::Format16: even(1, 16); break;
        case BufferDataFormat::Format16_16: even(2, 16); break;
        case BufferDataFormat::Format16_16_16_16: even(4, 16); break;
        case BufferDataFormat::Format2_10_10_10:
            // The two bit field is the last component, at the top.
            parts.push_back(TypedComponent{0, 0, 10});
            parts.push_back(TypedComponent{0, 10, 10});
            parts.push_back(TypedComponent{0, 20, 10});
            parts.push_back(TypedComponent{0, 30, 2});
            break;
        case BufferDataFormat::Format10_10_10_2:
            // And here it is the first, at the bottom.
            parts.push_back(TypedComponent{0, 0, 2});
            parts.push_back(TypedComponent{0, 2, 10});
            parts.push_back(TypedComponent{0, 12, 10});
            parts.push_back(TypedComponent{0, 22, 10});
            break;
        default:
            break;
    }
    return parts;
}

// Whether a format is one this can unpack at all.
inline bool typed_format_is_unpackable(const BufferFormat& format) {
    if (typed_components_of(format).empty()) {
        return false;
    }
    switch (static_cast<BufferNumberFormat>(format.number)) {
        case BufferNumberFormat::Unorm:
        case BufferNumberFormat::Snorm:
        case BufferNumberFormat::SnormOgl:
        case BufferNumberFormat::Uscaled:
        case BufferNumberFormat::Sscaled:
        case BufferNumberFormat::Uint:
        case BufferNumberFormat::Sint:
            return true;
        case BufferNumberFormat::Float:
            // Only the half float is packed; a whole word float is not
            // this path's business.
            return static_cast<BufferDataFormat>(format.data) ==
                    BufferDataFormat::Format16 ||
                static_cast<BufferDataFormat>(format.data) ==
                    BufferDataFormat::Format16_16 ||
                static_cast<BufferDataFormat>(format.data) ==
                    BufferDataFormat::Format16_16_16_16;
    }
    return false;
}

// How many words an element occupies.
inline std::uint32_t typed_element_words(const BufferFormat& format) {
    std::uint32_t highest = 0;
    for (const auto& part : typed_components_of(format)) {
        highest = std::max(highest, part.word + 1);
    }
    return highest;
}

// Emits the components of one element, as register bits.
//
// `words` are the loaded dwords. The result is one value per component, in
// the order the format lists them, already in the form a register holds -
// a float's bits rather than a float.
class TypedReader {
public:
    TypedReader(
        ps5spirv::ModuleBuilder& module,
        std::uint32_t uint_type,
        std::uint32_t float_type,
        std::uint32_t glsl)
        : module_(module),
          uint_type_(uint_type),
          float_type_(float_type),
          glsl_(glsl) {}

    std::vector<std::uint32_t> emit(
        const BufferFormat& format,
        const std::vector<std::uint32_t>& words) {
        std::vector<std::uint32_t> values;
        const auto number =
            static_cast<BufferNumberFormat>(format.number);
        for (const auto& part : typed_components_of(format)) {
            if (part.word >= words.size()) {
                break;
            }
            const auto raw = extract(words[part.word], part);
            values.push_back(convert(raw, part, number));
        }
        return values;
    }

private:
    std::uint32_t constant(std::uint32_t value) {
        return module_.constant(uint_type_, value);
    }

    std::uint32_t binary(
        AluSpirvOp op, std::uint32_t left, std::uint32_t right,
        std::uint32_t type) {
        const auto id = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(op), {type, id, left, right});
        return id;
    }

    // The field's bits, moved down and masked.
    std::uint32_t extract(
        std::uint32_t word, const TypedComponent& part) {
        auto value = word;
        if (part.offset != 0) {
            value = binary(
                AluSpirvOp::ShiftRightLogical, value,
                constant(part.offset), uint_type_);
        }
        if (part.width < 32) {
            value = binary(
                AluSpirvOp::BitwiseAnd, value,
                constant((1u << part.width) - 1u), uint_type_);
        }
        return value;
    }

    // Sign extends a field that is narrower than a word.
    std::uint32_t sign_extend(
        std::uint32_t value, std::uint32_t width) {
        if (width >= 32) {
            return value;
        }
        const auto shift = constant(32u - width);
        const auto up = binary(
            AluSpirvOp::ShiftLeftLogical, value, shift, uint_type_);
        return binary(
            AluSpirvOp::ShiftRightArithmetic, up, shift, uint_type_);
    }

    std::uint32_t as_float(std::uint32_t bits) {
        const auto id = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(124), {float_type_, id, bits});
        return id;
    }

    std::uint32_t as_bits(std::uint32_t value) {
        const auto id = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(124), {uint_type_, id, value});
        return id;
    }

    std::uint32_t float_constant(float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return module_.constant(float_type_, bits);
    }

    std::uint32_t convert_to_float(std::uint32_t value, bool is_signed) {
        const auto id = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(is_signed ? 111 : 112),
            {float_type_, id, value});
        return id;
    }

    std::uint32_t convert(
        std::uint32_t raw,
        const TypedComponent& part,
        BufferNumberFormat number) {
        const auto largest = part.width >= 32
            ? 4294967295.0f
            : static_cast<float>((1ull << part.width) - 1ull);
        switch (number) {
            case BufferNumberFormat::Uint:
                return raw;
            case BufferNumberFormat::Sint:
                return sign_extend(raw, part.width);
            case BufferNumberFormat::Uscaled:
                return as_bits(convert_to_float(raw, false));
            case BufferNumberFormat::Sscaled:
                return as_bits(convert_to_float(
                    sign_extend(raw, part.width), true));
            case BufferNumberFormat::Unorm: {
                const auto value = convert_to_float(raw, false);
                const auto scaled = module_.allocate_id();
                module_.add_function_word(
                    static_cast<ps5spirv::Op>(133),
                    {float_type_, scaled, value,
                     float_constant(1.0f / largest)});
                return as_bits(scaled);
            }
            case BufferNumberFormat::Snorm:
            case BufferNumberFormat::SnormOgl: {
                // The signed range is one short at the bottom, and the
                // result is clamped so that the most negative value reads
                // as minus one rather than slightly past it.
                const auto extended = sign_extend(raw, part.width);
                const auto value = convert_to_float(extended, true);
                const auto half = part.width >= 32
                    ? 2147483647.0f
                    : static_cast<float>((1ull << (part.width - 1)) - 1ull);
                const auto scaled = module_.allocate_id();
                module_.add_function_word(
                    static_cast<ps5spirv::Op>(133),
                    {float_type_, scaled, value,
                     float_constant(1.0f / half)});
                const auto clamped = module_.allocate_id();
                module_.add_function_word(
                    ps5spirv::Op::ExtInst,
                    std::vector<std::uint32_t>{
                        float_type_, clamped, glsl_, 40, scaled,
                        float_constant(-1.0f)});
                return as_bits(clamped);
            }
            case BufferNumberFormat::Float: {
                // A half float, which the packed pair instruction reads
                // two at a time - so one is taken from the low half of a
                // word built for the purpose.
                const auto vector2 = module_.type_vector(float_type_, 2);
                const auto pair = module_.allocate_id();
                module_.add_function_word(
                    ps5spirv::Op::ExtInst,
                    std::vector<std::uint32_t>{
                        vector2, pair, glsl_, 62, raw});
                const auto taken = module_.allocate_id();
                module_.add_function_word(
                    ps5spirv::Op::CompositeExtract,
                    {float_type_, taken, pair, 0});
                return as_bits(taken);
            }
        }
        return raw;
    }

    ps5spirv::ModuleBuilder& module_;
    std::uint32_t uint_type_ = 0;
    std::uint32_t float_type_ = 0;
    std::uint32_t glsl_ = 0;
};

}  // namespace ps5gen5

#endif  // PS5_GEN5_EMIT_TYPED_H
