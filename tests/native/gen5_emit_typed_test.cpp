// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Taking a typed buffer element apart. Reading a narrow format as though
// it were wide does not fail - it puts the bit pattern of four bytes into
// one float - so what these checks are about is where each component's
// bits are and how wide they are.
#include <cstdio>

#include "gen5_emit_typed.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

static ps5gen5::BufferFormat format_of(
    ps5gen5::BufferDataFormat data, ps5gen5::BufferNumberFormat number) {
    return ps5gen5::BufferFormat{
        static_cast<std::uint32_t>(data),
        static_cast<std::uint32_t>(number)};
}

int main() {
    using namespace ps5gen5;

    // Four bytes in one word, in order from the bottom.
    {
        const auto parts = typed_components_of(
            format_of(BufferDataFormat::Format8_8_8_8,
                      BufferNumberFormat::Unorm));
        check(parts.size() == 4, "four components");
        check(parts[0].word == 0 && parts[0].offset == 0 &&
                  parts[0].width == 8,
              "the first byte is at the bottom of the first word");
        check(parts[3].word == 0 && parts[3].offset == 24,
              "the fourth is at the top of the same word");
        check(typed_element_words(
                  format_of(BufferDataFormat::Format8_8_8_8,
                            BufferNumberFormat::Unorm)) == 1,
              "and the element is one word");
    }

    // Four halves need two words, two components to each.
    {
        const auto wide = format_of(
            BufferDataFormat::Format16_16_16_16,
            BufferNumberFormat::Float);
        const auto parts = typed_components_of(wide);
        check(parts.size() == 4, "four halves");
        check(parts[2].word == 1 && parts[2].offset == 0,
              "the third half starts the second word");
        check(typed_element_words(wide) == 2, "two words for the element");
    }

    // The odd one out: ten, ten, ten and two, and which end the two is at
    // depends on which of the two spellings the format is. Getting this
    // the wrong way round reads a normal as an alpha.
    {
        const auto parts = typed_components_of(
            format_of(BufferDataFormat::Format2_10_10_10,
                      BufferNumberFormat::Snorm));
        check(parts.size() == 4 && parts[3].width == 2 &&
                  parts[3].offset == 30,
              "2_10_10_10 puts its two bit field at the top");
        const auto other = typed_components_of(
            format_of(BufferDataFormat::Format10_10_10_2,
                      BufferNumberFormat::Snorm));
        check(other.size() == 4 && other[0].width == 2 &&
                  other[0].offset == 0,
              "10_10_10_2 puts it at the bottom");
    }

    // A format whose components are whole words is not this path's
    // business, and saying otherwise would unpack something that needs no
    // unpacking.
    {
        const auto wide = format_of(
            BufferDataFormat::Format32_32_32_32,
            BufferNumberFormat::Float);
        check(buffer_format_is_whole_words(wide),
              "whole words are read as they lie");
        check(!typed_format_is_unpackable(wide),
              "and are not unpacked");
        const auto narrow = format_of(
            BufferDataFormat::Format8_8_8_8,
            BufferNumberFormat::Unorm);
        check(!buffer_format_is_whole_words(narrow) &&
                  typed_format_is_unpackable(narrow),
              "narrow components are unpacked instead");
    }

    // The table is the one the descriptor indexes, not a guess: format 14
    // in the unified numbering is 16 bit with no normalisation, which is
    // two fields rather than one.
    {
        const auto format = buffer_format_of(14);
        check(format.data == static_cast<std::uint32_t>(
                                 BufferDataFormat::Format8_8),
              "a unified format decodes to a layout");
        check(buffer_format_component_count(format) == 2,
              "and the layout says how many components there are");
    }

    if (failures == 0) {
        std::printf("all typed read checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
