// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// The manifest the translator writes is parsed by the runtime using the
// bridge's own structs, so the two layouts have to agree byte for byte.
// This checks the sizes against those structs rather than against a
// comment, and checks that a built manifest passes the validation the
// runtime applies before it will look at a single record - a manifest that
// fails it is rejected whole, which looks like a shader with no resources
// rather than like a malformed manifest.
#include <cstdio>
#include <cstring>

#include "gen5_manifest.h"
#include "ps5gpu_bridge_api.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

int main() {
    using namespace ps5gen5;

    check(kManifestHeaderSize == sizeof(Ps5GpuResourceManifestHeader),
          "the header is the size the runtime reads");
    check(kManifestGlobalSize == sizeof(Ps5GpuResourceGlobal),
          "a buffer record is the size the runtime reads");
    check(kManifestImageSize == sizeof(Ps5GpuResourceImage),
          "an image record is the size the runtime reads");
    check(kManifestMagic == PS5GPU_RESOURCE_MANIFEST_MAGIC,
          "the magic is the bridge's magic");
    check(kManifestVersion == PS5GPU_RESOURCE_MANIFEST_VERSION,
          "the version is the bridge's version");

    ManifestBuffer buffer;
    buffer.binding = 2;
    buffer.base_address = 0x0000123456789000ull;
    buffer.size_bytes = 4096;
    ManifestImage image;
    image.binding = 1;
    image.pc = 40;
    image.base_address = 0x0000900000000000ull;
    image.width = 256;
    image.height = 128;
    image.resource[0] = 0x11111111u;
    image.sampler[3] = 0x22222222u;

    const auto bytes = build_manifest(0x8u, {buffer}, {image});

    Ps5GpuResourceManifestHeader header = {};
    std::memcpy(&header, bytes.data(), sizeof(header));
    check(header.magic == PS5GPU_RESOURCE_MANIFEST_MAGIC &&
              header.version == PS5GPU_RESOURCE_MANIFEST_VERSION &&
              header.header_size == sizeof(header) &&
              header.total_size == bytes.size() &&
              header.data_offset <= bytes.size(),
          "a built manifest passes the runtime's validation");
    check(header.global_count == 1 && header.image_count == 1 &&
              header.vertex_count == 0,
          "the counts say what was put in");

    Ps5GpuResourceGlobal global = {};
    std::memcpy(
        &global, bytes.data() + header.global_offset, sizeof(global));
    check(global.descriptor_index == 2 &&
              global.base_address == 0x0000123456789000ull &&
              global.data_size == 4096,
          "a buffer record survives the round trip");

    Ps5GpuResourceImage loaded = {};
    std::memcpy(
        &loaded, bytes.data() + header.image_offset, sizeof(loaded));
    check(loaded.binding == 1 && loaded.pc == 40 &&
              loaded.width == 256 && loaded.height == 128 &&
              loaded.resource_descriptor[0] == 0x11111111u &&
              loaded.sampler_descriptor[3] == 0x22222222u,
          "an image record survives the round trip");

    // The two halves of a V#: the address is forty-eight bits across two
    // words, and the size is the record count times the stride unless
    // there is no stride, when the records are already bytes.
    {
        const std::array<std::uint32_t, 4> descriptor{
            0x89ABCDEFu, (16u << 16) | 0x1234u, 64u, 0u};
        check(buffer_base_address(descriptor) == 0x0000123489ABCDEFull,
              "a buffer address joins its two words");
        check(buffer_size_bytes(descriptor) == 64u * 16u,
              "a strided buffer counts elements");
    }
    {
        const std::array<std::uint32_t, 4> descriptor{0u, 0u, 4096u, 0u};
        check(buffer_size_bytes(descriptor) == 4096u,
              "a buffer without a stride counts bytes");
    }

    // A T# says how big the image is where the runtime reads it, which is
    // two bits of width in one word and the rest in the next.
    {
        std::array<std::uint32_t, 8> descriptor{};
        descriptor[1] = 3u << 30;
        descriptor[2] = 63u | (127u << 14);
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        image_extent(descriptor, width, height);
        check(width == ((3u | (63u << 2)) + 1u),
              "the width crosses the word boundary");
        check(height == 128u, "the height is the field above it");
    }

    // The UI plane's mask: a 1x1 8_8_8_8_UNORM image, which no pipeline
    // may bind and which must still read as its texel, not as zero.
    {
        const std::array<std::uint32_t, 8> descriptor = {
            0x05074057u, 0x03800000u, 0u, 0x90000FACu, 0u, 0u, 0u, 0u};
        check(!image_descriptor_is_real(descriptor) &&
                  image_descriptor_is_single_texel(descriptor),
              "a 1x1 image is a single texel, not a bindable image");
        const auto white = single_texel_value(descriptor, 0xFFFFFFFFu, true);
        check(white[0] == 0x3F800000u && white[3] == 0x3F800000u,
              "an opaque white texel reads as ones");
        const auto unread = single_texel_value(descriptor, 0u, false);
        check(unread[1] == 0x3F800000u,
              "a texel that cannot be read is taken as white");
        const std::array<std::uint32_t, 8> empty{};
        check(!image_descriptor_is_single_texel(empty),
              "a descriptor of zeros is not a texel");
    }

    // The output pass's exposure: a 1x1 32_32_32_32_FLOAT image a compute
    // stores to. It is bound like any other image, not read as a constant.
    {
        const std::array<std::uint32_t, 8> descriptor = {
            0x05567600u, 0x04D00000u, 0u, 0x81B00FACu, 0u, 0u, 0u, 0u};
        check(image_descriptor_is_real(descriptor) &&
                  !image_descriptor_is_single_texel(descriptor),
              "a 1x1 float image is a bindable image");
    }

    if (failures == 0) {
        std::printf("all manifest checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
