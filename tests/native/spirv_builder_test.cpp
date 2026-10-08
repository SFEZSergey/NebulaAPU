// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Builds a minimal compute module with the native builder and writes it
// out, so the words can be walked by the same parser used on the shaders
// the C# translator produces.
#include <cstdio>
#include <vector>

#include "spirv_builder.h"

int main(int argc, char** argv) {
    ps5spirv::ModuleBuilder module;
    module.add_capability(ps5spirv::Capability::Shader);
    const auto glsl = module.import_ext_inst("GLSL.std.450");
    (void)glsl;
    module.set_logical_glsl450_memory_model();

    const auto void_type = module.type_void();
    const auto uint_type = module.type_int(32, false);
    const auto function_type = module.type_function(void_type);
    const auto uint_pointer =
        module.type_pointer(ps5spirv::StorageClass::Private, uint_type);

    // Deduping has to hold: asking twice must not emit twice.
    if (module.type_int(32, false) != uint_type ||
        module.type_function(void_type) != function_type ||
        module.type_pointer(ps5spirv::StorageClass::Private, uint_type) !=
            uint_pointer) {
        std::printf("dedupe failed\n");
        return 1;
    }
    const auto four = module.constant(uint_type, 4);
    if (module.constant(uint_type, 4) != four) {
        std::printf("constant dedupe failed\n");
        return 1;
    }

    const auto counter = module.allocate_id();
    module.add_global(
        ps5spirv::Op::Variable,
        {uint_pointer, counter,
         static_cast<std::uint32_t>(ps5spirv::StorageClass::Private)});
    module.add_name(counter, "counter");

    const auto entry = module.allocate_id();
    module.add_entry_point(
        ps5spirv::ExecutionModel::GLCompute, entry, "main", {counter});
    module.add_execution_mode(
        entry, ps5spirv::ExecutionMode::LocalSize, {64, 1, 1});

    module.add_function_word(
        ps5spirv::Op::Function, {void_type, entry, 0, function_type});
    module.add_function_word(
        ps5spirv::Op::Label, {module.allocate_id()});
    module.add_function_word(ps5spirv::Op::Store, {counter, four});
    module.add_function_word(ps5spirv::Op::Return, {});
    module.add_function_word(ps5spirv::Op::FunctionEnd, {});

    const auto words = module.build();
    std::printf("words=%zu bound=%u\n", words.size(), words[3]);
    if (words[0] != 0x07230203u) {
        std::printf("bad magic\n");
        return 1;
    }
    // Walk it the way a consumer would: every instruction's length must
    // land exactly on the end.
    std::size_t index = 5;
    std::size_t instructions = 0;
    while (index < words.size()) {
        const auto length = words[index] >> 16;
        if (length == 0) {
            std::printf("zero length instruction at %zu\n", index);
            return 1;
        }
        index += length;
        ++instructions;
    }
    if (index != words.size()) {
        std::printf("stream overruns by %zu words\n", index - words.size());
        return 1;
    }
    std::printf("instructions=%zu, stream is well formed\n", instructions);

    if (argc > 1) {
        std::FILE* out = std::fopen(argv[1], "wb");
        if (out != nullptr) {
            std::fwrite(
                words.data(), sizeof(std::uint32_t), words.size(), out);
            std::fclose(out);
        }
    }
    return 0;
}
