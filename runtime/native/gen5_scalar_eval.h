// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Running a shader's scalar half ahead of time, to find out what it reads.
//
// A GCN shader does not name its resources. It is handed a few words of
// user data in its scalar registers, and it computes addresses from them -
// a scalar load fetches a descriptor, whose bits are then a buffer or a
// texture. Which resource an instruction touches is therefore not in the
// instruction: it is the value a register happens to hold by the time
// control reaches it.
//
// So the descriptors are found by interpretation. The scalar half is
// uniform across a wave, which makes it interpretable without knowing
// anything about lanes, and the values it produces are the same every time
// for a given set of user data. Walking every scalar path and recording
// what each memory instruction would address gives the resource manifest
// the runtime binds from.
//
// This is the last dependency between the native translator and the C#
// one, and it is what the resource half of the translation waits on.
// Ported here as the interpreter alone: the operations already have a
// table, so what is new is the walk, the state, and knowing when a value
// is not knowable.

#ifndef PS5_GEN5_SCALAR_EVAL_H
#define PS5_GEN5_SCALAR_EVAL_H

#include <array>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <set>
#include <vector>

#include "gen5_cfg.h"
#include "gen5_emit_alu.h"

namespace ps5gen5 {

constexpr std::size_t kScalarRegisterCount = 256;
// Every lane of a wave64 active, which is what a shader starts with.
constexpr std::uint64_t kWaveMask = 0xFFFFFFFFFFFFFFFFull;

struct ScalarState {
    std::array<std::uint32_t, kScalarRegisterCount> registers{};
    // Which registers hold a value this walk actually knows. A register
    // written by the vector half, or loaded from memory that could not be
    // read, holds nothing - and using a stale zero as if it were a real
    // address is how a resource gets resolved to the wrong place.
    std::array<bool, kScalarRegisterCount> known{};
    // Which source word a register holds, untouched: one more than its
    // index in the evaluation's sources, or zero for a value the walk
    // computed. See ScalarSource.
    std::array<std::uint16_t, kScalarRegisterCount> origin{};
    std::uint64_t exec = kWaveMask;
    bool scc = false;
    // Whether the condition code holds a value this walk knows. The
    // instructions that read it are useless without it, and assuming false
    // would resolve a descriptor out of the wrong arm of a select.
    bool scc_known = false;
};

// What a compare leaves in the condition code.
inline bool compare_result(
    AluOperation operation, std::uint32_t source0, std::uint32_t source1) {
    const auto signed0 = static_cast<std::int32_t>(source0);
    const auto signed1 = static_cast<std::int32_t>(source1);
    switch (operation) {
        case AluOperation::CompareEqual: return source0 == source1;
        case AluOperation::CompareNotEqual: return source0 != source1;
        case AluOperation::CompareLessUnsigned: return source0 < source1;
        case AluOperation::CompareGreaterUnsigned: return source0 > source1;
        case AluOperation::CompareLessEqualUnsigned: return source0 <= source1;
        case AluOperation::CompareGreaterEqualUnsigned:
            return source0 >= source1;
        case AluOperation::CompareLessSigned: return signed0 < signed1;
        case AluOperation::CompareGreaterSigned: return signed0 > signed1;
        case AluOperation::CompareLessEqualSigned: return signed0 <= signed1;
        case AluOperation::CompareGreaterEqualSigned: return signed0 >= signed1;
        default: return false;
    }
}

// What a memory instruction was found to address.
struct ResolvedLoad {
    std::uint32_t pc = 0;
    std::uint64_t address = 0;
    std::uint32_t dword_count = 0;
    std::uint32_t destination = 0;
    bool address_known = false;
    // Where the address came from, so a wrong one can be traced to
    // the register that held it rather than guessed at.
    std::uint32_t base_register = 0;
    std::uint32_t base_low = 0;
    std::uint32_t base_high = 0;
    // The address split the way a binding needs it: where the memory
    // begins and how far into it this load reaches. A load that becomes a
    // real load rather than a constant is a read at this offset from a
    // buffer bound at this base.
    std::uint64_t base_address = 0;
    std::uint32_t byte_offset = 0;
    std::uint32_t region_size = 0;
    // Which encoding the fields were read from, because two of them
    // produce the same instruction names and put their fields in
    // different places.
    bool from_smem = false;
    std::uint32_t word0 = 0;
    // A load whose base is known but whose offset is a register the walk
    // cannot follow - an index read out of a vector lane. The register,
    // or 0x7D (null) when the offset is all immediate.
    std::uint32_t offset_register = 0x7Du;
    bool base_known = false;
    // For replaying the walk against other memory: whether the base is a
    // V#, the words it was decoded from and the source each came from, and
    // the source the first word this load fetched became.
    bool buffer_load = false;
    bool base_words_known = false;
    std::array<std::uint32_t, 4> base_words{};
    std::array<std::uint16_t, 4> base_origin{};
    // The source each word this load fetched is. A word of memory is one
    // source however many loads read it.
    std::array<std::uint16_t, 16> word_source{};
};

// An image instruction and the descriptor it was found to use. The
// descriptor is eight words of T# sitting in the scalar registers the
// instruction names, put there by a scalar load this walk already
// followed - which is the whole reason the walk exists.
struct ResolvedImage {
    std::uint32_t pc = 0;
    std::uint32_t resource_register = 0;
    std::uint32_t sampler_register = 0;
    std::uint32_t dmask = 0;
    std::array<std::uint32_t, 8> descriptor{};
    std::array<std::uint32_t, 4> sampler{};
    std::array<std::uint16_t, 8> descriptor_origin{};
    std::array<std::uint16_t, 4> sampler_origin{};
    bool descriptor_known = false;
    bool sampler_known = false;
    bool is_sample = false;
    // Which of the eight registers the descriptor lives in were not known,
    // as a bitmap. An image that resolves nothing says only that; this says
    // whether none of it was there or only the tail.
    std::uint32_t missing_mask = 0;
};

// A buffer instruction and the V# it names. Four registers rather than
// eight, and unlike an image the address it forms is partly computed on
// the GPU - the descriptor gives the base, the instruction gives the rest.
struct ResolvedBuffer {
    std::uint32_t pc = 0;
    std::uint32_t resource_register = 0;
    std::array<std::uint32_t, 4> descriptor{};
    std::array<std::uint16_t, 4> descriptor_origin{};
    bool descriptor_known = false;
    bool is_store = false;
};

// A word the walk was given rather than computed: one of the user data
// registers, or a word a load fetched. Most of what a draw changes from
// the last one is here - a buffer's address in a V#, a pointer to this
// draw's table - and the walk does nothing with such a word but read
// memory through it or hand it on as a descriptor. Those uses can be
// replayed against the new words without walking the shader again. A word
// the walk computed with, compared or merged is different: what it did
// depended on the value, so the value has to be the same - `exact`.
struct ScalarSource {
    std::uint32_t value = 0;
    bool known = false;
    bool exact = false;
    // Where the walk first used it as a value, to say why a replay
    // refused.
    std::uint32_t exact_pc = 0;
};

// A compare the walk made of two words, and what came of it. Only the
// outcome reaches anything - the condition code a select reads - so a
// replay needs the outcome the same, not the words: a flag that is still
// set, a count that is still not zero.
struct ScalarCompare {
    AluOperation operation = AluOperation::Unknown;
    std::uint16_t origin0 = 0;
    std::uint16_t origin1 = 0;
    std::uint32_t value0 = 0;
    std::uint32_t value1 = 0;
    bool result = false;
};

struct ScalarEvaluation {
    ScalarState initial;
    ScalarState final_state;
    // The sources, user data first, and whether every one the walk used
    // was recorded - without which it cannot be replayed.
    std::vector<ScalarSource> sources;
    std::vector<ScalarCompare> compares;
    std::uint32_t user_sources = 0;
    bool replayable = true;
    std::vector<ResolvedLoad> loads;
    std::vector<ResolvedImage> images;
    std::vector<ResolvedBuffer> buffers;
    std::uint32_t paths_walked = 0;
    std::uint32_t instructions_executed = 0;
    // Paths abandoned because the walk had already seen the same program
    // counter with the same state, which is what stops a loop from being
    // followed forever.
    std::uint32_t paths_merged = 0;
    // Words of guest memory the walk asked for, and how many it got.
    // A load whose address is known but whose memory is not readable
    // resolves nothing, and a descriptor is eight such words.
    std::uint32_t memory_reads = 0;
    std::uint32_t memory_reads_failed = 0;
    // Why a buffer load did not resolve: a descriptor register the walk
    // never learned, or four registers it knew that do not describe a
    // buffer. The two are different problems and look identical in a
    // count of unresolved loads.
    std::uint32_t descriptors_unknown = 0;
    std::uint32_t descriptors_rejected = 0;
    // Which register a descriptor was expected in, when it was not there.
    // A count of unknown descriptors says how many; this says where to
    // look, which is the difference between guessing at user data and
    // knowing the shader reads a register nothing wrote.
    std::map<std::uint32_t, std::uint32_t> descriptor_unknown_at;
    // Loads that resolved nothing, by the name of the instruction and by
    // the register they would have filled. A descriptor missing from s24
    // is a load into s24 that did not resolve, and this says which load.
    std::map<std::string, std::uint32_t> unresolved_loads_by_name;
    // Which instructions made a scalar register unknown. A register
    // that has to be unknown is honest; one that is unknown only
    // because this walk cannot execute the instruction that wrote it
    // is work waiting to be done, and the names say which.
    std::map<std::string, std::uint32_t> unknown_by_name;
    bool exhausted = false;
};

// The sources of the evaluation this thread is in the middle of, for the
// register readers below to mark.
inline std::vector<ScalarSource>*& scalar_sources_in_progress() {
    static thread_local std::vector<ScalarSource>* sources = nullptr;
    return sources;
}

inline std::uint32_t& scalar_pc_in_progress() {
    static thread_local std::uint32_t pc = 0;
    return pc;
}

// The walk used this register's value for something other than reading
// memory through it or passing it on, so a replay needs it unchanged.
inline void scalar_mark_exact(const ScalarState& state, std::uint32_t reg) {
    auto* sources = scalar_sources_in_progress();
    if (sources == nullptr || reg >= kScalarRegisterCount) {
        return;
    }
    const auto origin = state.origin[reg];
    if (origin != 0 && origin <= sources->size()) {
        auto& source = (*sources)[origin - 1];
        if (!source.exact) {
            source.exact = true;
            source.exact_pc = scalar_pc_in_progress();
        }
    }
}

// A scalar register pair holds a 64-bit value, low half first.
inline void write_pair(
    ScalarState& state, std::uint32_t reg, std::uint64_t value) {
    if (reg + 1 >= kScalarRegisterCount) {
        return;
    }
    state.registers[reg] = static_cast<std::uint32_t>(value);
    state.registers[reg + 1] = static_cast<std::uint32_t>(value >> 32);
    state.known[reg] = true;
    state.known[reg + 1] = true;
    state.origin[reg] = 0;
    state.origin[reg + 1] = 0;
}

// The pair as it stands, for a use that can be replayed: a load's base.
inline std::uint64_t read_pair_unmarked(
    const ScalarState& state, std::uint32_t reg) {
    if (reg + 1 >= kScalarRegisterCount) {
        return 0;
    }
    return static_cast<std::uint64_t>(state.registers[reg]) |
        (static_cast<std::uint64_t>(state.registers[reg + 1]) << 32);
}

inline std::uint64_t read_pair(
    const ScalarState& state, std::uint32_t reg) {
    scalar_mark_exact(state, reg);
    scalar_mark_exact(state, reg + 1);
    return read_pair_unmarked(state, reg);
}

// The value an encoded scalar source means. Below 128 it is a register;
// 128 is zero; 129 to 192 are the small positive constants; 193 to 208 the
// small negative ones. Getting this wrong turns a literal into a register
// read and resolves a descriptor from whatever that register held.
inline bool scalar_source_value(
    const ScalarState& state,
    std::uint32_t encoded,
    std::uint32_t literal,
    std::uint32_t& value,
    // Whether reading it is using it. Not where the fields of a word are
    // read before it is known to be an instruction that has them.
    bool mark = true) {
    if (encoded < kScalarRegisterCount && encoded < 128) {
        if (mark) {
            scalar_mark_exact(state, encoded);
        }
        value = state.registers[encoded];
        return state.known[encoded];
    }
    if (encoded == 128) {
        value = 0;
        return true;
    }
    if (encoded >= 129 && encoded <= 192) {
        value = encoded - 128;
        return true;
    }
    if (encoded >= 193 && encoded <= 208) {
        value = static_cast<std::uint32_t>(-static_cast<std::int32_t>(
            encoded - 192));
        return true;
    }
    if (encoded == 255) {
        value = literal;
        return true;
    }
    // EXEC and VCC are registers 126 and 106 by convention, already
    // covered above; anything else is not a value this can know.
    value = 0;
    return false;
}

// A buffer descriptor - a V# - is four registers, and its base address is
// not a flat 64-bit value: 32 bits in the first word and 16 more in the
// low half of the second, with the stride above them. A buffer load whose
// base is read as a plain pair therefore takes the stride and the format
// bits as part of the address.
struct BufferDescriptor {
    std::uint64_t base_address = 0;
    std::uint32_t stride = 0;
    std::uint32_t num_records = 0;
    bool valid = false;
};

inline BufferDescriptor decode_buffer_descriptor(
    const ScalarState& state, std::uint32_t base_register) {
    BufferDescriptor descriptor;
    if (base_register + 3 >= kScalarRegisterCount) {
        return descriptor;
    }
    for (std::uint32_t index = 0; index < 4; ++index) {
        if (!state.known[base_register + index]) {
            return descriptor;
        }
    }
    const auto word0 = state.registers[base_register];
    const auto word1 = state.registers[base_register + 1];
    const auto word2 = state.registers[base_register + 2];
    const auto word3 = state.registers[base_register + 3];
    if (word0 == 0 && word1 == 0 && word2 == 0 && word3 == 0) {
        // All zero is an unbound descriptor, which is a legal state rather
        // than a failure to read one.
        descriptor.valid = true;
        return descriptor;
    }
    // The top two bits say what kind of descriptor this is; anything but
    // zero is not a buffer, and reading it as one gives an address from
    // fields that mean something else.
    if ((word3 >> 30) != 0) {
        return descriptor;
    }
    descriptor.base_address =
        word0 | (static_cast<std::uint64_t>(word1 & 0xFFFFu) << 32);
    descriptor.stride = (word1 >> 16) & 0x3FFFu;
    descriptor.num_records = word2;
    descriptor.valid = true;
    return descriptor;
}

// Interprets the scalar half. `read_word` fetches instruction words;
// `read_memory` fetches a dword of guest memory and says whether it could.
template <typename ReadWord, typename ReadMemory>
ScalarEvaluation evaluate_scalar(
    std::uint32_t entry_pc,
    const std::vector<std::uint32_t>& user_data,
    std::uint32_t user_data_base,
    const ReadWord& read_word,
    const ReadMemory& read_memory,
    // Where the shader's first word lives in guest memory. SGetpcB64 gives
    // a shader its own address to compute resource addresses from, and
    // without this the walk cannot follow that - 53 registers a run.
    std::uint64_t code_base = 0,
    // Bounds, because this runs while the guest waits for its shader to
    // compile. An unbounded walk over a real shader - 347 blocks, branches
    // everywhere - does not take a long time, it does not finish: the
    // title stalled in a semaphore loop with 29 imports resolved instead of
    // 62 and no frame at all. What a bound costs is a descriptor that goes
    // unresolved, which is the failure that declines to bind rather than
    // binding the wrong thing.
    // Raised once the inner loop had its own bound: a backward branch can
    // no longer spin, so the path count can be generous. Including the
    // buffer loads widened the walk enough that 256 paths was binding -
    // shaders came back with 2 of 26 loads resolved and exhausted=1.
    std::uint32_t path_limit = 4096,
    std::uint32_t instruction_limit = 2000000) {
    ScalarEvaluation evaluation;
    struct SourcesScope {
        explicit SourcesScope(std::vector<ScalarSource>* sources)
            : previous(scalar_sources_in_progress()) {
            scalar_sources_in_progress() = sources;
        }
        ~SourcesScope() { scalar_sources_in_progress() = previous; }
        std::vector<ScalarSource>* previous;
    } sources_scope(&evaluation.sources);
    // A source for a register, or none once there are more than an origin
    // can name - after which the walk is not one to replay.
    const auto new_source = [&](std::uint32_t value, bool known) {
        if (evaluation.sources.size() >= 0xFFFFu) {
            evaluation.replayable = false;
            return std::uint16_t{0};
        }
        ScalarSource source;
        source.value = value;
        source.known = known;
        evaluation.sources.push_back(source);
        return static_cast<std::uint16_t>(evaluation.sources.size());
    };

    std::map<std::uint64_t, std::uint16_t> source_at;

    ScalarState start;
    for (std::size_t index = 0; index < user_data.size(); ++index) {
        // One for every word of user data, in order, whether or not it
        // fits a register: a replay lines its user data up by index.
        const auto origin = new_source(user_data[index], true);
        if (user_data_base + index < kScalarRegisterCount) {
            start.registers[user_data_base + index] = user_data[index];
            start.known[user_data_base + index] = true;
            start.origin[user_data_base + index] = origin;
        }
    }
    evaluation.user_sources =
        static_cast<std::uint32_t>(evaluation.sources.size());
    // VCC starts clear and EXEC starts with every lane on, which is what
    // the hardware hands a shader.
    write_pair(start, 106, 0);
    write_pair(start, 126, kWaveMask);
    start.exec = kWaveMask;
    evaluation.initial = start;

// Merging what two paths know: a register survives only if both sides
    // know it and agree. Anything else depends on which way control came,
    // and that is exactly what must not become an address.
    std::set<std::pair<std::uint16_t, std::uint16_t>> merged_origins;
    const auto merge_into = [&](ScalarState& into, const ScalarState& from) {
        auto changed = false;
        for (std::size_t index = 0; index < kScalarRegisterCount; ++index) {
            if (!into.known[index]) {
                continue;
            }
            // Two paths' words compared, and what the walk keeps depends
            // on whether they agree: a replay needs them to agree or
            // differ as they did, unless they are the same word. When
            // they agree the register is either word.
            if (from.known[index] && from.origin[index] != into.origin[index] &&
                merged_origins
                    .insert({into.origin[index], from.origin[index]})
                    .second) {
                ScalarCompare compare;
                compare.operation = AluOperation::CompareEqual;
                compare.origin0 = into.origin[index];
                compare.origin1 = from.origin[index];
                compare.value0 = into.registers[index];
                compare.value1 = from.registers[index];
                compare.result =
                    into.registers[index] == from.registers[index];
                evaluation.compares.push_back(compare);
            }
            if (!from.known[index] ||
                from.registers[index] != into.registers[index]) {
                into.known[index] = false;
                changed = true;
            }
        }
        return changed;
    };

    // Keyed by program counter rather than by counter and state. Keying on
    // both explores every combination of branches, which grows
    // exponentially: adding the buffer loads was enough to make eleven of
    // this title's thirty-four shaders run out of budget, and a shader that
    // runs out reports two resolved loads of twenty-six rather than an
    // honest twenty-six unknowns. Merging at the point where paths meet
    // visits each place a handful of times whatever the shape, and
    // terminates because a merge only ever takes knowledge away.
    //
    // The walks are also taken in address order, with everything arriving
    // at one place merged before it is walked, and a walk that runs into a
    // place a branch goes to stops there. Otherwise a path that falls
    // through into a join carries on past it with what only it knew, to
    // the end of the program, once per path: a scene's vertex shaders
    // walked three hundred paths, recorded each load once per path, and
    // spent eight milliseconds a translation doing it.
    // PS5RT_SCALAR_WALK_OLD=1 walks the old way, for comparison.
    static const bool old_walk = [] {
        const auto* value = std::getenv("PS5RT_SCALAR_WALK_OLD");
        return value != nullptr && value[0] == '1';
    }();
    std::map<std::uint32_t, ScalarState> arriving;
    std::vector<std::pair<std::uint32_t, ScalarState>> pending;
    std::map<std::uint32_t, ScalarState> pending_by_pc;
    std::set<std::uint32_t> targets;
    const auto queue = [&](std::uint32_t target, const ScalarState& state) {
        if (old_walk) {
            pending.push_back({target, state});
            return;
        }
        targets.insert(target);
        const auto found = pending_by_pc.find(target);
        if (found == pending_by_pc.end()) {
            pending_by_pc.emplace(target, state);
        } else {
            merge_into(found->second, state);
        }
    };
    queue(entry_pc, start);

    while (old_walk ? !pending.empty() : !pending_by_pc.empty()) {
        if (evaluation.paths_walked >= path_limit ||
            evaluation.instructions_executed >= instruction_limit) {
            evaluation.exhausted = true;
            break;
        }
        std::uint32_t pc = 0;
        ScalarState state;
        if (old_walk) {
            pc = pending.back().first;
            state = pending.back().second;
            pending.pop_back();
        } else {
            const auto first = pending_by_pc.begin();
            pc = first->first;
            state = first->second;
            pending_by_pc.erase(first);
        }
        const auto arrived = arriving.find(pc);
        if (arrived == arriving.end()) {
            arriving.emplace(pc, state);
        } else if (merge_into(arrived->second, state)) {
            // Something this path knew is not known on all of them, so the
            // walk carries on with less rather than with a value that
            // depends on the way it came.
            state = arrived->second;
            ++evaluation.paths_merged;
        } else {
            // Nothing new to learn here.
            ++evaluation.paths_merged;
            continue;
        }
        ++evaluation.paths_walked;

        const auto walk_start = pc;
        while (true) {
            if (evaluation.instructions_executed >= instruction_limit) {
                evaluation.exhausted = true;
                break;
            }
            if (!old_walk && pc != walk_start && targets.count(pc) != 0) {
                queue(pc, state);
                break;
            }
            std::uint32_t size = 0;
            const auto decoded = decode_one(pc, read_word, size);
            if (!decoded.ok() || size == 0) {
                break;
            }
            scalar_pc_in_progress() = pc;
            ++evaluation.instructions_executed;
            const auto word = read_word(pc);
            const auto literal = size > 1 ? read_word(pc + 1) : 0u;
            // The vector instructions that write scalar registers: a
            // compare's lane mask (VCC, or the pair a VOP3 or SDWA form
            // names), a carry out, a lane read. What they write depends on
            // the lanes, so the registers become unknown. Left as they
            // were, s[0:1] still held a descriptor after a v_cmp had put a
            // mask there, and the s_or_b64 that combined the masks read the
            // descriptor as a value.
            {
                const auto forget_scalar = [&](std::uint32_t reg,
                                               std::uint32_t count) {
                    for (std::uint32_t index = 0; index < count; ++index) {
                        if (reg + index < kScalarRegisterCount) {
                            state.known[reg + index] = false;
                            state.origin[reg + index] = 0;
                        }
                    }
                };
                if ((word >> 25) == 0x3Eu) {
                    // VOPC: VCC, or with SDWA the pair it names.
                    std::uint32_t target = 106;
                    if ((word & 0x1FFu) == 0xF9u && size > 1) {
                        const auto extra = read_word(pc + 1);
                        if (((extra >> 15) & 1u) != 0) {
                            target = (extra >> 8) & 0x7Fu;
                        }
                    }
                    forget_scalar(target, 2);
                } else if ((word >> 31) == 0u && (word >> 25) >= 0x28u &&
                           (word >> 25) <= 0x2Au) {
                    // VOP2 add/sub with carry: the carry out is VCC.
                    forget_scalar(106, 2);
                } else if ((word >> 25) == 0x3Fu &&
                           ((word >> 9) & 0xFFu) == 0x02u) {
                    // v_readfirstlane_b32: its destination is an SGPR.
                    forget_scalar((word >> 17) & 0xFFu, 1);
                } else if ((word >> 26) == 0x35u) {
                    const auto opcode = (word >> 16) & 0x3FFu;
                    if (opcode < 0x100u) {
                        // VOPC in VOP3: the destination pair.
                        forget_scalar(word & 0xFFu, 2);
                    } else if (opcode == 0x128u || opcode == 0x129u ||
                               opcode == 0x12Au || opcode == 0x16Du ||
                               opcode == 0x16Eu || opcode == 0x176u ||
                               opcode == 0x177u || opcode == 0x30Fu ||
                               opcode == 0x310u || opcode == 0x319u) {
                        // VOP3B: a carry or a flag into a pair.
                        forget_scalar((word >> 8) & 0x7Fu, 2);
                    } else if (opcode == 0x182u || opcode == 0x360u) {
                        // v_readfirstlane, v_readlane: one SGPR.
                        forget_scalar(word & 0xFFu, 1);
                    }
                }
            }

            if (is_program_end(decoded.name)) {
                break;
            }
            if (is_unconditional_branch(decoded.name)) {
                // Queued rather than followed here. Following it in place
                // bypasses the merge, so a backward branch spins this loop:
                // eleven of the title's shaders reported one path walked
                // and still exhausted, having executed two million
                // instructions going round one loop.
                queue(branch_target(pc, word), state);
                break;
            }
            if (is_conditional_branch(decoded.name)) {
                // Both ways, because which is taken depends on values the
                // vector half may decide.
                queue(branch_target(pc, word), state);
                pc += size;
                continue;
            }

            // A scalar load reads a descriptor out of memory. Its address
            // is a register pair plus an offset, and it is the whole reason
            // this walk exists.
            // Both families of scalar load: the plain one that reads an
            // address, and the buffer one that reads a descriptor. Testing
            // only the prefix "SLoad" left every SBufferLoad out of the
            // walk entirely, which is why decoding their descriptors
            // changed nothing at all.
            if ((decoded.name.size() > 5 &&
                 decoded.name.substr(0, 5) == "SLoad") ||
                (decoded.name.size() > 11 &&
                 decoded.name.substr(0, 11) == "SBufferLoad")) {
                // SMEM sits at two places in the encoding, 0x33 and
                // 0x3D, and this title uses the second: every load
                // it issues has 0xF4 in its top byte. Testing only
                // the first read every one of them with SMRD's
                // field layout, taking a base register out of bits
                // that hold something else.
                const auto top6 = word >> 26;
                const auto is_smem = top6 == 0x33u || top6 == 0x3Du;
                std::uint32_t base_register = 0;
                std::uint32_t destination = 0;
                std::uint64_t byte_offset = 0;
                std::uint32_t soffset_register = 0x7Du;
                if (is_smem) {
                    // SMEM: base pair at the bottom in units of two, data
                    // register above it, and a byte offset in the second
                    // word.
                    base_register = (word & 0x3Fu) * 2;
                    destination = (word >> 6) & 0x7Fu;
                    byte_offset = read_word(pc + 1) & 0x1FFFFFu;
                    // And a register added to it, at the top of the second
                    // word, unless that names null. A table indexed by a
                    // value the shader computed - a V# picked out of the
                    // vertex buffer table by attribute - reads its first
                    // entry for every attribute without this.
                    soffset_register = read_word(pc + 1) >> 25;
                } else {
                    // SMRD: a byte of dword offset at the bottom, the flag
                    // that says it is immediate at bit 8, the base pair at
                    // 9 and the destination at 15.
                    base_register = ((word >> 9) & 0x3Fu) * 2;
                    destination = (word >> 15) & 0x7Fu;
                    byte_offset = (word & 0xFFu) * 4ull;
                }
                ResolvedLoad load;
                load.pc = pc;
                load.destination = destination;
                // By the suffix, not by the whole name. Spelled out in
                // full it matched SLoadDword and its widths and nothing
                // else, so every SBufferLoad carried a count of zero -
                // and a load of zero words reads no memory and fills no
                // register, while still reporting its address as known.
                const auto ends_with =
                    [&](std::string_view suffix) {
                        return decoded.name.size() >= suffix.size() &&
                            decoded.name.substr(
                                decoded.name.size() - suffix.size()) ==
                                suffix;
                    };
                load.dword_count =
                    ends_with("Dwordx16") ? 16
                    : ends_with("Dwordx8") ? 8
                    : ends_with("Dwordx4") ? 4
                    : ends_with("Dwordx3") ? 3
                    : ends_with("Dwordx2") ? 2
                    : ends_with("Dword") ? 1
                    : 0;
                // A buffer load names a descriptor rather than an
                // address, and the two are read differently.
                const auto is_buffer_load =
                    decoded.name.size() > 11 &&
                    decoded.name.substr(0, 11) == "SBufferLoad";
                std::uint64_t base = 0;
                if (is_buffer_load) {
                    const auto descriptor =
                        decode_buffer_descriptor(state, base_register);
                    base = descriptor.base_address;
                    load.address_known = descriptor.valid;
                    load.base_known = descriptor.valid;
                    load.buffer_load = true;
                    load.base_words_known =
                        base_register + 3 < kScalarRegisterCount;
                    for (std::uint32_t index = 0;
                         load.base_words_known && index < 4; ++index) {
                        load.base_words_known =
                            state.known[base_register + index];
                    }
                    if (load.base_words_known) {
                        for (std::uint32_t index = 0; index < 4; ++index) {
                            load.base_words[index] =
                                state.registers[base_register + index];
                            load.base_origin[index] =
                                state.origin[base_register + index];
                        }
                    }
                    if (!descriptor.valid) {
                        auto all_known = base_register + 3 <
                            kScalarRegisterCount;
                        for (std::uint32_t index = 0;
                             all_known && index < 4; ++index) {
                            all_known = state.known[base_register + index];
                        }
                        if (all_known) {
                            ++evaluation.descriptors_rejected;
                        } else {
                            ++evaluation.descriptors_unknown;
                            ++evaluation
                                  .descriptor_unknown_at[base_register];
                        }
                    }
                } else {
                    base = read_pair_unmarked(state, base_register);
                    load.address_known =
                        base_register + 1 < kScalarRegisterCount &&
                        state.known[base_register] &&
                        state.known[base_register + 1];
                    load.base_known = load.address_known;
                    load.base_words_known = load.base_known;
                    if (load.base_words_known) {
                        for (std::uint32_t index = 0; index < 2; ++index) {
                            load.base_words[index] =
                                state.registers[base_register + index];
                            load.base_origin[index] =
                                state.origin[base_register + index];
                        }
                    }
                }
                if (soffset_register != 0x7Du) {
                    std::uint32_t added = 0;
                    if (scalar_source_value(
                            state, soffset_register, 0, added)) {
                        byte_offset += added;
                    } else {
                        load.address_known = false;
                        load.offset_register = soffset_register;
                    }
                }
                load.address = base + byte_offset;
                load.base_address = base;
                load.byte_offset = static_cast<std::uint32_t>(byte_offset);
                if (is_buffer_load) {
                    const auto descriptor =
                        decode_buffer_descriptor(state, base_register);
                    load.region_size = descriptor.stride == 0
                        ? descriptor.num_records
                        : descriptor.num_records * descriptor.stride;
                }
                load.from_smem = is_smem;
                load.word0 = word;
                load.base_register = base_register;
                load.base_low = base_register < kScalarRegisterCount
                    ? state.registers[base_register] : 0;
                load.base_high = base_register + 1 <
                        kScalarRegisterCount
                    ? state.registers[base_register + 1] : 0;
                if (load.address_known && load.dword_count != 0) {
                    for (std::uint32_t index = 0;
                         index < load.dword_count &&
                         destination + index < kScalarRegisterCount;
                         ++index) {
                        std::uint32_t fetched = 0;
                        ++evaluation.memory_reads;
                        const auto read =
                            read_memory(load.address + index * 4ull, fetched);
                        // The same word read again - a table loaded on
                        // both arms of a branch - is the same source, so
                        // the arms still agree where they meet.
                        auto& origin =
                            source_at[load.address + index * 4ull];
                        if (origin == 0) {
                            origin = new_source(read ? fetched : 0, read);
                        }
                        if (index < load.word_source.size()) {
                            load.word_source[index] = origin;
                        }
                        state.origin[destination + index] = origin;
                        if (read) {
                            state.registers[destination + index] = fetched;
                            state.known[destination + index] = true;
                        } else {
                            ++evaluation.memory_reads_failed;
                            state.known[destination + index] = false;
                        }
                    }
                }
                if (!load.address_known) {
                    ++evaluation.unresolved_loads_by_name[
                        std::string(decoded.name)];
                    // What the registers held before is not what the load
                    // put there. Left marked known, a list walk's "next"
                    // kept its old value, and so did everything computed
                    // from it.
                    for (std::uint32_t index = 0;
                         index < load.dword_count &&
                         destination + index < kScalarRegisterCount;
                         ++index) {
                        state.known[destination + index] = false;
                    }
                }
                evaluation.loads.push_back(load);
                pc += size;
                continue;
            }

            // An image instruction names its descriptor by register, and
            // by this point the walk has usually loaded it. MIMG puts the
            // resource at bits 16 to 20 of its second word and the sampler
            // at 21 to 25, both counted in fours, because a T# is eight
            // registers and an S# is four.
            // A buffer instruction names a V# the same way, at bits 16
            // to 20 of its second word and in fours. MUBUF and MTBUF are
            // the same shape here; what differs between them is how the
            // texel is formatted, which the descriptor says rather than the
            // instruction.
            if ((word >> 26) == 0x38u || (word >> 26) == 0x3Au) {
                const auto second = read_word(pc + 1);
                ResolvedBuffer buffer;
                buffer.pc = pc;
                buffer.resource_register = ((second >> 16) & 0x1Fu) * 4;
                buffer.is_store =
                    decoded.name.size() > 11 &&
                    decoded.name.substr(0, 11) == "BufferStore";
                buffer.descriptor_known = true;
                for (std::uint32_t index = 0; index < 4; ++index) {
                    const auto reg = buffer.resource_register + index;
                    if (reg >= kScalarRegisterCount || !state.known[reg]) {
                        buffer.descriptor_known = false;
                        continue;
                    }
                    buffer.descriptor[index] = state.registers[reg];
                    buffer.descriptor_origin[index] = state.origin[reg];
                }
                evaluation.buffers.push_back(buffer);
                pc += size;
                continue;
            }

            if ((word >> 26) == 0x3Cu) {
                const auto second = read_word(pc + 1);
                ResolvedImage image;
                image.pc = pc;
                image.dmask = (word >> 8) & 0xFu;
                // The second word holds the vector address at the bottom,
                // the vector data above it, then the resource and the
                // sampler, each five bits naming a group of four registers.
                // These were read five bits high, so what the walk called
                // the resource was the sampler and what it called the
                // sampler was nothing: the descriptor came out with its
                // tail missing, which is what an image reporting 0xC0 or
                // 0xF0 of eight words unknown was saying.
                image.resource_register = ((second >> 16) & 0x1Fu) * 4;
                image.sampler_register = ((second >> 21) & 0x1Fu) * 4;
                // ">= 11", not "> 11": the plain sample is named exactly
                // ImageSample, and it was declared a storage image and
                // read without a sampler.
                image.is_sample =
                    decoded.name.size() >= 11 &&
                    decoded.name.substr(0, 11) == "ImageSample";
                image.descriptor_known = true;
                for (std::uint32_t index = 0; index < 8; ++index) {
                    const auto reg = image.resource_register + index;
                    if (reg >= kScalarRegisterCount || !state.known[reg]) {
                        image.descriptor_known = false;
                        image.missing_mask |= 1u << index;
                        continue;
                    }
                    image.descriptor[index] = state.registers[reg];
                    image.descriptor_origin[index] = state.origin[reg];
                }
                image.sampler_known = image.is_sample;
                if (image.is_sample) {
                    for (std::uint32_t index = 0; index < 4; ++index) {
                        const auto reg = image.sampler_register + index;
                        if (reg >= kScalarRegisterCount ||
                            !state.known[reg]) {
                            image.sampler_known = false;
                            break;
                        }
                        image.sampler[index] = state.registers[reg];
                        image.sampler_origin[index] = state.origin[reg];
                    }
                }
                evaluation.images.push_back(image);
                pc += size;
                continue;
            }

            {
                const auto encoded0 = word & 0xFFu;
                const auto encoded1 = (word >> 8) & 0xFFu;
                const auto destination = (word >> 16) & 0x7Fu;
                std::uint32_t source0 = 0;
                std::uint32_t source1 = 0;
                // Read from every word that gets here, and only a use when
                // the word is a scalar instruction with sources there:
                // SOP2 and SOPC have two, SOP1 one, SOPK and SOPP none -
                // their low half is an immediate - and nothing else any.
                // Marked from a vector instruction's fields, s0 and s28
                // were "computed with" in every shader, and no walk could
                // be replayed for a draw whose textures had moved.
                const auto known0 = scalar_source_value(
                    state, encoded0, literal, source0, false);
                const auto known1 = scalar_source_value(
                    state, encoded1, literal, source1, false);
                if ((word >> 30) == 2u) {
                    const auto encoding = word >> 23;
                    // A compare (SOPC) is not a use of the values either:
                    // it records its outcome, below.
                    const auto immediate_form =
                        encoding == 0x17Fu || encoding == 0x17Eu ||
                        ((word >> 28) == 0xBu && encoding != 0x17Du &&
                         encoding != 0x17Eu);
                    if (!immediate_form) {
                        // The high half of a 64-bit operand is read, and
                        // marked, where the operation reads it.
                        if (encoded0 < 128) {
                            scalar_mark_exact(state, encoded0);
                        }
                        if (encoding != 0x17Du && encoded1 < 128) {
                            scalar_mark_exact(state, encoded1);
                        }
                    }
                }
                const auto put = [&](std::uint32_t reg, std::uint32_t value) {
                    if (reg < kScalarRegisterCount) {
                        state.registers[reg] = value;
                        state.known[reg] = true;
                        state.origin[reg] = 0;
                    }
                };
                const auto forget = [&](std::uint32_t reg, std::uint32_t n) {
                    for (std::uint32_t index = 0; index < n; ++index) {
                        if (reg + index < kScalarRegisterCount) {
                            state.known[reg + index] = false;
                        }
                    }
                };
                const auto pair_known = [&](std::uint32_t reg) {
                    return reg + 1 < kScalarRegisterCount &&
                        state.known[reg] && state.known[reg + 1];
                };
                auto handled = true;
                if (decoded.name == "SCselectB32" ||
                    decoded.name == "SCselectB64") {
                    // Picks a source on the condition code and leaves the
                    // code alone. Only knowable when the code is known.
                    const auto wide = decoded.name == "SCselectB64";
                    if (!state.scc_known || !known0 || !known1) {
                        forget(destination, wide ? 2 : 1);
                    } else {
                        put(destination, state.scc ? source0 : source1);
                        if (wide) {
                            std::uint32_t high0 = 0;
                            std::uint32_t high1 = 0;
                            const auto high_known0 = scalar_source_value(
                                state, encoded0 + 1, literal, high0);
                            const auto high_known1 = scalar_source_value(
                                state, encoded1 + 1, literal, high1);
                            if (high_known0 && high_known1) {
                                put(destination + 1,
                                    state.scc ? high0 : high1);
                            } else {
                                forget(destination + 1, 1);
                            }
                        }
                    }
                } else if (decoded.name == "SBitset0B32" ||
                           decoded.name == "SBitset1B32") {
                    // Reads its own destination and changes one bit of it.
                    const auto setting = decoded.name == "SBitset1B32";
                    if (known0 && destination < kScalarRegisterCount &&
                        state.known[destination]) {
                        const auto mask = 1u << (source0 & 31u);
                        scalar_mark_exact(state, destination);
                        const auto before = state.registers[destination];
                        put(destination, setting ? (before | mask)
                                                 : (before & ~mask));
                    } else {
                        forget(destination, 1);
                    }
                } else if (decoded.name == "SAddcU32") {
                    // Adds the carry, which is where the condition code goes
                    // in and comes back out.
                    if (known0 && known1 && state.scc_known) {
                        const auto sum = static_cast<std::uint64_t>(source0) +
                            source1 + (state.scc ? 1u : 0u);
                        put(destination, static_cast<std::uint32_t>(sum));
                        state.scc = sum > 0xFFFFFFFFull;
                    } else {
                        forget(destination, 1);
                        state.scc_known = false;
                    }
                } else if (decoded.name == "SBfeU32" ||
                           decoded.name == "SBfeI32" ||
                           decoded.name == "SBfeU64" ||
                           decoded.name == "SBfeI64") {
                    // Bitfield extract: offset in the low bits of the second
                    // source, width above it.
                    const auto wide = decoded.name == "SBfeU64" ||
                        decoded.name == "SBfeI64";
                    const auto is_signed = decoded.name == "SBfeI32" ||
                        decoded.name == "SBfeI64";
                    const auto have_value =
                        wide ? pair_known(encoded0) : known0;
                    if (!have_value || !known1) {
                        forget(destination, wide ? 2 : 1);
                        state.scc_known = false;
                    } else {
                        const auto offset = source1 & 0x3Fu;
                        const auto width = (source1 >> 16) & 0x7Fu;
                        const auto value = wide
                            ? read_pair(state, encoded0)
                            : static_cast<std::uint64_t>(source0);
                        std::uint64_t extracted = 0;
                        if (width != 0 && width < 64) {
                            extracted = (value >> offset) &
                                ((1ull << width) - 1ull);
                            if (is_signed &&
                                (extracted >> (width - 1)) != 0) {
                                extracted |= ~((1ull << width) - 1ull);
                            }
                        }
                        if (wide) {
                            write_pair(state, destination, extracted);
                        } else {
                            put(destination,
                                static_cast<std::uint32_t>(extracted));
                        }
                        state.scc = extracted != 0;
                        state.scc_known = true;
                    }
                } else if (decoded.name == "SAndn1SaveexecB64" ||
                           decoded.name == "SAndn2SaveexecB64" ||
                           decoded.name == "SAndSaveexecB64" ||
                           decoded.name == "SOrSaveexecB64" ||
                           decoded.name == "SXorSaveexecB64") {
                    // Keeps the old mask in the destination and narrows the
                    // live one. The half that matters here is the save: a
                    // descriptor address is often held in the register a
                    // saveexec writes.
                    const auto old_exec = read_pair(state, 126);
                    const auto exec_known = pair_known(126);
                    if (exec_known) {
                        write_pair(state, destination, old_exec);
                    } else {
                        forget(destination, 2);
                    }
                    if (pair_known(encoded0) && exec_known) {
                        const auto source = read_pair(state, encoded0);
                        const auto narrowed =
                            decoded.name == "SAndn1SaveexecB64"
                                ? (~source & old_exec)
                            : decoded.name == "SAndn2SaveexecB64"
                                ? (old_exec & ~source)
                            : decoded.name == "SOrSaveexecB64"
                                ? (source | old_exec)
                            : decoded.name == "SXorSaveexecB64"
                                ? (source ^ old_exec)
                                : (source & old_exec);
                        write_pair(state, 126, narrowed);
                        state.exec = narrowed;
                    } else {
                        forget(126, 2);
                    }
                    state.scc_known = false;
                } else if (decoded.name == "SLshl1AddU32" ||
                           decoded.name == "SLshl2AddU32" ||
                           decoded.name == "SLshl3AddU32" ||
                           decoded.name == "SLshl4AddU32") {
                    // Shift and add in one, which is how an index becomes a
                    // byte offset into a table of descriptors.
                    if (known0 && known1) {
                        const auto shift = static_cast<std::uint32_t>(
                            decoded.name[5] - '0');
                        const auto sum =
                            static_cast<std::uint64_t>(source0 << shift) +
                            source1;
                        put(destination, static_cast<std::uint32_t>(sum));
                        state.scc = sum > 0xFFFFFFFFull;
                        state.scc_known = true;
                    } else {
                        forget(destination, 1);
                        state.scc_known = false;
                    }
                } else if (decoded.name == "SFF1I32B64" ||
                           decoded.name == "SFF0I32B64" ||
                           decoded.name == "SFF1I32B32" ||
                           decoded.name == "SFF0I32B32") {
                    // Find the first set or clear bit, or minus one when
                    // there is none. A wave uses it to pick a lane, and the
                    // lane index becomes an offset into a descriptor table.
                    const auto wide = decoded.name == "SFF1I32B64" ||
                        decoded.name == "SFF0I32B64";
                    const auto looking_for_set =
                        decoded.name == "SFF1I32B64" ||
                        decoded.name == "SFF1I32B32";
                    const auto have = wide ? pair_known(encoded0) : known0;
                    if (!have) {
                        forget(destination, 1);
                    } else {
                        const auto value = wide ? read_pair(state, encoded0)
                                                : source0;
                        const auto bits = wide ? 64u : 32u;
                        std::uint32_t found = 0xFFFFFFFFu;
                        for (std::uint32_t bit = 0; bit < bits; ++bit) {
                            const auto set = ((value >> bit) & 1ull) != 0;
                            if (set == looking_for_set) {
                                found = bit;
                                break;
                            }
                        }
                        put(destination, found);
                    }
                } else if (decoded.name == "SBrevB32") {
                    if (known0) {
                        std::uint32_t reversed = 0;
                        for (std::uint32_t bit = 0; bit < 32; ++bit) {
                            reversed |= ((source0 >> bit) & 1u)
                                << (31 - bit);
                        }
                        put(destination, reversed);
                    } else {
                        forget(destination, 1);
                    }
                } else if (decoded.name == "SMulHiU32" ||
                           decoded.name == "SMulHiI32") {
                    if (known0 && known1) {
                        const auto value = decoded.name == "SMulHiU32"
                            ? static_cast<std::uint32_t>(
                                  (static_cast<std::uint64_t>(source0) *
                                   source1) >> 32)
                            : static_cast<std::uint32_t>(
                                  (static_cast<std::int64_t>(
                                       static_cast<std::int32_t>(source0)) *
                                   static_cast<std::int32_t>(source1)) >> 32);
                        put(destination, value);
                    } else {
                        forget(destination, 1);
                    }
                } else if (decoded.name == "SMovkI32") {
                    // SOPK: a signed sixteen-bit immediate, and nothing
                    // else. Descriptors built in registers use it for the
                    // record count.
                    put(destination,
                        static_cast<std::uint32_t>(static_cast<std::int32_t>(
                            static_cast<std::int16_t>(word & 0xFFFFu))));
                } else if (decoded.name == "SMulkI32" ||
                           decoded.name == "SAddkI32") {
                    // SOPK: the destination is also a source, and the other
                    // is a signed sixteen-bit immediate in the low half.
                    const auto immediate = static_cast<std::uint32_t>(
                        static_cast<std::int32_t>(
                            static_cast<std::int16_t>(word & 0xFFFFu)));
                    if (destination < kScalarRegisterCount &&
                        state.known[destination]) {
                        scalar_mark_exact(state, destination);
                        const auto before = state.registers[destination];
                        put(destination, decoded.name == "SMulkI32"
                                             ? before * immediate
                                             : before + immediate);
                    } else {
                        forget(destination, 1);
                    }
                    state.scc_known = false;
                } else if (decoded.name == "SGetpcB64") {
                    // Gives the address of the instruction after it, which
                    // is a thing this walk knows exactly.
                    write_pair(
                        state, destination,
                        code_base +
                            static_cast<std::uint64_t>(pc + size) * 4);
                } else if (decoded.name == "SNorB64" ||
                           decoded.name == "SNandB64" ||
                           decoded.name == "SXnorB64" ||
                           decoded.name == "SOrn2B64" ||
                           decoded.name == "SAndn2B64") {
                    // The 64-bit logic the table does not carry.
                    if (pair_known(encoded0) && pair_known(encoded1)) {
                        const auto a = read_pair(state, encoded0);
                        const auto b = read_pair(state, encoded1);
                        const auto value =
                            decoded.name == "SNorB64" ? ~(a | b)
                            : decoded.name == "SNandB64" ? ~(a & b)
                            : decoded.name == "SXnorB64" ? ~(a ^ b)
                            : decoded.name == "SOrn2B64" ? (a | ~b)
                                                         : (a & ~b);
                        write_pair(state, destination, value);
                        state.scc = value != 0;
                        state.scc_known = true;
                    } else {
                        forget(destination, 2);
                        state.scc_known = false;
                    }
                } else if (decoded.name == "SBfmB32") {
                    // Builds a mask: width in the first source, offset in
                    // the second. It is what a bitfield insert is built out
                    // of, and it writes no condition code.
                    if (known0 && known1) {
                        const auto width = source0 & 0x1Fu;
                        const auto offset = source1 & 0x1Fu;
                        put(destination,
                            ((1u << width) - 1u) << offset);
                    } else {
                        forget(destination, 1);
                    }
                } else if (decoded.name == "SPackLlB32B16") {
                    // Two halves into one word, low half of each source.
                    if (known0 && known1) {
                        put(destination,
                            (source0 & 0xFFFFu) | ((source1 & 0xFFFFu) << 16));
                    } else {
                        forget(destination, 1);
                    }
                } else if (decoded.name == "SLshlB64" ||
                           decoded.name == "SLshrB64" ||
                           decoded.name == "SAshrI64") {
                    if (known1 && pair_known(encoded0)) {
                        const auto value = read_pair(state, encoded0);
                        const auto amount = source1 & 63u;
                        const auto shifted =
                            decoded.name == "SLshlB64" ? (value << amount)
                            : decoded.name == "SLshrB64" ? (value >> amount)
                            : static_cast<std::uint64_t>(
                                  static_cast<std::int64_t>(value) >> amount);
                        write_pair(state, destination, shifted);
                        state.scc = shifted != 0;
                        state.scc_known = true;
                    } else {
                        forget(destination, 2);
                        state.scc_known = false;
                    }
                } else {
                    handled = false;
                }
                if (handled) {
                    pc += size;
                    continue;
                }
            }

            const auto form = scalar_form(decoded.name);
            if (form.operation != AluOperation::Unknown) {
                const auto encoded0 = word & 0xFFu;
                // SOP1 has one source. Bits 8 to 15 are its opcode, and
                // reading them as a second source made every move depend
                // on whether s3 or s4 happened to be known - which in a
                // vertex stage, whose user data starts at s8, they are
                // not, so every s_mov there forgot its result.
                const auto is_sop1 = (word & 0xFF800000u) == 0xBE800000u;
                const auto encoded1 = is_sop1 ? 128u : (word >> 8) & 0xFFu;
                const auto destination = (word >> 16) & 0x7Fu;
                std::uint32_t source0 = 0;
                std::uint32_t source1 = 0;
                const auto known0 = scalar_source_value(
                    state, encoded0, literal, source0,
                    !form.writes_condition);
                const auto known1 = scalar_source_value(
                    state, encoded1, literal, source1,
                    !form.writes_condition);
                // A compare has no destination - it writes the condition
                // code, and bits 16 to 22 belong to its second source. This
                // used to clear the register those bits named, which had
                // nothing to do with the instruction.
                if (form.writes_condition) {
                    state.scc_known = known0 && known1;
                    if (state.scc_known) {
                        state.scc =
                            compare_result(form.operation, source0, source1);
                        ScalarCompare compare;
                        compare.operation = form.operation;
                        compare.origin0 = encoded0 < 128
                            ? state.origin[encoded0] : std::uint16_t{0};
                        compare.origin1 = encoded1 < 128
                            ? state.origin[encoded1] : std::uint16_t{0};
                        compare.value0 = source0;
                        compare.value1 = source1;
                        compare.result = state.scc;
                        if (compare.origin0 != 0 || compare.origin1 != 0) {
                            evaluation.compares.push_back(compare);
                        }
                    }
                } else if (destination < kScalarRegisterCount) {
                    // Nearly every scalar operation that is not a move also
                    // writes the condition code, so unless the value is
                    // modelled here the code stops being known rather than
                    // staying stale from an earlier compare.
                    if (form.operation == AluOperation::Add && known0 &&
                        known1) {
                        state.scc = static_cast<std::uint64_t>(source0) +
                                source1 > 0xFFFFFFFFull;
                        state.scc_known = true;
                    } else if (form.operation != AluOperation::Move) {
                        state.scc_known = false;
                    }
                    if (!known0 || !known1) {
                        state.known[destination] = false;
                    } else {
                        std::uint32_t value = 0;
                        switch (form.operation) {
                            case AluOperation::Move: value = source0; break;
                            case AluOperation::Not: value = ~source0; break;
                            case AluOperation::Add:
                                value = source0 + source1; break;
                            case AluOperation::Subtract:
                                value = source0 - source1; break;
                            case AluOperation::Multiply:
                                value = source0 * source1; break;
                            case AluOperation::And:
                                value = source0 & source1; break;
                            case AluOperation::Or:
                                value = source0 | source1; break;
                            case AluOperation::Xor:
                                value = source0 ^ source1; break;
                            case AluOperation::ShiftLeft:
                                value = source0 << (source1 & 31); break;
                            case AluOperation::ShiftRightLogical:
                                value = source0 >> (source1 & 31); break;
                            case AluOperation::ShiftRightArithmetic:
                                value = static_cast<std::uint32_t>(
                                    static_cast<std::int32_t>(source0) >>
                                    (source1 & 31));
                                break;
                            default:
                                // A compare sets the condition code, which
                                // this walk follows both ways anyway.
                                state.known[destination] = false;
                                value = 0;
                                break;
                        }
                        if (!form.writes_condition) {
                            state.registers[destination] = value;
                            state.known[destination] = true;
                            state.origin[destination] = 0;
                            if (form.is_pair &&
                                destination + 1 < kScalarRegisterCount) {
                                std::uint32_t high0 = 0;
                                std::uint32_t high1 = 0;
                                const auto high_known0 = scalar_source_value(
                                    state, encoded0 + 1, literal, high0);
                                const auto high_known1 = scalar_source_value(
                                    state, encoded1 + 1, literal, high1);
                                if (!high_known0 || !high_known1) {
                                    state.known[destination + 1] = false;
                                } else {
                                    std::uint32_t high = 0;
                                    switch (form.operation) {
                                        case AluOperation::Move:
                                            high = high0; break;
                                        case AluOperation::Not:
                                            high = ~high0; break;
                                        case AluOperation::And:
                                            high = high0 & high1; break;
                                        case AluOperation::Or:
                                            high = high0 | high1; break;
                                        case AluOperation::Xor:
                                            high = high0 ^ high1; break;
                                        default:
                                            high = 0; break;
                                    }
                                    state.registers[destination + 1] = high;
                                    state.known[destination + 1] = true;
                                    state.origin[destination + 1] = 0;
                                }
                            }
                        }
                    }
                }
                pc += size;
                continue;
            }

            // Anything else may write a scalar register with a value this
            // cannot follow. Saying so is the point: a register whose value
            // is unknown must not be used as an address.
            // Only the families that have a destination lose one. SOPP
            // has none at all and SOPC writes the condition code, so
            // clearing bits 16 to 22 for them threw away a register that
            // had nothing to do with the instruction: SNop alone cost 86
            // of them, and the compares another 376 between them.
            const auto sop_opcode = (word >> 23) & 0x7Fu;
            const auto writes_scalar =
                (word & 0xC0000000u) == 0x80000000u &&
                sop_opcode != 0x7Eu &&   // SOPC
                sop_opcode != 0x7Fu;     // SOPP
            const auto destination = (word >> 16) & 0x7Fu;
            if (writes_scalar) {
                // Almost everything that writes a register writes the
                // condition code as well, so a select downstream must not
                // read one an earlier compare left behind.
                state.scc_known = false;
            }
            if (writes_scalar &&
                destination < kScalarRegisterCount) {
                if (state.known[destination]) {
                    ++evaluation.unknown_by_name[
                        std::string(decoded.name)];
                }
                state.known[destination] = false;
            }
            pc += size;
        }
        evaluation.final_state = state;
    }
    return evaluation;
}

// An earlier walk of the same shader, brought up to date with other user
// data and other memory without walking it again. Every load is read again
// through the words its base came from, every descriptor takes the words
// its registers came from, and the walk stands if each word it computed
// with is what it was and each read succeeded or failed as it did. False
// when it does not stand, and the shader has to be walked.
// Why a replay did not stand, for the caller to count.
struct ScalarReplayRefusal {
    // 1 not replayable, 2 a user data word, 3 a V# stopped or started
    // being one, 4 a read succeeded where it had failed or the reverse,
    // 5 a loaded word, 6 a compare that came out the other way.
    std::uint32_t reason = 0;
    std::uint32_t pc = 0;
    std::uint32_t index = 0;
    std::uint32_t was = 0;
    std::uint32_t now = 0;
};

template <typename ReadMemory>
bool replay_scalar(
    const ScalarEvaluation& stored,
    const std::vector<std::uint32_t>& user_data,
    const ReadMemory& read_memory,
    ScalarEvaluation& replayed,
    ScalarReplayRefusal* refusal = nullptr) {
    const auto refuse = [&](std::uint32_t reason, std::uint32_t pc,
                            std::uint32_t index, std::uint32_t was,
                            std::uint32_t now) {
        if (refusal != nullptr) {
            *refusal = {reason, pc, index, was, now};
        }
        return false;
    };
    if (!stored.replayable || user_data.size() != stored.user_sources) {
        return refuse(1, 0, 0, 0, 0);
    }
    std::vector<std::uint32_t> current(stored.sources.size());
    std::vector<bool> fetched_once(stored.sources.size(), false);
    for (std::uint32_t index = 0; index < stored.user_sources; ++index) {
        const auto& source = stored.sources[index];
        if (source.exact && source.value != user_data[index]) {
            return refuse(
                2, 0, index | (source.exact_pc << 8), source.value,
                user_data[index]);
        }
        current[index] = user_data[index];
    }
    const auto word_of = [&](std::uint16_t origin, std::uint32_t otherwise) {
        return origin != 0 ? current[origin - 1] : otherwise;
    };
    replayed = stored;
    for (auto& load : replayed.loads) {
        if (load.base_words_known) {
            std::array<std::uint32_t, 4> words{};
            for (std::uint32_t index = 0; index < 4; ++index) {
                words[index] =
                    word_of(load.base_origin[index], load.base_words[index]);
            }
            std::uint64_t base = 0;
            if (load.buffer_load) {
                // As decode_buffer_descriptor reads them.
                auto valid = true;
                std::uint32_t stride = 0;
                std::uint32_t records = 0;
                if (words[0] != 0 || words[1] != 0 || words[2] != 0 ||
                    words[3] != 0) {
                    if ((words[3] >> 30) != 0) {
                        valid = false;
                    } else {
                        base = words[0] |
                            (static_cast<std::uint64_t>(words[1] & 0xFFFFu)
                             << 32);
                        stride = (words[1] >> 16) & 0x3FFFu;
                        records = words[2];
                    }
                }
                if (valid != load.base_known) {
                    return refuse(3, load.pc, 0, 0, 0);
                }
                load.region_size = stride == 0 ? records : records * stride;
            } else {
                base = words[0] |
                    (static_cast<std::uint64_t>(words[1]) << 32);
            }
            load.base_words = words;
            load.base_low = words[0];
            load.base_high = words[1];
            if (load.base_known) {
                load.base_address = base;
                load.address = base + load.byte_offset;
            }
        }
        if (!load.address_known || load.dword_count == 0) {
            continue;
        }
        for (std::uint32_t index = 0;
             index < load.dword_count &&
             load.destination + index < kScalarRegisterCount;
             ++index) {
            const auto id = index < load.word_source.size()
                ? load.word_source[index] : std::uint16_t{0};
            if (id == 0 || id > stored.sources.size()) {
                return refuse(1, load.pc, index, 0, 0);
            }
            const auto& source = stored.sources[id - 1];
            std::uint32_t fetched = 0;
            const auto read =
                read_memory(load.address + index * 4ull, fetched);
            if (read != source.known) {
                return refuse(4, load.pc, index, source.value, fetched);
            }
            if (read && source.exact && fetched != source.value) {
                return refuse(
                    5, load.pc, index | (source.exact_pc << 8),
                    source.value, fetched);
            }
            // Two loads that read one word have to read one word still.
            if (fetched_once[id - 1] &&
                current[id - 1] != (read ? fetched : 0)) {
                return refuse(7, load.pc, index, current[id - 1], fetched);
            }
            fetched_once[id - 1] = true;
            current[id - 1] = read ? fetched : 0;
        }
    }
    for (const auto& compare : stored.compares) {
        if (compare_result(
                compare.operation,
                word_of(compare.origin0, compare.value0),
                word_of(compare.origin1, compare.value1)) != compare.result) {
            return refuse(6, 0, 0, compare.value0, compare.value1);
        }
    }
    for (auto& buffer : replayed.buffers) {
        for (std::uint32_t index = 0; index < 4; ++index) {
            buffer.descriptor[index] = word_of(
                buffer.descriptor_origin[index], buffer.descriptor[index]);
        }
    }
    for (auto& image : replayed.images) {
        for (std::uint32_t index = 0; index < 8; ++index) {
            image.descriptor[index] = word_of(
                image.descriptor_origin[index], image.descriptor[index]);
        }
        for (std::uint32_t index = 0; index < 4; ++index) {
            image.sampler[index] =
                word_of(image.sampler_origin[index], image.sampler[index]);
        }
    }
    for (std::size_t index = 0; index < current.size(); ++index) {
        replayed.sources[index].value = current[index];
    }
    return true;
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_SCALAR_EVAL_H
