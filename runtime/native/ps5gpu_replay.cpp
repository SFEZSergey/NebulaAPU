// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ps5gpu_bridge_api.h"
#include "ps5gpu_capture.h"
#include "ps5gpu_gen5_preflight.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

// A real capture of one frame of this title carries 73,396 memory
// records, so the previous cap of 65,536 rejected it outright with
// "Invalid GPU state or memory capture table" - which is why the replay
// tool sat built and unused while every GPU-side question was answered
// by running the game instead. The byte totals below are the real guard
// against a corrupt header; the record counts only need to stop an
// absurd allocation.
constexpr std::uint64_t kMaximumCaptureRecords = 4ULL * 1024ULL * 1024ULL;

template <typename T>
T load_export(HMODULE module, const char* name) {
    const auto raw = GetProcAddress(module, name);
    T function = nullptr;
    static_assert(sizeof(function) == sizeof(raw));
    std::memcpy(&function, &raw, sizeof(function));
    return function;
}

std::filesystem::path executable_directory() {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(
        nullptr,
        path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    return std::filesystem::path(path.data(), path.data() + length)
        .parent_path();
}

struct CapturedShader {
    Ps5GpuCaptureShader record = {};
    std::vector<std::uint8_t> bytes;
};

struct CapturedState {
    Ps5GpuCaptureShaderState record = {};
    std::vector<Ps5GpuNativeRegisterValue> sh_registers;
    std::vector<Ps5GpuNativeRegisterValue> cx_registers;
    // The modules, from the capture's trailer when it has one.
    std::vector<std::uint8_t> es_spirv;
    std::vector<std::uint8_t> es_manifest;
    std::vector<std::uint8_t> ps_spirv;
    std::vector<std::uint8_t> ps_manifest;
};

struct CapturedMemory {
    Ps5GpuCaptureMemory record = {};
    std::vector<std::uint8_t> bytes;
};

struct CapturedComputeState {
    Ps5GpuCaptureComputeState record = {};
    std::vector<std::uint8_t> spirv;
    std::vector<std::uint8_t> resource_manifest;
};

struct ShaderWordReader {
    std::uint64_t base_address = 0;
    const std::vector<std::uint32_t>* words = nullptr;
};

bool read_shader_word(
    std::uint64_t address,
    void* destination,
    std::size_t size,
    void* user) {
    const auto* reader = static_cast<const ShaderWordReader*>(user);
    if (reader == nullptr ||
        reader->words == nullptr ||
        destination == nullptr ||
        size != sizeof(std::uint32_t) ||
        address < reader->base_address) {
        return false;
    }
    const auto byte_offset = address - reader->base_address;
    if (byte_offset % sizeof(std::uint32_t) != 0) {
        return false;
    }
    const auto index = static_cast<std::size_t>(
        byte_offset / sizeof(std::uint32_t));
    if (index >= reader->words->size()) {
        return false;
    }
    std::memcpy(
        destination,
        &(*reader->words)[index],
        sizeof(std::uint32_t));
    return true;
}

const char* scalar_opcode_name(
    ps5gpu::gen5::Encoding encoding,
    std::uint32_t word) {
    if (encoding == ps5gpu::gen5::Encoding::Sop1) {
        switch ((word >> 8) & 0xFFu) {
        case 0x03: return "s_mov_b32";
        case 0x04: return "s_mov_b64";
        case 0x07: return "s_not_b32";
        case 0x08: return "s_not_b64";
        case 0x0A: return "s_wqm_b64";
        case 0x0B: return "s_brev_b32";
        case 0x0F: return "s_bcnt1_i32_b32";
        case 0x13: return "s_ff1_i32_b32";
        case 0x14: return "s_ff1_i32_b64";
        case 0x1D: return "s_bitset1_b32";
        case 0x1F: return "s_getpc_b64";
        case 0x20: return "s_setpc_b64";
        case 0x21: return "s_swappc_b64";
        default: return "sop1";
        }
    }
    if (encoding == ps5gpu::gen5::Encoding::Sop2) {
        static constexpr const char* names[] = {
            "s_add_u32", "s_sub_u32", "s_add_i32", "s_sub_i32",
            "s_addc_u32", "s_subb_u32", "s_min_i32", "s_min_u32",
            "s_max_i32", "s_max_u32", "s_cselect_b32",
            "s_cselect_b64", "sop2", "sop2", "s_and_b32", "s_and_b64",
            "s_or_b32", "s_or_b64", "s_xor_b32", "s_xor_b64",
            "s_andn2_b32", "s_andn2_b64", "s_orn2_b32",
            "s_orn2_b64", "s_nand_b32", "s_nand_b64", "s_nor_b32",
            "s_nor_b64", "s_xnor_b32", "s_xnor_b64", "s_lshl_b32",
            "s_lshl_b64", "s_lshr_b32", "s_lshr_b64", "s_ashr_i32",
            "s_ashr_i64", "s_bfm_b32", "s_bfm_b64", "s_mul_i32",
            "s_bfe_u32", "s_bfe_i32", "s_bfe_u64", "s_bfe_i64",
            "sop2", "s_absdiff_i32", "s_lshl1_add_u32",
            "s_lshl2_add_u32", "s_lshl3_add_u32",
            "s_lshl4_add_u32", "s_pack_ll_b32_b16",
            "s_pack_lh_b32_b16", "s_pack_hh_b32_b16",
            "s_mul_hi_u32", "s_mul_hi_i32",
        };
        const auto opcode = (word >> 23) & 0x7Fu;
        return opcode < std::size(names) ? names[opcode] : "sop2";
    }
    if (encoding == ps5gpu::gen5::Encoding::Sopc) {
        static constexpr const char* names[] = {
            "s_cmp_eq_i32", "s_cmp_lg_i32", "s_cmp_gt_i32",
            "s_cmp_ge_i32", "s_cmp_lt_i32", "s_cmp_le_i32",
            "s_cmp_eq_u32", "s_cmp_lg_u32", "s_cmp_gt_u32",
            "s_cmp_ge_u32", "s_cmp_lt_u32", "s_cmp_le_u32",
            "s_bitcmp0_b32", "s_bitcmp1_b32", "s_bitcmp0_b64",
            "s_bitcmp1_b64",
        };
        const auto opcode = (word >> 16) & 0x7Fu;
        return opcode < std::size(names) ? names[opcode] : "sopc";
    }
    if (encoding == ps5gpu::gen5::Encoding::Sopp) {
        switch ((word >> 16) & 0x7Fu) {
        case 0x00: return "s_nop";
        case 0x01: return "s_endpgm";
        case 0x02: return "s_branch";
        case 0x04: return "s_cbranch_scc0";
        case 0x05: return "s_cbranch_scc1";
        case 0x06: return "s_cbranch_vccz";
        case 0x07: return "s_cbranch_vccnz";
        case 0x08: return "s_cbranch_execz";
        case 0x09: return "s_cbranch_execnz";
        case 0x0C: return "s_waitcnt";
        case 0x21: return "s_clause";
        case 0x23: return "s_waitcnt_depctr";
        default: return "sopp";
        }
    }
    if (encoding == ps5gpu::gen5::Encoding::Sopk) {
        return "sopk";
    }
    return "";
}

void trace_shader_resource_operands(
    const char* stage,
    const CapturedShader& shader) {
    if (shader.bytes.empty() ||
        shader.bytes.size() % sizeof(std::uint32_t) != 0) {
        return;
    }
    std::vector<std::uint32_t> words(
        shader.bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), shader.bytes.data(), shader.bytes.size());
    ShaderWordReader reader = {
        shader.record.address,
        &words,
    };

    for (std::size_t word_index = 0; word_index < words.size();) {
        const auto pc = static_cast<std::uint32_t>(
            word_index * sizeof(std::uint32_t));
        ps5gpu::gen5::InstructionInfo info;
        if (!ps5gpu::gen5::decode_instruction(
                read_shader_word,
                &reader,
                shader.record.address + pc,
                words[word_index],
                info) ||
            info.size_dwords == 0 ||
            word_index + info.size_dwords > words.size()) {
            std::cout
                << "state_resource stage=" << stage
                << " pc=0x" << std::hex << pc
                << " decode_failed=1 word=0x" << words[word_index]
                << std::dec << '\n';
            break;
        }

        const auto word = words[word_index];
        const auto extra = info.size_dwords >= 2
            ? words[word_index + 1]
            : 0u;
        if (info.encoding == ps5gpu::gen5::Encoding::Sop1 ||
            info.encoding == ps5gpu::gen5::Encoding::Sop2 ||
            info.encoding == ps5gpu::gen5::Encoding::Sopc ||
            info.encoding == ps5gpu::gen5::Encoding::Sopp ||
            info.encoding == ps5gpu::gen5::Encoding::Sopk) {
            std::cout
                << "state_scalar stage=" << stage
                << " pc=0x" << std::hex << pc
                << " word=0x" << word << std::dec
                << " opcode=" << scalar_opcode_name(info.encoding, word)
                << " src0=" << (word & 0xFFu)
                << " src1=" << ((word >> 8) & 0xFFu)
                << " dst=" << ((word >> 16) & 0x7Fu)
                << '\n';
        }
        switch (info.encoding) {
        case ps5gpu::gen5::Encoding::Smrd:
            std::cout
                << "state_resource stage=" << stage
                << " pc=0x" << std::hex << pc << std::dec
                << " encoding=smrd base=s"
                << (((word >> 9) & 0x3Fu) * 2u)
                << " destination=s" << ((word >> 15) & 0x7Fu)
                << '\n';
            break;
        case ps5gpu::gen5::Encoding::Smem:
            std::cout
                << "state_resource stage=" << stage
                << " pc=0x" << std::hex << pc
                << " word=0x" << word
                << " extra=0x" << extra << std::dec
                << " encoding=smem base=s"
                << ((word & 0x3Fu) * 2u)
                << " destination=s" << ((word >> 6) & 0x7Fu)
                << " opcode=0x" << std::hex
                << ((word >> 18) & 0xFFu) << std::dec
                << " offset=0x" << std::hex
                << (extra & 0x1FFFFFu) << std::dec
                << '\n';
            break;
        case ps5gpu::gen5::Encoding::Mubuf:
        case ps5gpu::gen5::Encoding::Mtbuf:
            std::cout
                << "state_resource stage=" << stage
                << " pc=0x" << std::hex << pc << std::dec
                << " encoding="
                << (info.encoding == ps5gpu::gen5::Encoding::Mubuf
                    ? "mubuf"
                    : "mtbuf")
                << " resource=s"
                << (((extra >> 16) & 0x1Fu) * 4u)
                << " scalar_offset=s" << ((extra >> 24) & 0xFFu)
                << " vector_address=v" << (extra & 0xFFu)
                << '\n';
            break;
        case ps5gpu::gen5::Encoding::Mimg:
            std::cout
                << "state_resource stage=" << stage
                << " pc=0x" << std::hex << pc << std::dec
                << " encoding=mimg resource=s"
                << (((extra >> 16) & 0x1Fu) * 4u)
                << " sampler=s"
                << (((extra >> 21) & 0x1Fu) * 4u)
                << " vector_address=v" << (extra & 0xFFu)
                << '\n';
            break;
        case ps5gpu::gen5::Encoding::Flat:
            std::cout
                << "state_resource stage=" << stage
                << " pc=0x" << std::hex << pc << std::dec
                << " encoding=flat scalar_address=s"
                << ((extra >> 16) & 0x7Fu)
                << " vector_address=v" << (extra & 0xFFu)
                << '\n';
            break;
        default:
            break;
        }
        word_index += info.size_dwords;
        if (info.end_program) {
            break;
        }
    }
}

std::optional<std::uint32_t> environment_u32(const char* name) {
    const auto* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    try {
        const auto parsed = std::stoull(value, nullptr, 0);
        if (parsed <= UINT32_MAX) {
            return static_cast<std::uint32_t>(parsed);
        }
    } catch (...) {
    }
    std::cerr << "Ignoring invalid " << name << '=' << value << '\n';
    return std::nullopt;
}

bool environment_flag_enabled(const char* name) {
    const auto* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    return std::strcmp(value, "1") == 0 ||
        std::strcmp(value, "true") == 0 ||
        std::strcmp(value, "on") == 0 ||
        std::strcmp(value, "yes") == 0;
}

std::uint32_t find_register_value(
    const std::vector<Ps5GpuNativeRegisterValue>& registers,
    std::uint32_t address) {
    const auto entry = std::lower_bound(
        registers.begin(),
        registers.end(),
        address,
        [](const Ps5GpuNativeRegisterValue& value,
           std::uint32_t key) {
            return value.address < key;
        });
    return entry != registers.end() && entry->address == address
        ? entry->value
        : 0;
}

std::uint32_t shader_user_data_count(
    const CapturedState& state,
    std::uint32_t base_register) {
    const auto rsrc2 = find_register_value(
        state.sh_registers,
        base_register - 1);
    auto count = (rsrc2 >> 1) & 0x1Fu;
    if ((base_register == 0x0Cu ||
         base_register == 0x4Cu ||
         base_register == 0x8Cu) &&
        (rsrc2 & (1u << 27)) != 0) {
        count |= 0x20u;
    }
    return std::min(count, 64u);
}

const CapturedMemory* find_captured_memory(
    const std::vector<CapturedMemory>& memory,
    std::uint64_t address) {
    const auto range = std::find_if(
        memory.begin(),
        memory.end(),
        [address](const CapturedMemory& candidate) {
            const auto end =
                candidate.record.address + candidate.record.byte_size;
            return address >= candidate.record.address && address < end;
        });
    return range == memory.end() ? nullptr : &*range;
}

void trace_shader_user_data(
    const char* stage,
    const CapturedState& state,
    std::uint32_t base_register,
    const std::vector<CapturedMemory>& memory) {
    const auto rsrc2 = find_register_value(
        state.sh_registers,
        base_register - 1);
    const auto count = shader_user_data_count(state, base_register);
    std::cout
        << "state_trace stage=" << stage
        << " ud_base=0x" << std::hex << base_register
        << " rsrc2=0x" << rsrc2 << std::dec
        << " ud_count=" << count << '\n';

    std::vector<std::uint32_t> user_data(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        user_data[index] = find_register_value(
            state.sh_registers,
            base_register + index);
        std::cout
            << "state_ud stage=" << stage
            << " s" << index
            << "=0x" << std::hex << std::setw(8)
            << std::setfill('0') << user_data[index]
            << std::setfill(' ') << std::dec << '\n';
    }

    for (std::size_t index = 0;
         index + 1 < user_data.size();
         ++index) {
        const auto address =
            static_cast<std::uint64_t>(user_data[index]) |
            (static_cast<std::uint64_t>(user_data[index + 1]) << 32);
        if (address < 0x10000) {
            continue;
        }
        const auto* range = find_captured_memory(memory, address);
        std::cout
            << "state_ptr stage=" << stage
            << " s" << index << ":s" << index + 1
            << "=0x" << std::hex << address << std::dec;
        if (range != nullptr) {
            const auto range_offset = static_cast<std::size_t>(
                address - range->record.address);
            std::cout
                << " captured=1 page=0x" << std::hex
                << range->record.address
                << " offset=0x"
                << range_offset
                << " flags=0x" << range->record.flags
                << std::dec;
            const auto available =
                range_offset < range->bytes.size()
                ? range->bytes.size() - range_offset
                : 0;
            const auto dword_count =
                std::min<std::size_t>(available / 4, 8);
            if (dword_count != 0) {
                std::cout << " words=";
                for (std::size_t word_index = 0;
                     word_index < dword_count;
                     ++word_index) {
                    std::uint32_t word = 0;
                    std::memcpy(
                        &word,
                        range->bytes.data() + range_offset +
                            word_index * sizeof(word),
                        sizeof(word));
                    if (word_index != 0) {
                        std::cout << ',';
                    }
                    std::cout
                        << "0x" << std::hex << std::setw(8)
                        << std::setfill('0') << word
                        << std::setfill(' ') << std::dec;
                }
            }
            if (std::string(stage) == "es" &&
                index == 0 &&
                dword_count >= 4) {
                for (std::size_t descriptor_index = 0;
                     descriptor_index < dword_count / 4;
                     ++descriptor_index) {
                    std::array<std::uint32_t, 4> descriptor = {};
                    std::memcpy(
                        descriptor.data(),
                        range->bytes.data() + range_offset +
                            descriptor_index * sizeof(descriptor),
                        sizeof(descriptor));
                    const auto buffer_address =
                        static_cast<std::uint64_t>(descriptor[0]) |
                        (static_cast<std::uint64_t>(
                             descriptor[1] & 0xFFFFu)
                         << 32);
                    const auto stride =
                        (descriptor[1] >> 16) & 0x3FFFu;
                    const auto records = descriptor[2];
                    const auto* buffer =
                        find_captured_memory(memory, buffer_address);
                    std::cout
                        << '\n'
                        << "state_buffer_descriptor stage=" << stage
                        << " index=" << descriptor_index
                        << " address=0x" << std::hex
                        << buffer_address << std::dec
                        << " stride=" << stride
                        << " records=" << records
                        << " format=0x" << std::hex
                        << descriptor[3] << std::dec
                        << " captured=" << (buffer != nullptr ? 1 : 0);
                    if (buffer != nullptr) {
                        const auto buffer_offset =
                            static_cast<std::size_t>(
                                buffer_address -
                                buffer->record.address);
                        const auto buffer_available =
                            buffer_offset < buffer->bytes.size()
                            ? buffer->bytes.size() - buffer_offset
                            : 0;
                        const auto buffer_words =
                            std::min<std::size_t>(
                                buffer_available / 4,
                                18);
                        std::cout << " words=";
                        for (std::size_t word_index = 0;
                             word_index < buffer_words;
                             ++word_index) {
                            std::uint32_t word = 0;
                            std::memcpy(
                                &word,
                                buffer->bytes.data() + buffer_offset +
                                    word_index * sizeof(word),
                                sizeof(word));
                            if (word_index != 0) {
                                std::cout << ',';
                            }
                            std::cout
                                << "0x" << std::hex << std::setw(8)
                                << std::setfill('0') << word
                                << std::setfill(' ') << std::dec;
                        }
                    }
                }
            }
        } else {
            std::cout << " captured=0";
        }
        std::cout << '\n';
    }

    if (std::string(stage) == "ps") {
        for (std::uint32_t slot = 0; slot < 3; ++slot) {
            const auto base = slot * 8;
            if (base + 3 >= user_data.size()) {
                break;
            }
            const auto word0 = user_data[base];
            const auto word1 = user_data[base + 1];
            const auto word2 = user_data[base + 2];
            const auto word3 = user_data[base + 3];
            const auto address =
                ((static_cast<std::uint64_t>(word1 & 0xFFu) << 32) |
                 word0) << 8;
            const auto width =
                (((word1 >> 30) & 0x3u) |
                 ((word2 & 0x3FFFu) << 2)) + 1;
            const auto height = ((word2 >> 14) & 0xFFFFu) + 1;
            const auto format = (word1 >> 20) & 0x1FFu;
            const auto tile_mode = (word3 >> 20) & 0x1Fu;
            const auto type = (word3 >> 28) & 0xFu;
            std::cout
                << "state_image slot=" << slot
                << " address=0x" << std::hex << address
                << " size=" << std::dec << width << 'x' << height
                << " format=" << format
                << " tile=" << tile_mode
                << " type=" << type << '\n';
        }
        if (user_data.size() >= 32) {
            const auto base = 28u;
            const auto buffer_address =
                static_cast<std::uint64_t>(user_data[base]) |
                (static_cast<std::uint64_t>(user_data[base + 1] & 0xFFFFu)
                 << 32);
            const auto stride =
                (user_data[base + 1] >> 16) & 0x3FFFu;
            const auto records = user_data[base + 2];
            const auto data_format =
                (user_data[base + 3] >> 12) & 0x7Fu;
            std::cout
                << "state_buffer address=0x" << std::hex
                << buffer_address
                << std::dec
                << " stride=" << stride
                << " records=" << records
                << " format=" << data_format << '\n';
        }
    }
}

void trace_captured_state(
    std::uint32_t state_id,
    const std::vector<CapturedState>& states,
    const std::vector<CapturedShader>& shaders,
    const std::vector<CapturedMemory>& memory,
    const std::vector<Ps5GpuNativeDraw>& draws) {
    const auto state = std::find_if(
        states.begin(),
        states.end(),
        [state_id](const CapturedState& candidate) {
            return candidate.record.state_id == state_id;
        });
    if (state == states.end()) {
        std::cerr << "Captured state not found: " << state_id << '\n';
        return;
    }

    std::map<std::uint64_t, std::uint32_t> targets;
    std::uint32_t draw_count = 0;
    for (const auto& draw : draws) {
        if ((draw.flags & PS5GPU_NATIVE_DRAW_SHADER_STATE_KNOWN) != 0 &&
            draw.reserved0 == state_id) {
            ++draw_count;
            ++targets[draw.render_target_address];
        }
    }

    std::cout
        << "state_trace id=" << state_id
        << " hash=0x" << std::hex << state->record.hash
        << " es=0x" << state->record.es_address
        << " ps=0x" << state->record.ps_address
        << " es_header=0x" << state->record.es_header_address
        << " ps_header=0x" << state->record.ps_header_address
        << " input_ena=0x" << state->record.pixel_input_enable
        << " input_addr=0x" << state->record.pixel_input_address
        << std::dec
        << " sh_regs=" << state->sh_registers.size()
        << " cx_regs=" << state->cx_registers.size()
        << " draws=" << draw_count << '\n';
    for (const auto& [address, count] : targets) {
        std::cout
            << "state_target address=0x" << std::hex << address
            << std::dec << " draws=" << count << '\n';
    }
    trace_shader_user_data(
        "es",
        *state,
        state->record.export_user_data_base_register,
        memory);
    trace_shader_user_data(
        "ps",
        *state,
        state->record.pixel_user_data_base_register,
        memory);
    for (const auto& shader : shaders) {
        if (shader.record.address == state->record.es_address &&
            shader.record.stage == PS5GPU_CAPTURE_SHADER_STAGE_EXPORT) {
            trace_shader_resource_operands("es", shader);
        }
        if (shader.record.address == state->record.ps_address &&
            shader.record.stage == PS5GPU_CAPTURE_SHADER_STAGE_PIXEL) {
            trace_shader_resource_operands("ps", shader);
        }
    }
}

void trace_spirv_layout(
    const std::uint8_t* bytes,
    std::size_t byte_size,
    const char* stage) {
    if (bytes == nullptr ||
        byte_size < 5 * sizeof(std::uint32_t) ||
        byte_size % sizeof(std::uint32_t) != 0) {
        return;
    }
    const auto* words =
        reinterpret_cast<const std::uint32_t*>(bytes);
    const auto word_count = byte_size / sizeof(std::uint32_t);
    if (words[0] != 0x07230203u) {
        return;
    }

    constexpr std::uint32_t op_name = 5;
    constexpr std::uint32_t op_type_int = 21;
    constexpr std::uint32_t op_type_float = 22;
    constexpr std::uint32_t op_type_vector = 23;
    constexpr std::uint32_t op_type_image = 25;
    constexpr std::uint32_t op_type_sampled_image = 27;
    constexpr std::uint32_t op_type_pointer = 32;
    constexpr std::uint32_t op_variable = 59;
    constexpr std::uint32_t op_decorate = 71;
    constexpr std::uint32_t decoration_binding = 33;
    constexpr std::uint32_t decoration_set = 34;
    constexpr std::uint32_t decoration_builtin = 11;
    constexpr std::uint32_t decoration_location = 30;
    constexpr std::uint32_t storage_uniform_constant = 0;
    constexpr std::uint32_t storage_input = 1;
    constexpr std::uint32_t storage_uniform = 2;
    constexpr std::uint32_t storage_output = 3;
    constexpr std::uint32_t storage_storage_buffer = 12;

    struct Variable {
        std::uint32_t type = 0;
        std::uint32_t storage = 0;
        std::string name;
    };
    struct ImageType {
        std::uint32_t sampled = 0;
    };
    std::map<std::uint32_t, Variable> variables;
    std::map<std::uint32_t, ImageType> image_types;
    std::map<std::uint32_t, std::uint32_t> sampled_image_types;
    std::map<std::uint32_t, std::uint32_t> pointer_types;
    std::map<std::uint32_t, std::uint32_t> bindings;
    std::map<std::uint32_t, std::uint32_t> sets;
    std::map<std::uint32_t, std::uint32_t> locations;
    std::map<std::uint32_t, std::uint32_t> builtins;
    std::map<std::uint32_t, std::uint32_t> vector_components;
    std::map<std::uint32_t, std::string> scalar_types;

    for (std::size_t offset = 5; offset < word_count;) {
        const auto instruction = words[offset];
        const auto count = instruction >> 16;
        const auto opcode = instruction & 0xFFFFu;
        if (count == 0 || offset + count > word_count) {
            break;
        }
        if (opcode == op_name && count >= 3) {
            const auto id = words[offset + 1];
            std::string name;
            bool terminated = false;
            for (std::size_t index = offset + 2;
                 index < offset + count && !terminated;
                 ++index) {
                const auto value = words[index];
                for (std::size_t byte = 0; byte < 4; ++byte) {
                    const auto character =
                        static_cast<char>((value >> (byte * 8)) & 0xFFu);
                    if (character == '\0') {
                        terminated = true;
                        break;
                    }
                    name.push_back(character);
                }
            }
            variables[id].name = std::move(name);
        } else if (opcode == op_type_int && count >= 4) {
            scalar_types[words[offset + 1]] =
                words[offset + 3] != 0 ? "sint" : "uint";
        } else if (opcode == op_type_float && count >= 3) {
            scalar_types[words[offset + 1]] = "float";
        } else if (opcode == op_type_vector && count >= 4) {
            vector_components[words[offset + 1]] = words[offset + 3];
            scalar_types[words[offset + 1]] =
                scalar_types[words[offset + 2]];
        } else if (opcode == op_type_image && count >= 8) {
            image_types[words[offset + 1]] = {words[offset + 7]};
        } else if (opcode == op_type_sampled_image && count >= 3) {
            sampled_image_types[words[offset + 1]] = words[offset + 2];
        } else if (opcode == op_type_pointer && count >= 4) {
            pointer_types[words[offset + 1]] = words[offset + 3];
        } else if (opcode == op_variable && count >= 4) {
            variables[words[offset + 2]].type = words[offset + 1];
            variables[words[offset + 2]].storage = words[offset + 3];
        } else if (opcode == op_decorate && count >= 4) {
            if (words[offset + 2] == decoration_binding) {
                bindings[words[offset + 1]] = words[offset + 3];
            } else if (words[offset + 2] == decoration_set) {
                sets[words[offset + 1]] = words[offset + 3];
            } else if (words[offset + 2] == decoration_location) {
                locations[words[offset + 1]] = words[offset + 3];
            } else if (words[offset + 2] == decoration_builtin) {
                builtins[words[offset + 1]] = words[offset + 3];
            }
        }
        offset += count;
    }

    for (const auto& [id, variable] : variables) {
        if (variable.storage != storage_input &&
            variable.storage != storage_output) {
            continue;
        }
        const auto pointer = pointer_types.find(variable.type);
        const auto object_type = pointer == pointer_types.end()
            ? variable.type
            : pointer->second;
        const auto components = vector_components.contains(object_type)
            ? vector_components[object_type]
            : 1u;
        const auto type = scalar_types.contains(object_type)
            ? scalar_types[object_type]
            : "?";
        std::cout
            << "spirv_interface stage=" << stage
            << " storage="
            << (variable.storage == storage_input ? "input" : "output")
            << " location="
            << (locations.contains(id)
                ? std::to_string(locations[id])
                : "-")
            << " builtin="
            << (builtins.contains(id)
                ? std::to_string(builtins[id])
                : "-")
            << " type=" << type
            << " components=" << components
            << " name=" << (variable.name.empty() ? "?" : variable.name)
            << '\n';
    }

    for (const auto& [id, variable] : variables) {
        const auto binding = bindings.find(id);
        const auto set = sets.find(id);
        if (binding == bindings.end() || set == sets.end()) {
            continue;
        }
        const auto pointer = pointer_types.find(variable.type);
        const auto object_type = pointer == pointer_types.end()
            ? variable.type
            : pointer->second;
        std::string kind = "other";
        if (sampled_image_types.contains(object_type)) {
            kind = "sampled-image";
        } else if (const auto image = image_types.find(object_type);
                   image != image_types.end()) {
            kind = image->second.sampled == 2
                ? "storage-image"
                : "image";
        } else if (variable.storage == storage_storage_buffer ||
                   variable.storage == storage_uniform) {
            kind = "storage-buffer";
        } else if (variable.storage == storage_uniform_constant) {
            kind = "uniform-constant";
        }
        std::cout
            << "spirv_binding stage=" << stage
            << " set=" << set->second
            << " binding=" << binding->second
            << " id=" << id
            << " kind=" << kind
            << " name=" << (variable.name.empty() ? "?" : variable.name)
            << '\n';
    }
}

struct GuestMemoryMappings {
    std::map<std::uintptr_t, void*> allocations;

    ~GuestMemoryMappings() {
        for (const auto& [address, allocation] : allocations) {
            (void)address;
            VirtualFree(allocation, 0, MEM_RELEASE);
        }
    }

    bool map_blob(
        std::uint64_t address,
        const std::uint8_t* bytes,
        std::size_t size) {
        if (address < 0x10000 || bytes == nullptr || size == 0) {
            return false;
        }
        SYSTEM_INFO system_info = {};
        GetSystemInfo(&system_info);
        const auto granularity = static_cast<std::uintptr_t>(
            system_info.dwAllocationGranularity);
        if (granularity == 0) {
            return false;
        }
        const auto start = static_cast<std::uintptr_t>(address);
        if (size > UINTPTR_MAX - start) {
            return false;
        }
        const auto end = start + size;
        const auto first = start & ~(granularity - 1);
        const auto last =
            (end + granularity - 1) & ~(granularity - 1);
        for (auto base = first; base < last; base += granularity) {
            if (allocations.contains(base)) {
                continue;
            }
            auto* allocation = VirtualAlloc(
                reinterpret_cast<void*>(base),
                granularity,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE);
            if (allocation != reinterpret_cast<void*>(base)) {
                if (allocation != nullptr) {
                    VirtualFree(allocation, 0, MEM_RELEASE);
                }
                std::cerr
                    << "Could not map guest memory page at 0x"
                    << std::hex << base << std::dec
                    << " error=" << GetLastError() << '\n';
                return false;
            }
            allocations.emplace(base, allocation);
        }
        std::memcpy(
            reinterpret_cast<void*>(start),
            bytes,
            size);
        return true;
    }

    bool map(
        const std::vector<CapturedShader>& shaders,
        const std::vector<CapturedMemory>& memory) {
        for (const auto& shader : shaders) {
            if (shader.bytes.size() != shader.record.byte_size ||
                !map_blob(
                    shader.record.address,
                    shader.bytes.data(),
                    shader.bytes.size())) {
                return false;
            }
        }
        for (const auto& range : memory) {
            if (range.bytes.size() != range.record.byte_size ||
                !map_blob(
                    range.record.address,
                    range.bytes.data(),
                    range.bytes.size())) {
                return false;
            }
        }
        return true;
    }
};

bool compile_captured_stage(
    Ps5GpuCompileSpirv compile_spirv,
    Ps5GpuFree free_bridge_memory,
    const CapturedState& state,
    Ps5GpuShaderStage stage,
    const std::filesystem::path& output_path,
    std::int32_t global_buffer_base,
    std::int32_t total_global_buffer_count,
    std::int32_t image_binding_base,
    std::int32_t required_vertex_output_count,
    bool write_output,
    std::uint32_t* global_memory_binding_count = nullptr,
    std::uint32_t* image_binding_count = nullptr) {
    std::vector<Ps5GpuRegisterValue> registers;
    registers.reserve(state.sh_registers.size());
    for (const auto& reg : state.sh_registers) {
        registers.push_back({reg.address, reg.value});
    }

    Ps5GpuShaderRequest request = {};
    request.struct_size = sizeof(request);
    request.abi_version = PS5GPU_ABI_VERSION;
    request.stage = stage;
    request.flags = 0;
    request.shader_address = stage == PS5GPU_STAGE_PIXEL
        ? state.record.ps_address
        : state.record.es_address;
    request.shader_header_address = stage == PS5GPU_STAGE_PIXEL
        ? state.record.ps_header_address
        : state.record.es_header_address;
    request.registers = registers.data();
    request.register_count =
        static_cast<std::uint32_t>(registers.size());
    request.user_data_base_register = stage == PS5GPU_STAGE_PIXEL
        ? state.record.pixel_user_data_base_register
        : state.record.export_user_data_base_register;
    request.user_data_scalar_register_base =
        stage == PS5GPU_STAGE_VERTEX ? 8u : 0u;
    request.wave_lane_count = 32;
    request.storage_buffer_offset_alignment = 1;
    request.pixel_input_enable =
        state.record.pixel_input_enable;
    request.pixel_input_address =
        state.record.pixel_input_address;
    request.global_buffer_base = global_buffer_base;
    request.total_global_buffer_count = total_global_buffer_count;
    request.image_binding_base = image_binding_base;
    request.initial_scalar_buffer_index = -1;
    request.required_vertex_output_count =
        required_vertex_output_count;
    request.compute_work_group_x_register = -1;
    request.compute_work_group_y_register = -1;
    request.compute_work_group_z_register = -1;
    request.compute_thread_group_size_register = -1;

    Ps5GpuShaderResult result = {};
    result.struct_size = sizeof(result);
    char error[2048] = {};
    const auto status = compile_spirv(
        &request,
        &result,
        error,
        sizeof(error));
    const auto* stage_name =
        stage == PS5GPU_STAGE_PIXEL ? "ps" : "es";
    if (status != PS5GPU_OK ||
        result.spirv == nullptr ||
        result.spirv_size == 0) {
        std::cerr
            << "bridge_compile stage=" << stage_name
            << " state=" << state.record.state_id
            << " shader=0x" << std::hex << request.shader_address
            << std::dec << " status=" << status
            << " error=" << (error[0] == '\0' ? "?" : error)
            << '\n';
        if (result.spirv != nullptr) {
            free_bridge_memory(result.spirv);
        }
        if (result.resource_manifest != nullptr) {
            free_bridge_memory(result.resource_manifest);
        }
        return false;
    }
    if (global_memory_binding_count != nullptr) {
        *global_memory_binding_count =
            result.global_memory_binding_count;
    }
    if (image_binding_count != nullptr) {
        *image_binding_count = result.image_binding_count;
    }

    auto wrote_output = true;
    auto wrote_resources = true;
    std::filesystem::path resource_path;
    if (write_output) {
        std::ofstream output(output_path, std::ios::binary);
        output.write(
            reinterpret_cast<const char*>(result.spirv),
            result.spirv_size);
        wrote_output = static_cast<bool>(output);

        resource_path = output_path;
        resource_path.replace_extension(".resources.bin");
        std::ofstream resources(resource_path, std::ios::binary);
        if (result.resource_manifest == nullptr ||
            result.resource_manifest_size <
                sizeof(Ps5GpuResourceManifestHeader)) {
            wrote_resources = false;
        } else {
            resources.write(
                reinterpret_cast<const char*>(
                    result.resource_manifest),
                result.resource_manifest_size);
            wrote_resources = static_cast<bool>(resources);
        }
    }
    std::cout
        << "bridge_compile stage=" << stage_name
        << " state=" << state.record.state_id
        << " shader=0x" << std::hex << request.shader_address
        << std::dec
        << " bytes=" << result.spirv_size
        << " attributes=" << result.attribute_count
        << " globals=" << result.global_memory_binding_count
        << " images=" << result.image_binding_count
        << " vertex_inputs=" << result.vertex_input_count
        << " manifest_bytes=" << result.resource_manifest_size
        << " output=" << (write_output
            ? output_path.string()
            : std::string("<probe>"))
        << " wrote=" << (wrote_output ? 1 : 0)
        << " resources=" << (write_output
            ? resource_path.string()
            : std::string("<probe>"))
        << " wrote_resources=" << (wrote_resources ? 1 : 0)
        << '\n';
    trace_spirv_layout(
        result.spirv,
        result.spirv_size,
        stage_name);
    free_bridge_memory(result.spirv);
    free_bridge_memory(result.resource_manifest);
    return wrote_output && wrote_resources;
}

bool compile_captured_states(
    std::optional<std::uint32_t> state_id,
    const std::vector<CapturedState>& states,
    const std::filesystem::path& bridge_path,
    const std::filesystem::path& output_directory) {
    const auto bridge = LoadLibraryW(bridge_path.c_str());
    if (bridge == nullptr) {
        std::cerr
            << "Could not load GPU bridge: " << bridge_path
            << " error=" << GetLastError() << '\n';
        return false;
    }
    const auto get_abi = load_export<Ps5GpuGetAbiVersion>(
        bridge,
        "ps5gpu_get_abi_version");
    const auto compile_spirv = load_export<Ps5GpuCompileSpirv>(
        bridge,
        "ps5gpu_compile_spirv");
    const auto free_bridge_memory = load_export<Ps5GpuFree>(
        bridge,
        "ps5gpu_free");
    if (get_abi == nullptr ||
        compile_spirv == nullptr ||
        free_bridge_memory == nullptr ||
        get_abi() != PS5GPU_ABI_VERSION) {
        std::cerr << "GPU bridge exports or ABI are incompatible\n";
        FreeLibrary(bridge);
        return false;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(
        output_directory,
        directory_error);
    std::size_t selected_states = 0;
    std::size_t compiled_stages = 0;
    std::size_t failed_stages = 0;
    for (const auto& state : states) {
        if (state_id.has_value() &&
            state.record.state_id != *state_id) {
            continue;
        }
        ++selected_states;
        const auto stem =
            output_directory /
            ("state-" + std::to_string(state.record.state_id));
        std::uint32_t es_global_count = 0;
        std::uint32_t ps_global_count = 0;
        std::uint32_t es_image_count = 0;
        std::uint32_t ps_image_count = 0;
        const auto es_probe_ok = compile_captured_stage(
            compile_spirv,
            free_bridge_memory,
            state,
            PS5GPU_STAGE_VERTEX,
            std::filesystem::path(stem.string() + "-es.spv"),
            0,
            -1,
            0,
            1,
            false,
            &es_global_count,
            &es_image_count);
        const auto ps_probe_ok = compile_captured_stage(
            compile_spirv,
            free_bridge_memory,
            state,
            PS5GPU_STAGE_PIXEL,
            std::filesystem::path(stem.string() + "-ps.spv"),
            0,
            -1,
            0,
            -1,
            false,
            &ps_global_count,
            &ps_image_count);
        const auto total_global_count =
            static_cast<std::int32_t>(
                es_global_count + ps_global_count);
        const auto es_ok =
            es_probe_ok &&
            compile_captured_stage(
                compile_spirv,
                free_bridge_memory,
                state,
                PS5GPU_STAGE_VERTEX,
                std::filesystem::path(stem.string() + "-es.spv"),
                0,
                total_global_count,
                0,
                1,
                true);
        const auto ps_ok =
            ps_probe_ok &&
            compile_captured_stage(
                compile_spirv,
                free_bridge_memory,
                state,
                PS5GPU_STAGE_PIXEL,
                std::filesystem::path(stem.string() + "-ps.spv"),
                static_cast<std::int32_t>(es_global_count),
                total_global_count,
                static_cast<std::int32_t>(es_image_count),
                -1,
                true);
        compiled_stages += static_cast<std::size_t>(es_ok) +
            static_cast<std::size_t>(ps_ok);
        failed_stages += static_cast<std::size_t>(!es_ok) +
            static_cast<std::size_t>(!ps_ok);
    }
    FreeLibrary(bridge);
    if (selected_states == 0) {
        if (state_id.has_value()) {
            std::cerr
                << "Captured state not found: " << *state_id << '\n';
        }
        return false;
    }
    std::cout
        << "bridge_compile_summary states=" << selected_states
        << " compiled_stages=" << compiled_stages
        << " failed_stages=" << failed_stages
        << " output=" << output_directory
        << '\n';
    return compiled_stages != 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        std::cerr
            << "Usage: Ps5GpuReplay.exe <capture.bin> "
               "[Ps5GpuRuntime.dll] [target-address]\n";
        return 1;
    }

    const std::filesystem::path capture_path = argv[1];
    const auto runtime_path = argc >= 3
        ? std::filesystem::path(argv[2])
        : executable_directory() / "Ps5GpuRuntime.dll";
    std::ifstream capture(capture_path, std::ios::binary);
    if (!capture) {
        std::cerr << "Could not open capture: " << capture_path << '\n';
        return 2;
    }
    // A capture that was cut short - the probe is killed the moment its flip
    // arrives, and the file runs to hundreds of megabytes - used to surface as
    // whatever record happened to straddle the end, which pointed at the wrong
    // problem. Say plainly that the file is short.
    std::error_code size_error;
    const auto capture_bytes =
        std::filesystem::file_size(capture_path, size_error);
    if (size_error) {
        std::cerr << "Could not size capture: " << capture_path << '\n';
        return 2;
    }

    Ps5GpuCaptureHeaderV1 header_v1 = {};
    capture.read(reinterpret_cast<char*>(&header_v1), sizeof(header_v1));
    if (!capture ||
        header_v1.magic != PS5GPU_CAPTURE_MAGIC ||
        header_v1.draw_size != sizeof(Ps5GpuNativeDraw) ||
        header_v1.flip_size != sizeof(Ps5GpuNativeFlip) ||
        header_v1.draw_count > 1'000'000) {
        std::cerr << "Invalid or incompatible GPU capture\n";
        return 3;
    }
    Ps5GpuCaptureHeader header = {};
    Ps5GpuCaptureHeaderV4 header_v4 = {};
    std::vector<CapturedShader> shaders;
    std::vector<CapturedState> states;
    std::vector<CapturedMemory> memory;
    std::vector<CapturedComputeState> compute_states;
    std::vector<Ps5GpuCaptureCommand> commands;
    if (header_v1.version == PS5GPU_CAPTURE_VERSION_LEGACY &&
        header_v1.header_size == sizeof(header_v1)) {
        std::memcpy(&header, &header_v1, sizeof(header_v1));
    } else if (
        header_v1.version == PS5GPU_CAPTURE_VERSION_SHADER_ONLY &&
        header_v1.header_size == sizeof(Ps5GpuCaptureHeaderV2)) {
        Ps5GpuCaptureHeaderV2 header_v2 = {};
        std::memcpy(&header_v2, &header_v1, sizeof(header_v1));
        capture.read(
            reinterpret_cast<char*>(&header_v2) + sizeof(header_v1),
            sizeof(header_v2) - sizeof(header_v1));
        if (!capture) {
            std::cerr << "GPU capture header is truncated\n";
            return 4;
        }
        std::memcpy(&header, &header_v2, sizeof(header_v2));
    } else if (header_v1.version ==
                   PS5GPU_CAPTURE_VERSION_GRAPHICS &&
               header_v1.header_size == sizeof(header)) {
        std::memcpy(&header, &header_v1, sizeof(header_v1));
        capture.read(
            reinterpret_cast<char*>(&header) + sizeof(header_v1),
            sizeof(header) - sizeof(header_v1));
        if (!capture) {
            std::cerr << "GPU capture header is truncated\n";
            return 4;
        }
    } else if ((header_v1.version == PS5GPU_CAPTURE_VERSION_COMPUTE ||
                header_v1.version == PS5GPU_CAPTURE_VERSION_MULTIFLIP) &&
               header_v1.header_size == sizeof(header_v4)) {
        // Version 5 keeps the version 4 header and differs only in carrying
        // flips inside the command stream, so the same parse serves both.
        std::memcpy(&header_v4, &header_v1, sizeof(header_v1));
        capture.read(
            reinterpret_cast<char*>(&header_v4) + sizeof(header_v1),
            sizeof(header_v4) - sizeof(header_v1));
        if (!capture) {
            std::cerr << "GPU capture header is truncated\n";
            return 4;
        }
        header = header_v4.base;
    } else {
        std::cerr << "Unsupported GPU capture version\n";
        return 3;
    }

    // Everything the header promises, checked against what is on disk, before
    // any of it is parsed.
    const std::uint64_t declared_bytes =
        header.header_size +
        static_cast<std::uint64_t>(header.shader_count) *
            header.shader_record_size +
        header.shader_bytes +
        static_cast<std::uint64_t>(header.state_count) *
            header.state_record_size +
        header.state_register_bytes +
        static_cast<std::uint64_t>(header.memory_count) *
            header.memory_record_size +
        header.memory_bytes +
        header.draw_count * header.draw_size +
        static_cast<std::uint64_t>(header_v4.compute_state_count) *
            header_v4.compute_state_record_size +
        header_v4.compute_state_payload_bytes +
        header_v4.command_bytes +
        header.flip_size;
    if (capture_bytes < declared_bytes) {
        std::cerr << "GPU capture is truncated: file is " << capture_bytes
                  << " bytes, header describes " << declared_bytes
                  << ". The probe was most likely killed while writing it.\n";
        return 4;
    }

    if (header.version >= PS5GPU_CAPTURE_VERSION_SHADER_ONLY) {
        if (header.shader_record_size != sizeof(Ps5GpuCaptureShader) ||
            header.shader_count > kMaximumCaptureRecords ||
            header.shader_bytes > (512ULL * 1024ULL * 1024ULL)) {
            std::cerr << "Invalid GPU shader capture table\n";
            return 3;
        }
        shaders.resize(header.shader_count);
        std::uint64_t shader_bytes = 0;
        for (auto& shader : shaders) {
            capture.read(
                reinterpret_cast<char*>(&shader.record),
                sizeof(shader.record));
            if (!capture ||
                shader.record.byte_size > (512u * 1024u) ||
                shader.record.byte_size % sizeof(std::uint32_t) != 0) {
                std::cerr << "Invalid GPU shader record\n";
                return 3;
            }
            shader.bytes.resize(shader.record.byte_size);
            if (!shader.bytes.empty()) {
                capture.read(
                    reinterpret_cast<char*>(shader.bytes.data()),
                    static_cast<std::streamsize>(shader.bytes.size()));
            }
            shader_bytes += shader.bytes.size();
        }
        if (!capture || shader_bytes != header.shader_bytes) {
            std::cerr << "GPU shader capture is truncated\n";
            return 4;
        }
    }

    if (header.version >= PS5GPU_CAPTURE_VERSION_GRAPHICS) {
        if (header.state_record_size !=
                sizeof(Ps5GpuCaptureShaderState) ||
            header.state_count > kMaximumCaptureRecords ||
            header.state_register_bytes >
                (512ULL * 1024ULL * 1024ULL) ||
            header.memory_record_size != sizeof(Ps5GpuCaptureMemory) ||
            header.memory_count > kMaximumCaptureRecords ||
            header.memory_bytes > (2ULL * 1024ULL * 1024ULL * 1024ULL)) {
            std::cerr << "Invalid GPU state or memory capture table\n";
            return 3;
        }
        states.resize(header.state_count);
        std::uint64_t register_bytes = 0;
        for (auto& state : states) {
            capture.read(
                reinterpret_cast<char*>(&state.record),
                sizeof(state.record));
            if (!capture ||
                state.record.state_id == 0 ||
                state.record.sh_register_count > 4096 ||
                state.record.cx_register_count > 8192) {
                std::cerr << "Invalid GPU shader state record\n";
                return 3;
            }
            state.sh_registers.resize(
                state.record.sh_register_count);
            state.cx_registers.resize(
                state.record.cx_register_count);
            if (!state.sh_registers.empty()) {
                capture.read(
                    reinterpret_cast<char*>(
                        state.sh_registers.data()),
                    static_cast<std::streamsize>(
                        state.sh_registers.size() *
                        sizeof(Ps5GpuNativeRegisterValue)));
            }
            if (!state.cx_registers.empty()) {
                capture.read(
                    reinterpret_cast<char*>(
                        state.cx_registers.data()),
                    static_cast<std::streamsize>(
                        state.cx_registers.size() *
                        sizeof(Ps5GpuNativeRegisterValue)));
            }
            register_bytes +=
                (state.sh_registers.size() +
                 state.cx_registers.size()) *
                sizeof(Ps5GpuNativeRegisterValue);
        }
        if (!capture ||
            register_bytes != header.state_register_bytes) {
            std::cerr << "GPU shader state capture is truncated\n";
            return 4;
        }

        memory.resize(header.memory_count);
        std::uint64_t memory_bytes = 0;
        for (auto& range : memory) {
            capture.read(
                reinterpret_cast<char*>(&range.record),
                sizeof(range.record));
            if (!capture ||
                range.record.address < 0x10000 ||
                range.record.byte_size == 0 ||
                range.record.byte_size > (16u * 1024u * 1024u)) {
                std::cerr << "Invalid GPU memory capture record\n";
                return 3;
            }
            range.bytes.resize(range.record.byte_size);
            capture.read(
                reinterpret_cast<char*>(range.bytes.data()),
                static_cast<std::streamsize>(range.bytes.size()));
            memory_bytes += range.bytes.size();
        }
        if (!capture || memory_bytes != header.memory_bytes) {
            std::cerr << "GPU memory capture is truncated\n";
            return 4;
        }
    }

    std::vector<Ps5GpuNativeDraw> draws(
        static_cast<std::size_t>(header.draw_count));
    if (!draws.empty()) {
        capture.read(
            reinterpret_cast<char*>(draws.data()),
            static_cast<std::streamsize>(
                draws.size() * sizeof(Ps5GpuNativeDraw)));
    }
    if (header.version >= PS5GPU_CAPTURE_VERSION_COMPUTE) {
        if (header_v4.compute_state_record_size !=
                sizeof(Ps5GpuCaptureComputeState) ||
            header_v4.compute_state_count > kMaximumCaptureRecords ||
            header_v4.compute_state_payload_bytes >
                (8ULL * 1024ULL * 1024ULL * 1024ULL) ||
            header_v4.command_record_size !=
                sizeof(Ps5GpuCaptureCommand) ||
            header_v4.command_count > 1'000'000 ||
            header_v4.command_bytes !=
                static_cast<std::uint64_t>(
                    header_v4.command_count) *
                    sizeof(Ps5GpuCaptureCommand)) {
            std::cerr << "Invalid GPU compute capture table\n";
            return 3;
        }
        compute_states.resize(header_v4.compute_state_count);
        std::uint64_t compute_state_payload_bytes = 0;
        for (auto& state : compute_states) {
            capture.read(
                reinterpret_cast<char*>(&state.record),
                sizeof(state.record));
            if (!capture ||
                state.record.state_id == 0 ||
                state.record.spirv_size <
                    5 * sizeof(std::uint32_t) ||
                state.record.spirv_size > (64u * 1024u * 1024u) ||
                state.record.spirv_size % sizeof(std::uint32_t) != 0 ||
                state.record.resource_manifest_size <
                    sizeof(Ps5GpuResourceManifestHeader) ||
                state.record.resource_manifest_size >
                    (256u * 1024u * 1024u)) {
                std::cerr << "Invalid GPU compute state record\n";
                return 3;
            }
            state.spirv.resize(state.record.spirv_size);
            state.resource_manifest.resize(
                state.record.resource_manifest_size);
            capture.read(
                reinterpret_cast<char*>(state.spirv.data()),
                static_cast<std::streamsize>(state.spirv.size()));
            capture.read(
                reinterpret_cast<char*>(
                    state.resource_manifest.data()),
                static_cast<std::streamsize>(
                    state.resource_manifest.size()));
            compute_state_payload_bytes +=
                state.spirv.size() + state.resource_manifest.size();
        }
        if (!capture ||
            compute_state_payload_bytes !=
                header_v4.compute_state_payload_bytes) {
            std::cerr << "GPU compute state capture is truncated\n";
            return 4;
        }
        commands.resize(header_v4.command_count);
        if (!commands.empty()) {
            capture.read(
                reinterpret_cast<char*>(commands.data()),
                static_cast<std::streamsize>(header_v4.command_bytes));
        }
        for (const auto& command : commands) {
            const auto valid_draw =
                command.type == PS5GPU_CAPTURE_COMMAND_DRAW &&
                command.payload_size == sizeof(Ps5GpuNativeDraw);
            const auto valid_compute =
                command.type == PS5GPU_CAPTURE_COMMAND_COMPUTE &&
                command.payload_size ==
                    sizeof(Ps5GpuNativeComputeDispatch);
            const auto valid_flip =
                command.type == PS5GPU_CAPTURE_COMMAND_FLIP &&
                command.payload_size == sizeof(Ps5GpuNativeFlip);
            if (!valid_draw && !valid_compute && !valid_flip) {
                std::cerr << "Invalid GPU capture command\n";
                return 3;
            }
        }
        if (!capture) {
            std::cerr << "GPU command capture is truncated\n";
            return 4;
        }
    }
    Ps5GpuNativeFlip flip = {};
    capture.read(reinterpret_cast<char*>(&flip), sizeof(flip));
    if (!capture) {
        std::cerr << "GPU capture is truncated\n";
        return 4;
    }
    // The graphics states' modules, when the capture carries them.
    {
        std::uint32_t magic = 0;
        std::uint32_t count = 0;
        capture.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        capture.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (capture && magic == PS5GPU_CAPTURE_GRAPHICS_PAYLOAD_MAGIC &&
            count <= kMaximumCaptureRecords) {
            std::map<std::uint32_t, CapturedState*> by_id;
            for (auto& state : states) {
                by_id[state.record.state_id] = &state;
            }
            std::uint32_t attached = 0;
            for (std::uint32_t index = 0; index < count; ++index) {
                Ps5GpuCaptureGraphicsPayload record = {};
                capture.read(
                    reinterpret_cast<char*>(&record), sizeof(record));
                if (!capture ||
                    record.es_spirv_size > (64u << 20) ||
                    record.ps_spirv_size > (64u << 20) ||
                    record.es_manifest_size > (256u << 20) ||
                    record.ps_manifest_size > (256u << 20)) {
                    std::cerr << "GPU graphics payload is truncated\n";
                    return 4;
                }
                std::vector<std::uint8_t> es_spirv(record.es_spirv_size);
                std::vector<std::uint8_t> es_manifest(record.es_manifest_size);
                std::vector<std::uint8_t> ps_spirv(record.ps_spirv_size);
                std::vector<std::uint8_t> ps_manifest(record.ps_manifest_size);
                for (auto* part : {&es_spirv, &es_manifest, &ps_spirv,
                                   &ps_manifest}) {
                    capture.read(
                        reinterpret_cast<char*>(part->data()),
                        static_cast<std::streamsize>(part->size()));
                }
                if (!capture) {
                    std::cerr << "GPU graphics payload is truncated\n";
                    return 4;
                }
                const auto found = by_id.find(record.state_id);
                if (found != by_id.end()) {
                    found->second->es_spirv = std::move(es_spirv);
                    found->second->es_manifest = std::move(es_manifest);
                    found->second->ps_spirv = std::move(ps_spirv);
                    found->second->ps_manifest = std::move(ps_manifest);
                    ++attached;
                }
            }
            std::cerr << "graphics_payloads=" << attached << "/" << count
                      << "\n";
        }
        capture.clear();
    }
    if (const auto state_id =
            environment_u32("PS5GPU_REPLAY_TRACE_STATE")) {
        trace_captured_state(*state_id, states, shaders, memory, draws);
    }
    // Kept beyond the block below so the flips carried in the command stream
    // can be repointed the same way the trailing one is.
    std::uint64_t target_address = 0;
    if (argc == 4) {
        std::uint64_t target = 0;
        try {
            target = std::stoull(argv[3], nullptr, 0);
        } catch (...) {
            std::cerr << "Invalid target address: " << argv[3] << '\n';
            return 4;
        }
        const auto selected = std::find_if(
            draws.rbegin(),
            draws.rend(),
            [target](const Ps5GpuNativeDraw& draw) {
                return draw.render_target_address == target;
            });
        if (selected == draws.rend()) {
            std::cerr
                << "Target is not referenced by this capture: 0x"
                << std::hex << target << std::dec << '\n';
            return 4;
        }
        target_address = target;
        flip.display_address = target;
        flip.width = selected->render_target_width;
        flip.height = selected->render_target_height;
        flip.pitch_in_pixels = selected->render_target_width;
        flip.tiling_mode = selected->render_target_tile_mode;
    }

    GuestMemoryMappings guest_mappings;
    if (!guest_mappings.map(shaders, memory)) {
        return 5;
    }
    const auto compile_state =
        environment_u32("PS5GPU_REPLAY_COMPILE_STATE");
    const auto compile_all =
        environment_flag_enabled("PS5GPU_REPLAY_COMPILE_ALL");
    if (compile_state.has_value() || compile_all) {
        const auto* bridge_environment =
            std::getenv("PS5GPU_REPLAY_BRIDGE");
        const auto bridge_path =
            bridge_environment != nullptr &&
                bridge_environment[0] != '\0'
            ? std::filesystem::path(bridge_environment)
            : runtime_path.parent_path() / "Ps5GpuBridge.dll";
        const auto* output_environment =
            std::getenv("PS5GPU_REPLAY_SHADER_OUTPUT_DIR");
        const auto output_directory =
            output_environment != nullptr &&
                output_environment[0] != '\0'
            ? std::filesystem::path(output_environment)
            : capture_path.parent_path();
        const auto compile_ok = compile_captured_states(
            compile_all ? std::nullopt : compile_state,
            states,
            bridge_path,
            output_directory);
        if (environment_flag_enabled("PS5GPU_REPLAY_COMPILE_ONLY")) {
            return compile_ok ? 0 : 7;
        }
    }

    const auto module = LoadLibraryW(runtime_path.c_str());
    if (module == nullptr) {
        std::cerr << "Could not load runtime: " << runtime_path
                  << " error=" << GetLastError() << '\n';
        return 5;
    }
    const auto get_abi = load_export<Ps5GpuNativeGetAbiVersion>(
        module,
        "ps5gpu_native_get_abi_version");
    const auto create = load_export<Ps5GpuNativeCreate>(
        module,
        "ps5gpu_native_create");
    const auto destroy = load_export<Ps5GpuNativeDestroy>(
        module,
        "ps5gpu_native_destroy");
    const auto register_shader_state =
        load_export<Ps5GpuNativeRegisterShaderState>(
            module,
            "ps5gpu_native_register_shader_state");
    const auto register_compute_state =
        load_export<Ps5GpuNativeRegisterComputeState>(
            module,
            "ps5gpu_native_register_compute_state");
    const auto submit_draw = load_export<Ps5GpuNativeSubmitDraw>(
        module,
        "ps5gpu_native_submit_draw");
    const auto submit_compute = load_export<Ps5GpuNativeSubmitCompute>(
        module,
        "ps5gpu_native_submit_compute");
    const auto submit_flip = load_export<Ps5GpuNativeSubmitFlip>(
        module,
        "ps5gpu_native_submit_flip");
    const auto flush = load_export<Ps5GpuNativeFlush>(
        module,
        "ps5gpu_native_flush");
    const auto get_stats = load_export<Ps5GpuNativeGetStats>(
        module,
        "ps5gpu_native_get_stats");
    if (get_abi == nullptr ||
        create == nullptr ||
        destroy == nullptr ||
        register_shader_state == nullptr ||
        register_compute_state == nullptr ||
        submit_draw == nullptr ||
        submit_compute == nullptr ||
        submit_flip == nullptr ||
        flush == nullptr ||
        get_stats == nullptr ||
        get_abi() != PS5GPU_NATIVE_ABI_VERSION) {
        std::cerr << "Runtime exports or ABI are incompatible\n";
        FreeLibrary(module);
        return 6;
    }

    Ps5GpuNativeCreateInfo create_info = {};
    create_info.struct_size = sizeof(create_info);
    create_info.abi_version = PS5GPU_NATIVE_ABI_VERSION;
    create_info.queue_capacity = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(
            commands.empty() ? header.draw_count + 8 :
                commands.size() + 8,
            64));
    Ps5GpuNativeHandle runtime = nullptr;
    auto result = create(&create_info, &runtime);
    if (result != PS5GPU_NATIVE_OK || runtime == nullptr) {
        std::cerr << "Could not create runtime: " << result << '\n';
        FreeLibrary(module);
        return 7;
    }

    std::map<std::uint32_t, std::uint32_t> state_ids;
    for (const auto& captured_state : states) {
        Ps5GpuNativeShaderState state = {};
        state.struct_size = sizeof(state);
        state.abi_version = PS5GPU_NATIVE_ABI_VERSION;
        state.flags = captured_state.record.flags;
        state.es_address = captured_state.record.es_address;
        state.ps_address = captured_state.record.ps_address;
        state.es_header_address =
            captured_state.record.es_header_address;
        state.ps_header_address =
            captured_state.record.ps_header_address;
        state.export_user_data_base_register =
            captured_state.record.export_user_data_base_register;
        state.pixel_user_data_base_register =
            captured_state.record.pixel_user_data_base_register;
        state.pixel_input_enable =
            captured_state.record.pixel_input_enable;
        state.pixel_input_address =
            captured_state.record.pixel_input_address;
        state.sh_registers = captured_state.sh_registers.data();
        state.sh_register_count = static_cast<std::uint32_t>(
            captured_state.sh_registers.size());
        state.cx_registers = captured_state.cx_registers.data();
        state.cx_register_count = static_cast<std::uint32_t>(
            captured_state.cx_registers.size());
        if (!captured_state.es_spirv.empty() &&
            !captured_state.ps_spirv.empty()) {
            state.es_spirv = captured_state.es_spirv.data();
            state.es_spirv_size = static_cast<std::uint32_t>(
                captured_state.es_spirv.size());
            state.es_resource_manifest = captured_state.es_manifest.data();
            state.es_resource_manifest_size = static_cast<std::uint32_t>(
                captured_state.es_manifest.size());
            state.ps_spirv = captured_state.ps_spirv.data();
            state.ps_spirv_size = static_cast<std::uint32_t>(
                captured_state.ps_spirv.size());
            state.ps_resource_manifest = captured_state.ps_manifest.data();
            state.ps_resource_manifest_size = static_cast<std::uint32_t>(
                captured_state.ps_manifest.size());
        }
        std::uint32_t runtime_state_id = 0;
        result = register_shader_state(
            runtime,
            &state,
            &runtime_state_id);
        if (result != PS5GPU_NATIVE_OK || runtime_state_id == 0) {
            std::cerr << "Shader state registration failed at "
                      << captured_state.record.state_id
                      << ": " << result << '\n';
            destroy(runtime);
            FreeLibrary(module);
            return 8;
        }
        state_ids[captured_state.record.state_id] = runtime_state_id;
    }

    std::map<std::uint32_t, std::uint32_t> compute_state_ids;
    for (const auto& captured_state : compute_states) {
        Ps5GpuNativeComputeState state = {};
        state.struct_size = sizeof(state);
        state.abi_version = PS5GPU_NATIVE_ABI_VERSION;
        state.flags = captured_state.record.flags;
        state.state_hash = captured_state.record.state_hash;
        state.shader_address = captured_state.record.shader_address;
        state.shader_header_address =
            captured_state.record.shader_header_address;
        state.spirv = captured_state.spirv.data();
        state.spirv_size = static_cast<std::uint32_t>(
            captured_state.spirv.size());
        state.resource_manifest =
            captured_state.resource_manifest.data();
        state.resource_manifest_size = static_cast<std::uint32_t>(
            captured_state.resource_manifest.size());
        std::uint32_t runtime_state_id = 0;
        result = register_compute_state(
            runtime,
            &state,
            &runtime_state_id);
        if (result != PS5GPU_NATIVE_OK || runtime_state_id == 0) {
            std::cerr << "Compute state registration failed at "
                      << captured_state.record.state_id
                      << ": " << result << '\n';
            destroy(runtime);
            FreeLibrary(module);
            return 8;
        }
        compute_state_ids[captured_state.record.state_id] =
            runtime_state_id;
    }

    const auto replay_draw = [&](const Ps5GpuNativeDraw& captured_draw) {
        auto draw = captured_draw;
        draw.struct_size = sizeof(draw);
        draw.abi_version = PS5GPU_NATIVE_ABI_VERSION;
        if ((draw.flags &
             PS5GPU_NATIVE_DRAW_SHADER_STATE_KNOWN) != 0) {
            const auto state = state_ids.find(draw.reserved0);
            if (state == state_ids.end()) {
                std::cerr << "Draw references missing shader state "
                          << draw.reserved0 << '\n';
                return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
            }
            draw.reserved0 = state->second;
        }
        return submit_draw(runtime, &draw);
    };
    const auto replay_compute = [&] (
        const Ps5GpuNativeComputeDispatch& captured_dispatch) {
        auto dispatch = captured_dispatch;
        dispatch.struct_size = sizeof(dispatch);
        dispatch.abi_version = PS5GPU_NATIVE_ABI_VERSION;
        const auto state = compute_state_ids.find(
            dispatch.compute_state_id);
        if (state == compute_state_ids.end()) {
            std::cerr << "Compute dispatch references missing state "
                      << dispatch.compute_state_id << '\n';
            return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
        }
        dispatch.compute_state_id = state->second;
        // An indirect dispatch reaches the capture with its group counts
        // still in GPU memory, as zeros. The runtime refuses it, the live
        // run skipped it too, and ending the replay over it loses the frame.
        if (dispatch.group_count_x == 0 || dispatch.group_count_y == 0 ||
            dispatch.group_count_z == 0) {
            std::cerr << "Compute dispatch with no groups skipped, state "
                      << captured_dispatch.compute_state_id << '\n';
            return PS5GPU_NATIVE_OK;
        }
        return submit_compute(runtime, &dispatch);
    };

    std::uint64_t replay_draw_count = 0;
    std::uint64_t replay_flip_count = 0;
    std::uint64_t replay_compute_count = 0;
    if (commands.empty()) {
        for (const auto& draw : draws) {
            result = replay_draw(draw);
            if (result != PS5GPU_NATIVE_OK) {
                std::cerr << "Draw submission failed at "
                          << draw.total_draw_id << ": " << result << '\n';
                destroy(runtime);
                FreeLibrary(module);
                return 8;
            }
            ++replay_draw_count;
        }
    } else {
        for (const auto& command : commands) {
            if (command.type == PS5GPU_CAPTURE_COMMAND_DRAW) {
                result = replay_draw(command.payload.draw);
                ++replay_draw_count;
            } else if (command.type == PS5GPU_CAPTURE_COMMAND_FLIP) {
                // Flips ride in the stream from version 5 on, so a capture can
                // span several frames and replay reproduces them in order.
                auto stream_flip = command.payload.flip;
                stream_flip.struct_size = sizeof(stream_flip);
                stream_flip.abi_version = PS5GPU_NATIVE_ABI_VERSION;
                if (target_address != 0) {
                    stream_flip.display_address = target_address;
                }
                result = submit_flip(runtime, &stream_flip);
                ++replay_flip_count;
                flip = stream_flip;
            } else {
                result = replay_compute(command.payload.compute);
                ++replay_compute_count;
            }
            if (result != PS5GPU_NATIVE_OK) {
                std::cerr << "Command submission failed at "
                          << replay_draw_count + replay_compute_count - 1
                          << ": " << result << " type=" << command.type
                          << '\n';
                destroy(runtime);
                FreeLibrary(module);
                return 8;
            }
        }
    }
    /*
     * The v4 draw array remains in the stream for tooling and backwards
     * inspection. Execution uses the ordered command array above.
     */
    if (replay_flip_count == 0 && replay_draw_count != draws.size()) {
        std::cerr << "GPU command draw count does not match capture\n";
        destroy(runtime);
        FreeLibrary(module);
        return 8;
    }
    if (replay_flip_count == 0) {
        flip.struct_size = sizeof(flip);
        flip.abi_version = PS5GPU_NATIVE_ABI_VERSION;
        result = submit_flip(runtime, &flip);
        ++replay_flip_count;
    }
    if (result == PS5GPU_NATIVE_OK) {
        // The wait has to cover the work, not one frame of it. A three-flip
        // capture takes about seventy seconds to replay, so a flat thirty
        // second flush returned early and the stats read back mid-run: 81 of
        // 211 draws and one of three flips, while the runtime went on to
        // finish all of them.
        const auto flush_timeout = static_cast<std::uint32_t>(
            30000u + replay_flip_count * 60000u);
        result = flush(runtime, flush_timeout);
    }

    Ps5GpuNativeStats stats = {};
    stats.struct_size = sizeof(stats);
    const auto stats_result = get_stats(runtime, &stats);
    std::cout
        << "capture=" << capture_path << '\n'
        << "draws=" << draws.size() << '\n'
        << "shaders=" << shaders.size() << '\n'
        << "states=" << states.size() << '\n'
        << "compute_states=" << compute_states.size() << '\n'
        << "commands=" << commands.size() << '\n'
        << "replayed_flips=" << replay_flip_count << '\n'
        << "memory_pages=" << memory.size() << '\n'
        << "flip_address=0x" << std::hex << flip.display_address << std::dec
        << '\n'
        << "result=" << result << '\n'
        << "stats_result=" << stats_result << '\n'
        << "processed_draws=" << stats.draws_processed << '\n'
        << "processed_compute="
        << stats.compute_dispatches_processed << '\n'
        << "processed_flips=" << stats.flips_processed << '\n'
        << "dropped_draws=" << stats.draws_dropped << '\n'
        << "dropped_compute="
        << stats.compute_dispatches_dropped << '\n'
        << "dropped_flips=" << stats.flips_dropped << '\n';

    destroy(runtime);
    FreeLibrary(module);
    return result == PS5GPU_NATIVE_OK &&
            stats_result == PS5GPU_NATIVE_OK &&
            stats.draws_processed == replay_draw_count &&
            stats.compute_dispatches_processed ==
                replay_compute_count &&
            stats.flips_processed == replay_flip_count &&
            stats.draws_dropped == 0 &&
            stats.compute_dispatches_dropped == 0 &&
            stats.flips_dropped == 0
        ? 0
        : 9;
}
