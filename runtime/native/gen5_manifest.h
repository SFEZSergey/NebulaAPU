// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The resource manifest, which is the other half of what a translated
// shader hands the runtime.
//
// The SPIR-V says a shader reads binding three; the manifest says what
// binding three is - which guest address, how many bytes, which descriptor
// it came from. The runtime builds its descriptor set from this and never
// looks at the module, so a manifest that disagrees with the module binds
// the wrong memory in a way nothing downstream can detect.
//
// The layout is not this translator's to choose. It is the format the
// bridge already produces and the runtime already parses, field for field.

#ifndef PS5_GEN5_MANIFEST_H
#define PS5_GEN5_MANIFEST_H

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ps5gen5 {

// A buffer the module declared, and where its contents live.
struct ManifestBuffer {
    std::uint32_t binding = 0;
    std::uint32_t scalar_address = 0;
    std::uint64_t base_address = 0;
    std::uint32_t size_bytes = 0;
};

// An image the module declared, with the descriptors it was declared from.
// The runtime re-reads those words rather than trusting anything derived
// from them, which is why they travel whole.
struct ManifestImage {
    std::uint32_t binding = 0;
    std::uint32_t pc = 0;
    std::uint32_t flags = 0;
    std::uint64_t base_address = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::array<std::uint32_t, 8> resource{};
    std::array<std::uint32_t, 4> sampler{};
};

// The base address a V# names, which is forty-eight bits split across two
// words rather than a flat pointer.
inline std::uint64_t buffer_base_address(
    const std::array<std::uint32_t, 4>& descriptor) {
    return descriptor[0] |
        (static_cast<std::uint64_t>(descriptor[1] & 0xFFFFu) << 32);
}

// How many bytes a V# covers. The record count is in elements when there
// is a stride and in bytes when there is not.
inline std::uint32_t buffer_size_bytes(
    const std::array<std::uint32_t, 4>& descriptor) {
    const auto stride = (descriptor[1] >> 16) & 0x3FFFu;
    const auto records = descriptor[2];
    if (stride == 0) {
        return records;
    }
    const auto total = static_cast<std::uint64_t>(records) * stride;
    return total > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                 : static_cast<std::uint32_t>(total);
}

// A T# carries its size where the runtime reads it: fourteen bits of width
// split across two words, and sixteen of height above them.
inline void image_extent(
    const std::array<std::uint32_t, 8>& descriptor,
    std::uint32_t& width,
    std::uint32_t& height) {
    width = (((descriptor[1] >> 30) & 0x3u) |
             ((descriptor[2] & 0x3FFFu) << 2)) + 1;
    height = ((descriptor[2] >> 14) & 0xFFFFu) + 1;
}

inline std::uint64_t image_base_address(
    const std::array<std::uint32_t, 8>& descriptor) {
    return ((static_cast<std::uint64_t>(descriptor[1] & 0xFFu) << 32) |
            descriptor[0]) << 8;
}

// Whether a T# is a description of an image at all. The runtime applies
// this same test before it will touch one, and a manifest that declares an
// image failing it hands the runtime an address to write to that nothing
// ever put an image at. The first run that let the native producer drive
// declared two such images per dispatch - both 1x1, both at addresses the
// descriptor never held - and the process died inside the host driver six
// seconds in.
//
// A 1x1 image in a float format is real all the same. It is data a pass
// computes for the next - the exposure the output pass scales the frame
// by, stored by one compute and loaded by the output pass - and read as a
// constant it was whatever the guest memory held when the reader was
// translated: zero, or white for a format the texel reader does not know.
// Every frame after the intro was black, or clamped to white.
inline bool image_format_is_float_data(std::uint32_t format) {
    // 32_FLOAT, 16_16_16_16_FLOAT and 32_32_32_32_FLOAT by their GFX10
    // numbers.
    return format == 22 || format == 71 || format == 77;
}

inline bool image_descriptor_is_real(
    const std::array<std::uint32_t, 8>& descriptor) {
    const auto address = image_base_address(descriptor);
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    image_extent(descriptor, width, height);
    const auto format = (descriptor[1] >> 20) & 0x1FFu;
    const auto type = (descriptor[3] >> 28) & 0xFu;
    const auto big_enough = (width > 1 && height > 1) ||
        (width == 1 && height == 1 && image_format_is_float_data(format));
    return address >= 0x10000 && big_enough &&
        format != 0 &&
        !(type >= 1 && type <= 7);
}

// A T# for a single texel: a 1x1 image at a real address in a real
// format. Titles bind one where a material has no texture - a white
// mask, a neutral normal - and read it like any other. It fails the test
// above, which bars 1x1 images from being bound, so a translation reads
// the texel itself and uses it as a constant.
inline bool image_descriptor_is_single_texel(
    const std::array<std::uint32_t, 8>& descriptor) {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    image_extent(descriptor, width, height);
    const auto format = (descriptor[1] >> 20) & 0x1FFu;
    const auto type = (descriptor[3] >> 28) & 0xFu;
    return image_base_address(descriptor) >= 0x10000 && width == 1 &&
        height == 1 && format != 0 && !image_format_is_float_data(format) &&
        !(type >= 1 && type <= 7);
}

// The four components a single-texel image reads as, as float bits, with
// the descriptor's swizzle applied: from the texel's bytes where the format
// is one this knows, white where it is not - a default texture is far more
// often white than black, and black is what reading nothing already gave.
inline std::array<std::uint32_t, 4> single_texel_value(
    const std::array<std::uint32_t, 8>& descriptor, std::uint32_t texel,
    bool texel_read) {
    constexpr std::uint32_t kOne = 0x3F800000u;
    std::array<float, 4> channels = {1.0f, 1.0f, 1.0f, 1.0f};
    const auto format = (descriptor[1] >> 20) & 0x1FFu;
    // 8_8_8_8_UNORM and 8_UNORM, by their GFX10 numbers.
    constexpr std::uint32_t kFormat8888Unorm = 56;
    constexpr std::uint32_t kFormat8Unorm = 1;
    if (texel_read && format == kFormat8888Unorm) {
        for (std::uint32_t index = 0; index < 4; ++index) {
            channels[index] =
                static_cast<float>((texel >> (index * 8)) & 0xFFu) / 255.0f;
        }
    } else if (texel_read && format == kFormat8Unorm) {
        channels = {static_cast<float>(texel & 0xFFu) / 255.0f, 0.0f, 0.0f,
                    1.0f};
    }
    std::array<std::uint32_t, 4> result{};
    for (std::uint32_t index = 0; index < 4; ++index) {
        // dst_sel: 0 is zero, 1 is one, 4 to 7 are x, y, z and w.
        const auto select = (descriptor[3] >> (index * 3)) & 0x7u;
        float value = 0.0f;
        if (select == 1) {
            value = 1.0f;
        } else if (select >= 4) {
            value = channels[select - 4];
        }
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        result[index] = select == 1 ? kOne : bits;
    }
    return result;
}

// The same for a V#. An address below the first page is not one.
inline bool buffer_descriptor_is_real(
    const std::array<std::uint32_t, 4>& descriptor) {
    return buffer_base_address(descriptor) >= 0x10000 &&
        buffer_size_bytes(descriptor) != 0;
}

// The header and the three record kinds, laid out exactly as the bridge
// lays them out. They are written by hand rather than by including the
// bridge's header so that this stays a header-only translation unit with
// no dependency on the C interface, and a test pins the sizes.
constexpr std::uint32_t kManifestMagic = 0x4D524750u;  // "PGRM"
constexpr std::uint32_t kManifestVersion = 1;
constexpr std::uint32_t kManifestHeaderSize = 64;
constexpr std::uint32_t kManifestGlobalSize = 32;
constexpr std::uint32_t kManifestImageSize = 80;

inline void append_word(
    std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::uint32_t shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

inline void append_long(
    std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    append_word(bytes, static_cast<std::uint32_t>(value));
    append_word(bytes, static_cast<std::uint32_t>(value >> 32));
}

// The manifest for one stage. No data section: every buffer here has a
// real address, and the runtime reads the title's own memory rather than a
// copy - the copies were seventeen megabytes a state and killed the
// process at a hundred seconds.
inline std::vector<std::uint8_t> build_manifest(
    std::uint32_t stage,
    const std::vector<ManifestBuffer>& buffers,
    const std::vector<ManifestImage>& images) {
    std::vector<std::uint8_t> bytes;
    const auto global_offset = kManifestHeaderSize;
    const auto image_offset = global_offset +
        kManifestGlobalSize * static_cast<std::uint32_t>(buffers.size());
    const auto vertex_offset = image_offset +
        kManifestImageSize * static_cast<std::uint32_t>(images.size());
    const auto total = vertex_offset;

    append_word(bytes, kManifestMagic);
    append_word(bytes, kManifestVersion);
    append_word(bytes, kManifestHeaderSize);
    append_word(bytes, total);
    append_word(bytes, stage);
    append_word(bytes, static_cast<std::uint32_t>(buffers.size()));
    append_word(bytes, static_cast<std::uint32_t>(images.size()));
    append_word(bytes, 0);                 // no vertex inputs
    append_word(bytes, global_offset);
    append_word(bytes, image_offset);
    append_word(bytes, vertex_offset);
    // The data section sits past the end, which is how a manifest says it
    // carries no copies.
    append_word(bytes, total);
    for (std::uint32_t index = 0; index < 4; ++index) {
        append_word(bytes, 0);
    }

    for (const auto& buffer : buffers) {
        append_word(bytes, buffer.binding);
        append_word(bytes, buffer.scalar_address);
        append_long(bytes, buffer.base_address);
        append_word(bytes, 0);             // no data offset
        append_word(bytes, buffer.size_bytes);
        append_word(bytes, 0);             // flags
        append_word(bytes, 0);             // reserved
    }

    for (const auto& image : images) {
        append_word(bytes, image.binding);
        append_word(bytes, image.pc);
        append_word(bytes, image.flags);
        append_word(bytes, 0);             // mip level
        append_long(bytes, image.base_address);
        append_word(bytes, image.width);
        append_word(bytes, image.height);
        for (const auto word : image.resource) {
            append_word(bytes, word);
        }
        for (const auto word : image.sampler) {
            append_word(bytes, word);
        }
    }
    return bytes;
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_MANIFEST_H
