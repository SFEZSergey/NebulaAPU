// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// What a buffer descriptor says its contents look like.
//
// GFX10 packs the two older fields - how the components are laid out and
// how their bits are read as numbers - into one seven bit number, and the
// table that unpacks it is sparse rather than arithmetic. Deriving it from
// ranges makes encodings the hardware reserves look valid, so it is written
// out.
//
// The numbers are field values, taken from the table the C# translator
// already carried rather than restated from a specification, so the two
// cannot disagree about a format this title actually uses.

#ifndef PS5_GEN5_BUFFER_FORMAT_H
#define PS5_GEN5_BUFFER_FORMAT_H

#include <array>
#include <cstdint>

namespace ps5gen5 {

// How the components of one element are laid out.
enum class BufferDataFormat : std::uint32_t {
    Invalid = 0,
    Format8 = 1,
    Format16 = 2,
    Format8_8 = 3,
    Format32 = 4,
    Format16_16 = 5,
    Format10_11_11 = 6,
    Format11_11_10 = 7,
    Format10_10_10_2 = 8,
    Format2_10_10_10 = 9,
    Format8_8_8_8 = 10,
    Format32_32 = 11,
    Format16_16_16_16 = 12,
    Format32_32_32 = 13,
    Format32_32_32_32 = 14,
};

// How the bits of a component become a number.
enum class BufferNumberFormat : std::uint32_t {
    Unorm = 0,
    Snorm = 1,
    Uscaled = 2,
    Sscaled = 3,
    Uint = 4,
    Sint = 5,
    SnormOgl = 6,
    Float = 7,
};

struct BufferFormat {
    std::uint32_t data = 0;
    std::uint32_t number = 0;
};

inline BufferFormat buffer_format_of(std::uint32_t unified) {
    static constexpr std::array<BufferFormat, 128> kTable{{
    {0u, 0u},
    {1u, 0u},
    {1u, 1u},
    {1u, 2u},
    {1u, 3u},
    {1u, 4u},
    {1u, 5u},
    {2u, 0u},
    {2u, 1u},
    {2u, 2u},
    {2u, 3u},
    {2u, 4u},
    {2u, 5u},
    {2u, 7u},
    {3u, 0u},
    {3u, 1u},
    {3u, 2u},
    {3u, 3u},
    {3u, 4u},
    {3u, 5u},
    {4u, 4u},
    {4u, 5u},
    {4u, 7u},
    {5u, 0u},
    {5u, 1u},
    {5u, 2u},
    {5u, 3u},
    {5u, 4u},
    {5u, 5u},
    {5u, 7u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {6u, 7u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {7u, 7u},
    {8u, 0u},
    {8u, 1u},
    {0u, 0u},
    {0u, 0u},
    {8u, 4u},
    {8u, 5u},
    {9u, 0u},
    {9u, 1u},
    {9u, 2u},
    {9u, 3u},
    {9u, 4u},
    {9u, 5u},
    {10u, 0u},
    {10u, 1u},
    {10u, 2u},
    {10u, 3u},
    {10u, 4u},
    {10u, 5u},
    {11u, 4u},
    {11u, 5u},
    {11u, 7u},
    {12u, 0u},
    {12u, 1u},
    {12u, 2u},
    {12u, 3u},
    {12u, 4u},
    {12u, 5u},
    {12u, 7u},
    {13u, 4u},
    {13u, 5u},
    {13u, 7u},
    {14u, 4u},
    {14u, 5u},
    {14u, 7u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    {0u, 0u},
    }};
    return unified < kTable.size() ? kTable[unified] : BufferFormat{};
}

// The format a descriptor names, which lives in the fourth word.
inline BufferFormat buffer_format_of_descriptor(
    const std::array<std::uint32_t, 4>& descriptor) {
    return buffer_format_of((descriptor[3] >> 12) & 0x7Fu);
}

// Whether every component is a whole word, which is the case a typed read
// needs no conversion for: the bits in memory are the bits the shader
// wants. Anything else has to be unpacked, and saying so is the difference
// between a gap and a wrong colour.
inline bool buffer_format_is_whole_words(const BufferFormat& format) {
    const auto data = static_cast<BufferDataFormat>(format.data);
    const auto number = static_cast<BufferNumberFormat>(format.number);
    const auto wide = data == BufferDataFormat::Format32 ||
        data == BufferDataFormat::Format32_32 ||
        data == BufferDataFormat::Format32_32_32 ||
        data == BufferDataFormat::Format32_32_32_32;
    return wide && (number == BufferNumberFormat::Float ||
                    number == BufferNumberFormat::Uint ||
                    number == BufferNumberFormat::Sint);
}

// How many components a layout has.
inline std::uint32_t buffer_format_component_count(
    const BufferFormat& format) {
    switch (static_cast<BufferDataFormat>(format.data)) {
        case BufferDataFormat::Format8:
        case BufferDataFormat::Format16:
        case BufferDataFormat::Format32:
            return 1;
        case BufferDataFormat::Format8_8:
        case BufferDataFormat::Format16_16:
        case BufferDataFormat::Format32_32:
            return 2;
        case BufferDataFormat::Format10_11_11:
        case BufferDataFormat::Format11_11_10:
        case BufferDataFormat::Format32_32_32:
            return 3;
        case BufferDataFormat::Format10_10_10_2:
        case BufferDataFormat::Format2_10_10_10:
        case BufferDataFormat::Format8_8_8_8:
        case BufferDataFormat::Format16_16_16_16:
        case BufferDataFormat::Format32_32_32_32:
            return 4;
        default:
            return 0;
    }
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_BUFFER_FORMAT_H
