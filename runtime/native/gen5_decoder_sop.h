// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The scalar instruction families, ported from Gen5ShaderTranslator.cs.
//
// Third piece of the native shader path. Each of these turns one
// instruction word into a name and a length, and the length is the part
// that matters beyond the name: get it wrong and the decoder walks into the
// middle of the next instruction and every instruction after it is rubbish.
//
// Names are views onto literals rather than strings, so decoding a program
// allocates nothing. The C# returns freshly built strings for the errors
// too; those are built here only when a decode actually fails.

#ifndef PS5_GEN5_DECODER_SOP_H
#define PS5_GEN5_DECODER_SOP_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ps5gen5 {

struct DecodedOpcode {
    std::string_view name;
    std::uint32_t size_dwords = 1;

    bool ok() const { return !name.empty(); }
};

// What a classification by name worked out, kept by the name's address.
// Names are the decoder's string literals, so the address is stable, and
// the length is checked as well in case two tables share a prefix. The
// translator asked the same questions of the same names on every
// translation - a chain of up to sixty string compares an instruction -
// and it translates about eleven stages a frame during loading: a seventh
// of the title's main thread.
template <typename Value, typename Compute>
Value memo_by_name(std::string_view name, Compute compute) {
    // Open addressing, read without a lock: a slot is claimed by moving its
    // state from empty to writing, filled, and published as ready. A lock
    // around the lookup cost more than the compares it saved.
    struct Slot {
        std::atomic<int> state{0};
        const char* key = nullptr;
        std::size_t length = 0;
        Value value{};
    };
    constexpr std::size_t kSlots = 1024;
    static Slot slots[kSlots];
    const auto hash =
        (reinterpret_cast<std::uintptr_t>(name.data()) >> 3) *
        0x9E3779B97F4A7C15ull;
    for (std::size_t probe = 0; probe < kSlots; ++probe) {
        auto& slot = slots[(static_cast<std::size_t>(hash >> 54) + probe) &
                           (kSlots - 1)];
        auto state = slot.state.load(std::memory_order_acquire);
        if (state == 0) {
            if (slot.state.compare_exchange_strong(
                    state, 1, std::memory_order_acquire)) {
                const auto value = compute(name);
                slot.key = name.data();
                slot.length = name.size();
                slot.value = value;
                slot.state.store(2, std::memory_order_release);
                return value;
            }
        }
        if (state == 2 && slot.key == name.data() &&
            slot.length == name.size()) {
            return slot.value;
        }
    }
    return compute(name);
}

// SOP1: one source. The source encoding 0xFF means a literal follows, which
// makes the instruction two words rather than one.
inline DecodedOpcode decode_sop1(std::uint32_t word) {
    const auto opcode = (word >> 8) & 0xFFu;
    const auto src0 = word & 0xFFu;
    DecodedOpcode decoded;
    decoded.size_dwords = 1 + (src0 == 0xFFu ? 1u : 0u);
    switch (opcode) {
        case 0x03: decoded.name = "SMovB32"; break;
        case 0x04: decoded.name = "SMovB64"; break;
        case 0x07: decoded.name = "SNotB32"; break;
        case 0x08: decoded.name = "SNotB64"; break;
        case 0x09: decoded.name = "SWqmB32"; break;
        case 0x0A: decoded.name = "SWqmB64"; break;
        case 0x0B: decoded.name = "SBrevB32"; break;
        case 0x0F: decoded.name = "SBcnt1I32B32"; break;
        case 0x13: decoded.name = "SFF1I32B32"; break;
        case 0x14: decoded.name = "SFF1I32B64"; break;
        case 0x1D: decoded.name = "SBitset1B32"; break;
        case 0x1F: decoded.name = "SGetpcB64"; break;
        case 0x20: decoded.name = "SSetpcB64"; break;
        case 0x21: decoded.name = "SSwappcB64"; break;
        case 0x24: decoded.name = "SAndSaveexecB64"; break;
        case 0x25: decoded.name = "SOrSaveexecB64"; break;
        case 0x26: decoded.name = "SXorSaveexecB64"; break;
        case 0x27: decoded.name = "SAndn2SaveexecB64"; break;
        case 0x28: decoded.name = "SOrn2SaveexecB64"; break;
        case 0x29: decoded.name = "SNandSaveexecB64"; break;
        case 0x2A: decoded.name = "SNorSaveexecB64"; break;
        case 0x2B: decoded.name = "SXnorSaveexecB64"; break;
        case 0x37: decoded.name = "SAndn1SaveexecB64"; break;
        case 0x38: decoded.name = "SOrn1SaveexecB64"; break;
        case 0x3C: decoded.name = "SAndSaveexecB32"; break;
        case 0x3D: decoded.name = "SOrSaveexecB32"; break;
        case 0x3E: decoded.name = "SXorSaveexecB32"; break;
        case 0x3F: decoded.name = "SAndn2SaveexecB32"; break;
        case 0x40: decoded.name = "SOrn2SaveexecB32"; break;
        case 0x41: decoded.name = "SNandSaveexecB32"; break;
        case 0x42: decoded.name = "SNorSaveexecB32"; break;
        case 0x43: decoded.name = "SXnorSaveexecB32"; break;
        case 0x44: decoded.name = "SAndn1SaveexecB32"; break;
        case 0x45: decoded.name = "SOrn1SaveexecB32"; break;
        default: break;
    }
    return decoded;
}

// SOP2: two sources, either of which may be the literal escape.
inline DecodedOpcode decode_sop2(std::uint32_t word) {
    const auto opcode = (word >> 23) & 0x7Fu;
    const auto src0 = word & 0xFFu;
    const auto src1 = (word >> 8) & 0xFFu;
    DecodedOpcode decoded;
    decoded.size_dwords = (src0 == 0xFFu || src1 == 0xFFu) ? 2u : 1u;
    switch (opcode) {
        case 0x00: decoded.name = "SAddU32"; break;
        case 0x01: decoded.name = "SSubU32"; break;
        case 0x02: decoded.name = "SAddI32"; break;
        case 0x03: decoded.name = "SSubI32"; break;
        case 0x04: decoded.name = "SAddcU32"; break;
        case 0x05: decoded.name = "SSubbU32"; break;
        case 0x06: decoded.name = "SMinI32"; break;
        case 0x07: decoded.name = "SMinU32"; break;
        case 0x08: decoded.name = "SMaxI32"; break;
        case 0x09: decoded.name = "SMaxU32"; break;
        case 0x0A: decoded.name = "SCselectB32"; break;
        case 0x0B: decoded.name = "SCselectB64"; break;
        case 0x0E: decoded.name = "SAndB32"; break;
        case 0x0F: decoded.name = "SAndB64"; break;
        case 0x10: decoded.name = "SOrB32"; break;
        case 0x11: decoded.name = "SOrB64"; break;
        case 0x12: decoded.name = "SXorB32"; break;
        case 0x13: decoded.name = "SXorB64"; break;
        case 0x14: decoded.name = "SAndn2B32"; break;
        case 0x15: decoded.name = "SAndn2B64"; break;
        case 0x16: decoded.name = "SOrn2B32"; break;
        case 0x17: decoded.name = "SOrn2B64"; break;
        case 0x18: decoded.name = "SNandB32"; break;
        case 0x19: decoded.name = "SNandB64"; break;
        case 0x1A: decoded.name = "SNorB32"; break;
        case 0x1B: decoded.name = "SNorB64"; break;
        case 0x1C: decoded.name = "SXnorB32"; break;
        case 0x1D: decoded.name = "SXnorB64"; break;
        case 0x1E: decoded.name = "SLshlB32"; break;
        case 0x1F: decoded.name = "SLshlB64"; break;
        case 0x20: decoded.name = "SLshrB32"; break;
        case 0x21: decoded.name = "SLshrB64"; break;
        case 0x22: decoded.name = "SAshrI32"; break;
        case 0x23: decoded.name = "SAshrI64"; break;
        case 0x24: decoded.name = "SBfmB32"; break;
        case 0x25: decoded.name = "SBfmB64"; break;
        case 0x26: decoded.name = "SMulI32"; break;
        case 0x27: decoded.name = "SBfeU32"; break;
        case 0x28: decoded.name = "SBfeI32"; break;
        case 0x29: decoded.name = "SBfeU64"; break;
        case 0x2A: decoded.name = "SBfeI64"; break;
        case 0x2D: decoded.name = "SAbsdiffI32"; break;
        case 0x2E: decoded.name = "SLshl1AddU32"; break;
        case 0x2F: decoded.name = "SLshl2AddU32"; break;
        case 0x30: decoded.name = "SLshl3AddU32"; break;
        case 0x31: decoded.name = "SLshl4AddU32"; break;
        case 0x32: decoded.name = "SPackLlB32B16"; break;
        case 0x33: decoded.name = "SPackLhB32B16"; break;
        case 0x34: decoded.name = "SPackHhB32B16"; break;
        case 0x35: decoded.name = "SMulHiU32"; break;
        case 0x36: decoded.name = "SMulHiI32"; break;
        default: break;
    }
    return decoded;
}

// SOPC: two sources compared, result to SCC.
inline DecodedOpcode decode_sopc(std::uint32_t word) {
    const auto opcode = (word >> 16) & 0x7Fu;
    const auto src0 = word & 0xFFu;
    const auto src1 = (word >> 8) & 0xFFu;
    DecodedOpcode decoded;
    decoded.size_dwords = (src0 == 0xFFu || src1 == 0xFFu) ? 2u : 1u;
    switch (opcode) {
        case 0x00: decoded.name = "SCmpEqI32"; break;
        case 0x01: decoded.name = "SCmpLgI32"; break;
        case 0x02: decoded.name = "SCmpGtI32"; break;
        case 0x03: decoded.name = "SCmpGeI32"; break;
        case 0x04: decoded.name = "SCmpLtI32"; break;
        case 0x05: decoded.name = "SCmpLeI32"; break;
        case 0x06: decoded.name = "SCmpEqU32"; break;
        case 0x07: decoded.name = "SCmpLgU32"; break;
        case 0x08: decoded.name = "SCmpGtU32"; break;
        case 0x09: decoded.name = "SCmpGeU32"; break;
        case 0x0A: decoded.name = "SCmpLtU32"; break;
        case 0x0B: decoded.name = "SCmpLeU32"; break;
        case 0x0C: decoded.name = "SBitcmp0B32"; break;
        case 0x0D: decoded.name = "SBitcmp1B32"; break;
        case 0x0E: decoded.name = "SBitcmp0B64"; break;
        case 0x0F: decoded.name = "SBitcmp1B64"; break;
        case 0x12: decoded.name = "SCmpEqU64"; break;
        case 0x13: decoded.name = "SCmpLgU64"; break;
        default: break;
    }
    return decoded;
}

// SOPP: control flow and waits. Always one word - the immediate lives in
// the low half of the instruction rather than in a literal.
inline DecodedOpcode decode_sopp(std::uint32_t word) {
    const auto opcode = (word >> 16) & 0x7Fu;
    DecodedOpcode decoded;
    decoded.size_dwords = 1;
    switch (opcode) {
        case 0x00: decoded.name = "SNop"; break;
        case 0x01: decoded.name = "SEndpgm"; break;
        case 0x02: decoded.name = "SBranch"; break;
        case 0x04: decoded.name = "SCbranchScc0"; break;
        case 0x05: decoded.name = "SCbranchScc1"; break;
        case 0x06: decoded.name = "SCbranchVccz"; break;
        case 0x07: decoded.name = "SCbranchVccnz"; break;
        case 0x08: decoded.name = "SCbranchExecz"; break;
        case 0x09: decoded.name = "SCbranchExecnz"; break;
        case 0x0A: decoded.name = "SBarrier"; break;
        case 0x0C: decoded.name = "SWaitcnt"; break;
        case 0x10: decoded.name = "SSendmsg"; break;
        case 0x12: decoded.name = "STrap"; break;
        case 0x16: decoded.name = "STtraceData"; break;
        // Branches taken only while a debugger has the wave, which on a
        // retail console is never.
        case 0x17: decoded.name = "SCbranchCdbgsys"; break;
        case 0x18: decoded.name = "SCbranchCdbguser"; break;
        case 0x19: decoded.name = "SCbranchCdbgsysOrUser"; break;
        case 0x1A: decoded.name = "SCbranchCdbgsysAndUser"; break;
        case 0x20: decoded.name = "SInstPrefetch"; break;
        case 0x21: decoded.name = "SClause"; break;
        case 0x23: decoded.name = "SWaitcntDepctr"; break;
        default: break;
    }
    return decoded;
}

// SOPK: one source and a sixteen bit immediate, so always one word. The
// opcode is biased by 0x60, which is also how the dispatch recognises it.
inline DecodedOpcode decode_sopk(std::uint32_t word) {
    const auto opcode = ((word >> 23) & 0x7Fu) - 0x60u;
    DecodedOpcode decoded;
    decoded.size_dwords = 1;
    switch (opcode) {
        case 0x00: decoded.name = "SMovkI32"; break;
        case 0x03: decoded.name = "SCmpkEqI32"; break;
        case 0x04: decoded.name = "SCmpkLgI32"; break;
        case 0x05: decoded.name = "SCmpkGtI32"; break;
        case 0x06: decoded.name = "SCmpkGeI32"; break;
        case 0x07: decoded.name = "SCmpkLtI32"; break;
        case 0x08: decoded.name = "SCmpkLeI32"; break;
        case 0x09: decoded.name = "SCmpkEqU32"; break;
        case 0x0A: decoded.name = "SCmpkLgU32"; break;
        case 0x0B: decoded.name = "SCmpkGtU32"; break;
        case 0x0C: decoded.name = "SCmpkGeU32"; break;
        case 0x0D: decoded.name = "SCmpkLtU32"; break;
        case 0x0E: decoded.name = "SCmpkLeU32"; break;
        case 0x0F: decoded.name = "SAddkI32"; break;
        case 0x10: decoded.name = "SMulkI32"; break;
        case 0x17: decoded.name = "SWaitcntVscnt"; break;
        default: break;
    }
    return decoded;
}

// The scalar families share one encoding prefix and are told apart by the
// same field that carries the SOP2 opcode. 0x7D, 0x7E and 0x7F are SOP1,
// SOPC and SOPP; everything from 0x60 up is SOPK; the rest is SOP2. The
// order matters - SOPK's range would otherwise swallow the three above it.
inline DecodedOpcode decode_sop(std::uint32_t word) {
    const auto opcode = (word >> 23) & 0x7Fu;
    if (opcode == 0x7D) {
        return decode_sop1(word);
    }
    if (opcode == 0x7E) {
        return decode_sopc(word);
    }
    if (opcode == 0x7F) {
        return decode_sopp(word);
    }
    if (opcode >= 0x60) {
        return decode_sopk(word);
    }
    return decode_sop2(word);
}

}  // namespace ps5gen5

#endif  // PS5_GEN5_DECODER_SOP_H
