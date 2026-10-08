// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The decoded form of a guest shader, ported from Gen5ShaderIr.cs.
//
// Second piece of the native shader path, after the SPIR-V writer. It is
// only types: what an instruction is, what its operands are, and what each
// encoding carries alongside them. The decoder fills it and the SPIR-V
// translator reads it, so it has to say the same things the C# records say,
// in the same shapes, or the two cannot be compared.
//
// Three differences from the C# are deliberate. The control records are a
// variant rather than a class hierarchy, because a control is always
// exactly one of them and a variant says so without an allocation or a
// virtual call. Data arrays are plain vectors: the C# ones are rented from
// an ArrayPool and carry a length beside them because of it, and that pool
// is one of the things this port exists to leave behind. And the derived
// values the C# computes in a constructor are computed on demand here,
// beside the data they read.

#ifndef PS5_GEN5_SHADER_IR_H
#define PS5_GEN5_SHADER_IR_H

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ps5gen5 {

enum class Encoding {
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
};

enum class OperandKind {
    ScalarRegister,
    VectorRegister,
    EncodedConstant,
    LiteralConstant,
};

enum class ResourceKind {
    ReadOnlyTexture,
    ReadWriteTexture,
    Sampler,
    ConstantBuffer,
};

enum class PixelOutputKind {
    Float,
    Uint,
    Sint,
};

struct PixelOutputBinding {
    std::uint32_t guest_slot = 0;
    std::uint32_t host_location = 0;
    PixelOutputKind kind = PixelOutputKind::Float;
};

struct ResourceMapping {
    ResourceKind kind = ResourceKind::ReadOnlyTexture;
    std::uint32_t slot = 0;
    std::uint32_t offset_dwords = 0;
    bool size_flag = false;
};

struct ShaderMetadata {
    std::uint32_t extended_user_data_size_dwords = 0;
    std::uint32_t shader_resource_table_size_dwords = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> direct_resources;
    std::vector<ResourceMapping> resources;
};

// Which scalar registers the hardware has already filled with the dispatch's
// own coordinates before the shader starts. A register named here is not a
// value the program computed, so a translation has to read it from the
// built-in rather than from the register file.
struct ComputeSystemRegisters {
    std::optional<std::uint32_t> work_group_x;
    std::optional<std::uint32_t> work_group_y;
    std::optional<std::uint32_t> work_group_z;
    std::optional<std::uint32_t> thread_group_size;

    bool is_system_register(std::uint32_t scalar_register) const {
        return (work_group_x && *work_group_x == scalar_register) ||
            (work_group_y && *work_group_y == scalar_register) ||
            (work_group_z && *work_group_z == scalar_register) ||
            (thread_group_size && *thread_group_size == scalar_register);
    }

    // The static evaluation of a program must not carry a value for these:
    // whatever the last dispatch left there says nothing about this one.
    void clear_static_values(std::vector<std::uint32_t>& scalars) const {
        const std::optional<std::uint32_t> named[] = {
            work_group_x, work_group_y, work_group_z, thread_group_size};
        const auto count = scalars.size();
        for (const auto& which : named) {
            if (!which.has_value()) {
                continue;
            }
            const std::size_t index = *which;
            if (index >= count) {
                continue;
            }
            scalars[index] = 0;
        }
    }
};

struct Operand {
    OperandKind kind = OperandKind::ScalarRegister;
    std::uint32_t value = 0;

    static Operand scalar(std::uint32_t index) {
        return Operand{OperandKind::ScalarRegister, index};
    }

    static Operand vector(std::uint32_t index) {
        return Operand{OperandKind::VectorRegister, index};
    }

    // The encoding of a source operand: 256 and above is a vector register,
    // 249 and 255 introduce a literal in the instruction stream, and the
    // scalar range is not contiguous - 106, 107, 124, 126 and 127 are
    // registers among encodings that are not.
    static Operand source(
        std::uint32_t encoded,
        const std::optional<std::uint32_t>& literal = std::nullopt) {
        if (encoded >= 256) {
            return vector(encoded - 256);
        }
        if ((encoded == 249 || encoded == 255) && literal.has_value()) {
            return Operand{OperandKind::LiteralConstant, *literal};
        }
        if (encoded <= 105 || encoded == 106 || encoded == 107 ||
            encoded == 124 || encoded == 126 || encoded == 127) {
            return scalar(encoded);
        }
        return Operand{OperandKind::EncodedConstant, encoded};
    }
};

struct ImageControl {
    std::uint32_t dmask = 0;
    std::uint32_t vector_address = 0;
    std::vector<std::uint32_t> address_registers;
    std::uint32_t vector_data = 0;
    std::uint32_t scalar_resource = 0;
    std::uint32_t scalar_sampler = 0;
    std::uint32_t dimension = 0;
    bool is_array = false;
    bool glc = false;
    bool slc = false;
    bool a16 = false;
    bool d16 = false;

    std::uint32_t address_register(std::size_t component) const {
        return component < address_registers.size()
            ? address_registers[component]
            : vector_address + static_cast<std::uint32_t>(component);
    }
};

struct GlobalMemoryControl {
    std::uint32_t dword_count = 0;
    std::uint32_t vector_address = 0;
    std::uint32_t vector_data = 0;
    std::uint32_t scalar_address = 0;
    std::int32_t offset_bytes = 0;
    bool glc = false;
    bool slc = false;
};

struct BufferMemoryControl {
    std::uint32_t dword_count = 0;
    std::uint32_t vector_address = 0;
    std::uint32_t vector_data = 0;
    std::uint32_t scalar_resource = 0;
    std::int32_t offset_bytes = 0;
    bool index_enabled = false;
    bool offset_enabled = false;
    bool glc = false;
    bool slc = false;
};

struct ExportControl {
    std::uint32_t target = 0;
    std::uint32_t enable_mask = 0;
    bool compressed = false;
    bool done = false;
    bool valid_mask = false;
};

struct InterpolationControl {
    std::uint32_t attribute = 0;
    std::uint32_t channel = 0;
};

struct Vop3Control {
    std::uint32_t absolute_mask = 0;
    std::uint32_t negate_mask = 0;
    std::uint32_t output_modifier = 0;
    bool clamp = false;
    std::uint32_t operand_select = 0;
    std::optional<std::uint32_t> scalar_destination;
};

struct SdwaControl {
    std::uint32_t destination_select = 0;
    std::uint32_t destination_unused = 0;
    std::uint32_t source0_select = 0;
    std::uint32_t source1_select = 0;
    bool source0_sign_extend = false;
    bool source1_sign_extend = false;
    std::uint32_t absolute_mask = 0;
    std::uint32_t negate_mask = 0;
    std::uint32_t output_modifier = 0;
    bool clamp = false;
    std::optional<std::uint32_t> scalar_destination;
};

// Packed source and destination modifiers. Each mask holds one bit per
// source operand. OpSel and OpSelHi pick which 16-bit half of a source feeds
// the low and high result lanes; NegLo and NegHi negate the value routed to
// each lane. Clamp saturates each output half to [0, 1].
struct Vop3pControl {
    std::uint32_t op_sel_mask = 0;
    std::uint32_t op_sel_hi_mask = 0;
    std::uint32_t neg_lo_mask = 0;
    std::uint32_t neg_hi_mask = 0;
    bool clamp = false;
};

struct DppControl {
    std::uint32_t control = 0;
    bool fetch_inactive = false;
    bool bound_control = false;
    std::uint32_t absolute_mask = 0;
    std::uint32_t negate_mask = 0;
    std::uint32_t bank_mask = 0;
    std::uint32_t row_mask = 0;
};

struct Dpp8Control {
    std::uint32_t lane_selectors = 0;
    bool fetch_inactive = false;
};

struct ScalarMemoryControl {
    std::uint32_t destination_count = 0;
    std::int32_t immediate_offset_bytes = 0;
    std::optional<std::uint32_t> dynamic_offset_register;
};

struct DataShareControl {
    std::uint32_t offset0 = 0;
    std::uint32_t offset1 = 0;
    bool gds = false;
};

// An instruction carries exactly one of these, or none. A variant says that
// where the C# hierarchy leaves it to a type test.
using InstructionControl = std::variant<
    std::monostate,
    ImageControl,
    GlobalMemoryControl,
    BufferMemoryControl,
    ExportControl,
    InterpolationControl,
    Vop3Control,
    SdwaControl,
    Vop3pControl,
    DppControl,
    Dpp8Control,
    ScalarMemoryControl,
    DataShareControl>;

struct ImageBinding {
    std::uint32_t pc = 0;
    std::string opcode;
    ImageControl control;
    std::vector<std::uint32_t> resource_descriptor;
    std::vector<std::uint32_t> sampler_descriptor;
    std::optional<std::uint32_t> mip_level;
};

struct GlobalMemoryBinding {
    std::uint32_t scalar_address = 0;
    std::uint64_t base_address = 0;
    std::vector<std::uint32_t> instruction_pcs;
    std::vector<std::uint8_t> data;
    bool writable = false;
    // Writable describes shader access and also decides whether a compute
    // dispatch has observable work. A statically reachable resource can
    // still be unbound on the current scalar path, and the evaluator hands
    // Vulkan zero-filled storage in that case. Such storage stays
    // shader-writable but must never be copied back to the descriptor's
    // unmapped guest address.
    bool write_back_to_guest = true;
};

struct VertexInputBinding {
    std::uint32_t pc = 0;
    std::uint32_t location = 0;
    std::uint32_t component_count = 0;
    std::uint32_t data_format = 0;
    std::uint32_t number_format = 0;
    std::uint64_t base_address = 0;
    std::uint32_t stride = 0;
    std::uint32_t offset_bytes = 0;
    std::vector<std::uint8_t> data;
};

struct ShaderInstruction {
    std::uint32_t pc = 0;
    Encoding encoding = Encoding::Sop1;
    std::string opcode;
    std::vector<std::uint32_t> words;
    std::vector<Operand> sources;
    std::vector<Operand> destinations;
    InstructionControl control;
};

struct ShaderProgram {
    std::uint64_t address = 0;
    std::vector<ShaderInstruction> instructions;

    static constexpr std::uint32_t kPixelColorTargetCount = 8;
    static constexpr std::uint32_t kPixelColorMaskBits = 4;
    static constexpr std::size_t kScalarRegisterCount = 256;

    // Four bits per colour target, saying which channels the program
    // exports. Read by the runtime to decide whether a draw writes anything
    // the target accepts.
    std::uint32_t pixel_color_export_masks() const {
        std::uint32_t masks = 0;
        for (const auto& instruction : instructions) {
            const auto* control =
                std::get_if<ExportControl>(&instruction.control);
            if (control != nullptr &&
                control->target < kPixelColorTargetCount) {
                masks |= (control->enable_mask & 0xFu)
                    << (control->target * kPixelColorMaskBits);
            }
        }
        return masks;
    }
};

struct ShaderState {
    ShaderProgram program;
    std::vector<std::uint32_t> user_data;
    std::optional<ShaderMetadata> metadata;
    std::optional<ComputeSystemRegisters> compute_system_registers;
    std::uint32_t user_data_scalar_register_base = 0;
};

struct ShaderEvaluation {
    std::vector<std::uint32_t> initial_scalar_registers;
    std::vector<std::uint32_t> scalar_registers;
    std::vector<ImageBinding> image_bindings;
    std::vector<GlobalMemoryBinding> global_memory_bindings;
    std::optional<ComputeSystemRegisters> compute_system_registers;
    std::vector<std::uint32_t> runtime_scalar_registers;
    std::vector<VertexInputBinding> vertex_inputs;
};

}  // namespace ps5gen5

#endif  // PS5_GEN5_SHADER_IR_H
