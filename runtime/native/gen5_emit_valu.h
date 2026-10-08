// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The vector instructions, which are almost all of a shader.
//
// Of 13794 instructions across the title's shaders, the scalar family is
// 141. The rest is this: the floating point arithmetic that shades a pixel
// and the integer work that addresses memory, run once per lane.
//
// A register holds bits, not a number. GCN says what they mean in the
// instruction rather than in the register, so VAddF32 and VAddI32 read the
// same register differently, and a translation has to convert at the edges
// of each operation rather than once. SPIR-V's OpBitcast is free - it
// renames a value rather than computing anything - so the cost is in the
// module's size and not in what the GPU does.

#ifndef PS5_GEN5_EMIT_VALU_H
#define PS5_GEN5_EMIT_VALU_H

#include <cstdint>
#include <cstring>
#include <string_view>

#include "gen5_emit_alu.h"

namespace ps5gen5 {

enum class VectorOperation {
    Unknown,
    Move,
    // Floating point
    AddF32,
    SubtractF32,
    SubtractReverseF32,
    MultiplyF32,
    MinF32,
    MaxF32,
    FractF32,
    TruncF32,
    FloorF32,
    CeilF32,
    RoundEvenF32,
    RcpF32,
    RsqF32,
    SqrtF32,
    ExpF32,
    LogF32,
    SinF32,
    CosF32,
    FmaF32,
    // Integer and bit work
    AddI32,
    SubtractI32,
    MinI32,
    MaxI32,
    MinU32,
    MaxU32,
    AndB32,
    OrB32,
    XorB32,
    NotB32,
    ShiftLeftB32,
    ShiftRightB32,
    ShiftRightArithmeticI32,
    // Conversions between the two worlds
    ConvertF32FromI32,
    ConvertF32FromU32,
    ConvertI32FromF32,
    ConvertU32FromF32,
    MultiplyLoU32,
    MultiplyHiU32,
    // A byte of the source, zero extended, as a float; which byte is the
    // operation's own.
    ConvertF32FromUbyte0,
    ConvertF32FromUbyte1,
    ConvertF32FromUbyte2,
    ConvertF32FromUbyte3,
    BitReverseB32,
    Med3U32,
    ShiftLeftAddU32,
    BitfieldExtractU32,
    BitfieldInsert,
    // Selection
    CndMask,
    // Three-source forms. VMadF32 alone is 2244 of the instructions this
    // title's shaders contain - more than any other single one - because a
    // multiply and an add is what shading mostly is.
    Med3F32,
    Min3F32,
    Max3F32,
    // Vector compares write a lane mask rather than a register.
    CompareEqU32,
    CompareNeU32,
    CompareLtU32,
    CompareLeU32,
    CompareGeU32,
    CompareGtU32,
    CompareEqF32,
    CompareNeqF32,
    CompareLtF32,
    CompareGtF32,
    CompareLeF32,
    CompareGeF32,
    CompareNltF32,
    CompareNleF32,
    CompareNgtF32,
    CompareNgeF32,
    CompareEqI32,
    CompareNeI32,
    CompareLtI32,
    CompareGtI32,
    CompareLeI32,
    CompareGeI32,
    Add3U32,
    PackHalves,
    // Two floats clamped to [0, 1] or [-1, 1] and packed as sixteen-bit
    // normalised integers, low half first - how a vertex stage squeezes a
    // colour or a pair of coordinates into one parameter.
    PackUnorm16,
    PackSnorm16,
    Or3U32,
    XnorB32,
    AndOrB32,
    ConvertI32FromFloorF32,
    // Cube map addressing. Four instructions that classify the same
    // direction vector and then each return a different part of the
    // answer, so they share their classification.
    CubeFaceId,
    CubeS,
    CubeT,
    CubeMajorAxis,
    // Fused forms and bit counting, which GCN has as single instructions.
    ShiftLeftOrU32,
    BitCountU32,
};

struct VectorForm {
    VectorOperation operation = VectorOperation::Unknown;
    // How many sources the instruction reads, which decides how many the
    // caller has to fetch before emitting.
    std::uint32_t source_count = 2;
    // Whether the first source is taken with its operands swapped, which
    // is what the "rev" instructions mean.
    bool reversed = false;
};

// The vector instructions this emitter knows, by the name the decoder
// gives them. Unknown is an answer, not a failure: during the port a gap
// that is counted is better than an operation that is guessed.
inline VectorForm vector_form_uncached(std::string_view name) {
    const auto binary = [](VectorOperation op) {
        return VectorForm{op, 2, false};
    };
    const auto unary = [](VectorOperation op) {
        return VectorForm{op, 1, false};
    };
    if (name == "VMovB32") return unary(VectorOperation::Move);
    if (name == "VAddF32") return binary(VectorOperation::AddF32);
    if (name == "VSubF32") return binary(VectorOperation::SubtractF32);
    if (name == "VSubrevF32")
        return VectorForm{VectorOperation::SubtractReverseF32, 2, true};
    if (name == "VMulF32") return binary(VectorOperation::MultiplyF32);
    if (name == "VMinF32") return binary(VectorOperation::MinF32);
    if (name == "VMaxF32") return binary(VectorOperation::MaxF32);
    if (name == "VFractF32") return unary(VectorOperation::FractF32);
    if (name == "VTruncF32") return unary(VectorOperation::TruncF32);
    if (name == "VFloorF32") return unary(VectorOperation::FloorF32);
    if (name == "VCeilF32") return unary(VectorOperation::CeilF32);
    if (name == "VRndneF32") return unary(VectorOperation::RoundEvenF32);
    if (name == "VRcpF32") return unary(VectorOperation::RcpF32);
    if (name == "VRcpIflagF32") return unary(VectorOperation::RcpF32);
    if (name == "VRsqF32") return unary(VectorOperation::RsqF32);
    if (name == "VSqrtF32") return unary(VectorOperation::SqrtF32);
    if (name == "VExpF32") return unary(VectorOperation::ExpF32);
    if (name == "VLogF32") return unary(VectorOperation::LogF32);
    if (name == "VSinF32") return unary(VectorOperation::SinF32);
    if (name == "VCosF32") return unary(VectorOperation::CosF32);
    if (name == "VAddI32") return binary(VectorOperation::AddI32);
    if (name == "VSubI32") return binary(VectorOperation::SubtractI32);
    if (name == "VSubrevI32")
        return VectorForm{VectorOperation::SubtractI32, 2, true};
    if (name == "VCvtF32Ubyte0")
        return unary(VectorOperation::ConvertF32FromUbyte0);
    if (name == "VCvtF32Ubyte1")
        return unary(VectorOperation::ConvertF32FromUbyte1);
    if (name == "VCvtF32Ubyte2")
        return unary(VectorOperation::ConvertF32FromUbyte2);
    if (name == "VCvtF32Ubyte3")
        return unary(VectorOperation::ConvertF32FromUbyte3);
    if (name == "VBfrevB32") return unary(VectorOperation::BitReverseB32);
    if (name == "VMed3U32") return VectorForm{VectorOperation::Med3U32, 3};
    if (name == "VMinI32") return binary(VectorOperation::MinI32);
    if (name == "VMaxI32") return binary(VectorOperation::MaxI32);
    if (name == "VMinU32") return binary(VectorOperation::MinU32);
    if (name == "VMaxU32") return binary(VectorOperation::MaxU32);
    if (name == "VAndB32") return binary(VectorOperation::AndB32);
    if (name == "VOrB32") return binary(VectorOperation::OrB32);
    if (name == "VXorB32") return binary(VectorOperation::XorB32);
    if (name == "VNotB32") return unary(VectorOperation::NotB32);
    if (name == "VLshlB32" || name == "VLshlrevB32")
        return VectorForm{VectorOperation::ShiftLeftB32, 2,
                          name == "VLshlrevB32"};
    if (name == "VLshrB32" || name == "VLshrrevB32")
        return VectorForm{VectorOperation::ShiftRightB32, 2,
                          name == "VLshrrevB32"};
    if (name == "VAshrI32" || name == "VAshrrevI32")
        return VectorForm{VectorOperation::ShiftRightArithmeticI32, 2,
                          name == "VAshrrevI32"};
    if (name == "VCvtF32I32") return unary(VectorOperation::ConvertF32FromI32);
    if (name == "VCvtF32U32") return unary(VectorOperation::ConvertF32FromU32);
    if (name == "VCvtI32F32") return unary(VectorOperation::ConvertI32FromF32);
    if (name == "VCvtU32F32") return unary(VectorOperation::ConvertU32FromF32);
    if (name == "VCndmaskB32")
        return VectorForm{VectorOperation::CndMask, 3, false};
    if (name == "VMacF32" || name == "VFmacF32")
        return VectorForm{VectorOperation::FmaF32, 3, false};
    // The three-source arithmetic, which is VOP3 and carries its sources in
    // a second word.
    if (name == "VMadF32" || name == "VMadMkF32" || name == "VMadAkF32" ||
        name == "VFmaF32" || name == "VFmaMkF32" || name == "VFmaAkF32" ||
        name == "VFmaMixF32")
        return VectorForm{VectorOperation::FmaF32, 3, false};
    if (name == "VMed3F32") return VectorForm{VectorOperation::Med3F32, 3};
    if (name == "VMin3F32") return VectorForm{VectorOperation::Min3F32, 3};
    if (name == "VMax3F32") return VectorForm{VectorOperation::Max3F32, 3};
    if (name == "VCmpEqU32")
        return VectorForm{VectorOperation::CompareEqU32, 2};
    if (name == "VCmpNeU32" || name == "VCmpLgU32")
        return VectorForm{VectorOperation::CompareNeU32, 2};
    if (name == "VMulLoU32" || name == "VMulLoI32" ||
        name == "VMulU32U24" || name == "VMulI32I24")
        return binary(VectorOperation::MultiplyLoU32);
    if (name == "VMulHiU32" || name == "VMulHiU32U24")
        return binary(VectorOperation::MultiplyHiU32);
    if (name == "VLshlAddU32")
        return VectorForm{VectorOperation::ShiftLeftAddU32, 3};
    if (name == "VBfeU32" || name == "VBfeI32")
        return VectorForm{VectorOperation::BitfieldExtractU32, 3};
    if (name == "VBfiB32")
        return VectorForm{VectorOperation::BitfieldInsert, 3};
    if (name == "VCmpxEqU32") return VectorForm{VectorOperation::CompareEqU32, 2};
    if (name == "VCmpxNeU32" || name == "VCmpxLgU32")
        return VectorForm{VectorOperation::CompareNeU32, 2};
    if (name == "VCmpxLtU32") return VectorForm{VectorOperation::CompareLtU32, 2};
    if (name == "VCmpxGtU32") return VectorForm{VectorOperation::CompareGtU32, 2};
    if (name == "VCmpxLeU32") return VectorForm{VectorOperation::CompareLeU32, 2};
    if (name == "VCmpxGeU32") return VectorForm{VectorOperation::CompareGeU32, 2};
    if (name == "VCmpLeU32")
        return VectorForm{VectorOperation::CompareLeU32, 2};
    if (name == "VCmpGeU32")
        return VectorForm{VectorOperation::CompareGeU32, 2};
    if (name == "VCmpLtU32")
        return VectorForm{VectorOperation::CompareLtU32, 2};
    if (name == "VCmpGtU32")
        return VectorForm{VectorOperation::CompareGtU32, 2};
    if (name == "VCmpEqF32")
        return VectorForm{VectorOperation::CompareEqF32, 2};
    if (name == "VCmpNeqF32")
        return VectorForm{VectorOperation::CompareNeqF32, 2};
    if (name == "VCmpLtF32")
        return VectorForm{VectorOperation::CompareLtF32, 2};
    if (name == "VCmpGtF32")
        return VectorForm{VectorOperation::CompareGtF32, 2};
    // The ordered comparisons, and the negated ones - which are not the
    // opposite ordered comparison: GCN's "not less than" is true when
    // either operand is a NaN, and SPIR-V spells that as the unordered
    // form rather than by negating.
    if (name == "VCmpLeF32" || name == "VCmpxLeF32")
        return VectorForm{VectorOperation::CompareLeF32, 2};
    if (name == "VCmpGeF32" || name == "VCmpxGeF32")
        return VectorForm{VectorOperation::CompareGeF32, 2};
    if (name == "VCmpNltF32" || name == "VCmpxNltF32")
        return VectorForm{VectorOperation::CompareNltF32, 2};
    if (name == "VCmpNleF32" || name == "VCmpxNleF32")
        return VectorForm{VectorOperation::CompareNleF32, 2};
    if (name == "VCmpNgtF32" || name == "VCmpxNgtF32")
        return VectorForm{VectorOperation::CompareNgtF32, 2};
    if (name == "VCmpNgeF32" || name == "VCmpxNgeF32")
        return VectorForm{VectorOperation::CompareNgeF32, 2};
    if (name == "VCmpxEqF32")
        return VectorForm{VectorOperation::CompareEqF32, 2};
    if (name == "VCmpxLtF32")
        return VectorForm{VectorOperation::CompareLtF32, 2};
    if (name == "VCmpxGtF32")
        return VectorForm{VectorOperation::CompareGtF32, 2};
    if (name == "VCmpxNeqF32")
        return VectorForm{VectorOperation::CompareNeqF32, 2};
    if (name == "VCmpEqI32" || name == "VCmpxEqI32")
        return VectorForm{VectorOperation::CompareEqI32, 2};
    if (name == "VCmpNeI32" || name == "VCmpLgI32" ||
        name == "VCmpxNeI32" || name == "VCmpxLgI32")
        return VectorForm{VectorOperation::CompareNeI32, 2};
    if (name == "VCmpLtI32" || name == "VCmpxLtI32")
        return VectorForm{VectorOperation::CompareLtI32, 2};
    if (name == "VCmpGtI32" || name == "VCmpxGtI32")
        return VectorForm{VectorOperation::CompareGtI32, 2};
    if (name == "VCmpLeI32" || name == "VCmpxLeI32")
        return VectorForm{VectorOperation::CompareLeI32, 2};
    if (name == "VCmpGeI32" || name == "VCmpxGeI32")
        return VectorForm{VectorOperation::CompareGeI32, 2};
    if (name == "VCubeidF32")
        return VectorForm{VectorOperation::CubeFaceId, 3};
    if (name == "VCubescF32")
        return VectorForm{VectorOperation::CubeS, 3};
    if (name == "VCubetcF32")
        return VectorForm{VectorOperation::CubeT, 3};
    if (name == "VCubemaF32")
        return VectorForm{VectorOperation::CubeMajorAxis, 3};
    if (name == "VOr3U32")
        return VectorForm{VectorOperation::Or3U32, 3};
    if (name == "VXnorB32")
        return binary(VectorOperation::XnorB32);
    if (name == "VAndOrB32")
        return VectorForm{VectorOperation::AndOrB32, 3};
    if (name == "VCvtFlrI32F32")
        return unary(VectorOperation::ConvertI32FromFloorF32);
    if (name == "VCvtPkrtzF16F32" || name == "VCvtPkrtzF16F32E64")
        return binary(VectorOperation::PackHalves);
    if (name == "VCvtPknormU16F32")
        return binary(VectorOperation::PackUnorm16);
    if (name == "VCvtPknormI16F32")
        return binary(VectorOperation::PackSnorm16);
    if (name == "VAdd3U32")
        return VectorForm{VectorOperation::Add3U32, 3};
    if (name == "VLshlOrU32")
        return VectorForm{VectorOperation::ShiftLeftOrU32, 3};
    if (name == "VBcntU32B32")
        return VectorForm{VectorOperation::BitCountU32, 1};
    return VectorForm{};
}

inline VectorForm vector_form(std::string_view name) {
    return memo_by_name<VectorForm>(name, [](std::string_view n) {
        return vector_form_uncached(n);
    });
}

// SPIR-V opcodes and GLSL extended instruction numbers this emitter needs.
enum class ValuSpirvOp : std::uint16_t {
    FAdd = 129,
    FSub = 131,
    FMul = 133,
    FNegate = 127,
    ConvertFToU = 109,
    ConvertFToS = 110,
    ConvertSToF = 111,
    ConvertUToF = 112,
    Bitcast = 124,
    Select = 169,
    ExtInst = 12,
};

// The GLSL.std.450 instruction numbers, which are a separate space from
// SPIR-V's own opcodes.
enum class GlslOp : std::uint32_t {
    Round = 1,
    RoundEven = 2,
    Trunc = 3,
    Floor = 8,
    Ceil = 9,
    Fract = 10,
    Sin = 13,
    Cos = 14,
    Exp2 = 29,
    Log2 = 30,
    FAbs = 4,
    Sqrt = 31,
    InverseSqrt = 32,
    FMin = 37,
    FMax = 40,
    SMin = 39,
    SMax = 42,
    UMin = 38,
    UMax = 41,
    Fma = 50,
};

class VectorEmitter {
public:
    VectorEmitter(
        ps5spirv::ModuleBuilder& module,
        const AluTypes& types,
        std::uint32_t float_type,
        std::uint32_t glsl)
        : module_(module),
          types_(types),
          float_type_(float_type),
          glsl_(glsl) {}

    // Emits one instruction and returns the bits it produced, or zero when
    // the operation is not known.
    std::uint32_t emit(
        const VectorForm& form,
        std::uint32_t source0,
        std::uint32_t source1,
        std::uint32_t source2,
        std::uint32_t condition) {
        // "rev" instructions are the same operation with the operands the
        // other way round, which is how GCN gets a shift by a constant
        // amount without a second encoding.
        if (form.reversed) {
            const auto swap = source0;
            source0 = source1;
            source1 = swap;
        }
        switch (form.operation) {
            case VectorOperation::Move:
                return source0;
            case VectorOperation::AddF32:
                return float_binary(ValuSpirvOp::FAdd, source0, source1);
            case VectorOperation::SubtractF32:
            case VectorOperation::SubtractReverseF32:
                return float_binary(ValuSpirvOp::FSub, source0, source1);
            case VectorOperation::MultiplyF32:
                return float_binary(ValuSpirvOp::FMul, source0, source1);
            case VectorOperation::MinF32:
                return glsl_binary(GlslOp::FMin, source0, source1, true);
            case VectorOperation::MaxF32:
                return glsl_binary(GlslOp::FMax, source0, source1, true);
            case VectorOperation::FractF32:
                return glsl_unary(GlslOp::Fract, source0);
            case VectorOperation::TruncF32:
                return glsl_unary(GlslOp::Trunc, source0);
            case VectorOperation::FloorF32:
                return glsl_unary(GlslOp::Floor, source0);
            case VectorOperation::CeilF32:
                return glsl_unary(GlslOp::Ceil, source0);
            case VectorOperation::RoundEvenF32:
                return glsl_unary(GlslOp::RoundEven, source0);
            case VectorOperation::SqrtF32:
                return glsl_unary(GlslOp::Sqrt, source0);
            case VectorOperation::RsqF32:
                return glsl_unary(GlslOp::InverseSqrt, source0);
            case VectorOperation::ExpF32:
                return glsl_unary(GlslOp::Exp2, source0);
            case VectorOperation::LogF32:
                return glsl_unary(GlslOp::Log2, source0);
            case VectorOperation::SinF32:
                return glsl_unary(GlslOp::Sin, source0);
            case VectorOperation::CosF32:
                return glsl_unary(GlslOp::Cos, source0);
            case VectorOperation::RcpF32:
                return reciprocal(source0);
            case VectorOperation::FmaF32:
                return fma(source0, source1, source2);
            case VectorOperation::AddI32:
                return int_binary(AluSpirvOp::IAdd, source0, source1);
            case VectorOperation::SubtractI32:
                return int_binary(AluSpirvOp::ISub, source0, source1);
            case VectorOperation::MinI32:
                return glsl_binary(GlslOp::SMin, source0, source1, false);
            case VectorOperation::MaxI32:
                return glsl_binary(GlslOp::SMax, source0, source1, false);
            case VectorOperation::MinU32:
                return glsl_binary(GlslOp::UMin, source0, source1, false);
            case VectorOperation::MaxU32:
                return glsl_binary(GlslOp::UMax, source0, source1, false);
            case VectorOperation::AndB32:
                return int_binary(AluSpirvOp::BitwiseAnd, source0, source1);
            case VectorOperation::OrB32:
                return int_binary(AluSpirvOp::BitwiseOr, source0, source1);
            case VectorOperation::XorB32:
                return int_binary(AluSpirvOp::BitwiseXor, source0, source1);
            case VectorOperation::NotB32:
                return int_unary(AluSpirvOp::Not, source0);
            case VectorOperation::ShiftLeftB32:
                return int_binary(
                    AluSpirvOp::ShiftLeftLogical, source0, source1);
            case VectorOperation::ShiftRightB32:
                return int_binary(
                    AluSpirvOp::ShiftRightLogical, source0, source1);
            case VectorOperation::ShiftRightArithmeticI32:
                return int_binary(
                    AluSpirvOp::ShiftRightArithmetic, source0, source1);
            case VectorOperation::ConvertF32FromI32:
                return convert(ValuSpirvOp::ConvertSToF, source0, true);
            case VectorOperation::ConvertF32FromU32:
                return convert(ValuSpirvOp::ConvertUToF, source0, true);
            case VectorOperation::ConvertF32FromUbyte0:
            case VectorOperation::ConvertF32FromUbyte1:
            case VectorOperation::ConvertF32FromUbyte2:
            case VectorOperation::ConvertF32FromUbyte3: {
                const auto which = static_cast<std::uint32_t>(
                    form.operation) -
                    static_cast<std::uint32_t>(
                        VectorOperation::ConvertF32FromUbyte0);
                const auto shifted = int_binary(
                    AluSpirvOp::ShiftRightLogical, source0,
                    module_.constant(types_.uint_type, which * 8u));
                const auto byte = int_binary(
                    AluSpirvOp::BitwiseAnd, shifted,
                    module_.constant(types_.uint_type, 0xFFu));
                return convert(ValuSpirvOp::ConvertUToF, byte, true);
            }
            case VectorOperation::BitReverseB32:
                return int_unary(static_cast<AluSpirvOp>(204), source0);
            case VectorOperation::Med3U32: {
                const VectorForm min_form{VectorOperation::MinU32, 2, false};
                const VectorForm max_form{VectorOperation::MaxU32, 2, false};
                const auto low = emit(min_form, source0, source1, 0, 0);
                const auto high = emit(max_form, source0, source1, 0, 0);
                const auto capped = emit(min_form, high, source2, 0, 0);
                return emit(max_form, low, capped, 0, 0);
            }
            case VectorOperation::ConvertI32FromF32:
                return convert(ValuSpirvOp::ConvertFToS, source0, false);
            case VectorOperation::ConvertU32FromF32:
                return convert(ValuSpirvOp::ConvertFToU, source0, false);
            case VectorOperation::CndMask:
                return select(condition, source1, source0);
            case VectorOperation::ShiftLeftOrU32: {
                const auto shifted = int_binary(
                    AluSpirvOp::ShiftLeftLogical, source0, source1);
                return int_binary(
                    AluSpirvOp::BitwiseOr, shifted, source2);
            }
            case VectorOperation::BitCountU32: {
                const auto result = module_.allocate_id();
                // OpBitCount is 205.
                module_.add_function_word(
                    static_cast<ps5spirv::Op>(205),
                    {types_.uint_type, result, source0});
                return result;
            }
            case VectorOperation::Med3F32:
                // The middle of three, which GLSL has no instruction for:
                // clamping the first to the range the other two make is the
                // same value whichever way round they are.
                return glsl_binary(
                    GlslOp::FMax,
                    glsl_binary(GlslOp::FMin, source0, source1, true),
                    glsl_binary(GlslOp::FMin,
                                glsl_binary(GlslOp::FMax, source0, source1,
                                            true),
                                source2, true),
                    true);
            case VectorOperation::Min3F32:
                return glsl_binary(
                    GlslOp::FMin,
                    glsl_binary(GlslOp::FMin, source0, source1, true),
                    source2, true);
            case VectorOperation::Max3F32:
                return glsl_binary(
                    GlslOp::FMax,
                    glsl_binary(GlslOp::FMax, source0, source1, true),
                    source2, true);
            case VectorOperation::CompareEqU32:
                return compare_int(AluSpirvOp::IEqual, source0, source1);
            case VectorOperation::CompareNeU32:
                return compare_int(AluSpirvOp::INotEqual, source0, source1);
            case VectorOperation::MultiplyLoU32:
                return int_binary(AluSpirvOp::IMul, source0, source1);
            case VectorOperation::MultiplyHiU32:
                // The half a 32-bit multiply throws away. Splitting both
                // operands and adding the partial products keeps it inside
                // 32-bit arithmetic, which is what the module has: a 64-bit
                // multiply would mean asking for the Int64 capability for
                // one instruction.
                return multiply_high(source0, source1);
            case VectorOperation::ShiftLeftAddU32:
                // Shift then add, which is how an index becomes an offset.
                return int_binary(
                    AluSpirvOp::IAdd,
                    int_binary(
                        AluSpirvOp::ShiftLeftLogical, source0, source1),
                    source2);
            case VectorOperation::BitfieldExtractU32:
                return bitfield_extract(source0, source1, source2);
            case VectorOperation::BitfieldInsert:
                // Where the mask has bits take the second source, elsewhere
                // the third. GCN builds an insert from a mask rather than
                // from a width and an offset.
                return int_binary(
                    AluSpirvOp::BitwiseOr,
                    int_binary(AluSpirvOp::BitwiseAnd, source0, source1),
                    int_binary(
                        AluSpirvOp::BitwiseAnd,
                        int_unary(AluSpirvOp::Not, source0),
                        source2));
            case VectorOperation::CompareLeU32:
                return compare_int(
                    AluSpirvOp::ULessThanEqual, source0, source1);
            case VectorOperation::CompareGeU32:
                return compare_int(
                    AluSpirvOp::UGreaterThanEqual, source0, source1);
            case VectorOperation::CompareLtU32:
                return compare_int(AluSpirvOp::ULessThan, source0, source1);
            case VectorOperation::CompareGtU32:
                return compare_int(
                    AluSpirvOp::UGreaterThan, source0, source1);
            case VectorOperation::CompareEqF32:
                return compare_float(180, source0, source1);
            case VectorOperation::CompareNeqF32:
                return compare_float(182, source0, source1);
            case VectorOperation::CompareLtF32:
                return compare_float(184, source0, source1);
            case VectorOperation::CompareGtF32:
                return compare_float(186, source0, source1);
            case VectorOperation::CompareLeF32:
                return compare_float(188, source0, source1);
            case VectorOperation::CompareGeF32:
                return compare_float(190, source0, source1);
            case VectorOperation::CompareNltF32:
                // Unordered greater or equal: true when neither is less,
                // and true when either is a NaN.
                return compare_float(191, source0, source1);
            case VectorOperation::CompareNleF32:
                return compare_float(187, source0, source1);
            case VectorOperation::CompareNgtF32:
                return compare_float(189, source0, source1);
            case VectorOperation::CompareNgeF32:
                return compare_float(185, source0, source1);
            case VectorOperation::CompareEqI32:
                return compare_int(AluSpirvOp::IEqual, source0, source1);
            case VectorOperation::CompareNeI32:
                return compare_int(AluSpirvOp::INotEqual, source0, source1);
            case VectorOperation::CompareLtI32:
                return compare_int(AluSpirvOp::SLessThan, source0, source1);
            case VectorOperation::CompareGtI32:
                return compare_int(
                    AluSpirvOp::SGreaterThan, source0, source1);
            case VectorOperation::CompareLeI32:
                return compare_int(
                    AluSpirvOp::SLessThanEqual, source0, source1);
            case VectorOperation::CompareGeI32:
                return compare_int(
                    AluSpirvOp::SGreaterThanEqual, source0, source1);
            case VectorOperation::CubeFaceId:
            case VectorOperation::CubeS:
            case VectorOperation::CubeT:
            case VectorOperation::CubeMajorAxis:
                return cube(form.operation, source0, source1, source2);
            case VectorOperation::Or3U32:
                return int_binary(
                    AluSpirvOp::BitwiseOr,
                    int_binary(AluSpirvOp::BitwiseOr, source0, source1),
                    source2);
            case VectorOperation::XnorB32:
                return int_unary(
                    AluSpirvOp::Not,
                    int_binary(AluSpirvOp::BitwiseXor, source0, source1));
            case VectorOperation::AndOrB32:
                // Two ands worth of work in one instruction: the first two
                // sources are anded and the third is ored in.
                return int_binary(
                    AluSpirvOp::BitwiseOr,
                    int_binary(AluSpirvOp::BitwiseAnd, source0, source1),
                    source2);
            case VectorOperation::ConvertI32FromFloorF32: {
                // Rounds down rather than towards zero, which for a
                // negative number is a different integer.
                const auto floored = glsl_unary(GlslOp::Floor, source0);
                const auto value = module_.allocate_id();
                module_.add_function_word(
                    static_cast<ps5spirv::Op>(110),
                    {types_.uint_type, value, as_float(floored)});
                return value;
            }
            case VectorOperation::PackHalves: {
                // Two floats into one register as halves, which is what a
                // compressed colour export reads back out.
                const auto vector2 = module_.type_vector(float_type_, 2);
                const auto pair = module_.allocate_id();
                module_.add_function_word(
                    ps5spirv::Op::CompositeConstruct,
                    {vector2, pair, as_float(source0), as_float(source1)});
                const auto packed = module_.allocate_id();
                module_.add_function_word(
                    ps5spirv::Op::ExtInst,
                    std::vector<std::uint32_t>{
                        types_.uint_type, packed, glsl_, 58, pair});
                return packed;
            }
            case VectorOperation::PackUnorm16:
            case VectorOperation::PackSnorm16: {
                // PackUnorm2x16 and PackSnorm2x16, which clamp, scale and
                // round the way the hardware does and put the first
                // source in the low half. Left untranslated, the register
                // kept the first float's bits: the UI plane's vertex
                // colours arrived as 0x3F80 - a quarter - and its blue
                // as nothing.
                const auto vector2 = module_.type_vector(float_type_, 2);
                const auto pair = module_.allocate_id();
                module_.add_function_word(
                    ps5spirv::Op::CompositeConstruct,
                    {vector2, pair, as_float(source0), as_float(source1)});
                const auto packed = module_.allocate_id();
                module_.add_function_word(
                    ps5spirv::Op::ExtInst,
                    std::vector<std::uint32_t>{
                        types_.uint_type, packed, glsl_,
                        form.operation == VectorOperation::PackUnorm16
                            ? 57u : 56u,
                        pair});
                return packed;
            }
            case VectorOperation::Add3U32:
                return int_binary(
                    AluSpirvOp::IAdd,
                    int_binary(AluSpirvOp::IAdd, source0, source1),
                    source2);
            case VectorOperation::Unknown:
                break;
        }
        return 0;
    }

private:
    // Cube map addressing. The three sources are a direction vector, and
    // the face it points at is whichever component is largest in
    // magnitude. Each of the four instructions asks for a different part
    // of the same answer, so the classification is written once here.
    //
    // The face numbering and the coordinate signs are the ones the
    // hardware defines: positive X is face zero and the faces alternate
    // positive then negative, S runs along the axis that is not T and not
    // the major one, and the major axis comes back doubled and signed -
    // the shader that follows divides by its magnitude.
    std::uint32_t cube(
        VectorOperation which,
        std::uint32_t x_bits,
        std::uint32_t y_bits,
        std::uint32_t z_bits) {
        const auto x = as_float(x_bits);
        const auto y = as_float(y_bits);
        const auto z = as_float(z_bits);
        const auto absolute = [&](std::uint32_t value) {
            const auto result = module_.allocate_id();
            module_.add_function_word(
                ps5spirv::Op::ExtInst,
                std::vector<std::uint32_t>{
                    float_type_, result, glsl_,
                    static_cast<std::uint32_t>(GlslOp::FAbs), value});
            return result;
        };
        const auto at_least = [&](std::uint32_t left, std::uint32_t right) {
            const auto result = module_.allocate_id();
            module_.add_function_word(
                static_cast<ps5spirv::Op>(190),  // FOrdGreaterThanEqual
                {types_.bool_type, result, left, right});
            return result;
        };
        const auto both = [&](std::uint32_t left, std::uint32_t right) {
            const auto result = module_.allocate_id();
            module_.add_function_word(
                static_cast<ps5spirv::Op>(167),  // LogicalAnd
                {types_.bool_type, result, left, right});
            return result;
        };
        const auto negative = [&](std::uint32_t value) {
            const auto result = module_.allocate_id();
            module_.add_function_word(
                static_cast<ps5spirv::Op>(184),  // FOrdLessThan
                {types_.bool_type, result, value,
                 module_.constant(float_type_, 0)});
            return result;
        };
        const auto negate = [&](std::uint32_t value) {
            const auto result = module_.allocate_id();
            module_.add_function_word(
                static_cast<ps5spirv::Op>(ValuSpirvOp::FNegate),
                {float_type_, result, value});
            return result;
        };
        const auto pick = [&](std::uint32_t taken, std::uint32_t when_true,
                              std::uint32_t when_false) {
            const auto result = module_.allocate_id();
            module_.add_function_word(
                static_cast<ps5spirv::Op>(169),  // Select
                {float_type_, result, taken, when_true, when_false});
            return result;
        };
        const auto number = [&](float value) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return module_.constant(float_type_, bits);
        };

        const auto abs_x = absolute(x);
        const auto abs_y = absolute(y);
        const auto abs_z = absolute(z);
        const auto z_major =
            both(at_least(abs_z, abs_x), at_least(abs_z, abs_y));
        const auto y_major = at_least(abs_y, abs_x);
        const auto x_negative = negative(x);
        const auto y_negative = negative(y);
        const auto z_negative = negative(z);

        std::uint32_t value = 0;
        switch (which) {
            case VectorOperation::CubeFaceId: {
                const auto from_x =
                    pick(x_negative, number(1.0f), number(0.0f));
                const auto from_y =
                    pick(y_negative, number(3.0f), number(2.0f));
                const auto from_z =
                    pick(z_negative, number(5.0f), number(4.0f));
                value = pick(z_major, from_z, pick(y_major, from_y, from_x));
                break;
            }
            case VectorOperation::CubeS: {
                // Along Z the S axis is X, signed by which Z face it is;
                // along Y it is X unchanged; along X it is Z, signed the
                // other way.
                const auto from_x = pick(x_negative, z, negate(z));
                const auto from_z = pick(z_negative, negate(x), x);
                value = pick(z_major, from_z, pick(y_major, x, from_x));
                break;
            }
            case VectorOperation::CubeT: {
                // T is minus Y on four of the faces and Z, signed, on the
                // two the Y axis points at.
                const auto from_y = pick(y_negative, negate(z), z);
                const auto elsewhere = negate(y);
                value = pick(
                    z_major, elsewhere, pick(y_major, from_y, elsewhere));
                break;
            }
            case VectorOperation::CubeMajorAxis: {
                const auto major =
                    pick(z_major, z, pick(y_major, y, x));
                const auto doubled = module_.allocate_id();
                module_.add_function_word(
                    static_cast<ps5spirv::Op>(ValuSpirvOp::FMul),
                    {float_type_, doubled, major, number(2.0f)});
                value = doubled;
                break;
            }
            default:
                return 0;
        }
        return as_bits(value);
    }

    // A register holds bits; these rename them as a float and back.
    std::uint32_t as_float(std::uint32_t bits) {
        const auto value = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(ValuSpirvOp::Bitcast),
            {float_type_, value, bits});
        return value;
    }

    std::uint32_t as_bits(std::uint32_t value) {
        const auto bits = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(ValuSpirvOp::Bitcast),
            {types_.uint_type, bits, value});
        return bits;
    }

    std::uint32_t float_binary(
        ValuSpirvOp op, std::uint32_t left, std::uint32_t right) {
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(op),
            {float_type_, result, as_float(left), as_float(right)});
        return as_bits(result);
    }

    std::uint32_t int_binary(
        AluSpirvOp op, std::uint32_t left, std::uint32_t right) {
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(op),
            {types_.uint_type, result, left, right});
        return result;
    }

    // Bits [offset, offset + width) of a value, moved down to the bottom.
    std::uint32_t bitfield_extract(
        std::uint32_t value, std::uint32_t offset, std::uint32_t width) {
        const auto one = module_.constant(types_.uint_type, 1);
        const auto shifted =
            int_binary(AluSpirvOp::ShiftRightLogical, value, offset);
        const auto span =
            int_binary(AluSpirvOp::ShiftLeftLogical, one, width);
        const auto mask = int_binary(AluSpirvOp::ISub, span, one);
        return int_binary(AluSpirvOp::BitwiseAnd, shifted, mask);
    }

    // The high half of a 32 by 32 multiply, from four 16-bit products.
    std::uint32_t multiply_high(
        std::uint32_t left, std::uint32_t right) {
        const auto sixteen = module_.constant(types_.uint_type, 16);
        const auto low_mask = module_.constant(types_.uint_type, 0xFFFFu);
        const auto low_left =
            int_binary(AluSpirvOp::BitwiseAnd, left, low_mask);
        const auto high_left =
            int_binary(AluSpirvOp::ShiftRightLogical, left, sixteen);
        const auto low_right =
            int_binary(AluSpirvOp::BitwiseAnd, right, low_mask);
        const auto high_right =
            int_binary(AluSpirvOp::ShiftRightLogical, right, sixteen);
        const auto low_product =
            int_binary(AluSpirvOp::IMul, low_left, low_right);
        const auto cross_one =
            int_binary(AluSpirvOp::IMul, high_left, low_right);
        const auto cross_two =
            int_binary(AluSpirvOp::IMul, low_left, high_right);
        const auto high_product =
            int_binary(AluSpirvOp::IMul, high_left, high_right);
        // The carry out of the low half, which is what makes this more than
        // the product of the high halves.
        const auto carried = int_binary(
            AluSpirvOp::IAdd,
            int_binary(
                AluSpirvOp::IAdd,
                int_binary(
                    AluSpirvOp::ShiftRightLogical, low_product, sixteen),
                int_binary(AluSpirvOp::BitwiseAnd, cross_one, low_mask)),
            int_binary(AluSpirvOp::BitwiseAnd, cross_two, low_mask));
        return int_binary(
            AluSpirvOp::IAdd,
            int_binary(
                AluSpirvOp::IAdd,
                high_product,
                int_binary(
                    AluSpirvOp::ShiftRightLogical, cross_one, sixteen)),
            int_binary(
                AluSpirvOp::IAdd,
                int_binary(
                    AluSpirvOp::ShiftRightLogical, cross_two, sixteen),
                int_binary(
                    AluSpirvOp::ShiftRightLogical, carried, sixteen)));
    }

    std::uint32_t int_unary(AluSpirvOp op, std::uint32_t source) {
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(op),
            {types_.uint_type, result, source});
        return result;
    }

    std::uint32_t glsl_unary(GlslOp op, std::uint32_t source) {
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(ValuSpirvOp::ExtInst),
            {float_type_, result, glsl_, static_cast<std::uint32_t>(op),
             as_float(source)});
        return as_bits(result);
    }

    std::uint32_t glsl_binary(
        GlslOp op, std::uint32_t left, std::uint32_t right, bool is_float) {
        const auto type = is_float ? float_type_ : types_.uint_type;
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(ValuSpirvOp::ExtInst),
            {type, result, glsl_, static_cast<std::uint32_t>(op),
             is_float ? as_float(left) : left,
             is_float ? as_float(right) : right});
        return is_float ? as_bits(result) : result;
    }

    // GCN's reciprocal is an instruction; SPIR-V has no OpFRcp, so it is a
    // divide by the value, which is what the hardware approximates.
    std::uint32_t reciprocal(std::uint32_t source) {
        const auto one = module_.constant(float_type_, 0x3F800000u);
        const auto result = module_.allocate_id();
        // OpFDiv is 136.
        module_.add_function_word(
            static_cast<ps5spirv::Op>(136),
            {float_type_, result, one, as_float(source)});
        return as_bits(result);
    }

    std::uint32_t fma(
        std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(ValuSpirvOp::ExtInst),
            {float_type_, result, glsl_,
             static_cast<std::uint32_t>(GlslOp::Fma),
             as_float(a), as_float(b), as_float(c)});
        return as_bits(result);
    }

    std::uint32_t convert(
        ValuSpirvOp op, std::uint32_t source, bool to_float) {
        const auto result = module_.allocate_id();
        if (to_float) {
            module_.add_function_word(
                static_cast<ps5spirv::Op>(op),
                {float_type_, result, source});
            return as_bits(result);
        }
        module_.add_function_word(
            static_cast<ps5spirv::Op>(op),
            {types_.uint_type, result, as_float(source)});
        return result;
    }

    // A vector compare produces one bit per lane on hardware. Until the
    // lane mask is modelled it becomes a plain condition, which is right
    // for the uniform case and is what the branch on it will read.
    std::uint32_t compare_int(
        AluSpirvOp op, std::uint32_t left, std::uint32_t right) {
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(op),
            {types_.bool_type, result, left, right});
        last_condition_ = result;
        return 0;
    }

    std::uint32_t compare_float(
        std::uint16_t opcode, std::uint32_t left, std::uint32_t right) {
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(opcode),
            {types_.bool_type, result, as_float(left), as_float(right)});
        last_condition_ = result;
        return 0;
    }

public:
    // The condition the last compare produced, which the caller takes as
    // the branch's predicate.
    std::uint32_t last_condition() const { return last_condition_; }

private:
    std::uint32_t last_condition_ = 0;

    std::uint32_t select(
        std::uint32_t condition,
        std::uint32_t when_true,
        std::uint32_t when_false) {
        const auto result = module_.allocate_id();
        module_.add_function_word(
            static_cast<ps5spirv::Op>(ValuSpirvOp::Select),
            {types_.uint_type, result, condition, when_true, when_false});
        return result;
    }

    ps5spirv::ModuleBuilder& module_;
    AluTypes types_;
    std::uint32_t float_type_ = 0;
    std::uint32_t glsl_ = 0;
};

}  // namespace ps5gen5

#endif  // PS5_GEN5_EMIT_VALU_H
