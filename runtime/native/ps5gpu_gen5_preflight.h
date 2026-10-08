// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps5gpu::gen5 {

constexpr std::uint32_t kMaximumInstructions = 4096;

enum class Encoding : std::uint32_t {
    Sop1,
    Sop2,
    Sopc,
    Sopp,
    Sopk,
    Smrd,
    Smem,
    Mubuf,
    Mtbuf,
    Vop1,
    Vop2,
    Vopc,
    Vop3,
    Vintrp,
    Ds,
    Flat,
    Vop3p,
    Mimg,
    Exp,
    Count,
};

constexpr std::array<const char*, static_cast<std::size_t>(Encoding::Count)>
    kEncodingNames = {
        "sop1",
        "sop2",
        "sopc",
        "sopp",
        "sopk",
        "smrd",
        "smem",
        "mubuf",
        "mtbuf",
        "vop1",
        "vop2",
        "vopc",
        "vop3",
        "vintrp",
        "ds",
        "flat",
        "vop3p",
        "mimg",
        "exp",
    };

using ReadMemory = bool (*)(
    std::uint64_t address,
    void* destination,
    std::size_t size,
    void* user);

struct PreflightResult {
    std::vector<std::uint32_t> words;
    std::array<std::uint32_t, static_cast<std::size_t>(Encoding::Count)>
        encoding_counts = {};
    std::uint32_t instruction_count = 0;
    std::uint32_t failure_pc = 0;
    std::uint32_t failure_word = 0;
    bool terminated = false;
    bool read_failed = false;
    bool decode_failed = false;
};

struct InstructionInfo {
    Encoding encoding = Encoding::Vop2;
    std::uint32_t size_dwords = 1;
    bool end_program = false;
};

inline bool is_vector_literal(std::uint32_t source) {
    return source == 0xE9 ||
        source == 0xEA ||
        source == 0xF9 ||
        source == 0xFA ||
        source == 0xFF;
}

inline bool read_u32(
    ReadMemory read_memory,
    void* user,
    std::uint64_t address,
    std::uint32_t& value) {
    return read_memory != nullptr &&
        read_memory(address, &value, sizeof(value), user);
}

inline bool decode_instruction(
    ReadMemory read_memory,
    void* user,
    std::uint64_t address,
    std::uint32_t word,
    InstructionInfo& info) {
    info = {};
    if ((word & 0x80000000u) == 0) {
        const auto opcode = (word >> 25) & 0x3Fu;
        const auto source = word & 0x1FFu;
        if (opcode == 0x3E) {
            info.encoding = Encoding::Vopc;
            info.size_dwords = is_vector_literal(source) ? 2u : 1u;
        } else if (opcode == 0x3F) {
            info.encoding = Encoding::Vop1;
            info.size_dwords = is_vector_literal(source) ? 2u : 1u;
        } else {
            info.encoding = Encoding::Vop2;
            info.size_dwords =
                opcode == 0x20 ||
                opcode == 0x21 ||
                opcode == 0x2C ||
                opcode == 0x2D ||
                is_vector_literal(source)
                    ? 2u
                    : 1u;
        }
        return true;
    }

    if ((word & 0xF8000000u) == 0xC0000000u) {
        info.encoding = Encoding::Smrd;
        const auto offset = word & 0xFFu;
        const bool immediate_offset = ((word >> 8) & 1u) != 0;
        info.size_dwords =
            !immediate_offset && offset == 0xFFu ? 2u : 1u;
        return true;
    }

    if ((word & 0xC0000000u) == 0x80000000u) {
        const auto opcode = (word >> 23) & 0x7Fu;
        if (opcode == 0x7D) {
            info.encoding = Encoding::Sop1;
            info.size_dwords = (word & 0xFFu) == 0xFFu ? 2u : 1u;
        } else if (opcode == 0x7E) {
            info.encoding = Encoding::Sopc;
            info.size_dwords =
                (word & 0xFFu) == 0xFFu ||
                ((word >> 8) & 0xFFu) == 0xFFu
                    ? 2u
                    : 1u;
        } else if (opcode == 0x7F) {
            info.encoding = Encoding::Sopp;
            info.size_dwords = 1;
            info.end_program = ((word >> 16) & 0x7Fu) == 1u;
        } else if (opcode >= 0x60) {
            info.encoding = Encoding::Sopk;
            info.size_dwords = 1;
        } else {
            info.encoding = Encoding::Sop2;
            info.size_dwords =
                (word & 0xFFu) == 0xFFu ||
                ((word >> 8) & 0xFFu) == 0xFFu
                    ? 2u
                    : 1u;
        }
        return true;
    }

    if ((word & 0xFF800000u) == 0xCC000000u) {
        std::uint32_t extra = 0;
        if (!read_u32(read_memory, user, address + 4, extra)) {
            return false;
        }
        info.encoding = Encoding::Vop3p;
        const auto source0 = extra & 0x1FFu;
        const auto source1 = (extra >> 9) & 0x1FFu;
        const auto source2 = (extra >> 18) & 0x1FFu;
        info.size_dwords =
            source0 == 0xFFu ||
            source1 == 0xFFu ||
            source2 == 0xFFu
                ? 3u
                : 2u;
        return true;
    }

    switch (word >> 26) {
    case 0x32:
        info.encoding = Encoding::Vintrp;
        info.size_dwords = 1;
        return true;
    case 0x33:
    case 0x3D:
        info.encoding = Encoding::Smem;
        info.size_dwords = 2;
        return true;
    case 0x34:
    case 0x35:
    case 0x38:
    case 0x3A: {
        std::uint32_t extra = 0;
        if (!read_u32(read_memory, user, address + 4, extra)) {
            return false;
        }
        if ((word >> 26) == 0x34 || (word >> 26) == 0x35) {
            info.encoding = Encoding::Vop3;
            const auto source0 = extra & 0x1FFu;
            const auto source1 = (extra >> 9) & 0x1FFu;
            const auto source2 = (extra >> 18) & 0x1FFu;
            info.size_dwords =
                source0 == 0xFFu ||
                source1 == 0xFFu ||
                source2 == 0xFFu
                    ? 3u
                    : 2u;
        } else {
            info.encoding =
                (word >> 26) == 0x38
                    ? Encoding::Mubuf
                    : Encoding::Mtbuf;
            info.size_dwords = (extra >> 24) == 0xFFu ? 3u : 2u;
        }
        return true;
    }
    case 0x36:
        info.encoding = Encoding::Ds;
        info.size_dwords = 2;
        return true;
    case 0x37:
        info.encoding = Encoding::Flat;
        info.size_dwords = 2;
        return true;
    case 0x3C:
        info.encoding = Encoding::Mimg;
        info.size_dwords = 2u + ((word >> 1) & 0x3u);
        return true;
    case 0x3E:
        info.encoding = Encoding::Exp;
        info.size_dwords = 2;
        return true;
    case 0x3F:
        info.encoding = Encoding::Vop3p;
        info.size_dwords = 2;
        return true;
    default:
        return false;
    }
}

inline PreflightResult decode_program(
    std::uint64_t address,
    ReadMemory read_memory,
    void* user) {
    PreflightResult result;
    if (address == 0 || read_memory == nullptr) {
        result.read_failed = true;
        return result;
    }

    for (std::uint32_t instruction = 0, pc = 0;
         instruction < kMaximumInstructions;
         ++instruction) {
        std::uint32_t word = 0;
        if (!read_u32(read_memory, user, address + pc, word)) {
            result.failure_pc = pc;
            result.read_failed = true;
            break;
        }

        InstructionInfo info;
        if (!decode_instruction(
                read_memory,
                user,
                address + pc,
                word,
                info)) {
            result.failure_pc = pc;
            result.failure_word = word;
            result.decode_failed = true;
            break;
        }

        for (std::uint32_t index = 0; index < info.size_dwords; ++index) {
            std::uint32_t instruction_word = word;
            if (index != 0 &&
                !read_u32(
                    read_memory,
                    user,
                    address + pc + index * sizeof(std::uint32_t),
                    instruction_word)) {
                result.failure_pc =
                    pc + index * sizeof(std::uint32_t);
                result.read_failed = true;
                return result;
            }
            result.words.push_back(instruction_word);
        }

        ++result.instruction_count;
        ++result.encoding_counts[static_cast<std::size_t>(info.encoding)];
        pc += info.size_dwords * sizeof(std::uint32_t);
        if (info.end_program) {
            result.terminated = true;
            break;
        }
    }

    if (!result.terminated &&
        !result.read_failed &&
        !result.decode_failed) {
        result.failure_pc = static_cast<std::uint32_t>(
            result.words.size() * sizeof(std::uint32_t));
        result.decode_failed = true;
    }
    return result;
}

inline std::uint64_t hash_words(
    const std::vector<std::uint32_t>& words) {
    constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset_basis;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(
        words.data());
    for (std::size_t index = 0;
         index < words.size() * sizeof(std::uint32_t);
         ++index) {
        hash ^= bytes[index];
        hash *= prime;
    }
    return hash;
}

} // namespace ps5gpu::gen5
