// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest textures and storage images, declared the way the runtime binds
// them and read the way the guest reads them.
//
// Like the buffers, this half is not free to invent: the descriptor set
// the runtime builds has one binding per image starting after the buffers,
// and a module that numbers them differently binds against the wrong
// descriptor. What is new here is that the declaration depends on the
// descriptor's own bits - whether an image is sampled through a sampler or
// read and written directly changes its type, and declaring the wrong one
// binds against a descriptor of the other kind.

#ifndef PS5_GEN5_EMIT_IMAGE_H
#define PS5_GEN5_EMIT_IMAGE_H

#include <cstdint>
#include <vector>

#include "spirv_builder.h"

namespace ps5gen5 {

enum class ImageComponentKind {
    Float,
    Sint,
    Uint,
};

// What a T# descriptor says about an image, as far as the declaration
// needs. The fields are read from the descriptor's words rather than
// guessed: the number format decides the component type, and the numeric
// format alone decides whether the shader sees floats or integers.
struct ImageDescriptor {
    ImageComponentKind component = ImageComponentKind::Float;
    bool is_storage = false;
    ps5spirv::ImageDim dim = ps5spirv::ImageDim::Dim2D;
    bool arrayed = false;
    // A one dimensional image, declared as a row of a 2D one: the runtime
    // makes every surface a 2D image with a 2D view, and a 1D declaration
    // over one reads nothing. Its second coordinate is zero and its array
    // layer, if any, is the register after the first.
    bool one_dimensional = false;
};

// The declared image, with the types needed to sample or load from it.
struct DeclaredImage {
    std::uint32_t variable = 0;
    std::uint32_t image_type = 0;
    std::uint32_t object_type = 0;
    std::uint32_t component_type = 0;
    std::uint32_t result_type = 0;
    bool is_storage = false;
};

// GCN's number format field, which says how the bits in a texel are read.
// Only the distinction the declaration needs is drawn here: whether the
// shader sees floats, signed integers or unsigned ones.
inline ImageComponentKind component_kind_of(std::uint32_t number_format) {
    switch (number_format) {
        case 4:  // Sint
        case 6:  // Sscaled
            return ImageComponentKind::Sint;
        case 5:  // Uint
        case 7:  // Uscaled
            return ImageComponentKind::Uint;
        default:
            return ImageComponentKind::Float;
    }
}

// A T# as the runtime reads it. The layout is not this translator's to
// choose: the runtime creates the view the module binds against, so the
// two read the same words the same way or nothing lines up. The format
// is nine bits at 20 of the second word and the type is four at 28 of the
// fourth, both taken from where the runtime takes them.
struct ImageDescriptorWords {
    std::uint32_t format = 0;
    std::uint32_t type = 0;
};

inline ImageDescriptorWords read_image_descriptor_words(
    const std::uint32_t* descriptor) {
    ImageDescriptorWords words;
    words.format = (descriptor[1] >> 20) & 0x1FFu;
    words.type = (descriptor[3] >> 28) & 0xFu;
    return words;
}

// The formats the runtime maps to an integer Vulkan format. Sampling one
// of these as float would read the bits through the wrong conversion, and
// declaring float where the runtime binds an integer view is not a
// mismatch the module can recover from. The numbers come from the
// runtime's own table rather than from a specification, so the two cannot
// drift apart.
inline ImageComponentKind component_kind_of_format(std::uint32_t format) {
    switch (format) {
        case 5: case 11: case 18: case 20: case 27:
        case 54: case 60: case 62: case 69: case 75:
            return ImageComponentKind::Uint;
        case 6: case 12: case 19: case 21: case 28:
        case 61: case 63: case 70: case 76:
            return ImageComponentKind::Sint;
        default:
            return ImageComponentKind::Float;
    }
}

// The shape. Type 8 is a one dimensional image, 9 two, 10 three, 11 a
// cube, and 12 to 15 are the arrayed and multisampled forms of the first
// two. Anything else is a buffer view, which is not an image here.
inline ImageDescriptor image_descriptor_of(
    const std::uint32_t* descriptor, bool is_storage) {
    const auto words = read_image_descriptor_words(descriptor);
    ImageDescriptor image;
    image.component = component_kind_of_format(words.format);
    image.is_storage = is_storage;
    switch (words.type) {
        case 8:
            // The output pass's exposure is one of these, 1x1; declared
            // 1D over the runtime's 2D view it read zero and every frame
            // after the intro was black.
            image.dim = ps5spirv::ImageDim::Dim2D;
            image.one_dimensional = true;
            break;
        case 10:
            // A volume, bound to a 3D view now that the runtime makes them.
            // As its first slice it lost its third coordinate, and the
            // intro's froxel fog read one layer of a volume nothing had
            // filled.
            image.dim = ps5spirv::ImageDim::Dim3D;
            break;
        case 11:
            image.dim = ps5spirv::ImageDim::Cube;
            break;
        case 12:
            image.dim = ps5spirv::ImageDim::Dim2D;
            image.one_dimensional = true;
            image.arrayed = true;
            break;
        case 13:
        case 15:
            image.dim = ps5spirv::ImageDim::Dim2D;
            image.arrayed = true;
            break;
        default:
            image.dim = ps5spirv::ImageDim::Dim2D;
            break;
    }
    return image;
}

// How many coordinate components a shape takes. A sample needs exactly
// this many or the instruction is malformed, and an array layer counts as
// one of them.
inline std::uint32_t coordinate_count_of(const ImageDescriptor& image) {
    std::uint32_t count = image.dim == ps5spirv::ImageDim::Dim1D ? 1
        : image.dim == ps5spirv::ImageDim::Dim3D ? 3
        : image.dim == ps5spirv::ImageDim::Cube ? 3
                                                : 2;
    if (image.arrayed) {
        ++count;
    }
    return count;
}

inline DeclaredImage declare_image(
    ps5spirv::ModuleBuilder& module,
    const ImageDescriptor& descriptor,
    std::uint32_t binding,
    std::uint32_t float_type,
    std::uint32_t int_type,
    std::uint32_t uint_type) {
    DeclaredImage declared;
    declared.is_storage = descriptor.is_storage;
    declared.component_type =
        descriptor.component == ImageComponentKind::Sint ? int_type
        : descriptor.component == ImageComponentKind::Uint ? uint_type
                                                           : float_type;
    // A storage image whose format is Unknown needs the capabilities that
    // allow reading and writing it. This declared Shader instead, which is
    // already declared and says nothing about images - the module was
    // reading and writing formatless storage images without ever claiming
    // it could.
    if (descriptor.is_storage) {
        module.add_capability(
            ps5spirv::Capability::StorageImageReadWithoutFormat);
        module.add_capability(
            ps5spirv::Capability::StorageImageWriteWithoutFormat);
    }
    if (descriptor.dim == ps5spirv::ImageDim::Cube && descriptor.arrayed) {
        module.add_capability(
            descriptor.is_storage
                ? ps5spirv::Capability::ImageCubeArray
                : ps5spirv::Capability::SampledCubeArray);
    }
    declared.image_type = module.type_image(
        declared.component_type,
        descriptor.dim,
        false,
        descriptor.arrayed,
        false,
        descriptor.is_storage ? 2u : 1u,
        ps5spirv::ImageFormat::Unknown);
    declared.object_type = descriptor.is_storage
        ? declared.image_type
        : module.type_sampled_image(declared.image_type);
    declared.result_type = module.type_vector(declared.component_type, 4);

    const auto pointer = module.type_pointer(
        ps5spirv::StorageClass::UniformConstant, declared.object_type);
    declared.variable = module.allocate_id();
    module.add_global(
        ps5spirv::Op::Variable,
        {pointer, declared.variable,
         static_cast<std::uint32_t>(
             ps5spirv::StorageClass::UniformConstant)});
    module.add_decoration(
        declared.variable, ps5spirv::Decoration::DescriptorSet, {0});
    module.add_decoration(
        declared.variable, ps5spirv::Decoration::Binding, {binding});
    return declared;
}

// The SPIR-V instructions an image read uses, which are not in the
// builder's enum because nothing else needed them.
enum class ImageSpirvOp : std::uint16_t {
    ImageSampleImplicitLod = 87,
    ImageSampleExplicitLod = 88,
    ImageFetch = 95,
    ImageRead = 98,
    ImageWrite = 99,
};

// A sampled read. The coordinate is a float vector; the level is given
// explicitly because the guest instruction that reaches here - sample with
// level zero - says so, and an implicit level is not available outside a
// fragment shader anyway.
inline std::uint32_t emit_image_sample_lod(
    ps5spirv::ModuleBuilder& module,
    const DeclaredImage& image,
    std::uint32_t sampled_image_value,
    std::uint32_t coordinate,
    std::uint32_t lod) {
    const auto result = module.allocate_id();
    module.add_function_word(
        static_cast<ps5spirv::Op>(ImageSpirvOp::ImageSampleExplicitLod),
        {image.result_type, result, sampled_image_value, coordinate,
         // Image operands: Lod is bit 1.
         0x2u, lod});
    return result;
}

// An unfiltered read, which is what the guest's image load is: no sampler,
// integer coordinates, and the texel as it lies.
inline std::uint32_t emit_image_read(
    ps5spirv::ModuleBuilder& module,
    const DeclaredImage& image,
    std::uint32_t image_value,
    std::uint32_t coordinate) {
    const auto result = module.allocate_id();
    module.add_function_word(
        static_cast<ps5spirv::Op>(
            image.is_storage ? ImageSpirvOp::ImageRead
                             : ImageSpirvOp::ImageFetch),
        {image.result_type, result, image_value, coordinate});
    return result;
}

inline void emit_image_write(
    ps5spirv::ModuleBuilder& module,
    std::uint32_t image_value,
    std::uint32_t coordinate,
    std::uint32_t texel) {
    module.add_function_word(
        static_cast<ps5spirv::Op>(ImageSpirvOp::ImageWrite),
        {image_value, coordinate, texel});
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_EMIT_IMAGE_H
