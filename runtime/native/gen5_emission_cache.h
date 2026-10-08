// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Reusing an emitted module for a shader translated again.
//
// A title draws the same shader for object after object, and each object's
// draw hands it different memory: its own constants, its own ring slice.
// The addresses reach the manifest - which buffer goes at which binding -
// but the module reads a buffer by binding, not by address, so for every
// object after the first the module comes out the same. Emitting it is
// three quarters of a translation's cost, and after the intro that was
// most of a frame.
//
// So the part of a translation before emission - the walk that finds the
// addresses and builds the manifest - still runs every time, and what
// emission would read from it is written down with the addresses left out.
// When that matches an earlier translation, its module is used again.
//
// What emission reads is listed by hand, which is how a list goes stale.
// The first few matches for each key are therefore emitted anyway and
// compared word for word; a key whose modules differ is never reused.

#ifndef PS5_GEN5_EMISSION_CACHE_H
#define PS5_GEN5_EMISSION_CACHE_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gen5_registers.h"

namespace ps5gen5 {

// What emission adds to a translation's result.
struct EmissionOutputs {
    bool ok = false;
    std::vector<std::uint32_t> words;
    RegisterFileStats register_stats;
    std::uint32_t instructions_translated = 0;
    std::uint32_t instructions_skipped = 0;
    std::uint32_t dispatcher_loads = 0;
    std::uint32_t dispatcher_stores = 0;
    std::uint32_t subroutine_calls = 0;
    std::uint32_t loads_emitted = 0;
    std::uint32_t buffers_declared = 0;
    bool writes_memory = false;
    std::map<std::string, std::uint32_t> skipped_by_name;
    std::map<std::uint32_t, std::uint32_t> typed_formats_unhandled;
    std::map<std::uint32_t, std::uint32_t> exports_by_target;
};

// A key built up word by word.
class EmissionKey {
public:
    void add(std::uint32_t word) { words_.push_back(word); }
    void add64(std::uint64_t value) {
        add(static_cast<std::uint32_t>(value));
        add(static_cast<std::uint32_t>(value >> 32));
    }
    const std::vector<std::uint32_t>& words() const { return words_; }

private:
    std::vector<std::uint32_t> words_;
};

struct EmissionKeyHash {
    std::size_t operator()(const std::vector<std::uint32_t>& words) const {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        for (const auto word : words) {
            hash ^= word;
            hash *= 0x100000001b3ull;
        }
        return static_cast<std::size_t>(hash ^ (hash >> 32));
    }
};

class EmissionCache {
public:
    enum class Lookup {
        // Nothing reusable: emit, and store what comes out.
        Emit,
        // Reusable: the outputs were copied.
        Reused,
        // Seen, but not yet trusted: emit, and compare.
        Verify,
    };

    static EmissionCache& instance() {
        static EmissionCache cache;
        return cache;
    }

    bool enabled() const { return enabled_; }

    Lookup find(const std::vector<std::uint32_t>& key,
                EmissionOutputs& outputs) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end()) {
            return Lookup::Emit;
        }
        const auto& entry = found->second;
        if (entry.unstable) {
            return Lookup::Emit;
        }
        if (entry.verified < verify_count_) {
            return Lookup::Verify;
        }
        outputs = entry.outputs;
        return Lookup::Reused;
    }

    // After emitting. Returns false when an earlier module for the same key
    // differs from this one, which retires the key.
    bool store(const std::vector<std::uint32_t>& key,
               const EmissionOutputs& outputs) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end()) {
            // Kept from the second sighting on. In the scene after the
            // intro most keys are seen once - their descriptors are baked
            // in - and holding a module for each grew the process by a
            // gigabyte in two minutes of it.
            const auto hash = EmissionKeyHash{}(key);
            if (seen_once_.insert(hash).second) {
                if (seen_once_.size() >= kMaxSeenOnce) {
                    seen_once_.clear();
                }
                return true;
            }
            const auto bytes = entry_bytes(key, outputs);
            if (entries_.size() >= kMaxEntries ||
                entry_bytes_ + bytes > kMaxBytes) {
                entries_.clear();
                entry_bytes_ = 0;
            }
            Entry entry;
            entry.outputs = outputs;
            entries_.emplace(key, std::move(entry));
            entry_bytes_ += bytes;
            return true;
        }
        auto& entry = found->second;
        if (entry.unstable) {
            return true;
        }
        if (entry.outputs.words != outputs.words ||
            entry.outputs.ok != outputs.ok ||
            entry.outputs.writes_memory != outputs.writes_memory ||
            entry.outputs.exports_by_target != outputs.exports_by_target) {
            entry.unstable = true;
            entry.outputs = EmissionOutputs{};
            return false;
        }
        ++entry.verified;
        return true;
    }

private:
    static constexpr std::size_t kMaxEntries = 8192;
    static constexpr std::size_t kMaxBytes = 256u << 20;
    static constexpr std::size_t kMaxSeenOnce = 1u << 20;

    static std::size_t entry_bytes(const std::vector<std::uint32_t>& key,
                                   const EmissionOutputs& outputs) {
        return (key.size() + outputs.words.size()) * sizeof(std::uint32_t) +
            256;
    }

    struct Entry {
        EmissionOutputs outputs;
        std::uint32_t verified = 0;
        bool unstable = false;
    };

    EmissionCache() {
        const auto* off = std::getenv("PS5RT_EMISSION_REUSE");
        enabled_ = off == nullptr || off[0] != '0';
        if (const auto* verify = std::getenv("PS5RT_EMISSION_REUSE_VERIFY")) {
            verify_count_ = static_cast<std::uint32_t>(
                std::strtoul(verify, nullptr, 10));
        }
    }

    std::mutex mutex_;
    std::unordered_map<std::vector<std::uint32_t>, Entry, EmissionKeyHash>
        entries_;
    std::unordered_set<std::size_t> seen_once_;
    std::size_t entry_bytes_ = 0;
    bool enabled_ = true;
    std::uint32_t verify_count_ = 2;
};

}  // namespace ps5gen5

#endif  // PS5_GEN5_EMISSION_CACHE_H
