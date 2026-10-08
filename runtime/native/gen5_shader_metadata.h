// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The shader header, ported from Gen5ShaderMetadataReader.cs.
//
// This is the piece the port was missing and did not know it was missing.
// AGC does not hand a shader its descriptors in user data: it hands it a
// pointer, and the layout of what that pointer reaches is described here,
// in a structure hanging off the shader's header. Without reading it, the
// scalar walk treats a table index as if it were an address - which is
// exactly what it was doing, asking for words at 0x0070000000000018 and
// getting nothing, on every one of 212 reads.
//
// Everything in it is offsets into the shader resource table, in units of
// dwords, indexed by a resource class and a slot. The sharp - the word that
// names a resource - is fifteen bits of offset and one flag, and 0x7FFF
// means the slot is not used rather than an offset of 32767.

#ifndef PS5_GEN5_SHADER_METADATA_H
#define PS5_GEN5_SHADER_METADATA_H

#include <cstdint>
#include <map>
#include <vector>

#include "gen5_shader_ir.h"

namespace ps5gen5 {

// Where the header keeps the pointer to its user data description.
constexpr std::uint64_t kShaderUserDataOffset = 0x08;
constexpr std::size_t kResourceClassCount = 4;
// A count larger than this is a sign the structure was not what was
// expected, rather than a shader with thousands of resources.
constexpr std::uint32_t kMaxMetadataEntries = 4096;

// Reads the header. `read_u16`, `read_u32` and `read_u64` fetch guest
// memory and say whether they could; a false anywhere means the header is
// not there, which is different from a shader with no resources.
template <typename ReadU16, typename ReadU32, typename ReadU64>
bool read_shader_metadata(
    std::uint64_t shader_header_address,
    const ReadU16& read_u16,
    const ReadU32& read_u32,
    const ReadU64& read_u64,
    ShaderMetadata& metadata) {
    (void)read_u32;
    metadata = ShaderMetadata{};

    std::uint64_t user_data_address = 0;
    if (!read_u64(shader_header_address + kShaderUserDataOffset,
                  user_data_address) ||
        user_data_address == 0) {
        return false;
    }
    std::uint64_t direct_resource_offsets = 0;
    if (!read_u64(user_data_address, direct_resource_offsets)) {
        return false;
    }

    std::uint64_t resource_offsets[kResourceClassCount] = {};
    for (std::size_t resource_class = 0;
         resource_class < kResourceClassCount;
         ++resource_class) {
        if (!read_u64(
                user_data_address + 0x08 + resource_class * 8,
                resource_offsets[resource_class])) {
            return false;
        }
    }

    std::uint16_t extended_user_data_size = 0;
    std::uint16_t shader_resource_table_size = 0;
    std::uint16_t direct_resource_count = 0;
    if (!read_u16(user_data_address + 0x28, extended_user_data_size) ||
        !read_u16(user_data_address + 0x2A, shader_resource_table_size) ||
        !read_u16(user_data_address + 0x2C, direct_resource_count) ||
        direct_resource_count > kMaxMetadataEntries) {
        return false;
    }

    std::uint16_t resource_counts[kResourceClassCount] = {};
    for (std::size_t resource_class = 0;
         resource_class < kResourceClassCount;
         ++resource_class) {
        if (!read_u16(
                user_data_address + 0x2E + resource_class * 2,
                resource_counts[resource_class]) ||
            resource_counts[resource_class] > kMaxMetadataEntries) {
            return false;
        }
    }

    if (direct_resource_count != 0) {
        if (direct_resource_offsets == 0) {
            return false;
        }
        for (std::uint32_t type = 0; type < direct_resource_count; ++type) {
            std::uint16_t offset = 0;
            if (!read_u16(direct_resource_offsets + type * 2, offset)) {
                return false;
            }
            // 0xFFFF marks a type the shader does not use.
            if (offset != 0xFFFFu) {
                metadata.direct_resources.push_back({type, offset});
            }
        }
    }

    for (std::size_t resource_class = 0;
         resource_class < kResourceClassCount;
         ++resource_class) {
        const auto count = resource_counts[resource_class];
        if (count == 0) {
            continue;
        }
        if (resource_offsets[resource_class] == 0) {
            return false;
        }
        for (std::uint32_t slot = 0; slot < count; ++slot) {
            std::uint16_t sharp = 0;
            if (!read_u16(
                    resource_offsets[resource_class] + slot * 2, sharp)) {
                return false;
            }
            const std::uint32_t offset = sharp & 0x7FFFu;
            // Not an offset of 32767 but a slot that is not used, which is
            // a distinction worth keeping: treating it as an offset points
            // a descriptor a long way past the end of the table.
            if (offset == 0x7FFFu) {
                continue;
            }
            ResourceMapping mapping;
            mapping.kind = static_cast<ResourceKind>(resource_class);
            mapping.slot = slot;
            mapping.offset_dwords = offset;
            mapping.size_flag = (sharp & 0x8000u) != 0;
            metadata.resources.push_back(mapping);
        }
    }

    metadata.extended_user_data_size_dwords = extended_user_data_size;
    metadata.shader_resource_table_size_dwords = shader_resource_table_size;
    return true;
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_SHADER_METADATA_H
