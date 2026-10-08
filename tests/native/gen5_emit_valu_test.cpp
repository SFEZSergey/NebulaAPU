// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// The vector instructions are almost all of a shader, so the mistakes that
// matter here are the ones that produce plausible wrong numbers: reading
// bits as a float where they are an integer, taking an operand pair the
// wrong way round, or reaching a neighbouring GLSL instruction.
#include <cstdio>
#include <vector>

#include "gen5_emit_valu.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

struct Fixture {
    ps5spirv::ModuleBuilder module;
    ps5gen5::AluTypes types;
    std::uint32_t float_type = 0;
    std::uint32_t glsl = 0;

    Fixture() {
        glsl = module.import_ext_inst("GLSL.std.450");
        types = ps5gen5::declare_alu_types(module);
        float_type = module.type_float(32);
    }

    ps5gen5::VectorEmitter emitter() {
        return ps5gen5::VectorEmitter(module, types, float_type, glsl);
    }

    // Every instruction in the function section, as opcodes.
    std::vector<std::uint32_t> opcodes() {
        const auto words = module.build();
        std::vector<std::uint32_t> found;
        std::size_t index = 5;
        while (index < words.size()) {
            const auto length = words[index] >> 16;
            if (length == 0) {
                break;
            }
            found.push_back(words[index] & 0xFFFFu);
            index += length;
        }
        return found;
    }

    // The GLSL instruction number of the first OpExtInst emitted.
    std::uint32_t first_glsl_op() {
        const auto words = module.build();
        std::size_t index = 5;
        while (index < words.size()) {
            const auto length = words[index] >> 16;
            if (length == 0) {
                break;
            }
            if ((words[index] & 0xFFFFu) == 12u && length >= 5) {
                return words[index + 4];
            }
            index += length;
        }
        return 0;
    }

    bool contains(std::uint32_t opcode) {
        for (const auto found : opcodes()) {
            if (found == opcode) {
                return true;
            }
        }
        return false;
    }
};

int main() {
    using namespace ps5gen5;

    // The table reaches the right operations, and a name it does not know
    // stays unknown rather than becoming something plausible.
    check(vector_form("VAddF32").operation == VectorOperation::AddF32,
          "a float add is a float add");
    check(vector_form("VAddI32").operation == VectorOperation::AddI32,
          "an integer add is a different operation");
    check(vector_form("VNotAnInstruction").operation ==
              VectorOperation::Unknown,
          "an unknown name stays unknown");
    check(vector_form("VRcpF32").source_count == 1,
          "a reciprocal reads one source");
    check(vector_form("VCndmaskB32").source_count == 3,
          "a select reads three");

    // A float operation has to bitcast its operands in and its result out,
    // because the register holds bits. Without the casts SPIR-V would be
    // adding two integers and calling the result a float.
    {
        Fixture f;
        auto emitter = f.emitter();
        emitter.emit(vector_form("VAddF32"), 10, 11, 0, 0);
        check(f.contains(static_cast<std::uint32_t>(ValuSpirvOp::FAdd)),
              "a float add emits a float add");
        check(f.contains(static_cast<std::uint32_t>(ValuSpirvOp::Bitcast)),
              "a float operation casts between bits and floats");
    }

    // An integer operation must not cast: the bits already mean what the
    // instruction says.
    {
        Fixture f;
        auto emitter = f.emitter();
        emitter.emit(vector_form("VAndB32"), 10, 11, 0, 0);
        check(f.contains(
                  static_cast<std::uint32_t>(AluSpirvOp::BitwiseAnd)),
              "a bitwise and emits a bitwise and");
        check(!f.contains(static_cast<std::uint32_t>(ValuSpirvOp::Bitcast)),
              "an integer operation casts nothing");
    }

    // The reversed forms swap their operands. A shift that takes them the
    // wrong way round produces a number rather than an error, which is the
    // worst kind of mistake to make.
    {
        Fixture f;
        auto emitter = f.emitter();
        const auto plain = vector_form("VLshlB32");
        const auto reversed = vector_form("VLshlrevB32");
        check(!plain.reversed && reversed.reversed,
              "only the rev form is reversed");
        check(plain.operation == reversed.operation,
              "both are the same operation");
        (void)emitter;
    }

    // The GLSL instructions are a separate number space, and the
    // neighbours of each are other functions entirely: 31 is sqrt and 32 is
    // inversesqrt, so reaching for the wrong one is a plausible result.
    {
        Fixture f;
        auto emitter = f.emitter();
        emitter.emit(vector_form("VSqrtF32"), 10, 0, 0, 0);
        check(f.first_glsl_op() == static_cast<std::uint32_t>(GlslOp::Sqrt),
              "sqrt reaches sqrt");
    }
    {
        Fixture f;
        auto emitter = f.emitter();
        emitter.emit(vector_form("VRsqF32"), 10, 0, 0, 0);
        check(f.first_glsl_op() ==
                  static_cast<std::uint32_t>(GlslOp::InverseSqrt),
              "rsq reaches inversesqrt and not sqrt");
    }

    // GCN's exp and log are base two, and GLSL has both bases. Choosing the
    // natural ones would be wrong by a constant factor everywhere.
    {
        Fixture f;
        auto emitter = f.emitter();
        emitter.emit(vector_form("VExpF32"), 10, 0, 0, 0);
        check(f.first_glsl_op() == static_cast<std::uint32_t>(GlslOp::Exp2),
              "exp is the base two one");
    }
    {
        Fixture f;
        auto emitter = f.emitter();
        emitter.emit(vector_form("VLogF32"), 10, 0, 0, 0);
        check(f.first_glsl_op() == static_cast<std::uint32_t>(GlslOp::Log2),
              "log is the base two one");
    }

    // The signed and unsigned minimums are different GLSL instructions, and
    // the difference only shows on values above two billion.
    {
        Fixture f;
        auto emitter = f.emitter();
        emitter.emit(vector_form("VMinI32"), 10, 11, 0, 0);
        check(f.first_glsl_op() == static_cast<std::uint32_t>(GlslOp::SMin),
              "the signed minimum is signed");
    }
    {
        Fixture f;
        auto emitter = f.emitter();
        emitter.emit(vector_form("VMinU32"), 10, 11, 0, 0);
        check(f.first_glsl_op() == static_cast<std::uint32_t>(GlslOp::UMin),
              "the unsigned minimum is unsigned");
    }

    // A conversion from an integer must read the bits as an integer, and a
    // conversion to one must produce bits rather than a float.
    {
        Fixture f;
        auto emitter = f.emitter();
        emitter.emit(vector_form("VCvtF32I32"), 10, 0, 0, 0);
        check(f.contains(
                  static_cast<std::uint32_t>(ValuSpirvOp::ConvertSToF)),
              "int to float converts from signed");
    }
    {
        Fixture f;
        auto emitter = f.emitter();
        emitter.emit(vector_form("VCvtU32F32"), 10, 0, 0, 0);
        check(f.contains(
                  static_cast<std::uint32_t>(ValuSpirvOp::ConvertFToU)),
              "float to int converts to unsigned");
    }

    // The normalised packs are GLSL's own: unsigned for u16, signed for
    // i16. Untranslated, the register kept a float's bits.
    {
        Fixture f;
        auto emitter = f.emitter();
        check(emitter.emit(vector_form("VCvtPknormU16F32"), 10, 11, 0, 0) != 0 &&
                  f.first_glsl_op() == 57u,
              "a u16 normalised pack is PackUnorm2x16");
    }
    {
        Fixture f;
        auto emitter = f.emitter();
        check(emitter.emit(vector_form("VCvtPknormI16F32"), 10, 11, 0, 0) != 0 &&
                  f.first_glsl_op() == 56u,
              "an i16 normalised pack is PackSnorm2x16");
    }

    // A move produces no instruction; the bits are already the answer.
    {
        Fixture f;
        auto emitter = f.emitter();
        const auto before = f.opcodes().size();
        const auto moved = emitter.emit(vector_form("VMovB32"), 77, 0, 0, 0);
        check(moved == 77 && f.opcodes().size() == before,
              "a move emits nothing and gives back its source");
    }

    if (failures == 0) {
        std::printf("all vector ALU checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
