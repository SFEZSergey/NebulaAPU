// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The guest's registers, and where their values live while a block runs.
//
// This is the thing the port exists for. The C# translator emits a program
// counter dispatcher, and a value cannot stay in a register across a switch
// and a back edge, so every guest instruction becomes an access chain, a
// load, the work itself, and a store. In one compute shader that is 24551
// access chains, 21109 loads and 8530 stores - 40% of a 129517 instruction
// module, for a shader the GPU then runs two million times a frame.
//
// The control flow is structured now, so within a block a value can simply
// be a value. The registers still have their home in memory, because a
// value written in one block and read in another has to get there somehow,
// but inside a block:
//
//   - the first read of a register loads it, and every later read in that
//     block uses what was loaded;
//   - a write keeps the value and marks the register, touching no memory;
//   - a register written several times is stored once, at the end;
//   - a register only read is never stored at all.
//
// Which leaves exactly one load and one store per register per block that
// uses it, against one of each per instruction.

#ifndef PS5_GEN5_REGISTERS_H
#define PS5_GEN5_REGISTERS_H

#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "spirv_builder.h"

namespace ps5gen5 {

struct RegisterFileStats {
    std::uint32_t loads_emitted = 0;
    std::uint32_t stores_emitted = 0;
    std::uint32_t reads_served_from_cache = 0;
    std::uint32_t writes_kept_in_registers = 0;
};

// One bank of registers - the scalars or the vectors - held as an array in
// Private storage, with the values of the block currently being emitted
// kept beside it.
class RegisterBank {
public:
    RegisterBank() = default;

    RegisterBank(
        ps5spirv::ModuleBuilder* module,
        std::uint32_t array_variable,
        std::uint32_t element_pointer_type,
        std::uint32_t element_type,
        std::uint32_t index_type,
        bool per_register = false)
        : module_(module),
          array_(array_variable),
          pointer_type_(element_pointer_type),
          element_type_(element_type),
          index_type_(index_type),
          per_register_(per_register) {}

    // The variables made in per-register mode, for the entry point's
    // interface.
    const std::vector<std::uint32_t>& variables() const { return variables_; }

    // The scalar file's index space is the source operand encoding, and
    // most of the top half of it is not registers: 128 to 208 are the
    // integers 0, 1 to 64 and -1 to -16, 240 to 248 a handful of floats,
    // and 255 the literal word after the instruction. Read as registers,
    // those were private variables nothing ever stored - a shift by three
    // shifted by whatever the driver left there, and the intro's vertex
    // stage fetched its corners from nowhere.
    void set_encodes_constants(bool encodes) { encodes_constants_ = encodes; }

    // The literal word of the instruction being translated, for 255.
    void set_literal(std::uint32_t literal) { literal_ = literal; }

    // The 32-bit value an encoding stands for, when it is not a register.
    bool constant_of(std::uint32_t reg, std::uint32_t& value) const {
        if (!encodes_constants_) {
            return false;
        }
        if (reg >= 128 && reg <= 192) {
            value = reg - 128;
            return true;
        }
        if (reg >= 193 && reg <= 208) {
            value = static_cast<std::uint32_t>(-static_cast<std::int32_t>(
                reg - 192));
            return true;
        }
        static constexpr std::uint32_t kFloats[] = {
            0x3F000000u, 0xBF000000u, 0x3F800000u, 0xBF800000u,
            0x40000000u, 0xC0000000u, 0x40800000u, 0xC0800000u,
            0x3E22F983u};
        if (reg >= 240 && reg <= 248) {
            value = kFloats[reg - 240];
            return true;
        }
        if (reg == 255) {
            value = literal_;
            return true;
        }
        return false;
    }

    // The upper half of a 64-bit source: the next register, or for a
    // constant its sign extended - an integer constant is a 64-bit integer
    // to a 64-bit instruction, so -1 is all ones and not -1 then -2.
    std::uint32_t read_high(std::uint32_t reg) {
        std::uint32_t value = 0;
        if (constant_of(reg, value)) {
            const auto negative = reg >= 193 && reg <= 208;
            return module_->constant(element_type_, negative ? ~0u : 0u);
        }
        return read(reg + 1);
    }

    // The value of a register, loading it only if this block has not seen
    // it yet.
    std::uint32_t read(std::uint32_t reg) {
        if (std::uint32_t value = 0; constant_of(reg, value)) {
            return module_->constant(element_type_, value);
        }
        const auto cached = values_.find(reg);
        if (cached != values_.end()) {
            ++stats_.reads_served_from_cache;
            return cached->second;
        }
        const auto pointer = pointer_to(reg);
        const auto value = module_->allocate_id();
        module_->add_function_word(
            ps5spirv::Op::Load, {element_type_, value, pointer});
        ++stats_.loads_emitted;
        values_.emplace(reg, value);
        return value;
    }

    // A write touches no memory. The value stands until the block ends, and
    // a register written twice is stored once.
    void write(std::uint32_t reg, std::uint32_t value) {
        values_[reg] = value;
        dirty_.insert(reg);
        ++stats_.writes_kept_in_registers;
    }

    // Ends a block: everything written goes to memory, because the next
    // block reads it from there. Registers only read leave nothing behind.
    void flush() {
        for (const auto reg : dirty_) {
            const auto value = values_.find(reg);
            if (value == values_.end()) {
                continue;
            }
            const auto pointer = pointer_to(reg);
            module_->add_function_word(
                ps5spirv::Op::Store, {pointer, value->second});
            ++stats_.stores_emitted;
        }
        dirty_.clear();
        values_.clear();
    }

    // Whether a register has a value this block already knows, which the
    // caller needs when deciding if a read is free.
    bool is_live(std::uint32_t reg) const {
        return values_.find(reg) != values_.end();
    }

    const RegisterFileStats& stats() const { return stats_; }

private:
    // Where a register lives. Per-register mode gives each one its own
    // private variable, made the first time it is touched: a whole file of
    // 256 words as one array is local memory to the driver, and a pixel
    // stage paired with a vertex stage both carrying two of them wrote
    // nothing at all - the intro video's colour conversion among them.
    // Every index here is a constant, so nothing needs the array.
    std::uint32_t pointer_to(std::uint32_t reg) {
        if (per_register_) {
            const auto found = registers_.find(reg);
            if (found != registers_.end()) {
                return found->second;
            }
            const auto variable = module_->allocate_id();
            module_->add_global(
                ps5spirv::Op::Variable,
                {pointer_type_, variable,
                 static_cast<std::uint32_t>(ps5spirv::StorageClass::Private)});
            registers_.emplace(reg, variable);
            variables_.push_back(variable);
            return variable;
        }
        const auto pointer = module_->allocate_id();
        module_->add_function_word(
            ps5spirv::Op::AccessChain,
            {pointer_type_, pointer, array_,
             module_->constant(index_type_, reg)});
        return pointer;
    }

    ps5spirv::ModuleBuilder* module_ = nullptr;
    std::uint32_t array_ = 0;
    std::uint32_t pointer_type_ = 0;
    std::uint32_t element_type_ = 0;
    std::uint32_t index_type_ = 0;
    bool per_register_ = false;
    bool encodes_constants_ = false;
    std::uint32_t literal_ = 0;
    std::map<std::uint32_t, std::uint32_t> registers_;
    std::vector<std::uint32_t> variables_;
    std::map<std::uint32_t, std::uint32_t> values_;
    std::set<std::uint32_t> dirty_;
    RegisterFileStats stats_;
};

}  // namespace ps5gen5

#endif  // PS5_GEN5_REGISTERS_H
