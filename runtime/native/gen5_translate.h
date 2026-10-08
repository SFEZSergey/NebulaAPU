// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The pieces joined: a guest shader in, a SPIR-V module out.
//
// Decoding, the control flow graph, the structure, the emission plan, the
// control flow itself, the register bank and the arithmetic have each been
// built and tested apart. This is where they meet, and the first thing
// worth knowing is whether they fit.
//
// What it covers so far is the scalar family. Everything else decodes -
// the walk needs every instruction's length or it loses the program - but
// emits nothing yet, and the module says how many it skipped. A translation
// that quietly dropped instructions would be far worse than one that counts
// them, because the shader would run and be wrong in a way no test would
// name.

#ifndef PS5_GEN5_TRANSLATE_H
#define PS5_GEN5_TRANSLATE_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gen5_emission_cache.h"
#include "gen5_emit_alu.h"
#include "gen5_emit_valu.h"
#include "gen5_emit_control.h"
#include "gen5_emit_image.h"
#include "gen5_buffer_format.h"
#include "gen5_emit_memory.h"
#include "gen5_emit_typed.h"
#include "gen5_emit_stage.h"
#include "gen5_manifest.h"
#include "gen5_registers.h"
#include "gen5_scalar_eval.h"

namespace ps5gen5 {

struct TranslationResult {
    bool ok = false;
    std::vector<std::uint32_t> words;
    std::uint32_t blocks = 0;
    std::uint32_t instructions_translated = 0;
    std::uint32_t instructions_skipped = 0;
    RegisterFileStats register_stats;
    // What a dispatcher would have paid for the same instructions: one
    // load and one store each. Kept beside the real figures because the
    // whole point of the port is the difference between them.
    std::uint32_t dispatcher_loads = 0;
    std::uint32_t dispatcher_stores = 0;
    // Why a translation stopped, when it did. A refusal is the right
    // answer for a graph the walk cannot handle, but a refusal with no
    // reason is indistinguishable from a bug.
    // Which instructions were skipped, by name. Deciding what to
    // translate next from the shape of the instruction set rather than
    // from what the title actually uses would be guessing at the work.
    std::map<std::string, std::uint32_t> skipped_by_name;
    bool cfg_decoded = false;
    bool plan_complete = false;
    bool plan_duplicated_block = false;
    // What interpreting the scalar half found.
    std::uint32_t scalar_paths = 0;
    // Nanoseconds spent building the CFG and in the scalar walk, for
    // telling the two halves of a translation apart.
    std::uint64_t cfg_ns = 0;
    std::uint64_t scalar_ns = 0;
    // What became of emission: 0 not keyed, 1 emitted and stored, 2 an
    // earlier module reused, 3 emitted and matched the earlier one, 4
    // emitted and did not, which retires the key.
    std::uint32_t emission_reuse = 0;
    // Where decoding stopped, when it did, and the word there.
    std::uint32_t cfg_failed_pc = 0;
    std::uint32_t cfg_failed_word = 0;
    std::uint32_t scalar_loads = 0;
    std::uint32_t scalar_loads_resolved = 0;
    std::uint32_t images_seen = 0;
    std::uint32_t images_resolved = 0;
    // Distinct textures declared, which is fewer than the instructions
    // that read them.
    std::uint32_t images_declared = 0;
    // Distinct buffers declared, and how many of the instructions that
    // reach one had a descriptor to declare it from.
    std::uint32_t buffers_declared = 0;
    std::uint32_t buffer_sites = 0;
    std::uint32_t buffer_sites_resolved = 0;
    // What the runtime needs in order to bind what the module declared.
    // The module says a shader reads binding three; these say what binding
    // three is. They are produced together so that the two cannot be
    // built from different descriptors.
    // Whether the module writes anywhere outside its own registers. A
    // module that only reads cannot corrupt the title even if every
    // binding it declares is wrong - it reads the wrong memory and
    // produces a wrong pixel. One that writes can, and does: the first
    // run that let the native producer drive killed the title in six
    // seconds with a guest access violation.
    bool writes_memory = false;
    std::vector<ManifestBuffer> manifest_buffers;
    std::vector<ManifestImage> manifest_images;
    // Whether the walk ran out of budget rather than finishing. An
    // exhausted walk is why a descriptor goes unresolved without
    // anything being wrong with the shader.
    bool scalar_exhausted = false;
    std::uint32_t memory_reads = 0;
    std::uint32_t memory_reads_failed = 0;
    std::uint32_t descriptors_unknown = 0;
    std::uint32_t descriptors_rejected = 0;
    std::map<std::uint32_t, std::uint32_t> descriptor_unknown_at;
    std::map<std::string, std::uint32_t> unresolved_loads_by_name;
    // Typed read formats this does not take apart, as layout and number
    // format packed together.
    std::map<std::uint32_t, std::uint32_t> typed_formats_unhandled;
    // Buffer instructions whose V# the walk never learned, by the register
    // it would have been in. A count says how many; this says where to
    // look.
    std::map<std::uint32_t, std::uint32_t> buffer_descriptor_unknown_at;
    // For each of those, the scalar load that last filled the register
    // before the buffer instruction, and what the walk knew of its
    // address. An unknown V# is either a load nothing resolved or a
    // register no load wrote, and the fix differs.
    std::vector<std::string> buffer_missing_detail;
    // With PS5RT_NATIVE_TRACE_LOADS, one line per scalar load: where it
    // reads and whether it became a descriptor, data or nothing. Returned
    // rather than printed, because the caller may be on a guest thread.
    std::vector<std::string> load_trace;
    // The guest memory whose words became part of the module: loads that
    // filled a descriptor, or a pointer another load read through. The
    // module is only right while those words hold what they held here -
    // a title that moves a per-frame constant buffer rewrites the V# or
    // the pointer, not the registers the state is cached against.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> dependency_reads;
    // Which export targets a stage writes. A vertex shader that never
    // exports to 12 through 15 writes no position, and a rasteriser given
    // no position draws nothing - which is what the scene targets look
    // like.
    std::map<std::uint32_t, std::uint32_t> exports_by_target;
    // Whether the shader calls a subroutine. A vertex shader fetches its
    // attributes either with instructions of its own or by calling a fetch
    // shader whose address is in registers - and a call this translation
    // does not follow leaves the descriptors that call would have loaded
    // unset, which is what the buffer instructions wanting s0 and s4 look
    // like.
    std::uint32_t subroutine_calls = 0;
    // A resolved scalar load whose words go into the module as constants
    // is only safe if those words cannot change while the module is
    // cached. A descriptor can: the state hash covers the registers it
    // comes from, so a different descriptor is a different shader. Data
    // read through a pointer cannot - the pointer stays and the contents
    // move - so these two are counted apart.
    std::uint32_t loads_baked_descriptor = 0;
    std::uint32_t loads_baked_data = 0;
    // Loads that became real loads against a binding rather than
    // constants, and the buffers declared to make that possible.
    std::uint32_t loads_emitted = 0;
    // Data loads left as constants because there was no address to bind:
    // a descriptor of four zero words is a legal unbound buffer, and a
    // load through one resolves to nothing worth reading.
    std::uint32_t loads_unbound = 0;
    std::vector<ResolvedLoad> first_loads;
    std::vector<ResolvedImage> first_unresolved_images;
    // Which registers the loads this walk resolved actually wrote, so an
    // image that finds nothing can be compared against where anything was
    // put.
    std::vector<std::uint32_t> load_destinations;
    std::map<std::string, std::uint32_t> unknown_by_name;
};

// A vector instruction's source 0 is nine bits: 256 and above names a
// vector register, below that the scalar file and the inline constants,
// which is the same encoding the IR's Operand::source reads.
struct VectorOperands {
    std::uint32_t source2 = 0;
    bool source1_is_vector = false;
    bool source2_is_vector = false;
    bool has_source2 = false;
    std::uint32_t destination = 0;
    std::uint32_t source0 = 0;
    bool source0_is_vector = false;
    std::uint32_t source1 = 0;
    bool has_source1 = false;
    // Source modifiers: a sign flipped or cleared on the way in. They are
    // float modifiers - the sign bit - and only mean that for an operation
    // whose sources are floats.
    bool negate[3] = {false, false, false};
    bool absolute[3] = {false, false, false};
    // Result modifiers: clamped to [0, 1], and scaled by 2, 4 or a half.
    bool clamp = false;
    std::uint32_t output_modifier = 0;
    // SDWA: each source is a byte, a half or the whole register, and the
    // result lands in part of the destination.
    bool sdwa = false;
    std::uint32_t source_select[2] = {6, 6};
    bool source_sign_extend[2] = {false, false};
    std::uint32_t destination_select = 6;
    std::uint32_t destination_unused = 0;
    // VOP3P mix: a source that is a half float, and which half of the
    // register it is.
    bool half_source[3] = {false, false, false};
    bool half_high[3] = {false, false, false};
};

// OpSelect, which the builder's enum does not carry because nothing else
// needed it.
constexpr std::uint32_t kSelectOpcode = 169;
constexpr std::uint32_t kBitcastOpcode = 124;
constexpr std::uint32_t kBitReverseOpcode = 204;
constexpr std::uint32_t kControlBarrierOpcode = 224;
// Scope and semantics for a workgroup barrier: execution and memory both
// scoped to the workgroup, releasing and acquiring the memory it shares.
constexpr std::uint32_t kScopeWorkgroup = 2;
// Where M0 sits in the scalar operand encoding. It carries the address of
// the counter an append or a consume moves.
constexpr std::uint32_t kScalarRegisterM0 = 124;
constexpr std::uint32_t kSemanticsWorkgroupAcquireRelease = 0x8u | 0x100u;

inline VectorOperands vector_operands(
    std::string_view name,
    std::uint32_t word,
    std::uint32_t extra,
    std::uint32_t source_count) {
    VectorOperands operands;
    // VOP3 is a different instruction shape wearing the same opcode names.
    // Its destination is the low byte of the first word and all three of
    // its sources live in the second, each nine bits wide and each free to
    // name a scalar register or an inline constant. Reading it as VOP2 -
    // which is what this did - takes the destination from a field that
    // holds part of the opcode and the sources from bits that are not
    // operands at all, so every VMadF32 in the title, all 2244 of them,
    // was translated against the wrong registers.
    const auto is_vop3 = (word >> 26) == 0x35u || (word >> 26) == 0x33u;
    if (is_vop3) {
        const auto decode = [](std::uint32_t encoded, std::uint32_t& value) {
            const auto vector = encoded >= 256;
            value = vector ? encoded - 256 : encoded;
            return vector;
        };
        operands.destination = word & 0xFFu;
        operands.source0_is_vector =
            decode(extra & 0x1FFu, operands.source0);
        if (source_count >= 2) {
            operands.source1_is_vector =
                decode((extra >> 9) & 0x1FFu, operands.source1);
            operands.has_source1 = true;
        }
        if (source_count >= 3) {
            operands.source2_is_vector =
                decode((extra >> 18) & 0x1FFu, operands.source2);
            operands.has_source2 = true;
        }
        // The accumulating forms keep their accumulator in the destination
        // in VOP3 as in VOP2; the third source field is unused and zero.
        // Taking it as the source read scalar register 0 in place of the
        // running sum, and every v_mac of the FidelityFX sharpening pass
        // after the intro added garbage - the Team Asobi splash as noise.
        if (source_count >= 3 &&
            (name == "VMacF32" || name == "VFmacF32")) {
            operands.source2 = operands.destination;
            operands.source2_is_vector = true;
        }
        // VOP3A carries absolute value at bits 8 to 10 and clamp at 15 of
        // the first word, negation at 29 to 31 and the output scale at 27
        // of the second. VOP3B puts a scalar destination where the
        // absolute bits would be, and VOP3P packs its own - neither is
        // read here.
        const auto is_vop3b = (word >> 26) == 0x35u &&
            is_vop3b_opcode((word >> 16) & 0x3FFu);
        if ((word >> 26) == 0x35u) {
            for (std::uint32_t index = 0; index < 3; ++index) {
                operands.negate[index] = ((extra >> (29 + index)) & 1u) != 0;
                if (!is_vop3b) {
                    operands.absolute[index] =
                        ((word >> (8 + index)) & 1u) != 0;
                }
            }
            operands.clamp = ((word >> 15) & 1u) != 0;
            operands.output_modifier = (extra >> 27) & 3u;
        }
        // VOP3P. For the mix instructions op_sel_hi says a source is a
        // half (bits 27 and 28 of the second word for the first two, bit
        // 14 of the first for the third), op_sel which half (bits 11 to
        // 13), neg_lo negates and neg_hi takes the absolute value.
        if ((word >> 26) == 0x33u && name.substr(0, 7) == "VFmaMix") {
            const bool high_select[3] = {
                ((extra >> 27) & 1u) != 0,
                ((extra >> 28) & 1u) != 0,
                ((word >> 14) & 1u) != 0,
            };
            for (std::uint32_t index = 0; index < 3; ++index) {
                operands.negate[index] = ((extra >> (29 + index)) & 1u) != 0;
                operands.absolute[index] = ((word >> (8 + index)) & 1u) != 0;
                operands.half_source[index] = high_select[index];
                operands.half_high[index] =
                    ((word >> (11 + index)) & 1u) != 0;
            }
            operands.clamp = ((word >> 15) & 1u) != 0;
        }
        return operands;
    }
    const auto is_vop1 = (word >> 25) == 0x3Fu;
    const auto is_vopc = (word >> 25) == 0x3Eu;
    const auto encoded_source0 = word & 0x1FFu;
    operands.source0_is_vector = encoded_source0 >= 256;
    operands.source0 = operands.source0_is_vector
        ? encoded_source0 - 256
        : encoded_source0;
    operands.destination = (word >> 17) & 0xFFu;
    // VOP2's second source is always a vector register, held in its own
    // field rather than in the nine bit encoding.
    if (!is_vop1 && source_count >= 2) {
        operands.source1 = (word >> 9) & 0xFFu;
        operands.source1_is_vector = true;
        operands.has_source1 = true;
    }
    // Source zero naming 0xF9 is not a register: it says the real source
    // and a set of modifiers are in a second word. Reading it as scalar
    // register 249 - which is what this did - gave every such instruction
    // a value nothing wrote, and a pixel shader built on them paints noise.
    if (encoded_source0 == 0xF9u) {
        operands.sdwa = true;
        operands.source0 = extra & 0xFFu;
        operands.source0_is_vector = ((extra >> 23) & 1u) == 0;
        operands.source_select[0] = (extra >> 16) & 7u;
        operands.source_sign_extend[0] = ((extra >> 19) & 1u) != 0;
        operands.negate[0] = ((extra >> 20) & 1u) != 0;
        operands.absolute[0] = ((extra >> 21) & 1u) != 0;
        operands.source_select[1] = (extra >> 24) & 7u;
        operands.source_sign_extend[1] = ((extra >> 27) & 1u) != 0;
        operands.negate[1] = ((extra >> 28) & 1u) != 0;
        operands.absolute[1] = ((extra >> 29) & 1u) != 0;
        if (operands.has_source1 && ((extra >> 31) & 1u) != 0) {
            operands.source1_is_vector = false;
        }
        if (!is_vopc) {
            operands.destination_select = (extra >> 8) & 7u;
            operands.destination_unused = (extra >> 11) & 3u;
            operands.clamp = ((extra >> 13) & 1u) != 0;
            operands.output_modifier = (extra >> 14) & 3u;
        }
    } else if (encoded_source0 == 0xFAu || encoded_source0 == 0xE9u) {
        // DPP: the source is a vector register read from another lane.
        // One invocation here is one lane, so the lane it reads is its
        // own - wrong for the permutations, right for everything else,
        // and far closer than register 250.
        operands.source0 = extra & 0xFFu;
        operands.source0_is_vector = true;
        if (encoded_source0 == 0xFAu) {
            operands.negate[0] = ((extra >> 20) & 1u) != 0;
            operands.absolute[0] = ((extra >> 21) & 1u) != 0;
            operands.negate[1] = ((extra >> 22) & 1u) != 0;
            operands.absolute[1] = ((extra >> 23) & 1u) != 0;
        }
    }
    // The two VOP2 forms that carry a constant K after the instruction:
    // "ak" adds it, s0 * v1 + K, and "mk" multiplies by it, s0 * K + v1.
    // Read as an accumulator, which is what the other three-source VOP2
    // forms are, an ak took the destination's old value for K - the
    // intro's colour conversion added whatever the register held to every
    // channel. Encoding 255 is the literal, which the bank resolves.
    constexpr std::uint32_t kLiteralEncoding = 255;
    const auto ends_with = [&](std::string_view suffix) {
        return name.size() >= suffix.size() &&
            name.substr(name.size() - suffix.size()) == suffix;
    };
    if (ends_with("AkF32") || ends_with("AkF16")) {
        operands.source2 = kLiteralEncoding;
        operands.source2_is_vector = false;
        operands.has_source2 = true;
    } else if (ends_with("MkF32") || ends_with("MkF16")) {
        operands.source2 = operands.source1;
        operands.source2_is_vector = operands.source1_is_vector;
        operands.has_source2 = true;
        operands.source1 = kLiteralEncoding;
        operands.source1_is_vector = false;
    }
    return operands;
}

// The operand fields of the scalar formats. They are small enough to read
// here rather than carry through the IR, and the IR's operand encoding is
// shared with the vector formats which this does not translate yet.
struct ScalarOperands {
    std::uint32_t destination = 0;
    std::uint32_t source0 = 0;
    std::uint32_t source1 = 0;
    bool has_destination = false;
    bool has_source1 = false;
};

inline ScalarOperands scalar_operands(
    std::string_view name, std::uint32_t word) {
    ScalarOperands operands;
    operands.source0 = word & 0xFFu;
    // SOPC compares two sources and writes only SCC; SOP1 has one source
    // and a destination; SOP2 has both sources and a destination.
    const auto is_compare = name.size() > 4 && name.substr(0, 4) == "SCmp";
    if (!is_compare) {
        operands.destination = (word >> 16) & 0x7Fu;
        operands.has_destination = true;
    }
    const auto one_source = name == "SMovB32" || name == "SNotB32" ||
        name == "SWqmB32";
    if (!one_source) {
        operands.source1 = (word >> 8) & 0xFFu;
        operands.has_source1 = true;
    }
    return operands;
}

// What a translation works out from the shader's words alone: the graph,
// its structure and the order emission goes in. None of it depends on the
// draw, and a scene registers the same few shaders hundreds of times a
// frame - so a caller that has these from an earlier translation of the
// same words hands them back, and one that does not is given them to keep.
struct TranslationFrontEnd {
    bool has_code = false;
    CfgGraph graph;
    CfgSummary summary;
    std::uint64_t code_hash = 0;
    Structure structure;
    EmitPlan plan;
};

template <typename ReadWord, typename ReadMemory>
TranslationResult translate_shader(
    std::uint32_t entry_pc,
    const ReadWord& read,
    const std::vector<std::uint32_t>& user_data,
    std::uint32_t user_data_base,
    const ReadMemory& read_memory,
    // The guest address the shader's words were read from, which is what
    // SGetpcB64 hands the shader.
    std::uint64_t code_base = 0,
    // Emit everything except the stores. A module that cannot write
    // cannot corrupt the title, whatever its bindings say, so this is how
    // the rest of it - the declarations, the bindings, the reads, the
    // control flow - gets proven against a running title before the
    // writes are trusted with the title's memory.
    bool omit_stores = false,
    // Declare no images, and skip the instructions that would read them.
    // A module that declares nothing cannot disagree with the descriptor
    // set the runtime builds for it, which is how the declarations get
    // ruled in or out as the cause of a fault.
    bool omit_images = false,
    // The same for buffers.
    bool omit_buffers = false,
    // Which stage this shader is. A compute shader has no inputs and no
    // outputs; a vertex or pixel one is mostly defined by them.
    StageKind stage = StageKind::Compute,
    // Where this stage's resources start in the set it shares with the
    // other stage. A graphics pipeline has one buffer array and one run of
    // image bindings for both its shaders, and the pixel stage's come
    // after the vertex stage's; numbering both from zero puts two buffers
    // at index zero, and the runtime rejects the state as a duplicate -
    // which it did for every state that had buffers in both stages.
    std::uint32_t buffer_base = 0,
    // How long the shared buffer array is, when the caller knows; less
    // than zero for as long as this stage needs.
    std::int32_t buffer_total = -1,
    std::uint32_t image_base = 0,
    // Which system values a pixel shader is handed, and in which
    // registers: SPI_PS_INPUT_ENA and SPI_PS_INPUT_ADDR.
    std::uint32_t pixel_input_enable = 0,
    std::uint32_t pixel_input_address = 0,
    // SPI_PS_INPUT_CNTL_0 to _31, when the caller has them: which export
    // of the vertex stage each attribute reads (the low five bits), a
    // constant instead of any export (bit 5, the constant in bits 8-9),
    // and flat shading (bit 10). Without them attribute N reads export N.
    const std::uint32_t* pixel_input_control = nullptr,
    // A compute stage's workgroup: its shape, and which SGPRs the hardware
    // puts the workgroup's position in (COMPUTE_PGM_RSRC2 decides; -1 for
    // one it does not ask for). Without them every invocation of every
    // workgroup saw thread and group zero, and a dispatch over a whole
    // volume wrote its first voxel 4080 times.
    std::uint32_t compute_local_x = 64,
    std::uint32_t compute_local_y = 1,
    std::uint32_t compute_local_z = 1,
    std::int32_t compute_group_x_register = -1,
    std::int32_t compute_group_y_register = -1,
    std::int32_t compute_group_z_register = -1,
    // The code-only part of an earlier translation of these same words, or
    // somewhere to leave this one's.
    TranslationFrontEnd* front = nullptr,
    // The scalar walk: one already made for this user data and memory -
    // replay_scalar's - when scalar_given, otherwise somewhere to leave the
    // one made here.
    ScalarEvaluation* scalar_slot = nullptr,
    bool scalar_given = false) {
    TranslationResult result;

    TranslationFrontEnd local_front;
    auto& front_end = front != nullptr ? *front : local_front;
    const auto cfg_started = std::chrono::steady_clock::now();
    if (!front_end.has_code) {
        // Every word the graph was built from, for the reuse key: the same
        // address can hold a different shader later.
        std::uint64_t hashed = 0xcbf29ce484222325ull;
        const auto hashing_read = [&](std::uint32_t pc) {
            const auto word = read(pc);
            hashed = (hashed ^ ((static_cast<std::uint64_t>(pc) << 32) |
                                word)) * 0x100000001b3ull;
            return word;
        };
        front_end.summary =
            build_cfg(entry_pc, hashing_read, 1u << 20, &front_end.graph);
        front_end.code_hash = hashed;
        if (front_end.summary.decoded && front_end.graph.size() != 0) {
            front_end.structure = analyse_structure(front_end.graph);
            front_end.plan =
                plan_emission(front_end.graph, front_end.structure);
            front_end.has_code = true;
        }
    }
    const auto& graph = front_end.graph;
    const auto& summary = front_end.summary;
    const auto code_hash = front_end.code_hash;
    result.cfg_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - cfg_started).count());
    result.cfg_decoded = summary.decoded;
    result.cfg_failed_pc = summary.failed_pc;
    result.cfg_failed_word = summary.failed_word;
    if (!summary.decoded || graph.size() == 0) {
        return result;
    }
    const auto& plan = front_end.plan;
    // The scalar half is interpreted before anything is emitted, because
    // what a scalar load fetches is known at translation time: the address
    // comes from user data through arithmetic this can follow, and the
    // words behind it are a descriptor or a constant. A load whose value is
    // known needs no instruction at all - the value goes into the module.
    const auto scalar_started = std::chrono::steady_clock::now();
    ScalarEvaluation local_scalar;
    auto& walked = scalar_slot != nullptr ? *scalar_slot : local_scalar;
    if (!(scalar_given && scalar_slot != nullptr)) {
        walked = evaluate_scalar(
            entry_pc, user_data, user_data_base, read, read_memory,
            code_base);
    }
    const auto& scalar = walked;
    result.scalar_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - scalar_started).count());
    result.scalar_paths = scalar.paths_walked;
    result.scalar_exhausted = scalar.exhausted;
    result.memory_reads = scalar.memory_reads;
    result.memory_reads_failed = scalar.memory_reads_failed;
    result.descriptors_unknown = scalar.descriptors_unknown;
    result.descriptors_rejected = scalar.descriptors_rejected;
    result.descriptor_unknown_at = scalar.descriptor_unknown_at;
    result.unresolved_loads_by_name = scalar.unresolved_loads_by_name;
    result.unknown_by_name = scalar.unknown_by_name;
    for (const auto& buffer : scalar.buffers) {
        if (buffer.descriptor_known || result.buffer_missing_detail.size() >= 8) {
            continue;
        }
        const ResolvedLoad* writer = nullptr;
        for (const auto& load : scalar.loads) {
            if (load.pc < buffer.pc &&
                load.destination <= buffer.resource_register &&
                buffer.resource_register < load.destination + load.dword_count &&
                (writer == nullptr || load.pc >= writer->pc)) {
                writer = &load;
            }
        }
        char line[256] = {};
        if (writer == nullptr) {
            std::snprintf(line, sizeof(line),
                "buffer_pc=0x%X reg=s%u writer=none", buffer.pc,
                buffer.resource_register);
        } else {
            std::snprintf(line, sizeof(line),
                "buffer_pc=0x%X reg=s%u load_pc=0x%X dst=s%u n=%u "
                "known=%d base=s%u lo=0x%X hi=0x%X addr=0x%llX smem=%d",
                buffer.pc, buffer.resource_register, writer->pc,
                writer->destination, writer->dword_count,
                writer->address_known ? 1 : 0, writer->base_register,
                writer->base_low, writer->base_high,
                static_cast<unsigned long long>(writer->address),
                writer->from_smem ? 1 : 0);
        }
        result.buffer_missing_detail.push_back(line);
    }
    for (const auto& load : scalar.loads) {
        if (result.first_loads.size() < 4) {
            result.first_loads.push_back(load);
        }
    }
    // Which loads fill a descriptor, taken from the instructions that read
    // one: each register a descriptor is read from is credited to the last
    // load before that instruction that wrote it. A load all of whose
    // registers are credited is a descriptor load.
    //
    // Per register alone - any register ever read as a descriptor - a
    // shader that samples a texture from s[0:7] in one place and loads a
    // material's colour into s[4:7] in another had the colour baked in as
    // the words translation saw, which were zeros: the UI plane the intro
    // video is drawn onto came out transparent black. Program order stands
    // in for the flow here. A load it gets wrong is read at run time as
    // data, which costs a binding; the other mistake costs the value.
    std::map<std::uint32_t, std::set<std::uint32_t>> load_pcs_by_register;
    for (const auto& load : scalar.loads) {
        for (std::uint32_t index = 0; index < load.dword_count; ++index) {
            load_pcs_by_register[load.destination + index].insert(load.pc);
        }
    }
    std::set<std::pair<std::uint32_t, std::uint32_t>> descriptor_fills;
    const auto credit = [&](std::uint32_t use_pc, std::uint32_t reg) {
        const auto found = load_pcs_by_register.find(reg);
        if (found == load_pcs_by_register.end()) {
            return;
        }
        auto after = found->second.lower_bound(use_pc);
        if (after == found->second.begin()) {
            return;
        }
        --after;
        descriptor_fills.insert({*after, reg});
    };
    for (const auto& image : scalar.images) {
        for (std::uint32_t index = 0; index < 8; ++index) {
            credit(image.pc, image.resource_register + index);
        }
        for (std::uint32_t index = 0; index < 4; ++index) {
            credit(image.pc, image.sampler_register + index);
        }
    }
    for (const auto& buffer : scalar.buffers) {
        for (std::uint32_t index = 0; index < 4; ++index) {
            credit(buffer.pc, buffer.resource_register + index);
        }
    }
    const auto fills_descriptor = [&](const ResolvedLoad& load) {
        if (load.dword_count == 0) {
            return false;
        }
        for (std::uint32_t index = 0; index < load.dword_count; ++index) {
            if (descriptor_fills.count(
                    {load.pc, load.destination + index}) == 0) {
                return false;
            }
        }
        return true;
    };

    std::set<std::uint32_t> load_sites;
    std::map<std::uint32_t, ResolvedLoad> resolved_loads;
    for (const auto& load : scalar.loads) {
        load_sites.insert(load.pc);
        if (load.address_known && resolved_loads.count(load.pc) == 0) {
            resolved_loads[load.pc] = load;
        }
    }
    for (const auto& [pc, load] : resolved_loads) {
        (void)pc;
        if (fills_descriptor(load)) {
            ++result.loads_baked_descriptor;
        } else {
            ++result.loads_baked_data;
        }
    }
    {
        // A load another load's address came through: the last one
        // before it to write its base register.
        std::set<std::uint32_t> address_feeding;
        for (const auto& reader : scalar.loads) {
            const ResolvedLoad* writer = nullptr;
            for (const auto& load : scalar.loads) {
                if (load.pc < reader.pc && load.dword_count != 0 &&
                    load.destination <= reader.base_register + 1 &&
                    reader.base_register <
                        load.destination + load.dword_count &&
                    (writer == nullptr || load.pc >= writer->pc)) {
                    writer = &load;
                }
            }
            if (writer != nullptr) {
                address_feeding.insert(writer->pc);
            }
        }
        for (const auto& [pc, load] : resolved_loads) {
            if (load.dword_count != 0 &&
                (fills_descriptor(load) || address_feeding.count(pc) != 0)) {
                result.dependency_reads.push_back(
                    {load.address, load.dword_count});
            }
        }
    }
    result.scalar_loads = static_cast<std::uint32_t>(load_sites.size());
    result.scalar_loads_resolved =
        static_cast<std::uint32_t>(resolved_loads.size());

    for (const auto& load : scalar.loads) {
        if (load.address_known && load.dword_count != 0) {
            result.load_destinations.push_back(load.destination);
        }
    }
    std::sort(result.load_destinations.begin(),
              result.load_destinations.end());
    result.load_destinations.erase(
        std::unique(result.load_destinations.begin(),
                    result.load_destinations.end()),
        result.load_destinations.end());

    std::set<std::uint32_t> image_sites;
    std::set<std::uint32_t> image_sites_resolved;
    for (const auto& image : scalar.images) {
        image_sites.insert(image.pc);
        if (image.descriptor_known) {
            image_sites_resolved.insert(image.pc);
        }
    }
    for (const auto& image : scalar.images) {
        if (image.descriptor_known ||
            image_sites_resolved.count(image.pc) != 0 ||
            result.first_unresolved_images.size() >= 4) {
            continue;
        }
        result.first_unresolved_images.push_back(image);
    }
    result.images_seen = static_cast<std::uint32_t>(image_sites.size());
    result.images_resolved =
        static_cast<std::uint32_t>(image_sites_resolved.size());
    result.plan_complete = plan.complete;
    result.plan_duplicated_block = plan.duplicated_block;
    result.blocks = static_cast<std::uint32_t>(graph.size());
    if (!plan.complete) {
        return result;
    }
    ps5spirv::ModuleBuilder module;
    module.add_capability(ps5spirv::Capability::Shader);
    module.set_logical_glsl450_memory_model();
    const auto glsl = module.import_ext_inst("GLSL.std.450");
    const auto types = declare_alu_types(module);
    const auto float_type = module.type_float(32);
    const auto void_type = module.type_void();
    const auto function_type = module.type_function(void_type);

    // The scalar register file. It is still an array in memory, because a
    // value written in one block and read in another has to travel, but
    // the bank keeps a block's values out of it.
    const auto pointer_type = module.type_pointer(
        ps5spirv::StorageClass::Private, types.uint_type);
    const auto count = module.constant(types.uint_type, 256);
    const auto array_type = module.type_array(types.uint_type, count);
    const auto array_pointer = module.type_pointer(
        ps5spirv::StorageClass::Private, array_type);
    const auto scalar_registers = module.allocate_id();
    module.add_global(
        ps5spirv::Op::Variable,
        {array_pointer, scalar_registers,
         static_cast<std::uint32_t>(ps5spirv::StorageClass::Private)});
    module.add_name(scalar_registers, "sgpr");

    auto stage_io = make_stage_io(
        module, stage, float_type, types.uint_type);
    stage_io.debug_position =
        std::getenv("PS5RT_NATIVE_DEBUG_POSITION") != nullptr;

    const auto entry = module.allocate_id();
    // Every global variable the entry point reaches has to be named in its
    // interface. Before SPIR-V 1.4 only Input and Output ones did, and
    // this module declares none of those - it is 1.5, where Private,
    // Workgroup, StorageBuffer and UniformConstant all count. Naming only
    // the scalar registers made every module invalid in a way that built,
    // registered and traced without complaint, and took the process down
    // inside the driver six seconds into a run.
    // The register files are per-register variables (see RegisterBank);
    // the arrays stay declared for anything that indexes them, and join the
    // interface only if something does.
    std::vector<std::uint32_t> entry_interface;
    if (stage == StageKind::Compute) {
        module.add_execution_mode(
            entry, ps5spirv::ExecutionMode::LocalSize,
            {std::max<std::uint32_t>(1u, compute_local_x),
             std::max<std::uint32_t>(1u, compute_local_y),
             std::max<std::uint32_t>(1u, compute_local_z)});
    } else if (stage == StageKind::Pixel) {
        // Where the origin is. A fragment shader without this is not a
        // valid module rather than one that guesses.
        module.add_execution_mode(
            entry, ps5spirv::ExecutionMode::OriginUpperLeft, {});
    }
    module.add_function_word(
        ps5spirv::Op::Function, {void_type, entry, 0, function_type});

    // The vector register file, in its own array. A shader uses far fewer
    // of these than the 256 the encoding allows, but sizing it to the
    // encoding costs nothing: it is never read as a whole.
    const auto vector_count = module.constant(types.uint_type, 256);
    const auto vector_array_type =
        module.type_array(types.uint_type, vector_count);
    const auto vector_array_pointer = module.type_pointer(
        ps5spirv::StorageClass::Private, vector_array_type);
    const auto vector_registers = module.allocate_id();
    module.add_global(
        ps5spirv::Op::Variable,
        {vector_array_pointer, vector_registers,
         static_cast<std::uint32_t>(ps5spirv::StorageClass::Private)});
    module.add_name(vector_registers, "vgpr");

    // Local data share: memory a workgroup shares and nobody outside the
    // shader sees. Unlike the buffers there is no descriptor and no
    // binding - the hardware gives a dispatch a fixed allocation, so this
    // is declared at the largest size a Gen5 workgroup can be given, 64
    // kilobytes as words, and left to the shader to index.
    // Declared only when the shader uses it, and at half of what this
    // asked for. Sixty-four kilobytes was both wrong numbers at once: it
    // is more workgroup memory than the device allows, so a pipeline
    // built from it is not one the device can run - and it was declared
    // in every module, including the ones with no local memory at all.
    // That is what was losing the device on modules with one block, no
    // loads, no stores and nothing to hang on.
    // Workgroup storage exists only for compute in Vulkan, and a vertex
    // stage made from an NGG primitive shader uses LDS too: declared as
    // workgroup memory there, the module is invalid, and the scene after
    // the intro draws with two of them. With no lane dimension a lane can
    // only ever read back what it wrote itself, so outside compute LDS is
    // private to the invocation.
    const auto lds_storage = stage == StageKind::Compute
        ? ps5spirv::StorageClass::Workgroup
        : ps5spirv::StorageClass::Private;
    const auto lds_pointer = module.type_pointer(
        lds_storage, types.uint_type);
    std::uint32_t lds = 0;
    // The lane this invocation is. Each invocation of the translated
    // module is one lane of the wave, so counting the lanes below this one
    // - which is what the bit count instructions do against the exec mask -
    // is this number.
    std::uint32_t lane_index = 0;
    const auto ensure_lane_index = [&]() {
        if (lane_index != 0) {
            return lane_index;
        }
        const auto pointer = module.type_pointer(
            ps5spirv::StorageClass::Input, types.uint_type);
        lane_index = module.allocate_id();
        module.add_global(
            ps5spirv::Op::Variable,
            {pointer, lane_index,
             static_cast<std::uint32_t>(ps5spirv::StorageClass::Input)});
        module.add_decoration(
            lane_index, ps5spirv::Decoration::BuiltIn,
            {static_cast<std::uint32_t>(
                ps5spirv::BuiltIn::LocalInvocationIndex)});
        module.add_name(lane_index, "lane");
        entry_interface.push_back(lane_index);
        return lane_index;
    };
    // An atomic on LDS. Outside compute LDS is private to the invocation,
    // where SPIR-V allows no atomics - and needs none, one lane being the
    // only one to see it - so it is a load, the operation and a store.
    const auto emit_lds_atomic = [&](std::uint32_t op,
                                     std::uint32_t pointer,
                                     std::uint32_t value) -> std::uint32_t {
        const auto previous = module.allocate_id();
        if (lds_storage == ps5spirv::StorageClass::Workgroup) {
            module.add_function_word(
                static_cast<ps5spirv::Op>(op),
                {types.uint_type, previous, pointer,
                 module.constant(types.uint_type, kScopeWorkgroup),
                 module.constant(
                     types.uint_type, kSemanticsWorkgroupAcquireRelease),
                 value});
            return previous;
        }
        module.add_function_word(
            ps5spirv::Op::Load, {types.uint_type, previous, pointer});
        const auto binary = [&](std::uint32_t code) {
            const auto id = module.allocate_id();
            module.add_function_word(
                static_cast<ps5spirv::Op>(code),
                {types.uint_type, id, previous, value});
            return id;
        };
        const auto pick = [&](std::uint32_t compare) {
            const auto test = module.allocate_id();
            module.add_function_word(
                static_cast<ps5spirv::Op>(compare),
                {types.bool_type, test, value, previous});
            const auto id = module.allocate_id();
            module.add_function_word(
                static_cast<ps5spirv::Op>(169u),
                {types.uint_type, id, test, value, previous});
            return id;
        };
        std::uint32_t next = value;
        switch (op) {
            case 229u: next = value; break;              // Exchange
            case 234u: next = binary(128u); break;       // IAdd
            case 235u: next = binary(130u); break;       // ISub
            case 236u: next = pick(177u); break;         // SMin: v < p
            case 237u: next = pick(176u); break;         // UMin
            case 238u: next = pick(173u); break;         // SMax: v > p
            case 239u: next = pick(172u); break;         // UMax
            case 240u: next = binary(199u); break;       // And
            case 241u: next = binary(197u); break;       // Or
            case 242u: next = binary(198u); break;       // Xor
            default: next = value; break;
        }
        module.add_function_word(ps5spirv::Op::Store, {pointer, next});
        return previous;
    };
    const auto ensure_lds = [&]() {
        if (lds != 0) {
            return lds;
        }
        const auto count = module.constant(types.uint_type, 8192);
        const auto array = module.type_array(types.uint_type, count);
        const auto array_pointer = module.type_pointer(lds_storage, array);
        lds = module.allocate_id();
        module.add_global(
            ps5spirv::Op::Variable,
            {array_pointer, lds, static_cast<std::uint32_t>(lds_storage)});
        module.add_name(lds, "lds");
        entry_interface.push_back(lds);
        return lds;
    };

    // Which pixel inputs are read by v_interp_mov and which interpolated.
    std::set<std::uint32_t> moved_attributes;
    std::set<std::uint32_t> interpolated_attributes;
    // And which the input controls mark flat shaded.
    std::set<std::uint32_t> flat_attributes;
    RegisterBank bank(
        &module, scalar_registers, pointer_type, types.uint_type,
        types.uint_type, true);
    RegisterBank vector_bank(
        &module, vector_registers, pointer_type, types.uint_type,
        types.uint_type, true);
    bank.set_encodes_constants(true);
    AluEmitter alu(module, types);
    VectorEmitter valu(module, types, float_type, glsl);
    // Every image the walk resolved, declared once per descriptor and
    // bound after the buffers, which hold binding zero. Two instructions
    // naming the same eight words are the same texture, so keying on the
    // words rather than on the instruction keeps a texture sampled in four
    // places to one binding - and the runtime builds its descriptor set the
    // same way, so a module that numbered them per instruction would bind
    // against the wrong one.
    std::map<std::array<std::uint32_t, 8>, DeclaredImage> declared_images;
    std::map<std::uint32_t, DeclaredImage> image_at;
    std::map<std::uint32_t, ImageDescriptor> image_shape_at;
    // Instructions whose descriptor the walk read and which describes no
    // image. That is a legal state - an unbound texture - and the hardware
    // reads zero from one. Skipping the instruction instead leaves the
    // destination holding whatever was there before, which is neither zero
    // nor what the shader asked for.
    std::set<std::uint32_t> image_unbound_at;
    // Instructions reading a one-texel image, and what the texel is.
    std::map<std::uint32_t, std::array<std::uint32_t, 4>> image_texel_at;
    std::uint32_t next_image_binding = 1 + image_base;
    for (const auto& image : scalar.images) {
        // A descriptor the walk read is not the same as a descriptor.
        // Declaring a binding for one that describes no image hands the
        // runtime an address nothing put an image at, and the runtime
        // believes the manifest.
        if (image.descriptor_known &&
            !image_descriptor_is_real(image.descriptor)) {
            image_unbound_at.insert(image.pc);
            if (image_descriptor_is_single_texel(image.descriptor)) {
                std::uint32_t texel = 0;
                const auto read = read_memory(
                    image_base_address(image.descriptor), texel);
                image_texel_at[image.pc] =
                    single_texel_value(image.descriptor, texel, read);
            }
        }
        if (omit_images || !image.descriptor_known ||
            image_at.count(image.pc) != 0 ||
            !image_descriptor_is_real(image.descriptor)) {
            continue;
        }
        const auto shape =
            image_descriptor_of(image.descriptor.data(), !image.is_sample);
        const auto found = declared_images.find(image.descriptor);
        if (found != declared_images.end()) {
            image_at[image.pc] = found->second;
            image_shape_at[image.pc] = shape;
            continue;
        }
        const auto declared = declare_image(
            module, shape, next_image_binding, float_type, types.uint_type,
            types.uint_type);
        // The manifest entry for the same declaration. Binding zero is the
        // buffers, so images start at one - the runtime rejects an image
        // that claims zero.
        ManifestImage entry;
        entry.binding = next_image_binding;
        entry.pc = image.pc;
        // The flag decides whether the runtime binds a combined sampler or
        // a storage image, which has to match what was just declared: a
        // storage image is a bare image type and a sampled one is wrapped.
        entry.flags = shape.is_storage ? 1u : 0u;
        if (image.sampler_known) {
            entry.flags |= 2u;
        }
        entry.resource = image.descriptor;
        entry.sampler = image.sampler;
        entry.base_address = image_base_address(image.descriptor);
        image_extent(image.descriptor, entry.width, entry.height);
        if (entry.width != 0 && entry.height != 0) {
            entry.flags |= 4u;
        }
        result.manifest_images.push_back(entry);
        ++next_image_binding;
        declared_images[image.descriptor] = declared;
        entry_interface.push_back(declared.variable);
        image_at[image.pc] = declared;
        image_shape_at[image.pc] = shape;
    }
    result.images_declared =
        static_cast<std::uint32_t>(declared_images.size());

    // The buffers. Unlike the images these share one binding: the
    // declaration is an array of blocks and an instruction names the block
    // it reaches by index, so the list has to be complete before the first
    // access chain is built.
    std::map<std::array<std::uint32_t, 4>, std::uint32_t> buffer_index;
    std::map<std::uint32_t, std::uint32_t> buffer_at;
    std::map<std::uint32_t, std::uint32_t> buffer_stride_at;
    std::map<std::uint32_t, std::array<std::uint32_t, 4>> buffer_words_at;
    // The same as for images: a descriptor the walk read that describes no
    // buffer is an unbound one, and reads from it give zero.
    std::set<std::uint32_t> buffer_unbound_at;
    {
        std::set<std::uint32_t> sites;
        for (const auto& buffer : scalar.buffers) {
            sites.insert(buffer.pc);
            if (buffer.descriptor_known &&
                !buffer_descriptor_is_real(buffer.descriptor)) {
                buffer_unbound_at.insert(buffer.pc);
            }
            if (!buffer.descriptor_known) {
                ++result.buffer_descriptor_unknown_at[
                    buffer.resource_register];
            }
            if (omit_buffers || !buffer.descriptor_known ||
                buffer_at.count(buffer.pc) != 0 ||
                !buffer_descriptor_is_real(buffer.descriptor)) {
                continue;
            }
            const auto existing = buffer_index.find(buffer.descriptor);
            const auto index = existing != buffer_index.end()
                ? existing->second
                : buffer_base +
                    static_cast<std::uint32_t>(buffer_index.size());
            if (existing == buffer_index.end()) {
                buffer_index[buffer.descriptor] = index;
            }
            buffer_at[buffer.pc] = index;
            if (existing == buffer_index.end()) {
                ManifestBuffer entry;
                entry.binding = index;
                // Where the runtime can find this descriptor in the live
                // user data, so it binds what the draw holds now rather
                // than what translation saw. It is an index into the user
                // data, not a scalar register number - the two differ by
                // the register the user data starts at, which is eight
                // for a vertex stage.
                //
                // A descriptor that did not come from user data has no
                // index, and saying zero would have the runtime re-read
                // the first four words of user data and bind whatever
                // they name. Out of range instead, which is how the
                // runtime is told to keep what the manifest says.
                entry.scalar_address = 0xFFFFFFFFu;
                if (buffer.resource_register >= user_data_base &&
                    buffer.resource_register + 3 <
                        user_data_base + user_data.size()) {
                    entry.scalar_address =
                        buffer.resource_register - user_data_base;
                }
                entry.base_address = buffer_base_address(buffer.descriptor);
                entry.size_bytes = buffer_size_bytes(buffer.descriptor);
                result.manifest_buffers.push_back(entry);
            }
            // The stride is in the descriptor rather than the instruction,
            // and an indexed access is meaningless without it.
            buffer_stride_at[buffer.pc] = (buffer.descriptor[1] >> 16) &
                0x3FFFu;
            buffer_words_at[buffer.pc] = buffer.descriptor;
        }
        result.buffer_sites = static_cast<std::uint32_t>(sites.size());
        result.buffer_sites_resolved =
            static_cast<std::uint32_t>(buffer_at.size());
    }
    // The memory that scalar loads read, which needs binding as much as
    // the memory a buffer instruction reads. A load whose words went into
    // the module as constants is frozen at the moment of translation: the
    // module is cached against a state hash that covers the registers, so
    // a different pointer is a different shader - but the same pointer
    // with different contents behind it is not, and that is what a
    // per-frame constant buffer is.
    std::map<std::uint64_t, std::uint32_t> data_buffer_index;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>
        data_load_at;
    std::map<std::uint64_t, std::uint32_t> data_buffer_size;
    // A diagnostic: each resolved scalar load and what became of it.
    static const bool trace_loads =
        std::getenv("PS5RT_NATIVE_TRACE_LOADS") != nullptr;
    char trace_line[192] = {};
    if (trace_loads) {
        for (const auto& image : scalar.images) {
            const auto& words = image.descriptor;
            std::snprintf(
                trace_line, sizeof(trace_line),
                "gen5.image code=0x%llX pc=0x%X s%u known=%d real=%d "
                "t#=%08X %08X %08X %08X %08X %08X %08X %08X",
                static_cast<unsigned long long>(code_base), image.pc,
                image.resource_register, image.descriptor_known ? 1 : 0,
                image.descriptor_known &&
                        image_descriptor_is_real(image.descriptor)
                    ? 1 : 0,
                words[0], words[1], words[2], words[3], words[4], words[5],
                words[6], words[7]);
            result.load_trace.push_back(trace_line);
        }
        for (const auto& load : scalar.loads) {
            if (!load.address_known) {
                std::snprintf(
                    trace_line, sizeof(trace_line),
                    "gen5.load code=0x%llX pc=0x%X s%u count=%u "
                    "class=unresolved",
                    static_cast<unsigned long long>(code_base), load.pc,
                    load.destination, load.dword_count);
                result.load_trace.push_back(trace_line);
            }
        }
    }
    for (const auto& [pc, load] : resolved_loads) {
        if (trace_loads) {
            std::snprintf(
                trace_line, sizeof(trace_line),
                "gen5.load code=0x%llX pc=0x%X s%u count=%u base=0x%llX "
                "offset=0x%X class=%s",
                static_cast<unsigned long long>(code_base), pc,
                load.destination, load.dword_count,
                static_cast<unsigned long long>(load.base_address),
                load.byte_offset,
                load.dword_count == 0 ? "empty"
                : load.base_address == 0 ? "unbound"
                : fills_descriptor(load) ? "descriptor" : "data");
            result.load_trace.push_back(trace_line);
        }
        if (load.dword_count == 0) {
            continue;
        }
        if (load.base_address == 0) {
            ++result.loads_unbound;
            continue;
        }
        // A descriptor load is read at run time too, rather than baked in.
        // What it fetched decides the bindings either way - the walk read
        // it - but baked in, its words put an object's addresses into the
        // module, every object's module was different, and the driver
        // compiled the scene's heaviest pixel shader 640 times over in a
        // minute and a half. PS5RT_BAKE_DESCRIPTOR_LOADS=1 bakes them.
        static const bool bake_descriptors = [] {
            const auto* value = std::getenv("PS5RT_BAKE_DESCRIPTOR_LOADS");
            return value != nullptr && value[0] == '1';
        }();
        // Compute keeps baking them: the intro video's conversion reads
        // its descriptor words as values, and read at run time they could
        // already be the next frame's - half the video's frames went green
        // in half the runs. Graphics is where the compiles were.
        if ((bake_descriptors || stage == StageKind::Compute) &&
            fills_descriptor(load)) {
            continue;
        }
        const auto existing = data_buffer_index.find(load.base_address);
        const auto index = existing != data_buffer_index.end()
            ? existing->second
            : buffer_base + static_cast<std::uint32_t>(
                  buffer_index.size() + data_buffer_index.size());
        if (existing == data_buffer_index.end()) {
            data_buffer_index[load.base_address] = index;
        }
        data_load_at[pc] = {index, load.byte_offset / 4};
        auto& size = data_buffer_size[load.base_address];
        const auto reach = load.byte_offset + load.dword_count * 4;
        size = std::max(size, load.region_size != 0 ? load.region_size
                                                    : reach);
    }
    // Buffer loads whose descriptor is known and whose offset is a register
    // the walk could not follow: the buffer is bound like any data buffer
    // and the offset is added on the GPU. Skipped, as they were, the
    // registers kept what they held before - and a pixel shader walking a
    // linked list of lights never read the "next" that ends it, and ran
    // its loop until the GPU was reset.
    struct DynamicLoad {
        std::uint32_t binding = 0;
        std::uint32_t first_word = 0;
        std::uint32_t offset_register = 0;
        std::uint32_t destination = 0;
        std::uint32_t dword_count = 0;
        std::uint32_t size_words = 0;
    };
    std::map<std::uint32_t, DynamicLoad> dynamic_load_at;
    for (const auto& load : scalar.loads) {
        if (load.address_known || !load.base_known ||
            load.offset_register == 0x7Du || load.dword_count == 0 ||
            load.region_size == 0 || load.base_address == 0 ||
            resolved_loads.count(load.pc) != 0 ||
            dynamic_load_at.count(load.pc) != 0) {
            continue;
        }
        const auto existing = data_buffer_index.find(load.base_address);
        const auto index = existing != data_buffer_index.end()
            ? existing->second
            : buffer_base + static_cast<std::uint32_t>(
                  buffer_index.size() + data_buffer_index.size());
        if (existing == data_buffer_index.end()) {
            data_buffer_index[load.base_address] = index;
        }
        auto& size = data_buffer_size[load.base_address];
        size = std::max(size, load.region_size);
        DynamicLoad dynamic;
        dynamic.binding = index;
        dynamic.first_word = load.byte_offset / 4;
        dynamic.offset_register = load.offset_register;
        dynamic.destination = load.destination;
        dynamic.dword_count = load.dword_count;
        dynamic.size_words = load.region_size / 4;
        dynamic_load_at[load.pc] = dynamic;
    }
    for (const auto& [address, index] : data_buffer_index) {
        ManifestBuffer entry;
        entry.binding = index;
        // Read from memory rather than named by user data, so there is
        // nothing for the runtime to re-derive.
        entry.scalar_address = 0xFFFFFFFFu;
        entry.base_address = address;
        entry.size_bytes = data_buffer_size[address];
        result.manifest_buffers.push_back(entry);
    }

    // Everything emission reads from here on, with the addresses left
    // out: they are in the manifest, and the module names buffers and
    // images by binding.
    auto& emission_cache = EmissionCache::instance();
    EmissionKey key;
    if (emission_cache.enabled()) {
        key.add(0x454D4931u);
        key.add64(code_hash);
        key.add64(code_base);
        key.add(entry_pc);
        key.add(user_data_base);
        key.add(static_cast<std::uint32_t>(user_data.size()));
        key.add((omit_stores ? 1u : 0u) | (omit_images ? 2u : 0u) |
                (omit_buffers ? 4u : 0u));
        key.add(static_cast<std::uint32_t>(stage));
        key.add(buffer_base);
        key.add(static_cast<std::uint32_t>(buffer_total));
        key.add(image_base);
        key.add(pixel_input_enable);
        key.add(pixel_input_address);
        key.add(pixel_input_control != nullptr ? 1u : 0u);
        if (pixel_input_control != nullptr) {
            for (std::uint32_t index = 0; index < 32; ++index) {
                key.add(pixel_input_control[index]);
            }
        }
        key.add(compute_local_x);
        key.add(compute_local_y);
        key.add(compute_local_z);
        key.add(static_cast<std::uint32_t>(compute_group_x_register));
        key.add(static_cast<std::uint32_t>(compute_group_y_register));
        key.add(static_cast<std::uint32_t>(compute_group_z_register));
        // An image descriptor's address is its first word and the low
        // byte of its second; a buffer's, its first word and the low half
        // of its second.
        key.add(0x494D4731u);
        for (const auto& image : scalar.images) {
            key.add(image.pc);
            key.add((image.descriptor_known ? 1u : 0u) |
                    (image.sampler_known ? 2u : 0u) |
                    (image.is_sample ? 4u : 0u));
            key.add(image.descriptor[1] & ~0xFFu);
            for (std::uint32_t index = 2; index < 8; ++index) {
                key.add(image.descriptor[index]);
            }
            for (const auto word : image.sampler) {
                key.add(word);
            }
        }
        for (const auto& [pc, declared] : image_at) {
            key.add(pc);
            key.add(declared.variable);
        }
        for (const auto pc : image_unbound_at) {
            key.add(pc);
        }
        for (const auto& [pc, texel] : image_texel_at) {
            key.add(pc);
            for (const auto word : texel) {
                key.add(word);
            }
        }
        for (const auto& entry : result.manifest_images) {
            key.add(entry.binding);
            key.add(entry.pc);
            key.add(entry.flags);
            key.add(entry.width);
            key.add(entry.height);
        }
        key.add(0x42554631u);
        for (const auto& buffer : scalar.buffers) {
            key.add(buffer.pc);
            key.add(buffer.descriptor_known ? 1u : 0u);
            key.add(buffer.resource_register);
            key.add(buffer.descriptor[1] & ~0xFFFFu);
            key.add(buffer.descriptor[2]);
            key.add(buffer.descriptor[3]);
        }
        for (const auto& [pc, index] : buffer_at) {
            key.add(pc);
            key.add(index);
        }
        for (const auto pc : buffer_unbound_at) {
            key.add(pc);
        }
        for (const auto& entry : result.manifest_buffers) {
            key.add(entry.binding);
            key.add(entry.scalar_address);
            key.add(entry.size_bytes);
        }
        // A load emission reads from a buffer is keyed by where in the
        // buffer; one it bakes in, by the words it bakes.
        key.add(0x44594E31u);
        for (const auto& [pc, dynamic] : dynamic_load_at) {
            key.add(pc);
            key.add(dynamic.binding);
            key.add(dynamic.first_word);
            key.add(dynamic.offset_register);
            key.add(dynamic.destination);
            key.add(dynamic.dword_count);
            key.add(dynamic.size_words);
        }
        key.add(0x4C4F4131u);
        for (const auto& [pc, load] : resolved_loads) {
            key.add(pc);
            key.add(load.destination);
            key.add(load.dword_count);
            key.add(load.base_address == 0 ? 1u : 0u);
            const auto data = data_load_at.find(pc);
            if (data != data_load_at.end()) {
                key.add(data->second.first);
                key.add(data->second.second);
                continue;
            }
            key.add(0xFFFFFFFFu);
            for (std::uint32_t index = 0; index < load.dword_count; ++index) {
                std::uint32_t word = 0;
                const auto known = read_memory(
                    load.address + index * 4ull, word);
                key.add(known ? 1u : 0u);
                key.add(word);
            }
        }
        EmissionOutputs cached;
        const auto lookup = emission_cache.find(key.words(), cached);
        if (lookup == EmissionCache::Lookup::Reused) {
            result.ok = cached.ok;
            result.words = std::move(cached.words);
            result.register_stats = cached.register_stats;
            result.instructions_translated = cached.instructions_translated;
            result.instructions_skipped = cached.instructions_skipped;
            result.dispatcher_loads = cached.dispatcher_loads;
            result.dispatcher_stores = cached.dispatcher_stores;
            result.subroutine_calls = cached.subroutine_calls;
            result.loads_emitted = cached.loads_emitted;
            result.buffers_declared = cached.buffers_declared;
            result.writes_memory = cached.writes_memory;
            result.skipped_by_name = std::move(cached.skipped_by_name);
            result.typed_formats_unhandled =
                std::move(cached.typed_formats_unhandled);
            result.exports_by_target = std::move(cached.exports_by_target);
            result.emission_reuse = 2;
            return result;
        }
        result.emission_reuse =
            lookup == EmissionCache::Lookup::Verify ? 3u : 1u;
    }

    const auto local_buffer_count = static_cast<std::uint32_t>(
        buffer_index.size() + data_buffer_index.size());
    const auto guest_buffers = declare_guest_buffers(
        module, types.uint_type,
        local_buffer_count == 0
            ? 0u
            : std::max(
                  buffer_base + local_buffer_count,
                  buffer_total > 0 ? static_cast<std::uint32_t>(buffer_total)
                                   : 0u));
    result.buffers_declared =
        static_cast<std::uint32_t>(buffer_index.size());
    if (guest_buffers.declared) {
        entry_interface.push_back(guest_buffers.variable);
    }

    auto labels = allocate_control_labels(module, graph);

    // The condition codes, as variables rather than as a value carried
    // in this loop. A compare and the branch that reads it are almost
    // never in the same block - the compare ends one block and the branch
    // ends another - so a value that lives only while a block is being
    // emitted is false by the time the branch wants it. A loop whose exit
    // test reads false never exits, which is a hung shader and a lost
    // device rather than a wrong picture.
    const auto bool_pointer = module.type_pointer(
        ps5spirv::StorageClass::Private, types.bool_type);
    const auto scc_variable = module.allocate_id();
    module.add_global(
        ps5spirv::Op::Variable,
        {bool_pointer, scc_variable,
         static_cast<std::uint32_t>(ps5spirv::StorageClass::Private)});
    module.add_name(scc_variable, "scc");
    const auto vcc_variable = module.allocate_id();
    module.add_global(
        ps5spirv::Op::Variable,
        {bool_pointer, vcc_variable,
         static_cast<std::uint32_t>(ps5spirv::StorageClass::Private)});
    module.add_name(vcc_variable, "vcc");
    entry_interface.push_back(scc_variable);
    entry_interface.push_back(vcc_variable);

    const auto false_condition = module.constant(types.bool_type, 0);
    [[maybe_unused]] const auto true_condition = module.constant(types.bool_type, 1);
    std::uint32_t condition = false_condition;

    const auto store_condition =
        [&](std::uint32_t variable, std::uint32_t value) {
            module.add_function_word(
                ps5spirv::Op::Store, {variable, value});
        };
    const auto load_condition = [&](std::uint32_t variable) {
        const auto value = module.allocate_id();
        module.add_function_word(
            ps5spirv::Op::Load, {types.bool_type, value, variable});
        return value;
    };
    const auto negate_condition = [&](std::uint32_t value) {
        const auto negated = module.allocate_id();
        module.add_function_word(
            static_cast<ps5spirv::Op>(AluSpirvOp::LogicalNot),
            {types.bool_type, negated, value});
        return negated;
    };

    // Lane masks. One invocation here is one lane of the wave, so a 64-bit
    // mask of lanes - EXEC, VCC, what a compare writes to an SGPR pair - is
    // held as this lane's bit, spread over every bit of both halves: all
    // ones when the lane is in, zero when it is out. AND, OR, XOR and the
    // ANDN forms the shader combines masks with then give the right answer
    // for this lane without knowing the others.
    const auto lane_mask_of = [&](std::uint32_t condition_value) {
        const auto mask = module.allocate_id();
        module.add_function_word(
            static_cast<ps5spirv::Op>(kSelectOpcode),
            {types.uint_type, mask, condition_value,
             module.constant(types.uint_type, 0xFFFFFFFFu),
             module.constant(types.uint_type, 0u)});
        return mask;
    };
    const auto lane_in = [&](std::uint32_t mask) {
        const auto in = module.allocate_id();
        module.add_function_word(
            static_cast<ps5spirv::Op>(AluSpirvOp::INotEqual),
            {types.bool_type, in, mask,
             module.constant(types.uint_type, 0u)});
        return in;
    };

    // What the hardware has already put in the registers when a stage
    // begins. A shader does not read its vertex index from an input: the
    // PS5 hands it over in v5, its instance index in v8, and two words of
    // subgroup shape in s2 and s3. A translation that starts with empty
    // registers gives every invocation the same vertex - one point instead
    // of a triangle - which is a vertex shader that produces no geometry,
    // and the scene targets are empty.
    //
    // These numbers are the interface the hardware defines, not a choice.
    //
    // Emitted at the top of the first block rather than here: here is
    // before the function's first label, and a load outside any block is
    // not a module the driver rejects - it is one the driver crashes on,
    // which took the title down the first time a vertex stage compiled
    // during the run was built into a pipeline.
    auto stage_registers_seeded = false;
    const auto seed_pixel_registers = [&] {
        // A pixel shader's registers start with the barycentric weights
        // the address mask asks for, then one register each for the
        // position, the facing and the rest, in the order the hardware
        // defines. A full screen pass reads its pixel's own position here
        // to find its texel; left empty, every pixel reads texel zero and
        // the intro video's colour conversion painted the screen black.
        const auto address = pixel_input_address != 0
            ? pixel_input_address
            : pixel_input_enable;
        const std::uint32_t weight_registers[8] = {2, 2, 2, 3, 2, 2, 2, 1};
        std::uint32_t reg = 0;
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            if ((address & (1u << bit)) != 0) {
                reg += weight_registers[bit];
            }
        }
        std::uint32_t coord_components[4] = {};
        bool have_coord = false;
        const auto coord = [&](std::uint32_t component) {
            if (!have_coord) {
                const auto variable = stage_vector_builtin(
                    module, stage_io, ps5spirv::BuiltIn::FragCoord);
                const auto loaded = module.allocate_id();
                module.add_function_word(
                    ps5spirv::Op::Load,
                    {stage_io.vector4_type, loaded, variable});
                for (std::uint32_t index = 0; index < 4; ++index) {
                    const auto part = module.allocate_id();
                    module.add_function_word(
                        ps5spirv::Op::CompositeExtract,
                        {float_type, part, loaded, index});
                    coord_components[index] = part;
                }
                have_coord = true;
            }
            return coord_components[component];
        };
        const auto as_bits = [&](std::uint32_t value) {
            const auto bits = module.allocate_id();
            module.add_function_word(
                static_cast<ps5spirv::Op>(kBitcastOpcode),
                {types.uint_type, bits, value});
            return bits;
        };
        for (std::uint32_t bit = 8; bit < 16; ++bit) {
            if ((address & (1u << bit)) == 0) {
                continue;
            }
            std::uint32_t value = 0;
            if (bit <= 11) {
                value = as_bits(coord(bit - 8));
            } else if (bit == 12) {
                // Front facing: this translation draws without culling
                // and does not tell the two apart.
                value = module.constant(types.uint_type, 1);
            } else if (bit == 15) {
                // The integer pixel position, x in the low half and y in
                // the high.
                const auto to_uint = [&](std::uint32_t component) {
                    const auto converted = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(109),
                        {types.uint_type, converted, coord(component)});
                    return converted;
                };
                const auto high = module.allocate_id();
                module.add_function_word(
                    static_cast<ps5spirv::Op>(196),
                    {types.uint_type, high, to_uint(1),
                     module.constant(types.uint_type, 16)});
                value = module.allocate_id();
                module.add_function_word(
                    static_cast<ps5spirv::Op>(197),
                    {types.uint_type, value, to_uint(0), high});
            } else {
                value = module.constant(types.uint_type, 0);
            }
            vector_bank.write(reg, value);
            ++reg;
        }
    };
    auto exec_seeded = false;
    const auto seed_stage_registers = [&] {
    // Every lane starts in. Left at zero, the first s_and_saveexec would
    // switch every lane off, and with the branches below reading the mask
    // the whole shader would be skipped.
    if (!exec_seeded) {
        exec_seeded = true;
        bank.write(126, module.constant(types.uint_type, 0xFFFFFFFFu));
        bank.write(127, module.constant(types.uint_type, 0xFFFFFFFFu));
    }
    if (stage_registers_seeded) {
        return;
    }
    if (stage == StageKind::Pixel) {
        stage_registers_seeded = true;
        seed_pixel_registers();
        return;
    }
    if (stage == StageKind::Compute) {
        stage_registers_seeded = true;
        // v0, v1 and v2 hold the invocation's position in its workgroup,
        // and the system SGPRs after the user data hold the workgroup's
        // position in the dispatch.
        const auto uvec3 = module.type_vector(types.uint_type, 3);
        const auto uvec3_pointer =
            module.type_pointer(ps5spirv::StorageClass::Input, uvec3);
        const auto load_ids = [&](ps5spirv::BuiltIn which) {
            const auto variable = module.allocate_id();
            module.add_global(
                ps5spirv::Op::Variable,
                {uvec3_pointer, variable,
                 static_cast<std::uint32_t>(
                     ps5spirv::StorageClass::Input)});
            module.add_decoration(
                variable, ps5spirv::Decoration::BuiltIn,
                {static_cast<std::uint32_t>(which)});
            stage_io.interface_ids.push_back(variable);
            const auto loaded = module.allocate_id();
            module.add_function_word(
                ps5spirv::Op::Load, {uvec3, loaded, variable});
            std::array<std::uint32_t, 3> parts = {};
            for (std::uint32_t index = 0; index < 3; ++index) {
                parts[index] = module.allocate_id();
                module.add_function_word(
                    ps5spirv::Op::CompositeExtract,
                    {types.uint_type, parts[index], loaded, index});
            }
            return parts;
        };
        const auto local = load_ids(ps5spirv::BuiltIn::LocalInvocationId);
        for (std::uint32_t index = 0; index < 3; ++index) {
            vector_bank.write(index, local[index]);
        }
        const std::int32_t group_registers[3] = {
            compute_group_x_register,
            compute_group_y_register,
            compute_group_z_register,
        };
        if (group_registers[0] >= 0 || group_registers[1] >= 0 ||
            group_registers[2] >= 0) {
            const auto group = load_ids(ps5spirv::BuiltIn::WorkgroupId);
            for (std::uint32_t index = 0; index < 3; ++index) {
                if (group_registers[index] >= 0) {
                    bank.write(
                        static_cast<std::uint32_t>(group_registers[index]),
                        group[index]);
                }
            }
        }
        return;
    }
    if (stage != StageKind::Vertex) {
        return;
    }
    stage_registers_seeded = true;
    {
        const auto read_builtin = [&](ps5spirv::BuiltIn which) {
            const auto variable =
                stage_scalar_builtin(module, stage_io, which);
            const auto value = module.allocate_id();
            module.add_function_word(
                ps5spirv::Op::Load,
                {types.uint_type, value, variable});
            return value;
        };
        vector_bank.write(
            5, read_builtin(ps5spirv::BuiltIn::VertexIndex));
        vector_bank.write(
            8, read_builtin(ps5spirv::BuiltIn::InstanceIndex));
        // The wave's shape, which a shader reads to work out which of its
        // lanes it is. One invocation is one lane here, so the extent is
        // the whole wave.
        constexpr std::uint32_t kWaveSize = 32;
        bank.write(
            2, module.constant(types.uint_type, kWaveSize << 12));
        bank.write(
            3,
            module.constant(
                types.uint_type, (1u << 28) | kWaveSize));
    }
    };

    const auto emit_body = [&](std::size_t block) -> std::uint32_t {
        seed_stage_registers();
        condition = false_condition;
        auto pc = graph.blocks[block].start_pc;
        // A block runs to the start of the next one, or to its terminator.
        while (true) {
            std::uint32_t size = 0;
            const auto decoded = decode_one(pc, read, size);
            if (!decoded.ok() || size == 0) {
                break;
            }
            // An instruction's literal, when it has one, is its last word.
            bank.set_literal(read(pc + size - 1));
            // Instructions that mean nothing to a translation. A wait
            // orders memory against the hardware's own pipelines, which
            // SPIR-V expresses through its memory model rather than as an
            // instruction; a nop is a nop. Counting them as skipped would
            // overstate what is missing by a fifth.
            // Instructions about the hardware's own bookkeeping rather
            // than about the value a shader computes: a message to the
            // command processor, a priority, a cache hint. A barrier is
            // deliberately not here - it orders a workgroup against
            // itself, and dropping it silently would be wrong in a way
            // that shows up as a race rather than as a gap, so it stays
            // counted as untranslated until it is.
            if (decoded.name == "SSendmsg" ||
                decoded.name == "SSendmsghalt" ||
                decoded.name == "SSetprio" ||
                decoded.name == "SSethalt" ||
                decoded.name == "SSleep" ||
                decoded.name == "SIcacheInv" ||
                decoded.name == "SWaitcnt" ||
                decoded.name == "SWaitcntDepctr" ||
                decoded.name == "SWaitcntVscnt" ||
                decoded.name == "SNop" ||
                decoded.name == "SClause" ||
                decoded.name == "SInstPrefetch" ||
                decoded.name == "SCbranchCdbgsys" ||
                decoded.name == "SCbranchCdbguser" ||
                decoded.name == "SCbranchCdbgsysOrUser" ||
                decoded.name == "SCbranchCdbgsysAndUser") {
                pc += size;
                ++result.dispatcher_loads;
                ++result.dispatcher_stores;
                bool at_next = false;
                for (const auto& other : graph.blocks) {
                    if (other.start_pc == pc) {
                        at_next = true;
                        break;
                    }
                }
                if (at_next) {
                    break;
                }
                continue;
            }
            // A scalar load the interpreter resolved becomes the words it
            // read, as constants. The descriptor it fetches does not change
            // between translating the shader and running it - the runtime
            // binds the resource those bits name - so re-reading it on the
            // GPU would cost a memory access to learn something already
            // known.
            // Both spellings: a buffer load names a descriptor and a
            // plain load names an address, but the interpreter has already
            // told them apart and both arrive here as words it read. The
            // walk had this same prefix test and it silently excluded every
            // SBufferLoad, which is half of them.
            if ((decoded.name.size() > 5 &&
                 decoded.name.substr(0, 5) == "SLoad") ||
                (decoded.name.size() > 11 &&
                 decoded.name.substr(0, 11) == "SBufferLoad")) {
                const auto found = resolved_loads.find(pc);
                const auto data_load = data_load_at.find(pc);
                const auto dynamic_load = dynamic_load_at.find(pc);
                if (dynamic_load != dynamic_load_at.end() &&
                    guest_buffers.declared && found == resolved_loads.end()) {
                    // The offset register in bytes, as words, plus the
                    // immediate part; past the buffer's end it reads zero,
                    // as the hardware's range check makes it.
                    const auto& dynamic = dynamic_load->second;
                    const auto uint_binary = [&](std::uint32_t opcode,
                                                 std::uint32_t left,
                                                 std::uint32_t right) {
                        const auto id = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(opcode),
                            {types.uint_type, id, left, right});
                        return id;
                    };
                    const auto base_word = uint_binary(
                        128u,
                        uint_binary(194u, bank.read(dynamic.offset_register),
                                    module.constant(types.uint_type, 2)),
                        module.constant(types.uint_type, dynamic.first_word));
                    for (std::uint32_t index = 0;
                         index < dynamic.dword_count &&
                         dynamic.destination + index < kScalarRegisterCount;
                         ++index) {
                        const auto word_index = uint_binary(
                            128u, base_word,
                            module.constant(types.uint_type, index));
                        const auto inside = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(176u),
                            {types.bool_type, inside, word_index,
                             module.constant(
                                 types.uint_type, dynamic.size_words)});
                        const auto safe_index = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(169u),
                            {types.uint_type, safe_index, inside,
                             word_index,
                             module.constant(types.uint_type, 0)});
                        const auto loaded = emit_buffer_load(
                            module, guest_buffers, dynamic.binding,
                            safe_index);
                        const auto value = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(169u),
                            {types.uint_type, value, inside, loaded,
                             module.constant(types.uint_type, 0)});
                        bank.write(dynamic.destination + index, value);
                    }
                    ++result.loads_emitted;
                    ++result.instructions_translated;
                } else if (data_load != data_load_at.end() &&
                    guest_buffers.declared &&
                    found != resolved_loads.end()) {
                    // Data, so it is read rather than remembered.
                    const auto& load = found->second;
                    const auto binding = data_load->second.first;
                    const auto first_word = data_load->second.second;
                    for (std::uint32_t index = 0;
                         index < load.dword_count &&
                         load.destination + index < kScalarRegisterCount;
                         ++index) {
                        bank.write(
                            load.destination + index,
                            emit_buffer_load(
                                module, guest_buffers, binding,
                                module.constant(
                                    types.uint_type,
                                    first_word + index)));
                    }
                    ++result.loads_emitted;
                    ++result.instructions_translated;
                } else if (found != resolved_loads.end()) {
                    const auto& load = found->second;
                    for (std::uint32_t index = 0;
                         index < load.dword_count &&
                         load.destination + index < kScalarRegisterCount;
                         ++index) {
                        std::uint32_t word = 0;
                        if (!read_memory(
                                load.address + index * 4ull, word)) {
                            continue;
                        }
                        bank.write(
                            load.destination + index,
                            module.constant(types.uint_type, word));
                    }
                    ++result.instructions_translated;
                } else {
                    ++result.instructions_skipped;
                    ++result.skipped_by_name[std::string(decoded.name)];
                }
                pc += size;
                ++result.dispatcher_loads;
                ++result.dispatcher_stores;
                bool at_next_block = false;
                for (const auto& other : graph.blocks) {
                    if (other.start_pc == pc) {
                        at_next_block = true;
                        break;
                    }
                }
                if (at_next_block) {
                    break;
                }
                continue;
            }
            // Instructions that move a value between the two register
            // files, or read a 64-bit pair, and so belong to neither table.
            {
                auto handled = true;
                const auto word = read(pc);
                const auto extra = read(pc + 1);
                if (decoded.name == "VReadlaneB32" ||
                    decoded.name == "VReadfirstlaneB32") {
                    // Takes one lane of a vector register into a scalar
                    // one. This translation has no lane dimension - a
                    // register is one value, not sixty-four - so the lane
                    // select has nothing to select from and the instruction
                    // is the move it reduces to. That is exact for the
                    // uniform case, which is what a shader uses these for:
                    // pulling a value the whole wave agrees on out to the
                    // scalar side.
                    //
                    // v_readfirstlane has a VOP1 form, one word with the
                    // scalar destination in bits 17 to 24 and the source in
                    // the low nine bits, besides the VOP3 one. Read as
                    // VOP3, the VOP1 form wrote s1 with a field of the next
                    // instruction and left its real destination alone: a
                    // waterfall loop then never matched its own lane, never
                    // cleared its flag, and ran until the GPU was reset.
                    const auto vop1 = (word >> 25) == 0x3Fu;
                    const auto source = vop1 ? (word & 0x1FFu) : (extra & 0x1FFu);
                    const auto destination =
                        vop1 ? ((word >> 17) & 0xFFu) : (word & 0xFFu);
                    const auto value = source >= 256
                        ? vector_bank.read(source - 256)
                        : bank.read(source);
                    bank.write(destination, value);
                } else if (decoded.name == "VWritelaneB32") {
                    // The same move the other way.
                    const auto source = extra & 0x1FFu;
                    const auto value = source >= 256
                        ? vector_bank.read(source - 256)
                        : bank.read(source);
                    vector_bank.write(word & 0xFFu, value);
                } else if (decoded.name == "SPackLlB32B16" ||
                           decoded.name == "SPackLhB32B16" ||
                           decoded.name == "SPackHhB32B16" ||
                           decoded.name == "SMulHiU32" ||
                           decoded.name == "SBfmB32") {
                    // Two halves into one register, the high half of a
                    // product, a bit mask. Skipped, the destination kept
                    // what it held: a pixel shader built a tile index with
                    // s_pack_ll and walked the light list at the wrong
                    // tile, which never reached its end.
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto left = bank.read(word & 0xFFu);
                    const auto right = bank.read((word >> 8) & 0xFFu);
                    const auto op = [&](std::uint32_t opcode,
                                        std::uint32_t a, std::uint32_t b) {
                        const auto id = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(opcode),
                            {types.uint_type, id, a, b});
                        return id;
                    };
                    const auto constant = [&](std::uint32_t value) {
                        return module.constant(types.uint_type, value);
                    };
                    std::uint32_t value = 0;
                    if (decoded.name == "SPackLlB32B16") {
                        value = op(197u, op(199u, left, constant(0xFFFFu)),
                                   op(196u, right, constant(16)));
                    } else if (decoded.name == "SPackLhB32B16") {
                        value = op(197u, op(199u, left, constant(0xFFFFu)),
                                   op(199u, right, constant(0xFFFF0000u)));
                    } else if (decoded.name == "SPackHhB32B16") {
                        value = op(197u, op(194u, left, constant(16)),
                                   op(199u, right, constant(0xFFFF0000u)));
                    } else if (decoded.name == "SMulHiU32") {
                        value = valu.emit(
                            VectorForm{VectorOperation::MultiplyHiU32, 2,
                                       false},
                            left, right, 0, 0);
                    } else {
                        // ((1 << width) - 1) << offset, each five bits.
                        const auto width = op(199u, left, constant(31));
                        const auto offset = op(199u, right, constant(31));
                        const auto mask = op(
                            130u, op(196u, constant(1), width), constant(1));
                        value = op(196u, mask, offset);
                    }
                    bank.write(destination, value);
                } else if (decoded.name == "VMovrelsB32" &&
                           (word >> 25) == 0x3Fu &&
                           (word & 0x1FFu) >= 256u) {
                    // A vector register chosen at run time: the one named,
                    // plus M0. The register file is an array, so the bank
                    // writes back what it holds and the element is read by
                    // index. Six of the scene's shaders index this way,
                    // three hundred times; skipped, each read the
                    // destination's stale value.
                    vector_bank.flush();
                    const auto base = (word & 0x1FFu) - 256u;
                    const auto destination = (word >> 17) & 0xFFu;
                    const auto sum = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(128u),
                        {types.uint_type, sum,
                         module.constant(types.uint_type, base),
                         bank.read(kScalarRegisterM0)});
                    const auto inside = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(176u),
                        {types.bool_type, inside, sum,
                         module.constant(types.uint_type, 256)});
                    const auto index = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(169u),
                        {types.uint_type, index, inside, sum,
                         module.constant(types.uint_type, base)});
                    const auto element = module.allocate_id();
                    module.add_function_word(
                        ps5spirv::Op::AccessChain,
                        {pointer_type, element, vector_registers, index});
                    const auto value = module.allocate_id();
                    module.add_function_word(
                        ps5spirv::Op::Load,
                        {types.uint_type, value, element});
                    vector_bank.write(destination, value);
                } else if (decoded.name == "SBitcmp0B32" ||
                           decoded.name == "SBitcmp1B32") {
                    // SCC from one bit of the first source.
                    const auto source = bank.read(word & 0xFFu);
                    const auto bit_index = bank.read((word >> 8) & 0xFFu);
                    const auto masked_index = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(199u),
                        {types.uint_type, masked_index, bit_index,
                         module.constant(types.uint_type, 31)});
                    const auto shifted = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(194u),
                        {types.uint_type, shifted, source, masked_index});
                    const auto bit = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(199u),
                        {types.uint_type, bit, shifted,
                         module.constant(types.uint_type, 1)});
                    const auto set = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            decoded.name == "SBitcmp1B32" ? 171u : 170u),
                        {types.bool_type, set, bit,
                         module.constant(types.uint_type, 0)});
                    condition = set;
                    store_condition(scc_variable, condition);
                } else if (decoded.name == "SCselectB32" ||
                           decoded.name == "SCselectB64") {
                    // Takes a source on the condition code, which is a
                    // value here rather than a register - so this cannot be
                    // a table entry, which has no way to reach it.
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto left = word & 0xFFu;
                    const auto right = (word >> 8) & 0xFFu;
                    const auto halves =
                        decoded.name == "SCselectB64" ? 2u : 1u;
                    for (std::uint32_t half = 0; half < halves; ++half) {
                        const auto chosen = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(kSelectOpcode),
                            {types.uint_type, chosen, condition,
                             bank.read(left + half),
                             bank.read(right + half)});
                        bank.write(destination + half, chosen);
                    }
                } else if (
                    decoded.name.size() > 8 &&
                    decoded.name.find("Saveexec") != std::string_view::npos) {
                    // The destination takes the mask as it was, not the
                    // result: a table entry would write the narrowed mask
                    // to both places and lose the half the shader keeps to
                    // restore from.
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto source = word & 0xFFu;
                    const auto old_low = bank.read(126);
                    const auto old_high = bank.read(127);
                    bank.write(destination, old_low);
                    bank.write(destination + 1, old_high);
                    const auto narrow =
                        [&](std::uint32_t mask, std::uint32_t value) {
                            const auto inverted =
                                decoded.name.substr(0, 6) == "SAndn1" ||
                                decoded.name.substr(0, 6) == "SAndn2";
                            auto right = value;
                            if (inverted) {
                                const auto flipped = module.allocate_id();
                                module.add_function_word(
                                    static_cast<ps5spirv::Op>(
                                        AluSpirvOp::Not),
                                    {types.uint_type, flipped, value});
                                right = flipped;
                            }
                            const auto op =
                                decoded.name.substr(0, 3) == "SOr"
                                    ? AluSpirvOp::BitwiseOr
                                : decoded.name.substr(0, 4) == "SXor"
                                    ? AluSpirvOp::BitwiseXor
                                    : AluSpirvOp::BitwiseAnd;
                            const auto result_id = module.allocate_id();
                            module.add_function_word(
                                static_cast<ps5spirv::Op>(op),
                                {types.uint_type, result_id, mask, right});
                            return result_id;
                        };
                    bank.write(126, narrow(old_low, bank.read(source)));
                    bank.write(127, narrow(old_high, bank.read(source + 1)));
                } else if (decoded.name == "Exp") {
                    // What a vertex or pixel shader ends with. The mask
                    // says which of the four components are written and
                    // the target says where: position, a parameter, or a
                    // colour.
                    const auto enabled = word & 0xFu;
                    const auto target = (word >> 4) & 0x3Fu;
                    const auto compressed = ((word >> 10) & 1u) != 0;
                    const auto sources = read(pc + 1);
                    std::uint32_t components[4] = {};
                    if (compressed) {
                        // Four half floats in two registers, packed two to
                        // a register. Read as whole floats they are not
                        // dimmer or brighter - they are a different number
                        // entirely, so a pixel shader exporting this way
                        // and translated the plain way writes noise.
                        const auto vector2 = module.type_vector(
                            float_type, 2);
                        for (std::uint32_t half = 0; half < 2; ++half) {
                            if ((enabled & (3u << (half * 2))) == 0) {
                                continue;
                            }
                            const auto reg =
                                (sources >> (half * 8)) & 0xFFu;
                            const auto pair = module.allocate_id();
                            module.add_function_word(
                                ps5spirv::Op::ExtInst,
                                std::vector<std::uint32_t>{
                                    vector2, pair, glsl, 62,
                                    vector_bank.read(reg)});
                            for (std::uint32_t part = 0; part < 2; ++part) {
                                const auto taken = module.allocate_id();
                                module.add_function_word(
                                    ps5spirv::Op::CompositeExtract,
                                    {float_type, taken, pair, part});
                                components[half * 2 + part] = taken;
                            }
                        }
                    } else {
                        for (std::uint32_t index = 0; index < 4; ++index) {
                            if ((enabled & (1u << index)) == 0) {
                                continue;
                            }
                            const auto reg =
                                (sources >> (index * 8)) & 0xFFu;
                            const auto bits = vector_bank.read(reg);
                            const auto as_float = module.allocate_id();
                            module.add_function_word(
                                static_cast<ps5spirv::Op>(kBitcastOpcode),
                                {float_type, as_float, bits});
                            components[index] = as_float;
                        }
                    }
                    // A diagnostic: PS5RT_NATIVE_DEBUG_EXPORT="4,3,2" makes
                    // a pixel shader's first colour the named vector
                    // registers, read as floats, with an alpha of one - so a
                    // dump of the target shows what the shader held there.
                    auto debug_export = false;
                    if (stage == StageKind::Pixel && target == 0) {
                        if (const auto* list =
                                std::getenv("PS5RT_NATIVE_DEBUG_EXPORT");
                            list != nullptr && list[0] != '\0') {
                            debug_export = true;
                            // "in0": the raw input at location 0, past any
                            // translation of the interpolation.
                            // "c": a constant colour, to see whether the
                            // shader's store reaches the target at all.
                            if (list[0] == 'c') {
                                const std::uint32_t constant_bits[4] = {
                                    0x3F800000u, 0x3F000000u, 0x3E800000u,
                                    0x3F800000u};
                                for (std::uint32_t index = 0; index < 4;
                                     ++index) {
                                    components[index] = module.constant(
                                        float_type, constant_bits[index]);
                                }
                                list = "";
                            }
                            if (list[0] == 'i') {
                                for (std::uint32_t index = 0; index < 4;
                                     ++index) {
                                    components[index] = index < 3
                                        ? emit_interpolate(
                                              module, stage_io, 0, index)
                                        : module.constant(
                                              float_type, 0x3F800000u);
                                }
                                list = "";
                            }
                            const auto* cursor = list;
                            for (std::uint32_t index = 0;
                                 index < 4 && list[0] != '\0'; ++index) {
                                std::uint32_t value = 0x3F800000u;
                                if (index < 3 && *cursor != '\0') {
                                    char* end = nullptr;
                                    const auto reg = std::strtoul(cursor, &end, 10);
                                    cursor = (end != nullptr && *end == ',')
                                        ? end + 1 : end;
                                    const auto as_float = module.allocate_id();
                                    module.add_function_word(
                                        static_cast<ps5spirv::Op>(kBitcastOpcode),
                                        {float_type, as_float,
                                         vector_bank.read(
                                             static_cast<std::uint32_t>(reg))});
                                    components[index] = as_float;
                                    continue;
                                }
                                components[index] =
                                    module.constant(float_type, value);
                            }
                        }
                    }
                    // A compressed export enables its components in
                    // pairs; past the unpacking all four are present.
                    ++result.exports_by_target[target];
                    emit_export(
                        module, stage_io, target,
                        compressed || debug_export ? 0xFu : enabled,
                        components);
                } else if (decoded.name == "VInterpP1F32" ||
                           decoded.name == "VInterpP2F32" ||
                           decoded.name == "VInterpMovF32") {
                    // The hardware builds an interpolated value out of
                    // barycentric weights in two instructions. SPIR-V has
                    // the rasteriser do the interpolation, so both halves
                    // read the same input and the second is what the
                    // shader goes on to use.
                    const auto destination = (word >> 18) & 0xFFu;
                    const auto channel = (word >> 8) & 0x3u;
                    const auto attribute = (word >> 10) & 0x3Fu;
                    const auto control = pixel_input_control != nullptr
                        ? pixel_input_control[attribute & 31u]
                        : attribute;
                    if ((control & 0x20u) != 0) {
                        // No export feeds it: one of four constants,
                        // (0,0,0,0), (0,0,0,1), (1,1,1,0) or (1,1,1,1).
                        const auto which = (control >> 8) & 0x3u;
                        const auto one = channel == 3
                            ? (which & 1u) != 0
                            : (which & 2u) != 0;
                        vector_bank.write(
                            destination,
                            module.constant(
                                types.uint_type, one ? 0x3F800000u : 0u));
                    } else {
                        const auto location = control & 0x1Fu;
                        (decoded.name == "VInterpMovF32"
                             ? moved_attributes
                             : interpolated_attributes)
                            .insert(location);
                        if ((control & 0x400u) != 0) {
                            flat_attributes.insert(location);
                        }
                        const auto value = emit_interpolate(
                            module, stage_io, location, channel);
                        const auto bits = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(kBitcastOpcode),
                            {types.uint_type, bits, value});
                        vector_bank.write(destination, bits);
                    }
                } else if (decoded.name == "SBitset0B32" ||
                           decoded.name == "SBitset1B32") {
                    // Reads its own destination and changes one bit of it,
                    // which the table has no way to express: its forms
                    // take their sources from the source fields.
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto position = word & 0xFFu;
                    const auto one = module.constant(types.uint_type, 1);
                    const auto shifted = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            AluSpirvOp::ShiftLeftLogical),
                        {types.uint_type, shifted, one,
                         bank.read(position)});
                    auto mask = shifted;
                    if (decoded.name == "SBitset0B32") {
                        const auto inverted = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(AluSpirvOp::Not),
                            {types.uint_type, inverted, shifted});
                        mask = inverted;
                    }
                    const auto combined = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            decoded.name == "SBitset1B32"
                                ? AluSpirvOp::BitwiseOr
                                : AluSpirvOp::BitwiseAnd),
                        {types.uint_type, combined,
                         bank.read(destination), mask});
                    bank.write(destination, combined);
                } else if (decoded.name == "SBfeU32" ||
                           decoded.name == "SBfeI32") {
                    // Offset in the low six bits of the second source and
                    // width above it, both packed into one operand - which
                    // is why this is not two table entries.
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto value = bank.read(word & 0xFFu);
                    const auto control = bank.read((word >> 8) & 0xFFu);
                    const auto offset = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(AluSpirvOp::BitwiseAnd),
                        {types.uint_type, offset, control,
                         module.constant(types.uint_type, 0x3Fu)});
                    const auto width_bits = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            AluSpirvOp::ShiftRightLogical),
                        {types.uint_type, width_bits, control,
                         module.constant(types.uint_type, 16)});
                    const auto width = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(AluSpirvOp::BitwiseAnd),
                        {types.uint_type, width, width_bits,
                         module.constant(types.uint_type, 0x7Fu)});
                    const auto moved = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            decoded.name == "SBfeI32"
                                ? AluSpirvOp::ShiftRightArithmetic
                                : AluSpirvOp::ShiftRightLogical),
                        {types.uint_type, moved, value, offset});
                    const auto one = module.constant(types.uint_type, 1);
                    const auto span = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            AluSpirvOp::ShiftLeftLogical),
                        {types.uint_type, span, one, width});
                    const auto mask = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(AluSpirvOp::ISub),
                        {types.uint_type, mask, span, one});
                    const auto extracted = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(AluSpirvOp::BitwiseAnd),
                        {types.uint_type, extracted, moved, mask});
                    bank.write(destination, extracted);
                } else if (decoded.name == "SBarrier" &&
                           stage == StageKind::Compute) {
                    // Orders the workgroup against itself. Dropping it
                    // silently would be a race rather than a gap, which is
                    // why it was left counted as untranslated until now.
                    // Only a compute stage has a workgroup to order; in a
                    // graphics one the instruction is not valid and stays
                    // counted as untranslated.
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(kControlBarrierOpcode),
                        {module.constant(
                             types.uint_type, kScopeWorkgroup),
                         module.constant(
                             types.uint_type, kScopeWorkgroup),
                         module.constant(
                             types.uint_type,
                             kSemanticsWorkgroupAcquireRelease)});
                } else if (decoded.name == "VMbcntLoU32B32" ||
                           decoded.name == "VMbcntHiU32B32") {
                    // Counts the lanes below this one that are live. With
                    // one invocation per lane that count is the lane's own
                    // index, which the low half produces and the high half
                    // passes along - the pair exists on hardware only
                    // because the mask is two registers wide.
                    const auto destination = (word >> 17) & 0xFFu;
                    if (decoded.name == "VMbcntLoU32B32") {
                        // The built-in that carries it exists in a compute
                        // stage only. A graphics stage gets zero, which is
                        // what one lane counting the lanes below it would
                        // give anyway.
                        if (stage == StageKind::Compute) {
                            const auto value = module.allocate_id();
                            module.add_function_word(
                                ps5spirv::Op::Load,
                                {types.uint_type, value,
                                 ensure_lane_index()});
                            vector_bank.write(destination, value);
                        } else {
                            vector_bank.write(
                                destination,
                                module.constant(types.uint_type, 0));
                        }
                    } else {
                        const auto carried = (word >> 9) & 0xFFu;
                        vector_bank.write(
                            destination, vector_bank.read(carried));
                    }
                } else if (decoded.name == "VMadU64U32" ||
                           decoded.name == "VMadI64I32") {
                    // Two thirty-two bit numbers multiplied into sixty-four
                    // and added to a sixty-four bit accumulator. The whole
                    // product does not fit in a register, so both halves
                    // are built and the carry between them is worked out
                    // rather than assumed.
                    const auto operands = vector_operands(
                        decoded.name, word, read(pc + 1), 3);
                    const auto read_source =
                        [&](std::uint32_t index, bool is_vector) {
                            return is_vector ? vector_bank.read(index)
                                             : bank.read(index);
                        };
                    const auto binary =
                        [&](AluSpirvOp op, std::uint32_t a,
                            std::uint32_t b, std::uint32_t type) {
                            const auto id = module.allocate_id();
                            module.add_function_word(
                                static_cast<ps5spirv::Op>(op),
                                {type, id, a, b});
                            return id;
                        };
                    const auto left = read_source(
                        operands.source0, operands.source0_is_vector);
                    const auto right = read_source(
                        operands.source1, operands.source1_is_vector);
                    const auto accumulator_low = read_source(
                        operands.source2, operands.source2_is_vector);
                    const auto accumulator_high = operands.source2_is_vector
                        ? vector_bank.read(operands.source2 + 1)
                        : bank.read(operands.source2 + 1);

                    // The product, in halves. The upper half comes from
                    // four sixteen bit products rather than from a wider
                    // multiply, which would mean asking for the Int64
                    // capability for one instruction.
                    const auto sixteen =
                        module.constant(types.uint_type, 16);
                    const auto low_mask =
                        module.constant(types.uint_type, 0xFFFFu);
                    const auto split =
                        [&](std::uint32_t value, bool high) {
                            return high
                                ? binary(
                                      AluSpirvOp::ShiftRightLogical, value,
                                      sixteen, types.uint_type)
                                : binary(
                                      AluSpirvOp::BitwiseAnd, value,
                                      low_mask, types.uint_type);
                        };
                    const auto ll = binary(
                        AluSpirvOp::IMul, split(left, false),
                        split(right, false), types.uint_type);
                    const auto hl = binary(
                        AluSpirvOp::IMul, split(left, true),
                        split(right, false), types.uint_type);
                    const auto lh = binary(
                        AluSpirvOp::IMul, split(left, false),
                        split(right, true), types.uint_type);
                    const auto hh = binary(
                        AluSpirvOp::IMul, split(left, true),
                        split(right, true), types.uint_type);
                    const auto middle = binary(
                        AluSpirvOp::IAdd,
                        binary(
                            AluSpirvOp::IAdd,
                            binary(
                                AluSpirvOp::ShiftRightLogical, ll, sixteen,
                                types.uint_type),
                            binary(
                                AluSpirvOp::BitwiseAnd, hl, low_mask,
                                types.uint_type),
                            types.uint_type),
                        binary(
                            AluSpirvOp::BitwiseAnd, lh, low_mask,
                            types.uint_type),
                        types.uint_type);
                    const auto product_low = binary(
                        AluSpirvOp::IMul, left, right, types.uint_type);
                    const auto product_high = binary(
                        AluSpirvOp::IAdd,
                        binary(
                            AluSpirvOp::IAdd, hh,
                            binary(
                                AluSpirvOp::ShiftRightLogical, hl, sixteen,
                                types.uint_type),
                            types.uint_type),
                        binary(
                            AluSpirvOp::IAdd,
                            binary(
                                AluSpirvOp::ShiftRightLogical, lh, sixteen,
                                types.uint_type),
                            binary(
                                AluSpirvOp::ShiftRightLogical, middle,
                                sixteen, types.uint_type),
                            types.uint_type),
                        types.uint_type);

                    // And the sixty-four bit add, whose carry out of the
                    // low half is a sum that came out below what went in.
                    const auto sum_low = binary(
                        AluSpirvOp::IAdd, product_low, accumulator_low,
                        types.uint_type);
                    const auto carried = binary(
                        AluSpirvOp::ULessThan, sum_low, product_low,
                        types.bool_type);
                    const auto carry_value = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(kSelectOpcode),
                        {types.uint_type, carry_value, carried,
                         module.constant(types.uint_type, 1),
                         module.constant(types.uint_type, 0)});
                    const auto sum_high = binary(
                        AluSpirvOp::IAdd,
                        binary(
                            AluSpirvOp::IAdd, product_high,
                            accumulator_high, types.uint_type),
                        carry_value, types.uint_type);
                    vector_bank.write(operands.destination, sum_low);
                    vector_bank.write(operands.destination + 1, sum_high);
                } else if (decoded.name == "VAddcU32" ||
                           decoded.name == "VAddCoCiU32" ||
                           decoded.name == "VSubbU32" ||
                           decoded.name == "VSubbrevU32") {
                    // Carry in and carry out, which live in the lane mask
                    // rather than in a register. The table has no way to
                    // reach the mask, and leaving these out costs the
                    // arithmetic as well as the carry.
                    const auto operands = vector_operands(
                        decoded.name, word, read(pc + 1), 3);
                    const auto read_source =
                        [&](std::uint32_t index, bool is_vector) {
                            return is_vector ? vector_bank.read(index)
                                             : bank.read(index);
                        };
                    const auto left = read_source(
                        operands.source0, operands.source0_is_vector);
                    const auto right = operands.has_source1
                        ? read_source(
                              operands.source1, operands.source1_is_vector)
                        : module.constant(types.uint_type, 0);
                    const auto binary =
                        [&](AluSpirvOp op, std::uint32_t a,
                            std::uint32_t b, std::uint32_t type) {
                            const auto id = module.allocate_id();
                            module.add_function_word(
                                static_cast<ps5spirv::Op>(op),
                                {type, id, a, b});
                            return id;
                        };
                    const auto subtracting =
                        decoded.name.substr(0, 5) == "VSubb";
                    const auto carry_in = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(kSelectOpcode),
                        {types.uint_type, carry_in,
                         load_condition(vcc_variable),
                         module.constant(types.uint_type, 1),
                         module.constant(types.uint_type, 0)});
                    const auto first = binary(
                        subtracting ? AluSpirvOp::ISub : AluSpirvOp::IAdd,
                        left, right, types.uint_type);
                    const auto total = binary(
                        subtracting ? AluSpirvOp::ISub : AluSpirvOp::IAdd,
                        first, carry_in, types.uint_type);
                    // A sum that came out below one of its parts wrapped,
                    // and a difference that came out above the value it
                    // was taken from borrowed.
                    const auto wrapped_once = binary(
                        subtracting ? AluSpirvOp::UGreaterThan
                                    : AluSpirvOp::ULessThan,
                        first, left, types.bool_type);
                    const auto wrapped_twice = binary(
                        subtracting ? AluSpirvOp::UGreaterThan
                                    : AluSpirvOp::ULessThan,
                        total, first, types.bool_type);
                    // Either wrap carries: the first can happen without
                    // the second and the second without the first.
                    const auto either = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(166),
                        {types.bool_type, either, wrapped_once,
                         wrapped_twice});
                    vector_bank.write(operands.destination, total);
                    store_condition(vcc_variable, either);
                    condition = either;
                } else if (decoded.name == "SLshl1AddU32" ||
                           decoded.name == "SLshl2AddU32" ||
                           decoded.name == "SLshl3AddU32" ||
                           decoded.name == "SLshl4AddU32") {
                    // Shift by a fixed amount the name carries, then add.
                    // The table takes both its sources from the source
                    // fields and has nowhere to put the amount.
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto shift =
                        static_cast<std::uint32_t>(decoded.name[5] - '0');
                    const auto shifted = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            AluSpirvOp::ShiftLeftLogical),
                        {types.uint_type, shifted,
                         bank.read(word & 0xFFu),
                         module.constant(types.uint_type, shift)});
                    const auto sum = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(AluSpirvOp::IAdd),
                        {types.uint_type, sum, shifted,
                         bank.read((word >> 8) & 0xFFu)});
                    bank.write(destination, sum);
                } else if (decoded.name == "SBfeU64" ||
                           decoded.name == "SBfeI64") {
                    // Sixty-four bits of value, an offset in the low six
                    // bits of the second source and a width above it. The
                    // shift crosses the register boundary, so it is the
                    // same work the paired shifts do and is written the
                    // same way.
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto source = word & 0xFFu;
                    const auto control = bank.read((word >> 8) & 0xFFu);
                    const auto binary =
                        [&](AluSpirvOp op, std::uint32_t a,
                            std::uint32_t b) {
                            const auto id = module.allocate_id();
                            module.add_function_word(
                                static_cast<ps5spirv::Op>(op),
                                {types.uint_type, id, a, b});
                            return id;
                        };
                    const auto pick =
                        [&](std::uint32_t taken, std::uint32_t a,
                            std::uint32_t b) {
                            const auto id = module.allocate_id();
                            module.add_function_word(
                                static_cast<ps5spirv::Op>(kSelectOpcode),
                                {types.uint_type, id, taken, a, b});
                            return id;
                        };
                    const auto offset = binary(
                        AluSpirvOp::BitwiseAnd, control,
                        module.constant(types.uint_type, 0x3Fu));
                    const auto width = binary(
                        AluSpirvOp::BitwiseAnd,
                        binary(
                            AluSpirvOp::ShiftRightLogical, control,
                            module.constant(types.uint_type, 16)),
                        module.constant(types.uint_type, 0x7Fu));
                    const auto thirty_two =
                        module.constant(types.uint_type, 32);
                    const auto beyond_word = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            AluSpirvOp::UGreaterThanEqual),
                        {types.bool_type, beyond_word, offset,
                         thirty_two});
                    const auto low = bank.read(source);
                    const auto high = bank.read(source + 1);
                    const auto narrow =
                        binary(AluSpirvOp::ISub, thirty_two, offset);
                    const auto past =
                        binary(AluSpirvOp::ISub, offset, thirty_two);
                    // Shifted right by the offset, in two halves.
                    const auto moved_low = pick(
                        beyond_word,
                        binary(AluSpirvOp::ShiftRightLogical, high, past),
                        binary(
                            AluSpirvOp::BitwiseOr,
                            binary(
                                AluSpirvOp::ShiftRightLogical, low,
                                offset),
                            binary(
                                AluSpirvOp::ShiftLeftLogical, high,
                                narrow)));
                    const auto moved_high = pick(
                        beyond_word,
                        module.constant(types.uint_type, 0),
                        binary(
                            AluSpirvOp::ShiftRightLogical, high, offset));
                    // And masked to the width, which may cover both
                    // halves, one of them, or none.
                    const auto width_beyond = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            AluSpirvOp::UGreaterThanEqual),
                        {types.bool_type, width_beyond, width,
                         thirty_two});
                    const auto ones = module.constant(
                        types.uint_type, 0xFFFFFFFFu);
                    const auto low_mask = pick(
                        width_beyond, ones,
                        binary(
                            AluSpirvOp::ISub,
                            binary(
                                AluSpirvOp::ShiftLeftLogical,
                                module.constant(types.uint_type, 1),
                                width),
                            module.constant(types.uint_type, 1)));
                    const auto high_width = binary(
                        AluSpirvOp::ISub, width, thirty_two);
                    const auto high_mask = pick(
                        width_beyond,
                        binary(
                            AluSpirvOp::ISub,
                            binary(
                                AluSpirvOp::ShiftLeftLogical,
                                module.constant(types.uint_type, 1),
                                high_width),
                            module.constant(types.uint_type, 1)),
                        module.constant(types.uint_type, 0));
                    bank.write(
                        destination,
                        binary(
                            AluSpirvOp::BitwiseAnd, moved_low, low_mask));
                    bank.write(
                        destination + 1,
                        binary(
                            AluSpirvOp::BitwiseAnd, moved_high, high_mask));
                } else if (decoded.name == "SLshlB64" ||
                           decoded.name == "SLshrB64") {
                    // A shift across two registers, which the table cannot
                    // express: its paired forms repeat an operation on the
                    // high half independently and a shift moves bits
                    // between them. Written with selects because the
                    // amount is a value, and a shift by thirty-two or more
                    // has no answer in a thirty-two bit shift.
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto source = word & 0xFFu;
                    const auto amount_register = (word >> 8) & 0xFFu;
                    const auto left = decoded.name == "SLshlB64";
                    const auto low = bank.read(source);
                    const auto high = bank.read(source + 1);
                    const auto raw_amount = bank.read(amount_register);
                    const auto amount = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(AluSpirvOp::BitwiseAnd),
                        {types.uint_type, amount, raw_amount,
                         module.constant(types.uint_type, 63)});
                    const auto thirty_two =
                        module.constant(types.uint_type, 32);
                    const auto wide = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            AluSpirvOp::UGreaterThanEqual),
                        {types.bool_type, wide, amount, thirty_two});
                    const auto binary =
                        [&](AluSpirvOp op, std::uint32_t a,
                            std::uint32_t b) {
                            const auto id = module.allocate_id();
                            module.add_function_word(
                                static_cast<ps5spirv::Op>(op),
                                {types.uint_type, id, a, b});
                            return id;
                        };
                    const auto narrow = binary(
                        AluSpirvOp::ISub, thirty_two, amount);
                    const auto beyond = binary(
                        AluSpirvOp::ISub, amount, thirty_two);
                    const auto pick =
                        [&](std::uint32_t taken, std::uint32_t a,
                            std::uint32_t b) {
                            const auto id = module.allocate_id();
                            module.add_function_word(
                                static_cast<ps5spirv::Op>(kSelectOpcode),
                                {types.uint_type, id, taken, a, b});
                            return id;
                        };
                    const auto zero = module.constant(types.uint_type, 0);
                    if (left) {
                        const auto low_shifted = binary(
                            AluSpirvOp::ShiftLeftLogical, low, amount);
                        const auto high_shifted = binary(
                            AluSpirvOp::ShiftLeftLogical, high, amount);
                        const auto carried = binary(
                            AluSpirvOp::ShiftRightLogical, low, narrow);
                        const auto joined = binary(
                            AluSpirvOp::BitwiseOr, high_shifted, carried);
                        const auto crossed = binary(
                            AluSpirvOp::ShiftLeftLogical, low, beyond);
                        bank.write(
                            destination, pick(wide, zero, low_shifted));
                        bank.write(
                            destination + 1, pick(wide, crossed, joined));
                    } else {
                        const auto low_shifted = binary(
                            AluSpirvOp::ShiftRightLogical, low, amount);
                        const auto high_shifted = binary(
                            AluSpirvOp::ShiftRightLogical, high, amount);
                        const auto carried = binary(
                            AluSpirvOp::ShiftLeftLogical, high, narrow);
                        const auto joined = binary(
                            AluSpirvOp::BitwiseOr, low_shifted, carried);
                        const auto crossed = binary(
                            AluSpirvOp::ShiftRightLogical, high, beyond);
                        bank.write(destination, pick(wide, crossed, joined));
                        bank.write(
                            destination + 1, pick(wide, zero, high_shifted));
                    }
                } else if (decoded.name == "SMovkI32") {
                    // SOPK, and a move rather than a read of its own
                    // destination: the immediate is the whole value.
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto immediate = static_cast<std::uint32_t>(
                        static_cast<std::int32_t>(
                            static_cast<std::int16_t>(word & 0xFFFFu)));
                    bank.write(
                        destination,
                        module.constant(types.uint_type, immediate));
                } else if (decoded.name == "SGetpcB64") {
                    // The address of the instruction after it, which this
                    // translation knows exactly.
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto here = code_base +
                        static_cast<std::uint64_t>(pc + size) * 4;
                    bank.write(
                        destination,
                        module.constant(
                            types.uint_type,
                            static_cast<std::uint32_t>(here)));
                    bank.write(
                        destination + 1,
                        module.constant(
                            types.uint_type,
                            static_cast<std::uint32_t>(here >> 32)));
                } else if (decoded.name == "SBrevB32") {
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto reversed = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(kBitReverseOpcode),
                        {types.uint_type, reversed,
                         bank.read(word & 0xFFu)});
                    bank.write(destination, reversed);
                } else if (decoded.name == "SMulkI32" ||
                           decoded.name == "SAddkI32") {
                    // SOPK: the destination is also a source and the other
                    // operand is a signed sixteen bit immediate in the low
                    // half of the instruction.
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto immediate = static_cast<std::uint32_t>(
                        static_cast<std::int32_t>(
                            static_cast<std::int16_t>(word & 0xFFFFu)));
                    const auto combined = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            decoded.name == "SMulkI32" ? AluSpirvOp::IMul
                                                       : AluSpirvOp::IAdd),
                        {types.uint_type, combined, bank.read(destination),
                         module.constant(types.uint_type, immediate)});
                    bank.write(destination, combined);
                } else if (decoded.name == "SAddcU32") {
                    // Adds the carry, which is the condition code - a value
                    // here rather than a register, so the table cannot
                    // reach it either.
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto left = word & 0xFFu;
                    const auto right = (word >> 8) & 0xFFu;
                    const auto sum = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(AluSpirvOp::IAdd),
                        {types.uint_type, sum, bank.read(left),
                         bank.read(right)});
                    const auto carry = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(kSelectOpcode),
                        {types.uint_type, carry, condition,
                         module.constant(types.uint_type, 1),
                         module.constant(types.uint_type, 0)});
                    const auto total = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(AluSpirvOp::IAdd),
                        {types.uint_type, total, sum, carry});
                    bank.write(destination, total);
                } else if (decoded.name == "SCmpEqU64" ||
                           decoded.name == "SCmpLgU64") {
                    const auto left = word & 0xFFu;
                    const auto right = (word >> 8) & 0xFFu;
                    condition = alu.emit_pair_equality(
                        decoded.name == "SCmpEqU64",
                        bank.read(left),
                        bank.read(left + 1),
                        bank.read(right),
                        bank.read(right + 1));
                    store_condition(scc_variable, condition);
                } else if (decoded.name == "SFF1I32B32" ||
                           decoded.name == "SFF1I32B64") {
                    // The index of the lowest set bit, or minus one when
                    // there is none. The 64-bit form is the low half's
                    // answer when the low half has one, and the high half's
                    // answer plus 32 otherwise.
                    const auto source = word & 0xFFu;
                    const auto destination = (word >> 16) & 0x7Fu;
                    const auto minus_one =
                        module.constant(types.uint_type, 0xFFFFFFFFu);
                    const auto zero = module.constant(types.uint_type, 0);
                    const auto find_lowest = [&](std::uint32_t value) {
                        const auto found = module.allocate_id();
                        module.add_function_word(
                            ps5spirv::Op::ExtInst,
                            {types.uint_type, found, glsl, 73, value});
                        const auto present = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(
                                AluSpirvOp::INotEqual),
                            {types.bool_type, present, value, zero});
                        return std::pair<std::uint32_t, std::uint32_t>{
                            found, present};
                    };
                    const auto low = find_lowest(bank.read(source));
                    auto value = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(kSelectOpcode),
                        {types.uint_type, value, low.second, low.first,
                         minus_one});
                    if (decoded.name == "SFF1I32B64") {
                        const auto high =
                            find_lowest(bank.read(source + 1));
                        const auto thirty_two =
                            module.constant(types.uint_type, 32);
                        const auto biased = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(AluSpirvOp::IAdd),
                            {types.uint_type, biased, high.first,
                             thirty_two});
                        const auto from_high = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(kSelectOpcode),
                            {types.uint_type, from_high, high.second, biased,
                             minus_one});
                        const auto joined = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(kSelectOpcode),
                            {types.uint_type, joined, low.second, value,
                             from_high});
                        value = joined;
                    }
                    bank.write(destination, value);
                } else {
                    handled = false;
                }
                if (handled) {
                    ++result.instructions_translated;
                    ++result.dispatcher_loads;
                    ++result.dispatcher_stores;
                    pc += size;
                    bool at_next = false;
                    for (const auto& other : graph.blocks) {
                        if (other.start_pc == pc) {
                            at_next = true;
                            break;
                        }
                    }
                    if (at_next) {
                        break;
                    }
                    continue;
                }
            }

            // Local data share. The address is a byte address in one
            // vector register plus the instruction's own offsets, and the
            // read-two and write-two forms carry a second offset that is
            // counted in words rather than bytes.
            if (decoded.name.size() > 2 &&
                decoded.name.substr(0, 2) == "Ds") {
                const auto word = read(pc);
                const auto extra = read(pc + 1);
                const auto offset0 = word & 0xFFu;
                const auto offset1 = (word >> 8) & 0xFFu;
                const auto address_register = extra & 0xFFu;
                const auto data_register = (extra >> 8) & 0xFFu;
                const auto return_register = (extra >> 24) & 0xFFu;
                const auto reading =
                    decoded.name.substr(0, 6) == "DsRead";
                const auto writing =
                    decoded.name.substr(0, 7) == "DsWrite" &&
                    decoded.name.find("Wrxchg") == std::string_view::npos;
                // The read-modify-writes, which a workgroup uses to share
                // a counter or a queue. Ordinary loads and stores would
                // give the same answer only when no two invocations reach
                // one address, and the reason a shader uses these is that
                // they do.
                const auto atomic =
                    decoded.name == "DsAddU32" ? 234u
                    : decoded.name == "DsSubU32" ? 235u
                    : decoded.name == "DsMinU32" ? 236u
                    : decoded.name == "DsMaxU32" ? 238u
                    : decoded.name == "DsAndB32" ? 240u
                    : decoded.name == "DsOrB32" ? 241u
                    : decoded.name == "DsXorB32" ? 242u
                    : decoded.name == "DsWrxchgRtnB32" ? 229u
                    : decoded.name == "DsAddRtnU32" ? 234u
                    : decoded.name == "DsSubRtnU32" ? 235u
                    : decoded.name == "DsMinRtnU32" ? 236u
                    : decoded.name == "DsMaxRtnU32" ? 238u
                    : decoded.name == "DsAndRtnB32" ? 240u
                    : decoded.name == "DsOrRtnB32" ? 241u
                    : decoded.name == "DsXorRtnB32" ? 242u
                    : 0u;
                const auto paired =
                    decoded.name.find("2") != std::string_view::npos;
                const auto words_moved =
                    decoded.name.substr(decoded.name.size() - 4) == "B128"
                    ? 4u
                    : decoded.name.substr(decoded.name.size() - 3) == "B96"
                        ? 3u
                        : decoded.name.substr(
                              decoded.name.size() - 3) == "B64"
                            ? 2u
                            : 1u;
                // Append and consume move a counter whose address is in
                // M0 rather than in an address register, and move it by
                // the number of live lanes. One invocation is one lane, so
                // it moves by one.
                const auto counting =
                    decoded.name == "DsAppend" ||
                    decoded.name == "DsConsume";
                if (counting) {
                    const auto index = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            AluSpirvOp::ShiftRightLogical),
                        {types.uint_type, index,
                         bank.read(kScalarRegisterM0),
                         module.constant(types.uint_type, 2)});
                    const auto pointer = module.allocate_id();
                    module.add_function_word(
                        ps5spirv::Op::AccessChain,
                        {lds_pointer, pointer, ensure_lds(), index});
                    const auto previous = emit_lds_atomic(
                        decoded.name == "DsAppend" ? 234u : 235u,
                        pointer,
                        module.constant(types.uint_type, 1));
                    vector_bank.write(return_register, previous);
                    ++result.instructions_translated;
                } else if (atomic != 0) {
                    const auto base =
                        vector_bank.read(address_register);
                    const auto biased = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(AluSpirvOp::IAdd),
                        {types.uint_type, biased, base,
                         module.constant(
                             types.uint_type,
                             (static_cast<std::uint32_t>(offset1) << 8) |
                                 offset0)});
                    const auto index = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(
                            AluSpirvOp::ShiftRightLogical),
                        {types.uint_type, index, biased,
                         module.constant(types.uint_type, 2)});
                    const auto pointer = module.allocate_id();
                    module.add_function_word(
                        ps5spirv::Op::AccessChain,
                        {lds_pointer, pointer, ensure_lds(), index});
                    const auto previous = emit_lds_atomic(
                        atomic, pointer, vector_bank.read(data_register));
                    // The returning forms hand back what was there before;
                    // the others discard it, and the instruction is named
                    // for which.
                    if (decoded.name.find("Rtn") !=
                        std::string_view::npos) {
                        vector_bank.write(return_register, previous);
                    }
                    ++result.instructions_translated;
                } else if (!reading && !writing) {
                    ++result.instructions_skipped;
                    ++result.skipped_by_name[std::string(decoded.name)];
                } else {
                    const auto word_at =
                        [&](std::uint32_t byte_offset) {
                            const auto base = vector_bank.read(
                                address_register);
                            const auto biased = module.allocate_id();
                            module.add_function_word(
                                static_cast<ps5spirv::Op>(AluSpirvOp::IAdd),
                                {types.uint_type, biased, base,
                                 module.constant(
                                     types.uint_type, byte_offset)});
                            const auto index = module.allocate_id();
                            module.add_function_word(
                                static_cast<ps5spirv::Op>(
                                    AluSpirvOp::ShiftRightLogical),
                                {types.uint_type, index, biased,
                                 module.constant(types.uint_type, 2)});
                            const auto pointer = module.allocate_id();
                            module.add_function_word(
                                ps5spirv::Op::AccessChain,
                                {lds_pointer, pointer, ensure_lds(),
                                 index});
                            return pointer;
                        };
                    // A paired form moves one word from each of two places,
                    // and its offsets count words. A plain form moves
                    // several consecutive words from one.
                    const auto places = paired ? 2u : 1u;
                    const auto per_place = paired ? 1u : words_moved;
                    for (std::uint32_t place = 0; place < places; ++place) {
                        const auto start = paired
                            ? (place == 0 ? offset0 : offset1) * 4u
                            : (static_cast<std::uint32_t>(offset1) << 8) |
                                offset0;
                        for (std::uint32_t index = 0; index < per_place;
                             ++index) {
                            const auto pointer =
                                word_at(start + index * 4u);
                            const auto slot =
                                place * per_place + index;
                            if (writing) {
                                // Local data share is the workgroup's own
                                // memory and reaches nothing outside the
                                // dispatch, so it is not a write in the
                                // sense that matters here.
                                module.add_function_word(
                                    ps5spirv::Op::Store,
                                    {pointer,
                                     vector_bank.read(
                                         data_register + slot)});
                            } else {
                                const auto value = module.allocate_id();
                                module.add_function_word(
                                    ps5spirv::Op::Load,
                                    {types.uint_type, value, pointer});
                                vector_bank.write(
                                    return_register + slot, value);
                            }
                        }
                    }
                    ++result.instructions_translated;
                }
                ++result.dispatcher_loads;
                ++result.dispatcher_stores;
                pc += size;
                bool at_next_share = false;
                for (const auto& other : graph.blocks) {
                    if (other.start_pc == pc) {
                        at_next_share = true;
                        break;
                    }
                }
                if (at_next_share) {
                    break;
                }
                continue;
            }

            // A buffer read or write. The descriptor gives the base, so
            // the shader never computes one: what it computes is an offset
            // into the block the runtime bound, which is the instruction's
            // own offset plus whatever its address registers hold.
            if (decoded.name.size() > 6 &&
                decoded.name.substr(0, 6) == "Buffer") {
                const auto found = buffer_at.find(pc);
                if (found == buffer_at.end() &&
                    buffer_unbound_at.count(pc) != 0) {
                    // An unbound buffer reads zero, and a store to one
                    // goes nowhere.
                    const auto extra = read(pc + 1);
                    const auto data_register = (extra >> 8) & 0xFFu;
                    if (decoded.name.substr(0, 11) != "BufferStore") {
                        for (std::uint32_t index = 0; index < 4; ++index) {
                            vector_bank.write(
                                data_register + index,
                                module.constant(types.uint_type, 0));
                        }
                    }
                    ++result.instructions_translated;
                    ++result.dispatcher_loads;
                    ++result.dispatcher_stores;
                    pc += size;
                    bool at_next_unbound = false;
                    for (const auto& other : graph.blocks) {
                        if (other.start_pc == pc) {
                            at_next_unbound = true;
                            break;
                        }
                    }
                    if (at_next_unbound) {
                        break;
                    }
                    continue;
                }
                if (found == buffer_at.end() || !guest_buffers.declared) {
                    ++result.instructions_skipped;
                    ++result.skipped_by_name[std::string(decoded.name)];
                } else {
                    const auto binding = found->second;
                    const auto word = read(pc);
                    const auto extra = read(pc + 1);
                    const auto immediate_offset = word & 0xFFFu;
                    const auto offset_enabled = ((word >> 12) & 1u) != 0;
                    const auto index_enabled = ((word >> 13) & 1u) != 0;
                    const auto address_register = extra & 0xFFu;
                    const auto data_register = (extra >> 8) & 0xFFu;
                    const auto scalar_offset = (extra >> 24) & 0xFFu;
                    const auto storing =
                        decoded.name.substr(0, 11) == "BufferStore";
                    // A typed read takes its element count from its name
                    // and its meaning from the descriptor. Where every
                    // component is a whole word the bits in memory are the
                    // bits the shader wants and it is an ordinary read;
                    // anything narrower has to be unpacked, and until it is
                    // the instruction stays counted rather than guessed at.
                    const auto typed =
                        decoded.name.find("Format") != std::string_view::npos;
                    const auto format = typed
                        ? buffer_format_of_descriptor(buffer_words_at[pc])
                        : BufferFormat{};
                    const auto unpacking = typed && !storing &&
                        !buffer_format_is_whole_words(format) &&
                        typed_format_is_unpackable(format);
                    if (typed && !unpacking) {
                        if (!buffer_format_is_whole_words(format)) {
                            ++result.instructions_skipped;
                            ++result.skipped_by_name[
                                std::string(decoded.name)];
                            ++result.typed_formats_unhandled[
                                (format.data << 4) | format.number];
                            ++result.dispatcher_loads;
                            ++result.dispatcher_stores;
                            pc += size;
                            bool at_next_typed = false;
                            for (const auto& other : graph.blocks) {
                                if (other.start_pc == pc) {
                                    at_next_typed = true;
                                    break;
                                }
                            }
                            if (at_next_typed) {
                                break;
                            }
                            continue;
                        }
                    }
                    const auto typed_components =
                        decoded.name.size() >= 4 &&
                            decoded.name.substr(
                                decoded.name.size() - 4) == "Xyzw"
                        ? 4u
                        : decoded.name.size() >= 3 &&
                                decoded.name.substr(
                                    decoded.name.size() - 3) == "Xyz"
                            ? 3u
                            : decoded.name.size() >= 2 &&
                                    decoded.name.substr(
                                        decoded.name.size() - 2) == "Xy"
                                ? 2u
                                : 1u;
                    const auto words_moved = typed ? typed_components :
                        decoded.name.size() > 6 &&
                            decoded.name.substr(
                                decoded.name.size() - 2) == "x4"
                        ? 4u
                        : decoded.name.substr(
                              decoded.name.size() - 2) == "x3"
                            ? 3u
                            : decoded.name.substr(
                                  decoded.name.size() - 2) == "x2"
                                ? 2u
                                : 1u;

                    const auto add = [&](std::uint32_t left,
                                         std::uint32_t right) {
                        if (left == 0) {
                            return right;
                        }
                        if (right == 0) {
                            return left;
                        }
                        const auto sum = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(AluSpirvOp::IAdd),
                            {types.uint_type, sum, left, right});
                        return sum;
                    };

                    std::uint32_t byte_offset = immediate_offset != 0
                        ? module.constant(types.uint_type, immediate_offset)
                        : 0u;
                    // The address registers, in the order the encoding puts
                    // them: the index first when there is one, then the
                    // offset.
                    auto next_address = address_register;
                    if (index_enabled) {
                        const auto stride = buffer_stride_at[pc];
                        if (stride != 0) {
                            const auto scaled = module.allocate_id();
                            module.add_function_word(
                                static_cast<ps5spirv::Op>(AluSpirvOp::IMul),
                                {types.uint_type, scaled,
                                 vector_bank.read(next_address),
                                 module.constant(types.uint_type, stride)});
                            byte_offset = add(byte_offset, scaled);
                        }
                        ++next_address;
                    }
                    if (offset_enabled) {
                        byte_offset =
                            add(byte_offset, vector_bank.read(next_address));
                    }
                    if (scalar_offset < 128) {
                        byte_offset =
                            add(byte_offset, bank.read(scalar_offset));
                    } else if (scalar_offset > 128 && scalar_offset <= 192) {
                        byte_offset = add(
                            byte_offset,
                            module.constant(
                                types.uint_type, scalar_offset - 128));
                    }

                    // Words rather than bytes, because the block is an
                    // array of words.
                    auto word_offset =
                        module.constant(types.uint_type, 0);
                    if (byte_offset != 0) {
                        const auto shifted = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(
                                AluSpirvOp::ShiftRightLogical),
                            {types.uint_type, shifted, byte_offset,
                             module.constant(types.uint_type, 2)});
                        word_offset = shifted;
                    }

                    if (unpacking) {
                        // Narrow components, taken apart and turned into
                        // the numbers the format says they are. An element
                        // is one or two words wide however many components
                        // it holds, so the words are loaded once and the
                        // fields come out of them.
                        std::vector<std::uint32_t> loaded;
                        const auto element_words =
                            typed_element_words(format);
                        for (std::uint32_t index = 0;
                             index < element_words; ++index) {
                            const auto at = index == 0
                                ? word_offset
                                : add(word_offset,
                                      module.constant(
                                          types.uint_type, index));
                            loaded.push_back(emit_buffer_load(
                                module, guest_buffers, binding, at));
                        }
                        TypedReader reader(
                            module, types.uint_type, float_type, glsl);
                        const auto values = reader.emit(format, loaded);
                        for (std::uint32_t index = 0;
                             index < values.size(); ++index) {
                            vector_bank.write(
                                data_register + index, values[index]);
                        }
                    }
                    for (std::uint32_t index = 0;
                         index < (unpacking ? 0u : words_moved); ++index) {
                        const auto at = index == 0
                            ? word_offset
                            : add(word_offset,
                                  module.constant(types.uint_type, index));
                        if (storing) {
                            result.writes_memory = true;
                            if (omit_stores) {
                                continue;
                            }
                            emit_buffer_store(
                                module, guest_buffers, binding, at,
                                vector_bank.read(data_register + index));
                        } else {
                            vector_bank.write(
                                data_register + index,
                                emit_buffer_load(
                                    module, guest_buffers, binding, at));
                        }
                    }
                    ++result.instructions_translated;
                }
                ++result.dispatcher_loads;
                ++result.dispatcher_stores;
                pc += size;
                bool at_next_buffer = false;
                for (const auto& other : graph.blocks) {
                    if (other.start_pc == pc) {
                        at_next_buffer = true;
                        break;
                    }
                }
                if (at_next_buffer) {
                    break;
                }
                continue;
            }

            // An image read or write, against the descriptor the walk
            // found for this exact instruction.
            if (decoded.name.size() > 5 &&
                decoded.name.substr(0, 5) == "Image") {
                const auto found = image_at.find(pc);
                if (found == image_at.end() &&
                    image_unbound_at.count(pc) != 0) {
                    // An unbound texture reads zero. A store to one goes
                    // nowhere, which is what emitting nothing does.
                    const auto word = read(pc);
                    const auto extra = read(pc + 1);
                    const auto dmask = (word >> 8) & 0xFu;
                    const auto data_register = (extra >> 8) & 0xFFu;
                    // Unless it is one texel, which reads as that texel.
                    const auto texel = image_texel_at.find(pc);
                    if (decoded.name.substr(0, 10) != "ImageStore") {
                        std::uint32_t written = 0;
                        for (std::uint32_t index = 0; index < 4; ++index) {
                            if ((dmask & (1u << index)) == 0) {
                                continue;
                            }
                            vector_bank.write(
                                data_register + written,
                                module.constant(
                                    types.uint_type,
                                    texel != image_texel_at.end()
                                        ? texel->second[index]
                                        : 0u));
                            ++written;
                        }
                    }
                    ++result.instructions_translated;
                } else if (found == image_at.end()) {
                    ++result.instructions_skipped;
                    ++result.skipped_by_name[std::string(decoded.name)];
                } else {
                    const auto& declared = found->second;
                    const auto& shape = image_shape_at[pc];
                    const auto word = read(pc);
                    const auto extra = read(pc + 1);
                    const auto dmask = (word >> 8) & 0xFu;
                    const auto address_register = extra & 0xFFu;
                    const auto data_register = (extra >> 8) & 0xFFu;
                    const auto components = coordinate_count_of(shape);
                    const auto sampling =
                        decoded.name.substr(0, 11) == "ImageSample";
                    const auto storing =
                        decoded.name.substr(0, 10) == "ImageStore";

                    // The coordinate. A sample takes floats and a fetch
                    // takes integers, and the registers hold bits either
                    // way - which is the one place a bitcast is not
                    // optional.
                    const auto component_type =
                        sampling ? float_type : types.uint_type;
                    // NSA - non-sequential addresses. When bits 1 and 2
                    // of the first word are set, the address registers are
                    // not a run: the first is in the usual field and the
                    // rest are listed a byte each in the extra dwords that
                    // follow. Reading a run instead took the second
                    // coordinate from the register after the first, which
                    // is what put the intro video's samples outside the
                    // texture, onto a black border.
                    const auto nsa_dwords = (word >> 1) & 3u;
                    const auto address_of = [&](std::uint32_t index) {
                        if (nsa_dwords == 0 || index == 0) {
                            return address_register + index;
                        }
                        const auto slot = index - 1;
                        if (slot / 4 >= nsa_dwords) {
                            return address_register + index;
                        }
                        return (read(pc + 2 + slot / 4) >> ((slot % 4) * 8)) &
                            0xFFu;
                    };
                    std::vector<std::uint32_t> coordinate_words;
                    coordinate_words.reserve(components);
                    for (std::uint32_t index = 0; index < components;
                         ++index) {
                        // A 1D image is a row of a 2D one: its second
                        // coordinate is zero - the same bits as a float or
                        // an integer - and its layer the register after the
                        // first.
                        if (shape.one_dimensional && index == 1) {
                            coordinate_words.push_back(
                                sampling
                                    ? module.constant(float_type, 0u)
                                    : module.constant(types.uint_type, 0u));
                            continue;
                        }
                        const auto register_index =
                            shape.one_dimensional && index > 1 ? index - 1
                                                               : index;
                        const auto bits =
                            vector_bank.read(address_of(register_index));
                        if (!sampling) {
                            coordinate_words.push_back(bits);
                            continue;
                        }
                        const auto as_float = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(kBitcastOpcode),
                            {float_type, as_float, bits});
                        coordinate_words.push_back(as_float);
                    }
                    std::uint32_t coordinate = coordinate_words.front();
                    if (components > 1) {
                        const auto vector_type =
                            module.type_vector(component_type, components);
                        const auto built = module.allocate_id();
                        std::vector<std::uint32_t> operands{
                            vector_type, built};
                        operands.insert(
                            operands.end(), coordinate_words.begin(),
                            coordinate_words.end());
                        module.add_function_word(
                            ps5spirv::Op::CompositeConstruct, operands);
                        coordinate = built;
                    }

                    const auto object = module.allocate_id();
                    module.add_function_word(
                        ps5spirv::Op::Load,
                        {declared.object_type, object, declared.variable});

                    if (storing && omit_stores) {
                        result.writes_memory = true;
                    } else if (storing) {
                        result.writes_memory = true;
                        // The four components the instruction writes, in
                        // the registers after the data register.
                        const auto texel_type =
                            module.type_vector(declared.component_type, 4);
                        std::vector<std::uint32_t> parts{texel_type, 0};
                        const auto texel = module.allocate_id();
                        parts[1] = texel;
                        for (std::uint32_t index = 0; index < 4; ++index) {
                            const auto bits =
                                vector_bank.read(data_register + index);
                            if (declared.component_type == float_type) {
                                const auto as_float = module.allocate_id();
                                module.add_function_word(
                                    static_cast<ps5spirv::Op>(
                                        kBitcastOpcode),
                                    {float_type, as_float, bits});
                                parts.push_back(as_float);
                            } else {
                                parts.push_back(bits);
                            }
                        }
                        module.add_function_word(
                            ps5spirv::Op::CompositeConstruct, parts);
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(
                                ImageSpirvOp::ImageWrite),
                            {object, coordinate, texel});
                    } else {
                        const auto value = sampling
                            ? emit_image_sample_lod(
                                  module, declared, object, coordinate,
                                  // Level zero, which is the bit
                                  // pattern zero.
                                  module.constant(float_type, 0))
                            : emit_image_read(
                                  module, declared, object, coordinate);
                        // The mask says which components the instruction
                        // takes and where they land: the registers are
                        // consecutive from the data register, one per bit
                        // set, not one per component index.
                        std::uint32_t written = 0;
                        for (std::uint32_t index = 0; index < 4; ++index) {
                            if ((dmask & (1u << index)) == 0) {
                                continue;
                            }
                            const auto part = module.allocate_id();
                            module.add_function_word(
                                ps5spirv::Op::CompositeExtract,
                                {declared.component_type, part, value,
                                 index});
                            auto bits = part;
                            if (declared.component_type == float_type) {
                                const auto as_bits = module.allocate_id();
                                module.add_function_word(
                                    static_cast<ps5spirv::Op>(
                                        kBitcastOpcode),
                                    {types.uint_type, as_bits, part});
                                bits = as_bits;
                            }
                            vector_bank.write(
                                data_register + written, bits);
                            ++written;
                        }
                    }
                    ++result.instructions_translated;
                }
                ++result.dispatcher_loads;
                ++result.dispatcher_stores;
                pc += size;
                bool at_next_image = false;
                for (const auto& other : graph.blocks) {
                    if (other.start_pc == pc) {
                        at_next_image = true;
                        break;
                    }
                }
                if (at_next_image) {
                    break;
                }
                continue;
            }

            if (decoded.name == "SSwappcB64" ||
                decoded.name == "SSetpcB64") {
                ++result.subroutine_calls;
            }
            const auto vform = vector_form(decoded.name);
            if (vform.operation != VectorOperation::Unknown) {
                const auto operands = vector_operands(
                    decoded.name, read(pc), read(pc + 1),
                    vform.source_count);
                const auto read_operand =
                    [&](std::uint32_t index, bool is_vector) {
                        return is_vector ? vector_bank.read(index)
                                         : bank.read(index);
                    };
                const auto uint_op =
                    [&](std::uint32_t opcode, std::uint32_t left,
                        std::uint32_t right) {
                        const auto id = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(opcode),
                            {types.uint_type, id, left, right});
                        return id;
                    };
                const auto uint_constant = [&](std::uint32_t value) {
                    return module.constant(types.uint_type, value);
                };
                // Which of the instruction's sources and result are floats,
                // by its name - the modifiers are sign bits and a clamp,
                // and on an integer they would mean something else.
                const auto takes_floats =
                    decoded.name.find("F32") != std::string_view::npos ||
                    decoded.name == "VCndmaskB32";
                const auto makes_float =
                    decoded.name.find("F32") != std::string_view::npos &&
                    decoded.name.substr(0, 4) != "VCmp" &&
                    decoded.name.substr(0, 7) != "VCvtI32" &&
                    decoded.name.substr(0, 7) != "VCvtU32";
                // A byte or a half out of the register, sign extended when
                // asked, for SDWA.
                const auto select = [&](std::uint32_t bits,
                                        std::uint32_t which,
                                        bool sign_extend) {
                    if (which >= 6) {
                        return bits;
                    }
                    const auto width = which < 4 ? 8u : 16u;
                    const auto shift = which < 4 ? which * 8u
                                                 : (which - 4u) * 16u;
                    const auto up = uint_op(
                        196, bits, uint_constant(32u - width - shift));
                    return uint_op(
                        sign_extend ? 195u : 194u, up,
                        uint_constant(32u - width));
                };
                const auto modify = [&](std::uint32_t bits,
                                        std::uint32_t index) {
                    // A half float source is widened to a float first; the
                    // sign modifiers then apply to the float.
                    if (index < 3 && operands.half_source[index]) {
                        const auto half = operands.half_high[index]
                            ? uint_op(194, bits, uint_constant(16))
                            : uint_op(199, bits, uint_constant(0xFFFFu));
                        const auto pair_type =
                            module.type_vector(float_type, 2);
                        const auto pair = module.allocate_id();
                        module.add_function_word(
                            ps5spirv::Op::ExtInst,
                            std::vector<std::uint32_t>{
                                pair_type, pair, glsl, 62, half});
                        const auto widened = module.allocate_id();
                        module.add_function_word(
                            ps5spirv::Op::CompositeExtract,
                            {float_type, widened, pair, 0});
                        const auto widened_bits = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(kBitcastOpcode),
                            {types.uint_type, widened_bits, widened});
                        bits = widened_bits;
                    }
                    if (operands.sdwa && index < 2) {
                        bits = select(
                            bits, operands.source_select[index],
                            operands.source_sign_extend[index]);
                    }
                    if (!takes_floats) {
                        return bits;
                    }
                    if (operands.absolute[index]) {
                        bits = uint_op(199, bits, uint_constant(0x7FFFFFFFu));
                    }
                    if (operands.negate[index]) {
                        bits = uint_op(198, bits, uint_constant(0x80000000u));
                    }
                    return bits;
                };
                const auto source0 = modify(read_operand(
                    operands.source0, operands.source0_is_vector), 0);
                const auto source1 = operands.has_source1
                    ? modify(read_operand(
                          operands.source1, operands.source1_is_vector), 1)
                    : 0u;
                // A three source instruction either names its third source
                // - VOP3 does - or takes the destination read before it is
                // written, which is how VOP2 encodes an accumulator.
                const auto source2 = operands.has_source2
                    ? modify(read_operand(
                          operands.source2, operands.source2_is_vector), 2)
                    : vform.source_count >= 3
                        ? vector_bank.read(operands.destination)
                        : 0u;
                // v_cndmask in VOP3 names the mask it selects on in its
                // third source; only VCC is the flag already at hand.
                auto select_condition = condition;
                if (vform.operation == VectorOperation::CndMask &&
                    operands.has_source2 && !operands.source2_is_vector &&
                    operands.source2 != 106u) {
                    select_condition =
                        lane_in(bank.read(operands.source2));
                }
                auto value = valu.emit(
                    vform, source0, source1, source2, select_condition);
                if (value != 0 && makes_float &&
                    (operands.output_modifier != 0 || operands.clamp)) {
                    auto as_float = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(kBitcastOpcode),
                        {float_type, as_float, value});
                    if (operands.output_modifier != 0) {
                        const float scales[4] = {1.0f, 2.0f, 4.0f, 0.5f};
                        std::uint32_t scale_bits = 0;
                        std::memcpy(
                            &scale_bits,
                            &scales[operands.output_modifier],
                            sizeof(scale_bits));
                        const auto scaled = module.allocate_id();
                        module.add_function_word(
                            static_cast<ps5spirv::Op>(133),
                            {float_type, scaled, as_float,
                             module.constant(float_type, scale_bits)});
                        as_float = scaled;
                    }
                    if (operands.clamp) {
                        const auto clamped = module.allocate_id();
                        module.add_function_word(
                            ps5spirv::Op::ExtInst,
                            std::vector<std::uint32_t>{
                                float_type, clamped, glsl, 43, as_float,
                                module.constant(float_type, 0u),
                                module.constant(float_type, 0x3F800000u)});
                        as_float = clamped;
                    }
                    const auto bits = module.allocate_id();
                    module.add_function_word(
                        static_cast<ps5spirv::Op>(kBitcastOpcode),
                        {types.uint_type, bits, as_float});
                    value = bits;
                }
                // SDWA writes part of the destination: the rest is zero,
                // the sign of the part, or what the register held.
                if (value != 0 && operands.sdwa &&
                    operands.destination_select < 6) {
                    const auto which = operands.destination_select;
                    const auto width = which < 4 ? 8u : 16u;
                    const auto shift = which < 4 ? which * 8u
                                                 : (which - 4u) * 16u;
                    const auto mask = ((1u << width) - 1u) << shift;
                    const auto placed = uint_op(
                        199,
                        uint_op(196, value, uint_constant(shift)),
                        uint_constant(mask));
                    if (operands.destination_unused == 2) {
                        const auto kept = uint_op(
                            199, vector_bank.read(operands.destination),
                            uint_constant(~mask));
                        value = uint_op(197, placed, kept);
                    } else {
                        value = placed;
                    }
                }
                if (value != 0) {
                    vector_bank.write(operands.destination, value);
                } else if (valu.last_condition() != 0) {
                    // A compare wrote a lane mask rather than a register.
                    // It goes as a mask to the register pair it names, which
                    // s_and_saveexec, v_cndmask and the other mask
                    // operations read: VCC for the short encoding unless its
                    // SDWA word names an SGPR (bit 15, the SGPR in bits 8 to
                    // 14), the SGPR in the long one, EXEC itself for the
                    // v_cmpx forms.
                    const auto instruction = read(pc);
                    const auto short_form = (instruction >> 25) == 0x3Eu;
                    const auto writes_exec =
                        decoded.name.substr(0, 5) == "VCmpx";
                    auto mask_register = writes_exec
                        ? 126u
                        : short_form ? 106u : (instruction & 0xFFu);
                    if (short_form && !writes_exec &&
                        (instruction & 0x1FFu) == 0xF9u &&
                        ((read(pc + 1) >> 15) & 1u) != 0) {
                        mask_register = (read(pc + 1) >> 8) & 0x7Fu;
                    }
                    // VCC - and the flag the branches and v_cndmask read -
                    // only when the compare writes VCC. Taking every compare
                    // into it clobbered what the shader kept there: the
                    // HDR output pass holds its PQ scale in vcc_lo while its
                    // compares go to s0-s21, and lost it along with the
                    // masks - every pixel of the splash clamped to white.
                    const auto new_condition = valu.last_condition();
                    if (mask_register == 106u || writes_exec) {
                        condition = new_condition;
                        store_condition(vcc_variable, condition);
                    }
                    if (mask_register < 126u || writes_exec) {
                        const auto mask = lane_mask_of(new_condition);
                        bank.write(mask_register, mask);
                        bank.write(mask_register + 1, mask);
                    }
                }
                ++result.instructions_translated;
                pc += size;
                bool reached_next_block = false;
                for (const auto& other : graph.blocks) {
                    if (other.start_pc == pc) {
                        reached_next_block = true;
                        break;
                    }
                }
                ++result.dispatcher_loads;
                ++result.dispatcher_stores;
                if (reached_next_block) {
                    break;
                }
                continue;
            }
            const auto form = scalar_form(decoded.name);
            if (form.operation != AluOperation::Unknown) {
                const auto operands = scalar_operands(decoded.name, read(pc));
                const auto source0 = bank.read(operands.source0);
                const auto source1 = operands.has_source1
                    ? bank.read(operands.source1)
                    : 0u;
                std::uint32_t produced_condition = 0;
                const auto value =
                    alu.emit(form, source0, source1, &produced_condition);
                if (form.writes_condition) {
                    condition = produced_condition;
                    store_condition(scc_variable, produced_condition);
                } else if (operands.has_destination && value != 0) {
                    bank.write(operands.destination, value);
                }
                // A mask computed into VCC by scalar code - s_and_b64 vcc,
                // exec, vcc and the like - is what v_cndmask and the vcc
                // branches read next, and they read the flag.
                if (operands.has_destination &&
                    operands.destination == 106u && value != 0) {
                    store_condition(vcc_variable, lane_in(value));
                }
                // A 64-bit operation is the same work on the register after
                // each of its operands. None of the ones translated here
                // carries between halves, so the two are independent.
                if (form.is_pair && operands.has_destination) {
                    const auto high0 = bank.read_high(operands.source0);
                    const auto high1 = operands.has_source1
                        ? bank.read_high(operands.source1)
                        : 0u;
                    std::uint32_t ignored = 0;
                    const auto high =
                        alu.emit(form, high0, high1, &ignored);
                    if (high != 0) {
                        bank.write(operands.destination + 1, high);
                    }
                }
                ++result.instructions_translated;
            } else if (is_conditional_branch(decoded.name)) {
                // Which code it reads, and which way round. The successor
                // order is the branch target first and the fall-through
                // second, so the condition SPIR-V wants is "the branch is
                // taken" - and scc0 is taken when the code is clear, which
                // is the negation of what scc1 reads. Emitting both the
                // same way is right half the time.
                if (decoded.name == "SCbranchExecnz") {
                    // With EXEC held as this lane's bit, "some lane is
                    // still in" is "this lane is in": a wave loop goes
                    // round again for exactly the lanes it has not
                    // finished.
                    condition = lane_in(bank.read(126));
                } else if (decoded.name == "SCbranchExecz") {
                    // And "no lane is in" is "this lane is out": the lane
                    // skips the block its condition switched off. This used
                    // to be taken always, which skipped the body of every
                    // divergent if in every shader - the intro's fog pass
                    // lost all of its arithmetic and wrote the ray it had
                    // started from, the magenta wedge behind "Sony
                    // Interactive Entertainment".
                    condition = negate_condition(lane_in(bank.read(126)));
                } else {
                    const auto reads_vcc =
                        decoded.name == "SCbranchVccz" ||
                        decoded.name == "SCbranchVccnz";
                    const auto value = load_condition(
                        reads_vcc ? vcc_variable : scc_variable);
                    const auto taken_when_set =
                        decoded.name == "SCbranchScc1" ||
                        decoded.name == "SCbranchVccnz";
                    condition =
                        taken_when_set ? value : negate_condition(value);
                }
            } else if (!is_program_end(decoded.name) &&
                       !is_unconditional_branch(decoded.name) &&
                       !is_conditional_branch(decoded.name)) {
                ++result.instructions_skipped;
                ++result.skipped_by_name[std::string(decoded.name)];
            }
            ++result.dispatcher_loads;
            ++result.dispatcher_stores;
            if (is_program_end(decoded.name) ||
                is_unconditional_branch(decoded.name) ||
                is_conditional_branch(decoded.name)) {
                break;
            }
            pc += size;
            // The next block starts here, so this one is over.
            bool reached_next = false;
            for (const auto& other : graph.blocks) {
                if (other.start_pc == pc) {
                    reached_next = true;
                    break;
                }
            }
            if (reached_next) {
                break;
            }
        }
        // Everything this block wrote goes to memory before control leaves
        // it, because the next block reads it from there.
        bank.flush();
        vector_bank.flush();
        return condition;
    };

    result.ok = emit_control_flow(module, graph, plan, labels, emit_body);
    module.add_function_word(ps5spirv::Op::FunctionEnd, {});
    // An attribute the shader only ever moves, never interpolates, is
    // bits the vertex stage packed - two halves, an index - and has to
    // arrive as the provoking vertex wrote it. Interpolated, packed halves
    // that read as denormals came out as zero: the UI plane's alpha, and
    // with it the intro video, among them. One read the other way keeps
    // the attribute interpolated, since a variable is one or the other.
    for (const auto attribute : moved_attributes) {
        if (interpolated_attributes.count(attribute) != 0) {
            continue;
        }
        flat_attributes.insert(attribute);
    }
    for (const auto attribute : flat_attributes) {
        const auto input = stage_io.inputs.find(attribute);
        if (input != stage_io.inputs.end()) {
            module.add_decoration(
                input->second,
                static_cast<ps5spirv::Decoration>(kDecorationFlat), {});
        }
    }
    // The entry point goes in last, because its interface has to name every
    // variable the body declared - and the position, the colours and the
    // inputs are declared by the exports and interpolations as the body
    // meets them. Written before the body, it named none of them: a vertex
    // shader whose position is not in its interface is one whose position
    // the rasteriser never receives, and not one mesh reached the screen.
    for (const auto id : stage_io.interface_ids) {
        entry_interface.push_back(id);
    }
    if (labels.loop_budget != 0) {
        entry_interface.push_back(labels.loop_budget);
    }
    for (const auto id : bank.variables()) {
        entry_interface.push_back(id);
    }
    for (const auto id : vector_bank.variables()) {
        entry_interface.push_back(id);
    }
    // v_movrels reads the register array by index, past the bank, which
    // then does not list it.
    if (std::find(entry_interface.begin(), entry_interface.end(),
                  vector_registers) == entry_interface.end()) {
        entry_interface.push_back(vector_registers);
    }
    module.add_entry_point(
        stage == StageKind::Vertex ? ps5spirv::ExecutionModel::Vertex
        : stage == StageKind::Pixel ? ps5spirv::ExecutionModel::Fragment
                                    : ps5spirv::ExecutionModel::GLCompute,
        entry, "main", entry_interface);
    result.words = module.build();
    result.register_stats = bank.stats();
    const auto& vector_stats = vector_bank.stats();
    result.register_stats.loads_emitted += vector_stats.loads_emitted;
    result.register_stats.stores_emitted += vector_stats.stores_emitted;
    result.register_stats.reads_served_from_cache +=
        vector_stats.reads_served_from_cache;
    result.register_stats.writes_kept_in_registers +=
        vector_stats.writes_kept_in_registers;
    if (result.emission_reuse != 0) {
        EmissionOutputs outputs;
        outputs.ok = result.ok;
        outputs.words = result.words;
        outputs.register_stats = result.register_stats;
        outputs.instructions_translated = result.instructions_translated;
        outputs.instructions_skipped = result.instructions_skipped;
        outputs.dispatcher_loads = result.dispatcher_loads;
        outputs.dispatcher_stores = result.dispatcher_stores;
        outputs.subroutine_calls = result.subroutine_calls;
        outputs.loads_emitted = result.loads_emitted;
        outputs.buffers_declared = result.buffers_declared;
        outputs.writes_memory = result.writes_memory;
        outputs.skipped_by_name = result.skipped_by_name;
        outputs.typed_formats_unhandled = result.typed_formats_unhandled;
        outputs.exports_by_target = result.exports_by_target;
        if (!emission_cache.store(key.words(), outputs)) {
            result.emission_reuse = 4;
        }
    }
    return result;
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_TRANSLATE_H
