// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Image declarations bind against descriptors the runtime creates, so the
// things that must be right are the ones that make a binding match: the
// set and binding numbers, whether the image is sampled or storage, and
// what its components are. Getting any of them wrong binds against a
// descriptor of the wrong kind, which is not an error a shader reports.
#include <array>
#include <cstdio>
#include <vector>

#include "gen5_emit_image.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

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
            length >= 4 && words[index + 1] == target &&
            words[index + 2] == decoration) {
            return words[index + 3];
        }
        index += length;
    }
    return 0xFFFFFFFFu;
}

// The operands of the OpTypeImage that declares this image.
static std::vector<std::uint32_t> image_type_operands(
    const std::vector<std::uint32_t>& words, std::uint32_t type_id) {
    std::size_t index = 5;
    while (index < words.size()) {
        const auto length = words[index] >> 16;
        if (length == 0) {
            break;
        }
        if ((words[index] & 0xFFFFu) ==
                static_cast<std::uint32_t>(ps5spirv::Op::TypeImage) &&
            length >= 2 && words[index + 1] == type_id) {
            return std::vector<std::uint32_t>(
                words.begin() + index, words.begin() + index + length);
        }
        index += length;
    }
    return {};
}

struct Fixture {
    ps5spirv::ModuleBuilder module;
    std::uint32_t float_type = module.type_float(32);
    std::uint32_t int_type = module.type_int(32, true);
    std::uint32_t uint_type = module.type_int(32, false);
};

int main() {
    using namespace ps5gen5;

    // The number format decides what the shader sees. A texture of signed
    // integers declared as floats reads the same bits as nonsense.
    check(component_kind_of(4) == ImageComponentKind::Sint,
          "format 4 is signed");
    check(component_kind_of(5) == ImageComponentKind::Uint,
          "format 5 is unsigned");
    check(component_kind_of(0) == ImageComponentKind::Float,
          "the rest are floats");

    // A sampled image is sampled=1 and wrapped in a sampled image type; a
    // storage image is sampled=2 and is not wrapped. Declaring one as the
    // other binds against a descriptor of the wrong kind.
    {
        Fixture f;
        ImageDescriptor descriptor;
        const auto texture = declare_image(
            f.module, descriptor, 1, f.float_type, f.int_type, f.uint_type);
        check(texture.object_type != texture.image_type,
              "a texture is wrapped in a sampled image type");
        const auto words = f.module.build();
        const auto operands = image_type_operands(words, texture.image_type);
        check(operands.size() >= 8 && operands[7] == 1,
              "a texture declares sampled one");
    }
    {
        Fixture f;
        ImageDescriptor descriptor;
        descriptor.is_storage = true;
        const auto storage = declare_image(
            f.module, descriptor, 1, f.float_type, f.int_type, f.uint_type);
        check(storage.object_type == storage.image_type,
              "a storage image is not wrapped");
        const auto words = f.module.build();
        const auto operands = image_type_operands(words, storage.image_type);
        check(operands.size() >= 8 && operands[7] == 2,
              "a storage image declares sampled two");
    }

    // The binding number is what the runtime matches against. Images start
    // after the buffers, so an image at binding zero would collide with
    // them.
    {
        Fixture f;
        ImageDescriptor descriptor;
        const auto image = declare_image(
            f.module, descriptor, 5, f.float_type, f.int_type, f.uint_type);
        const auto words = f.module.build();
        check(decoration_of(words, image.variable,
                            static_cast<std::uint32_t>(
                                ps5spirv::Decoration::DescriptorSet)) == 0,
              "images are in descriptor set zero");
        check(decoration_of(words, image.variable,
                            static_cast<std::uint32_t>(
                                ps5spirv::Decoration::Binding)) == 5,
              "at the binding they were given");
    }

    // The component type follows the descriptor, and the result of a read
    // is four of them.
    {
        Fixture f;
        ImageDescriptor descriptor;
        descriptor.component = ImageComponentKind::Uint;
        const auto image = declare_image(
            f.module, descriptor, 2, f.float_type, f.int_type, f.uint_type);
        check(image.component_type == f.uint_type,
              "an unsigned texture has unsigned components");
        check(image.result_type != 0 &&
                  image.result_type != image.component_type,
              "a read produces a vector of them");
    }

    // Two images of the same shape share one type but never one variable:
    // they are different descriptors.
    {
        Fixture f;
        ImageDescriptor descriptor;
        const auto first = declare_image(
            f.module, descriptor, 1, f.float_type, f.int_type, f.uint_type);
        const auto second = declare_image(
            f.module, descriptor, 2, f.float_type, f.int_type, f.uint_type);
        check(first.image_type == second.image_type,
              "the same shape is the same type");
        check(first.variable != second.variable,
              "but each image is its own variable");
    }

    // A sampled read names its level explicitly, because an implicit one is
    // only available where derivatives are.
    {
        Fixture f;
        ImageDescriptor descriptor;
        const auto image = declare_image(
            f.module, descriptor, 1, f.float_type, f.int_type, f.uint_type);
        emit_image_sample_lod(f.module, image, 40, 41, 42);
        const auto words = f.module.build();
        bool found = false;
        std::size_t index = 5;
        while (index < words.size()) {
            const auto length = words[index] >> 16;
            if (length == 0) {
                break;
            }
            if ((words[index] & 0xFFFFu) ==
                static_cast<std::uint32_t>(
                    ImageSpirvOp::ImageSampleExplicitLod)) {
                // type, result, image, coordinate, operands mask, lod
                found = length == 7 && words[index + 5] == 0x2u;
            }
            index += length;
        }
        check(found, "a sampled read names the lod operand");
    }

    // A T# says what shape the image is and how its texels are read, and
    // the module has to agree with the runtime about both - the runtime
    // creates the view this binds against. The fields are the format at
    // bit 20 of the second word and the type at bit 28 of the fourth.
    {
        std::array<std::uint32_t, 8> descriptor{};
        descriptor[1] = 56u << 20;      // R8G8B8A8_UNORM
        descriptor[3] = 9u << 28;       // two dimensional
        const auto shape = image_descriptor_of(descriptor.data(), false);
        check(shape.dim == ps5spirv::ImageDim::Dim2D && !shape.arrayed,
              "type nine is a plain two dimensional image");
        check(shape.component == ImageComponentKind::Float,
              "an unorm format is sampled as float");
        check(coordinate_count_of(shape) == 2,
              "a two dimensional image takes two coordinates");
    }
    {
        std::array<std::uint32_t, 8> descriptor{};
        descriptor[1] = 60u << 20;      // R8G8B8A8_UINT
        descriptor[3] = 13u << 28;      // arrayed
        const auto shape = image_descriptor_of(descriptor.data(), true);
        check(shape.arrayed && shape.dim == ps5spirv::ImageDim::Dim2D,
              "type thirteen is an array of two dimensional images");
        check(shape.component == ImageComponentKind::Uint,
              "an integer format is read as integers");
        check(coordinate_count_of(shape) == 3,
              "the array layer is one of the coordinates");
    }

    if (failures == 0) {
        std::printf("all image declaration checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
