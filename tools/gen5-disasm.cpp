// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Lists a dumped shader instruction by instruction with the native
// decoder: the program counter, the name, and the raw words. Enough to see
// which instruction fills a register the translation did not resolve.
//
//   g++ -std=c++20 -O1 -I runtime/native tools/gen5-disasm.cpp -o disasm
//   disasm stage0_000000050081B500.code
#include <cstdio>
#include <string>
#include <vector>

#include "gen5_cfg.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: gen5-disasm <file.code>\n");
        return 1;
    }
    std::vector<std::uint32_t> words;
    if (auto* file = std::fopen(argv[1], "rb")) {
        std::uint32_t word = 0;
        while (std::fread(&word, 4, 1, file) == 1) {
            words.push_back(word);
        }
        std::fclose(file);
    }
    const auto read = [&](std::uint32_t pc) -> std::uint32_t {
        return pc < words.size() ? words[pc] : 0u;
    };
    std::uint32_t pc = 0;
    while (pc < words.size()) {
        std::uint32_t size = 0;
        const auto decoded = ps5gen5::decode_one(pc, read, size);
        if (size == 0) {
            size = 1;
        }
        std::printf("%04X  %-28s", pc, decoded.name.empty()
                                           ? "?"
                                           : std::string(decoded.name).c_str());
        for (std::uint32_t index = 0; index < size; ++index) {
            std::printf(" %08X", read(pc + index));
        }
        std::printf("\n");
        pc += size;
    }
    return 0;
}
