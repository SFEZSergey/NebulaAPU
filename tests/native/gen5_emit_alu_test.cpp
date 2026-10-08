// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Arithmetic on top of the register bank. What matters here is that the
// right SPIR-V operation comes out for each guest instruction, that signed
// and unsigned forms of the same operation are told apart, and that a
// compare goes to the condition rather than to a register - the three
// mistakes that produce a shader which runs and is wrong.
#include <cstdio>
#include <vector>

#include "gen5_emit_alu.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

// The opcode of the last instruction emitted into the function section.
static std::uint32_t last_opcode(
    const std::vector<std::uint32_t>& words) {
    std::size_t index = 5;
    std::uint32_t last = 0;
    while (index < words.size()) {
        const auto length = words[index] >> 16;
        if (length == 0) {
            break;
        }
        last = words[index] & 0xFFFFu;
        index += length;
    }
    return last;
}

int main() {
    using namespace ps5gen5;

    // The table: each guest name reaches the operation it means, and a name
    // that is not known says so rather than guessing.
    check(scalar_form("SAddU32").operation == AluOperation::Add,
          "an add is an add");
    check(!scalar_form("SAddU32").is_signed &&
              scalar_form("SAddI32").is_signed,
          "the signed and unsigned adds are told apart");
    check(scalar_form("SCmpLtI32").writes_condition,
          "a compare writes a condition");
    check(!scalar_form("SAndB32").writes_condition,
          "an and does not");
    check(scalar_form("SNotAnInstruction").operation ==
              AluOperation::Unknown,
          "an unknown name is unknown, not something else");

    // Signed and unsigned compares must reach different SPIR-V
    // instructions. Emitting the unsigned one for a signed compare is the
    // kind of mistake that only shows on negative values.
    {
        ps5spirv::ModuleBuilder module;
        const auto types = declare_alu_types(module);
        AluEmitter emitter(module, types);
        std::uint32_t condition = 0;
        emitter.emit(scalar_form("SCmpLtI32"), 10, 11, &condition);
        check(last_opcode(module.build()) ==
                  static_cast<std::uint32_t>(AluSpirvOp::SLessThan),
              "a signed compare emits the signed instruction");
    }
    {
        ps5spirv::ModuleBuilder module;
        const auto types = declare_alu_types(module);
        AluEmitter emitter(module, types);
        std::uint32_t condition = 0;
        emitter.emit(scalar_form("SCmpLtU32"), 10, 11, &condition);
        check(last_opcode(module.build()) ==
                  static_cast<std::uint32_t>(AluSpirvOp::ULessThan),
              "an unsigned compare emits the unsigned instruction");
    }

    // The two shifts right are different instructions, and choosing wrongly
    // is invisible until a value has its top bit set.
    {
        ps5spirv::ModuleBuilder module;
        const auto types = declare_alu_types(module);
        AluEmitter emitter(module, types);
        emitter.emit(scalar_form("SLshrB32"), 1, 2, nullptr);
        check(last_opcode(module.build()) ==
                  static_cast<std::uint32_t>(AluSpirvOp::ShiftRightLogical),
              "a logical shift right is logical");
    }
    {
        ps5spirv::ModuleBuilder module;
        const auto types = declare_alu_types(module);
        AluEmitter emitter(module, types);
        emitter.emit(scalar_form("SAshrI32"), 1, 2, nullptr);
        check(last_opcode(module.build()) ==
                  static_cast<std::uint32_t>(
                      AluSpirvOp::ShiftRightArithmetic),
              "an arithmetic shift right is arithmetic");
    }

    // A compare returns nothing to write to a register, and hands back the
    // condition instead. A translator that wrote the returned value into
    // the destination would store a zero over a live register.
    {
        ps5spirv::ModuleBuilder module;
        const auto types = declare_alu_types(module);
        AluEmitter emitter(module, types);
        std::uint32_t condition = 0;
        const auto written =
            emitter.emit(scalar_form("SCmpEqU32"), 3, 4, &condition);
        check(written == 0, "a compare writes no register");
        check(condition != 0, "a compare produces a condition");
    }

    // A move produces no instruction at all: the value is already there.
    {
        ps5spirv::ModuleBuilder module;
        const auto types = declare_alu_types(module);
        AluEmitter emitter(module, types);
        const auto before = module.build().size();
        const auto moved = emitter.emit(scalar_form("SMovB32"), 99, 0, nullptr);
        const auto after = module.build().size();
        check(moved == 99, "a move gives back its source");
        check(before == after, "a move emits nothing");
    }

    // Arithmetic chains through values rather than memory: the result of
    // one instruction is the operand of the next, with no load between.
    {
        ps5spirv::ModuleBuilder module;
        const auto types = declare_alu_types(module);
        AluEmitter emitter(module, types);
        const auto sum = emitter.emit(scalar_form("SAddU32"), 1, 2, nullptr);
        const auto product =
            emitter.emit(scalar_form("SMulI32"), sum, 3, nullptr);
        check(sum != 0 && product != 0 && sum != product,
              "each instruction produces its own value");
        const auto words = module.build();
        std::size_t loads = 0;
        std::size_t index = 5;
        while (index < words.size()) {
            const auto length = words[index] >> 16;
            if (length == 0) {
                break;
            }
            if ((words[index] & 0xFFFFu) ==
                static_cast<std::uint32_t>(ps5spirv::Op::Load)) {
                ++loads;
            }
            index += length;
        }
        check(loads == 0, "a chain of arithmetic touches no memory");
    }

    if (failures == 0) {
        std::printf("all ALU checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
