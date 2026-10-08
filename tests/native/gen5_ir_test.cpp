// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Checks the ported IR against the rules the C# encodes, using the cases
// that decide behaviour rather than the ones that merely compile.
#include <cstdio>

#include "gen5_shader_ir.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

int main() {
    using namespace ps5gen5;

    // Source encoding: the scalar range is not contiguous, and 249 and 255
    // only mean a literal when one was actually read from the stream.
    check(Operand::source(300).kind == OperandKind::VectorRegister &&
              Operand::source(300).value == 44,
          "encoded 300 is v44");
    check(Operand::source(105).kind == OperandKind::ScalarRegister,
          "encoded 105 is scalar");
    check(Operand::source(106).kind == OperandKind::ScalarRegister,
          "encoded 106 is scalar");
    check(Operand::source(108).kind == OperandKind::EncodedConstant,
          "encoded 108 is a constant");
    check(Operand::source(124).kind == OperandKind::ScalarRegister,
          "encoded 124 is scalar");
    check(Operand::source(127).kind == OperandKind::ScalarRegister,
          "encoded 127 is scalar");
    check(Operand::source(255).kind == OperandKind::EncodedConstant,
          "255 without a literal is not a literal");
    check(Operand::source(255, 0xDEADBEEFu).kind ==
                  OperandKind::LiteralConstant &&
              Operand::source(255, 0xDEADBEEFu).value == 0xDEADBEEFu,
          "255 with a literal carries it");
    check(Operand::source(249, 7).kind == OperandKind::LiteralConstant,
          "249 with a literal carries it");

    // Export masks: four bits per target, and targets at or above eight are
    // not colour.
    ShaderProgram program;
    ShaderInstruction first;
    first.control = ExportControl{0, 0xF, false, false, false};
    ShaderInstruction second;
    second.control = ExportControl{2, 0x3, false, false, false};
    ShaderInstruction beyond;
    beyond.control = ExportControl{8, 0xF, false, false, false};
    ShaderInstruction unrelated;
    unrelated.control = Vop3Control{};
    program.instructions = {first, second, beyond, unrelated};
    const auto masks = program.pixel_color_export_masks();
    check(masks == (0xFu | (0x3u << 8)),
          "export masks pack four bits per target and ignore target 8");

    // System registers: named ones are cleared from a static evaluation,
    // others are left alone.
    ComputeSystemRegisters system;
    system.work_group_x = 2;
    system.thread_group_size = 5;
    std::vector<std::uint32_t> scalars(8, 0x11111111u);
    system.clear_static_values(scalars);
    check(scalars[2] == 0 && scalars[5] == 0,
          "named system registers are cleared");
    check(scalars[3] == 0x11111111u, "other registers are untouched");
    check(system.is_system_register(2) && !system.is_system_register(3),
          "system registers are recognised");

    // An out of range register must not write past the end.
    ComputeSystemRegisters beyond_end;
    beyond_end.work_group_y = 99;
    beyond_end.clear_static_values(scalars);
    check(scalars.size() == 8, "clearing does not grow the register file");

    // Image address registers fall back to consecutive ones past the list.
    ImageControl image;
    image.vector_address = 10;
    image.address_registers = {4, 5};
    check(image.address_register(0) == 4 && image.address_register(1) == 5,
          "listed address registers are used");
    check(image.address_register(2) == 12,
          "past the list the address register is consecutive");

    if (failures == 0) {
        std::printf("all IR checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
