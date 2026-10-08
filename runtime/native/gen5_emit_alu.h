// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest arithmetic as SPIR-V, on top of the register bank.
//
// The scalar instructions first, because they are where a shader's control
// flow is decided: the compares that set SCC, the bit work that builds the
// exec mask, the adds that walk a buffer. Everything here reads its
// operands through the bank and writes its result back to it, so a chain of
// arithmetic inside a block becomes a chain of values with no memory
// between them - which is the difference this port is for.
//
// Two things are deliberately not hidden. GCN's 32-bit integer operations
// are untyped: the same bits are signed or unsigned depending on the
// instruction, so the emitter converts where SPIR-V insists on knowing and
// not otherwise. And an instruction that sets SCC sets it as a side effect
// rather than as a destination, which is why the scalar condition code is
// written through its own path instead of appearing among the operands.

#ifndef PS5_GEN5_EMIT_ALU_H
#define PS5_GEN5_EMIT_ALU_H

#include <cstdint>
#include <string_view>

#include "gen5_decoder_sop.h"
#include "gen5_registers.h"
#include "spirv_builder.h"

namespace ps5gen5 {

// The types an emitter needs to hand SPIR-V, gathered once so every
// instruction does not look them up again.
struct AluTypes {
    std::uint32_t uint_type = 0;
    std::uint32_t int_type = 0;
    std::uint32_t bool_type = 0;
};

inline AluTypes declare_alu_types(ps5spirv::ModuleBuilder& module) {
    AluTypes types;
    types.uint_type = module.type_int(32, false);
    types.int_type = module.type_int(32, true);
    types.bool_type = module.type_bool();
    return types;
}

// What an instruction does, once its name has been decoded. Kept apart from
// the emission so the mapping can be read as a table.
enum class AluOperation {
    Unknown,
    Move,
    Not,
    Add,
    Subtract,
    Multiply,
    And,
    Or,
    Xor,
    ShiftLeft,
    ShiftRightLogical,
    ShiftRightArithmetic,
    CompareEqual,
    CompareNotEqual,
    CompareLessSigned,
    CompareLessUnsigned,
    CompareGreaterSigned,
    CompareGreaterUnsigned,
    CompareLessEqualSigned,
    CompareLessEqualUnsigned,
    CompareGreaterEqualSigned,
    CompareGreaterEqualUnsigned,
    MinUnsigned,
    MaxUnsigned,
    MinSigned,
    MaxSigned,
    Nor,
    Nand,
    Xnor,
    OrNot,
    AndNot,
};

struct AluForm {
    AluOperation operation = AluOperation::Unknown;
    // Whether the operands are read as signed. GCN says so in the name -
    // SMinI32 against SMinU32 - and SPIR-V needs to know for comparisons
    // and arithmetic shifts.
    bool is_signed = false;
    // Whether the result is a condition rather than a value, which goes to
    // SCC instead of a register.
    bool writes_condition = false;
    // Whether the instruction works on a register pair. GCN's 64-bit
    // scalar operations are the exec mask's arithmetic, and a wave's
    // control flow is built out of them: 560 of the instructions still
    // missing are these. Two 32-bit operations on consecutive registers
    // say the same thing, because none of the ones here carry between
    // halves.
    bool is_pair = false;
};

// The scalar instructions this emitter knows. A name it does not know
// returns Unknown, and the caller decides whether that is fatal - during
// the port it is better to leave a gap visible than to guess an operation.
inline AluForm scalar_form_uncached(std::string_view name) {
    const auto binary = [](AluOperation op, bool sign = false) {
        return AluForm{op, sign, false};
    };
    const auto compare = [](AluOperation op, bool sign) {
        return AluForm{op, sign, true};
    };
    if (name == "SMovB32") return AluForm{AluOperation::Move, false, false};
    if (name == "SNotB32") return AluForm{AluOperation::Not, false, false};
    if (name == "SAddU32") return binary(AluOperation::Add);
    if (name == "SAddI32") return binary(AluOperation::Add, true);
    if (name == "SSubU32") return binary(AluOperation::Subtract);
    if (name == "SSubI32") return binary(AluOperation::Subtract, true);
    if (name == "SMulI32") return binary(AluOperation::Multiply, true);
    if (name == "SAndB32") return binary(AluOperation::And);
    if (name == "SOrB32") return binary(AluOperation::Or);
    if (name == "SXorB32") return binary(AluOperation::Xor);
    if (name == "SLshlB32") return binary(AluOperation::ShiftLeft);
    if (name == "SLshrB32") return binary(AluOperation::ShiftRightLogical);
    if (name == "SAshrI32")
        return binary(AluOperation::ShiftRightArithmetic, true);
    if (name == "SCmpEqI32") return compare(AluOperation::CompareEqual, true);
    if (name == "SCmpEqU32") return compare(AluOperation::CompareEqual, false);
    if (name == "SCmpLgI32")
        return compare(AluOperation::CompareNotEqual, true);
    if (name == "SCmpLgU32")
        return compare(AluOperation::CompareNotEqual, false);
    if (name == "SCmpLtI32")
        return compare(AluOperation::CompareLessSigned, true);
    if (name == "SCmpLtU32")
        return compare(AluOperation::CompareLessUnsigned, false);
    if (name == "SCmpGtI32")
        return compare(AluOperation::CompareGreaterSigned, true);
    if (name == "SCmpGtU32")
        return compare(AluOperation::CompareGreaterUnsigned, false);
    if (name == "SCmpLeI32")
        return compare(AluOperation::CompareLessEqualSigned, true);
    if (name == "SCmpLeU32")
        return compare(AluOperation::CompareLessEqualUnsigned, false);
    if (name == "SCmpGeI32")
        return compare(AluOperation::CompareGreaterEqualSigned, true);
    if (name == "SCmpGeU32")
        return compare(AluOperation::CompareGreaterEqualUnsigned, false);
    if (name == "SMinU32") return binary(AluOperation::MinUnsigned);
    if (name == "SMaxU32") return binary(AluOperation::MaxUnsigned);
    if (name == "SMinI32") return binary(AluOperation::MinSigned, true);
    if (name == "SMaxI32") return binary(AluOperation::MaxSigned, true);
    // The 64-bit forms. Each is its 32-bit operation done twice, on the
    // register named and the one after it. SaveExec additionally keeps the
    // old mask in the destination, which falls out of doing the work in
    // that order.
    // Whole quad mode expands a mask so that a quad is live if any lane
    // in it is. With no lane dimension there is one lane and the mask is
    // itself - which is the honest reading here, and better than leaving
    // the destination untouched.
    if (name == "SWqmB32") return AluForm{AluOperation::Move, false, false};
    if (name == "SWqmB64")
        return AluForm{AluOperation::Move, false, false, true};
    if (name == "SMovB64")
        return AluForm{AluOperation::Move, false, false, true};
    if (name == "SNotB64")
        return AluForm{AluOperation::Not, false, false, true};
    if (name == "SAndB64" || name == "SAndSaveexecB64")
        return AluForm{AluOperation::And, false, false, true};
    if (name == "SOrB64" || name == "SOrSaveexecB64")
        return AluForm{AluOperation::Or, false, false, true};
    if (name == "SXorB64" || name == "SXorSaveexecB64")
        return AluForm{AluOperation::Xor, false, false, true};
    // And-not, not and. The entry said And, which is the operation
    // without its defining half.
    if (name == "SAndn2B64" || name == "SAndn2SaveexecB64")
        return AluForm{AluOperation::AndNot, false, false, true};
    if (name == "SAndn1B64" || name == "SAndn1SaveexecB64")
        return AluForm{AluOperation::AndNot, false, false, true};
    if (name == "SNorB64")
        return AluForm{AluOperation::Nor, false, false, true};
    if (name == "SNandB64")
        return AluForm{AluOperation::Nand, false, false, true};
    if (name == "SXnorB64")
        return AluForm{AluOperation::Xnor, false, false, true};
    if (name == "SOrn2B64")
        return AluForm{AluOperation::OrNot, false, false, true};
    if (name == "SNorB32") return binary(AluOperation::Nor);
    if (name == "SNandB32") return binary(AluOperation::Nand);
    if (name == "SXnorB32") return binary(AluOperation::Xnor);
    if (name == "SOrn2B32") return binary(AluOperation::OrNot);
    if (name == "SAndn2B32") return binary(AluOperation::AndNot);
    return AluForm{};
}

inline AluForm scalar_form(std::string_view name) {
    return memo_by_name<AluForm>(name, [](std::string_view n) {
        return scalar_form_uncached(n);
    });
}

// SPIR-V opcodes that are not in the builder's enum because nothing else
// needed them yet. Written as their numbers with the name beside them, the
// way the specification lists them.
enum class AluSpirvOp : std::uint16_t {
    IAdd = 128,
    ISub = 130,
    IMul = 132,
    ShiftRightLogical = 194,
    ShiftRightArithmetic = 195,
    ShiftLeftLogical = 196,
    BitwiseOr = 197,
    BitwiseXor = 198,
    BitwiseAnd = 199,
    Not = 200,
    IEqual = 170,
    INotEqual = 171,
    UGreaterThan = 172,
    SGreaterThan = 173,
    UGreaterThanEqual = 174,
    SGreaterThanEqual = 175,
    ULessThan = 176,
    SLessThan = 177,
    ULessThanEqual = 178,
    SLessThanEqual = 179,
    LogicalAnd = 167,
    LogicalNot = 168,
};

class AluEmitter {
public:
    AluEmitter(ps5spirv::ModuleBuilder& module, const AluTypes& types)
        : module_(module), types_(types) {}

    // A 64-bit compare. Equality is both halves equal and inequality is
    // the negation of that, which is not what repeating a 32-bit compare on
    // the high half gives - so unlike the other paired forms this cannot be
    // a table entry.
    std::uint32_t emit_pair_equality(
        bool equal,
        std::uint32_t low0,
        std::uint32_t high0,
        std::uint32_t low1,
        std::uint32_t high1) {
        std::uint32_t low_equal = 0;
        std::uint32_t high_equal = 0;
        condition(AluSpirvOp::IEqual, low0, low1, &low_equal);
        condition(AluSpirvOp::IEqual, high0, high1, &high_equal);
        const auto both = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(AluSpirvOp::LogicalAnd),
            {types_.bool_type, both, low_equal, high_equal});
        if (equal) {
            return both;
        }
        const auto negated = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(AluSpirvOp::LogicalNot),
            {types_.bool_type, negated, both});
        return negated;
    }

    // Emits one instruction. Returns the value written, or zero when the
    // instruction produced a condition or was not understood.
    std::uint32_t emit(
        const AluForm& form,
        std::uint32_t source0,
        std::uint32_t source1,
        std::uint32_t* condition_out) {
        switch (form.operation) {
            case AluOperation::Move:
                return source0;
            case AluOperation::Not:
                return unary(AluSpirvOp::Not, source0);
            case AluOperation::Add:
                return binary(AluSpirvOp::IAdd, source0, source1);
            case AluOperation::Subtract:
                return binary(AluSpirvOp::ISub, source0, source1);
            case AluOperation::Multiply:
                return binary(AluSpirvOp::IMul, source0, source1);
            case AluOperation::And:
                return binary(AluSpirvOp::BitwiseAnd, source0, source1);
            case AluOperation::Or:
                return binary(AluSpirvOp::BitwiseOr, source0, source1);
            case AluOperation::Xor:
                return binary(AluSpirvOp::BitwiseXor, source0, source1);
            case AluOperation::ShiftLeft:
                return binary(AluSpirvOp::ShiftLeftLogical, source0, source1);
            case AluOperation::ShiftRightLogical:
                return binary(
                    AluSpirvOp::ShiftRightLogical, source0, source1);
            case AluOperation::ShiftRightArithmetic:
                return binary(
                    AluSpirvOp::ShiftRightArithmetic, source0, source1);
            case AluOperation::CompareEqual:
                return condition(
                    AluSpirvOp::IEqual, source0, source1, condition_out);
            case AluOperation::CompareNotEqual:
                return condition(
                    AluSpirvOp::INotEqual, source0, source1, condition_out);
            case AluOperation::CompareLessSigned:
                return condition(
                    AluSpirvOp::SLessThan, source0, source1, condition_out);
            case AluOperation::CompareLessUnsigned:
                return condition(
                    AluSpirvOp::ULessThan, source0, source1, condition_out);
            case AluOperation::CompareGreaterSigned:
                return condition(
                    AluSpirvOp::SGreaterThan, source0, source1,
                    condition_out);
            case AluOperation::CompareGreaterUnsigned:
                return condition(
                    AluSpirvOp::UGreaterThan, source0, source1,
                    condition_out);
            case AluOperation::CompareLessEqualSigned:
                return condition(
                    AluSpirvOp::SLessThanEqual, source0, source1,
                    condition_out);
            case AluOperation::CompareLessEqualUnsigned:
                return condition(
                    AluSpirvOp::ULessThanEqual, source0, source1,
                    condition_out);
            case AluOperation::CompareGreaterEqualSigned:
                return condition(
                    AluSpirvOp::SGreaterThanEqual, source0, source1,
                    condition_out);
            case AluOperation::CompareGreaterEqualUnsigned:
                return condition(
                    AluSpirvOp::UGreaterThanEqual, source0, source1,
                    condition_out);
            case AluOperation::MinUnsigned:
                return choose(
                    AluSpirvOp::ULessThan, source0, source1);
            case AluOperation::MaxUnsigned:
                return choose(
                    AluSpirvOp::UGreaterThan, source0, source1);
            case AluOperation::MinSigned:
                return choose(
                    AluSpirvOp::SLessThan, source0, source1);
            case AluOperation::MaxSigned:
                return choose(
                    AluSpirvOp::SGreaterThan, source0, source1);
            case AluOperation::Nor:
                return unary(
                    AluSpirvOp::Not,
                    binary(AluSpirvOp::BitwiseOr, source0, source1));
            case AluOperation::Nand:
                return unary(
                    AluSpirvOp::Not,
                    binary(AluSpirvOp::BitwiseAnd, source0, source1));
            case AluOperation::Xnor:
                return unary(
                    AluSpirvOp::Not,
                    binary(AluSpirvOp::BitwiseXor, source0, source1));
            case AluOperation::OrNot:
                return binary(
                    AluSpirvOp::BitwiseOr, source0,
                    unary(AluSpirvOp::Not, source1));
            case AluOperation::AndNot:
                return binary(
                    AluSpirvOp::BitwiseAnd, source0,
                    unary(AluSpirvOp::Not, source1));
            case AluOperation::Unknown:
                break;
        }
        return 0;
    }

private:
    // The smaller or larger of two, which SPIR-V has no integer
    // instruction for outside the GLSL set: a compare and a select say the
    // same thing and keep this emitter free of an import.
    std::uint32_t choose(
        AluSpirvOp comparison, std::uint32_t left, std::uint32_t right) {
        std::uint32_t taken = 0;
        condition(comparison, left, right, &taken);
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(169),
            {types_.uint_type, result, taken, left, right});
        return result;
    }

    std::uint32_t unary(AluSpirvOp op, std::uint32_t source) {
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(op),
            {types_.uint_type, result, source});
        return result;
    }

    std::uint32_t binary(
        AluSpirvOp op, std::uint32_t left, std::uint32_t right) {
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(op),
            {types_.uint_type, result, left, right});
        return result;
    }

    // A compare produces a bool, which is not a register value. It goes to
    // the caller's condition rather than being returned as one.
    std::uint32_t condition(
        AluSpirvOp op,
        std::uint32_t left,
        std::uint32_t right,
        std::uint32_t* condition_out) {
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(op),
            {types_.bool_type, result, left, right});
        if (condition_out != nullptr) {
            *condition_out = result;
        }
        return 0;
    }

    ps5spirv::ModuleBuilder& module_;
    AluTypes types_;
};

}  // namespace ps5gen5

#endif  // PS5_GEN5_EMIT_ALU_H
