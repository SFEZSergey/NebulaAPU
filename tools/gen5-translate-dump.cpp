// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Translates a dumped stage offline - the .code and .userdata the runtime
// writes under PS5RT_NATIVE_SHADER_DUMP - and writes the SPIR-V, so a
// module can be put through spirv-val without running the title. Guest
// memory is not in the dump, so every read the walk makes fails; the
// module differs from the live one in what it binds, not in its structure.
//
//   g++ -std=c++20 -O1 -I runtime/native tools/gen5-translate-dump.cpp
//   translate-dump stage1_X.code stage1_X.userdata out.spv [vertex|pixel|compute] [code_base]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gen5_translate.h"

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
    if (argc < 4) {
        std::printf("usage: translate-dump code userdata out.spv [stage] [code_base]\n");
        return 1;
    }
    const auto code = load(argv[1]);
    const auto user_data = load(argv[2]);
    const std::string stage_name = argc > 4 ? argv[4] : "pixel";
    const auto stage = stage_name == "vertex" ? ps5gen5::StageKind::Vertex
        : stage_name == "compute"             ? ps5gen5::StageKind::Compute
                                              : ps5gen5::StageKind::Pixel;
    const std::uint64_t code_base =
        argc > 5 ? std::strtoull(argv[5], nullptr, 16) : 0x500000000ull;
    const auto read = [&](std::uint32_t pc) -> std::uint32_t {
        return pc < code.size() ? code[pc] : 0u;
    };
    const auto memory = [](std::uint64_t, std::uint32_t&) { return false; };
    const auto result = ps5gen5::translate_shader(
        0, read, user_data,
        stage == ps5gen5::StageKind::Vertex ? 8u : 0u,
        memory, code_base, false, false, false, stage);
    if (result.words.empty()) {
        std::printf("no module (cfg=%d plan=%d)\n",
                    result.cfg_decoded ? 1 : 0, result.plan_complete ? 1 : 0);
        return 1;
    }
    if (auto* file = std::fopen(argv[3], "wb")) {
        std::fwrite(result.words.data(), 4, result.words.size(), file);
        std::fclose(file);
    }
    std::printf("words=%zu translated=%u skipped=%u\n",
                result.words.size(), result.instructions_translated,
                result.instructions_skipped);
    return 0;
}
