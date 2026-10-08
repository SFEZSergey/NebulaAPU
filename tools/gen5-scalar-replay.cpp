// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Replays the scalar walk over a dumped stage (code + user data) offline,
// and says for every buffer instruction which of its four descriptor
// registers the walk knew. Guest memory is not in the dump, so every
// memory read fails - which is fine for descriptors built in registers.
//
//   g++ -std=c++20 -O1 -I runtime/native tools/gen5-scalar-replay.cpp
//   replay stage0_X.code stage0_X.userdata [code_base_hex]
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "gen5_scalar_eval.h"

static std::vector<std::uint32_t> load(const char* path) {
    std::vector<std::uint32_t> words;
    if (auto* file = std::fopen(path, "rb")) {
        std::uint32_t word = 0;
        while (std::fread(&word, 4, 1, file) == 1) {
            words.push_back(word);
        }
        std::fclose(file);
    }
    return words;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        return 1;
    }
    const auto code = load(argv[1]);
    const auto user_data = load(argv[2]);
    const std::uint64_t code_base =
        argc > 3 ? std::strtoull(argv[3], nullptr, 16) : 0x500000000ull;
    const auto read = [&](std::uint32_t pc) -> std::uint32_t {
        return pc < code.size() ? code[pc] : 0u;
    };
    const auto memory = [](std::uint64_t, std::uint32_t&) { return false; };
    const auto result = ps5gen5::evaluate_scalar(
        0, user_data, 8, read, memory, code_base);
    for (const auto& buffer : result.buffers) {
        std::printf("buffer pc=0x%X s%u known=%d words=%08X %08X %08X %08X\n",
                    buffer.pc, buffer.resource_register,
                    buffer.descriptor_known ? 1 : 0, buffer.descriptor[0],
                    buffer.descriptor[1], buffer.descriptor[2],
                    buffer.descriptor[3]);
    }
    for (const auto& [name, count] : result.unknown_by_name) {
        std::printf("unknown %s %u\n", name.c_str(), count);
    }
    return 0;
}
