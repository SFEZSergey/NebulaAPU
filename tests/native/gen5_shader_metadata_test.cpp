// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// The header describes where a shader's descriptors live, so the failures
// that matter are the ones that produce a plausible wrong offset: a slot
// that is not used read as an offset, a flag bit read as part of one, or a
// structure that is not there read as if it were.
#include <cstdio>
#include <map>
#include <vector>

#include "gen5_shader_metadata.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

// A little guest memory, written the way the header lays itself out.
struct Memory {
    std::map<std::uint64_t, std::uint8_t> bytes;

    void put16(std::uint64_t address, std::uint16_t value) {
        bytes[address] = static_cast<std::uint8_t>(value);
        bytes[address + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    void put64(std::uint64_t address, std::uint64_t value) {
        for (int index = 0; index < 8; ++index) {
            bytes[address + index] =
                static_cast<std::uint8_t>(value >> (8 * index));
        }
    }
    bool get(std::uint64_t address, std::uint8_t& out) const {
        const auto found = bytes.find(address);
        if (found == bytes.end()) {
            return false;
        }
        out = found->second;
        return true;
    }
};

int main() {
    using namespace ps5gen5;

    Memory memory;
    const auto read16 = [&](std::uint64_t address, std::uint16_t& out) {
        std::uint8_t low = 0;
        std::uint8_t high = 0;
        if (!memory.get(address, low) || !memory.get(address + 1, high)) {
            return false;
        }
        out = static_cast<std::uint16_t>(low | (high << 8));
        return true;
    };
    const auto read32 = [&](std::uint64_t, std::uint32_t&) { return false; };
    const auto read64 = [&](std::uint64_t address, std::uint64_t& out) {
        out = 0;
        for (int index = 0; index < 8; ++index) {
            std::uint8_t byte = 0;
            if (!memory.get(address + index, byte)) {
                return false;
            }
            out |= static_cast<std::uint64_t>(byte) << (8 * index);
        }
        return true;
    };

    constexpr std::uint64_t kHeader = 0x1000;
    constexpr std::uint64_t kUserData = 0x2000;
    constexpr std::uint64_t kDirect = 0x3000;
    constexpr std::uint64_t kClass0 = 0x4000;

    // A header that is not there is not a shader with no resources. The
    // difference decides whether a translation declines or declares
    // nothing, and only one of those is honest.
    {
        ShaderMetadata metadata;
        check(!read_shader_metadata(kHeader, read16, read32, read64,
                                    metadata),
              "an absent header is not read as empty");
    }

    // A whole header, with one class holding three slots of which one is
    // unused.
    memory.put64(kHeader + 0x08, kUserData);
    memory.put64(kUserData, kDirect);
    memory.put64(kUserData + 0x08, kClass0);      // class 0 offsets
    memory.put64(kUserData + 0x10, 0);
    memory.put64(kUserData + 0x18, 0);
    memory.put64(kUserData + 0x20, 0);
    memory.put16(kUserData + 0x28, 12);           // extended user data
    memory.put16(kUserData + 0x2A, 64);           // resource table size
    memory.put16(kUserData + 0x2C, 2);            // direct resource count
    memory.put16(kUserData + 0x2E, 3);            // class 0 count
    memory.put16(kUserData + 0x30, 0);
    memory.put16(kUserData + 0x32, 0);
    memory.put16(kUserData + 0x34, 0);
    memory.put16(kDirect, 7);
    memory.put16(kDirect + 2, 0xFFFF);            // unused type
    memory.put16(kClass0, 4);                     // slot 0 at dword 4
    memory.put16(kClass0 + 2, 0x7FFF);            // slot 1 unused
    memory.put16(kClass0 + 4, 0x8000 | 9);        // slot 2, flag set

    {
        ShaderMetadata metadata;
        check(read_shader_metadata(kHeader, read16, read32, read64,
                                   metadata),
              "a complete header reads");
        check(metadata.extended_user_data_size_dwords == 12 &&
                  metadata.shader_resource_table_size_dwords == 64,
              "the sizes are read");

        // 0xFFFF means a type the shader does not use, and keeping it
        // would point a resource at the far end of the table.
        check(metadata.direct_resources.size() == 1 &&
                  metadata.direct_resources[0].first == 0 &&
                  metadata.direct_resources[0].second == 7,
              "an unused direct resource type is left out");

        // Likewise 0x7FFF in a slot: not an offset of 32767.
        check(metadata.resources.size() == 2,
              "an unused slot is left out rather than read as an offset");
        if (metadata.resources.size() == 2) {
            check(metadata.resources[0].slot == 0 &&
                      metadata.resources[0].offset_dwords == 4 &&
                      !metadata.resources[0].size_flag,
                  "the first slot keeps its offset and has no flag");
            // The top bit is a flag, not part of the offset. Reading it as
            // one turns offset 9 into 32777.
            check(metadata.resources[1].slot == 2 &&
                      metadata.resources[1].offset_dwords == 9 &&
                      metadata.resources[1].size_flag,
                  "the flag bit is a flag and not part of the offset");
            check(metadata.resources[0].kind ==
                      ResourceKind::ReadOnlyTexture,
                  "the class is the resource kind");
        }
    }

    // A class with a count but no offsets is a header that does not say
    // what it claims to, and reading on would invent resources at zero.
    {
        Memory broken = memory;
        broken.put16(kUserData + 0x30, 2);   // class 1 count, offsets zero
        const auto broken16 = [&](std::uint64_t address,
                                  std::uint16_t& out) {
            std::uint8_t low = 0;
            std::uint8_t high = 0;
            if (!broken.get(address, low) || !broken.get(address + 1, high)) {
                return false;
            }
            out = static_cast<std::uint16_t>(low | (high << 8));
            return true;
        };
        const auto broken64 = [&](std::uint64_t address,
                                  std::uint64_t& out) {
            out = 0;
            for (int index = 0; index < 8; ++index) {
                std::uint8_t byte = 0;
                if (!broken.get(address + index, byte)) {
                    return false;
                }
                out |= static_cast<std::uint64_t>(byte) << (8 * index);
            }
            return true;
        };
        ShaderMetadata metadata;
        check(!read_shader_metadata(kHeader, broken16, read32, broken64,
                                    metadata),
              "a class with a count and no offsets is refused");
    }

    if (failures == 0) {
        std::printf("all shader metadata checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
