// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
// Real HLE implementations for Astro Bot critical NIDs
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <atomic>
#include <thread>
#include <new>
#include <vector>
#include <bitset>
#include <map>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <set>
#include <tuple>
#include <string>
#include "ps5gpu_bridge_api.h"
#include "ps5rt_api.h"

#include "ps5rt_shared.h"
#include "ps5rt_hle_impl.h"
#include "gen5_cfg.h"
#include "gen5_translate.h"
// Generated from the game dump by tools/agc_extract_defaults.py; it
// lives outside the repository because its source is Sony's library.
#include "agc_default_state.h"

static Ps5RtGuestTlsRestore g_guest_tls_restore = nullptr;
static constexpr SIZE_T kGuestThreadStackCommit = 2 * 1024 * 1024;
static void restore_guest_fs() {
    if (g_guest_tls_restore != nullptr) {
        g_guest_tls_restore();
    }
}
#define PS5_HLE_GUARD() restore_guest_fs()
static void trace_stderr_stub(const char*, ...) {}
#define trace_stderr trace_stderr_stub
static void trace_sync_error(const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fflush(stderr);
}

extern "C" Ps5GpuResult ps5rt_gpu_compile_spirv(
    const Ps5GpuShaderRequest*,
    Ps5GpuShaderResult*,
    char*,
    std::uint32_t);
extern "C" void ps5rt_gpu_free(void*);

// Path mapping: PS5 paths -> PC paths
static std::string g_game_root;
static std::map<std::uint64_t, std::string> g_open_files;

static std::string map_ps5_path(const char* ps5_path) {
    if (!ps5_path) return "";
    std::string path(ps5_path);
    if (path.empty()) return "";
    // Map common PS5 paths to PC
    if (path.rfind("/app0/", 0) == 0) {
        return g_game_root + "/" + path.substr(6);
    }
    if (path.rfind("app0/", 0) == 0) {
        return g_game_root + "/" + path.substr(5);
    }
    if (path.rfind("/app1/", 0) == 0) {
        return g_game_root + "/../" + path.substr(6);
    }
    // Relative paths
    if (path[0] != '/') {
        return g_game_root + "/" + path;
    }
    return path;
}

static bool read_process_c_string(
    std::uint64_t address,
    std::size_t maximum_length,
    std::string& value) {
    value.clear();
    if (address < 0x10000 || maximum_length == 0) {
        return false;
    }
    value.reserve(std::min<std::size_t>(maximum_length, 256));
    for (std::size_t index = 0; index < maximum_length; ++index) {
        char character = '\0';
        SIZE_T bytes_read = 0;
        if (!ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(address + index),
                &character,
                sizeof(character),
                &bytes_read) ||
            bytes_read != sizeof(character)) {
            value.clear();
            return false;
        }
        if (character == '\0') {
            return true;
        }
        value.push_back(character);
    }
    value.clear();
    return false;
}

extern "C" void ps5rt_set_game_root(const char* root) {
    if (root) g_game_root = root;
}

// --- libc init_env ---
static bool g_env_initialized = false;

extern "C" void ps5rt_init_env() {
    if (g_env_initialized) return;
    g_env_initialized = true;
    trace_stderr("hle=_init_env\n");
}

// --- libc heap ---
//
// Guest allocations never come from the host CRT heap. Guest code can legally
// mix allocator entry points, and malformed early initialization code must not
// be able to overwrite the host allocator's metadata.
//
// That used to mean one VirtualAlloc per allocation. Windows rounds every
// reservation up to the 64KB allocation granularity, so a hundred-byte malloc
// cost 64KB of address space and a page of commit, and free never releases
// anything - the comment there explains why, and it still holds. Measured over
// a run: 600000 live allocations asking for 947MB held 18.3GB of committed
// memory across 968800 private regions, and the process died of a full commit
// at a hundred seconds.
//
// Small allocations are carved out of large slabs now. The slabs are this
// module's own, so the isolation the original note is about is unchanged, and
// a freed block is still never handed out again, so a stale pointer still
// reads the bytes it was written with rather than another allocation's. Large
// allocations keep a region to themselves, where the granule costs little
// against the size and isolation is worth most.

namespace {

std::uint64_t g_libc_heap_bytes = 0;

struct LibcHeapAllocation {
    void* base;
    std::size_t size;
    std::size_t alignment;
    std::size_t span;
    std::uint64_t caller;
};

struct LibcMspaceAllocation {
    std::uint64_t offset;
    std::uint64_t size;
    std::uint64_t alignment;
};

struct LibcMspaceFreeRange {
    std::uint64_t offset;
    std::uint64_t size;
};

struct LibcMspace {
    std::uint64_t magic;
    std::uint64_t base;
    std::uint64_t capacity;
    std::uint64_t high_water;
    SRWLOCK lock;
    std::map<std::uint64_t, LibcMspaceAllocation> allocations;
    std::vector<LibcMspaceFreeRange> free_ranges;

    LibcMspace(std::uint64_t region_base, std::uint64_t region_capacity)
        : magic(0x4D53504143450002ULL),
          base(region_base),
          capacity(region_capacity),
          high_water(0),
          lock{} {
        InitializeSRWLock(&lock);
    }
};

constexpr std::size_t kDefaultLibcHeapAlignment = 16;
constexpr std::size_t kGuestHeapSafetySlack = 0x1000;
constexpr std::uint64_t kLibcMspaceMagic = 0x4D53504143450002ULL;

SRWLOCK g_libc_heap_lock = SRWLOCK_INIT;

// What the heap knows about a block, kept in the block rather than in a map.
// A map node per allocation - two locks, a tree insert and an operator new
// behind every malloc the title makes - was a sixth of the main thread in the
// scene after the intro. The header sits at the block's base; the word just
// before the pointer handed out points back to it, and the magic is keyed on
// that pointer, so a stray pointer is not taken for a block.
struct LibcBlockHeader {
    std::uint64_t magic;
    std::uint64_t user;
    std::uint64_t size;
    std::uint64_t span;
    std::uint64_t alignment;
    std::uint64_t caller;
    std::uint64_t state;
    std::uint64_t reserved;
};
constexpr std::uint64_t kLibcBlockMagic = 0x4C4942484541500AULL;
constexpr std::uint64_t kLibcBlockLive = 1;
constexpr std::uint64_t kLibcBlockFreed = 2;
constexpr std::size_t kLibcBlockPrefix =
    sizeof(LibcBlockHeader) + sizeof(std::uint64_t);
// Where blocks can be: the slabs, and the regions of blocks too large for
// one. A pointer outside all of them is not the heap's, which is checked
// before any header is read.
std::vector<std::pair<std::uint64_t, std::uint64_t>> g_libc_slab_ranges;
std::map<std::uint64_t, std::uint64_t> g_libc_isolated_ranges;
std::uint64_t g_libc_live_blocks = 0;

// Anything at or above this keeps its own region.
constexpr std::size_t kGuestHeapSlabThreshold = 64 * 1024;
constexpr std::size_t kGuestHeapSlabBytes = 64 * 1024 * 1024;
// A slab's neighbours are other guest allocations, so the page of slack that
// used to sit past each one no longer turns an overrun into a fault. Keep a
// small gap, which still catches the common single-word overrun against the
// bookkeeping, and stop paying a page for it.
constexpr std::size_t kGuestHeapSlabSlack = 64;

std::uint8_t* g_libc_slab_cursor = nullptr;
std::size_t g_libc_slab_remaining = 0;
std::uint64_t g_libc_slab_count = 0;
std::uint64_t g_libc_slab_bytes = 0;
std::uint64_t g_libc_isolated_count = 0;

// Carves a block out of the current slab, taking a new one when it will not
// fit. Returns nullptr when the caller should keep its own region instead.
// The heap lock must be held.
std::uint8_t* libc_slab_take(
    std::size_t bytes, std::size_t alignment, std::size_t& taken) {
    if (bytes >= kGuestHeapSlabThreshold || alignment > 4096) {
        return nullptr;
    }
    const auto aligned_gap = [&](std::uint8_t* cursor) {
        const auto raw =
            reinterpret_cast<std::uintptr_t>(cursor) + kLibcBlockPrefix;
        const auto aligned =
            (raw + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
        return static_cast<std::size_t>(
            aligned - reinterpret_cast<std::uintptr_t>(cursor));
    };
    auto wanted = [&](std::uint8_t* cursor) {
        return aligned_gap(cursor) + bytes + kGuestHeapSlabSlack;
    };
    if (g_libc_slab_cursor == nullptr ||
        g_libc_slab_remaining < wanted(g_libc_slab_cursor)) {
        auto* slab = static_cast<std::uint8_t*>(
            VirtualAlloc(
                nullptr,
                kGuestHeapSlabBytes,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE));
        if (slab == nullptr) {
            return nullptr;
        }
        g_libc_slab_cursor = slab;
        g_libc_slab_remaining = kGuestHeapSlabBytes;
        ++g_libc_slab_count;
        g_libc_slab_bytes += kGuestHeapSlabBytes;
        g_libc_slab_ranges.push_back({
            reinterpret_cast<std::uint64_t>(slab),
            reinterpret_cast<std::uint64_t>(slab) + kGuestHeapSlabBytes,
        });
    }
    const auto span = wanted(g_libc_slab_cursor);
    if (span > g_libc_slab_remaining) {
        return nullptr;
    }
    auto* base = g_libc_slab_cursor;
    g_libc_slab_cursor += span;
    g_libc_slab_remaining -= span;
    taken = span;
    return base;
}

// The region an address falls in: [first, second), or {0, 0}. The heap
// lock must be held, shared or exclusive.
std::pair<std::uint64_t, std::uint64_t> libc_region_of(std::uint64_t address) {
    for (auto index = g_libc_slab_ranges.size(); index-- > 0;) {
        const auto& range = g_libc_slab_ranges[index];
        if (address >= range.first && address < range.second) {
            return range;
        }
    }
    const auto after = g_libc_isolated_ranges.upper_bound(address);
    if (after != g_libc_isolated_ranges.begin()) {
        const auto found = std::prev(after);
        if (address >= found->first && address < found->second) {
            return {found->first, found->second};
        }
    }
    return {0, 0};
}

// The header of the live block handed out at `user`, or nullptr. The heap
// lock must be held.
LibcBlockHeader* libc_block_of(std::uint64_t user) {
    if ((user & 7u) != 0) {
        return nullptr;
    }
    const auto region = libc_region_of(user);
    if (region.first == 0 || user < region.first + kLibcBlockPrefix) {
        return nullptr;
    }
    const auto base =
        *reinterpret_cast<const std::uint64_t*>(user - sizeof(std::uint64_t));
    if (base < region.first || base + sizeof(LibcBlockHeader) > user) {
        return nullptr;
    }
    auto* header = reinterpret_cast<LibcBlockHeader*>(base);
    if (header->magic != (kLibcBlockMagic ^ user) || header->user != user) {
        return nullptr;
    }
    return header;
}
std::atomic<std::uint64_t> g_libc_unknown_free_count{0};
SRWLOCK g_libc_mspace_registry_lock = SRWLOCK_INIT;
std::map<std::uint64_t, LibcMspace*> g_libc_mspaces;
std::atomic<std::uint64_t> g_libc_mspace_trace_count{0};
const bool g_trace_alloc = []() {
    char buffer[8] = {};
    return GetEnvironmentVariableA("PS5RT_TRACE_ALLOC", buffer, sizeof(buffer)) != 0 &&
           buffer[0] == '1';
}();

bool is_power_of_two(std::size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

std::size_t normalize_libc_alignment(std::size_t alignment) {
    return alignment < kDefaultLibcHeapAlignment
        ? kDefaultLibcHeapAlignment
        : alignment;
}

std::uint64_t align_up_u64(std::uint64_t value, std::uint64_t alignment) {
    if (alignment == 0 ||
        value > UINT64_MAX - (alignment - 1)) {
        return UINT64_MAX;
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

LibcMspace* find_libc_mspace(std::uint64_t handle) {
    AcquireSRWLockShared(&g_libc_mspace_registry_lock);
    const auto entry = g_libc_mspaces.find(handle);
    auto* result = entry == g_libc_mspaces.end() ? nullptr : entry->second;
    ReleaseSRWLockShared(&g_libc_mspace_registry_lock);
    return result;
}

void coalesce_mspace_free_ranges(LibcMspace& mspace) {
    std::sort(
        mspace.free_ranges.begin(),
        mspace.free_ranges.end(),
        [](const auto& left, const auto& right) {
            return left.offset < right.offset;
        });
    std::size_t write = 0;
    for (const auto& range : mspace.free_ranges) {
        if (range.size == 0) {
            continue;
        }
        if (write != 0) {
            auto& previous = mspace.free_ranges[write - 1];
            const auto previous_end = previous.offset + previous.size;
            if (range.offset <= previous_end) {
                const auto range_end = range.offset + range.size;
                if (range_end > previous_end) {
                    previous.size = range_end - previous.offset;
                }
                continue;
            }
        }
        mspace.free_ranges[write++] = range;
    }
    mspace.free_ranges.resize(write);
}

void* libc_mspace_allocate(
    LibcMspace& mspace,
    std::uint64_t requested_size,
    std::uint64_t requested_alignment,
    bool zero_fill) {
    const auto alignment = static_cast<std::uint64_t>(
        normalize_libc_alignment(
            static_cast<std::size_t>(requested_alignment)));
    if (!is_power_of_two(static_cast<std::size_t>(alignment))) {
        return nullptr;
    }
    const auto size = requested_size == 0 ? 1ULL : requested_size;
    if (size > mspace.capacity) {
        return nullptr;
    }

    AcquireSRWLockExclusive(&mspace.lock);
    std::uint64_t selected_offset = UINT64_MAX;
    for (std::size_t index = 0;
         index < mspace.free_ranges.size();
         ++index) {
        const auto range = mspace.free_ranges[index];
        const auto range_address = mspace.base + range.offset;
        const auto aligned_address = align_up_u64(range_address, alignment);
        if (aligned_address == UINT64_MAX ||
            aligned_address < mspace.base) {
            continue;
        }
        const auto offset = aligned_address - mspace.base;
        if (offset < range.offset ||
            offset > range.offset + range.size ||
            size > range.offset + range.size - offset) {
            continue;
        }

        selected_offset = offset;
        mspace.free_ranges.erase(mspace.free_ranges.begin() + index);
        if (offset > range.offset) {
            mspace.free_ranges.push_back({
                range.offset,
                offset - range.offset,
            });
        }
        const auto allocation_end = offset + size;
        const auto range_end = range.offset + range.size;
        if (allocation_end < range_end) {
            mspace.free_ranges.push_back({
                allocation_end,
                range_end - allocation_end,
            });
        }
        coalesce_mspace_free_ranges(mspace);
        break;
    }

    if (selected_offset == UINT64_MAX) {
        const auto current_address = mspace.base + mspace.high_water;
        const auto aligned_address = align_up_u64(current_address, alignment);
        if (aligned_address != UINT64_MAX &&
            aligned_address >= mspace.base) {
            const auto offset = aligned_address - mspace.base;
            if (offset <= mspace.capacity &&
                size <= mspace.capacity - offset) {
                selected_offset = offset;
                mspace.high_water = offset + size;
            }
        }
    }

    std::uint64_t address = 0;
    if (selected_offset != UINT64_MAX) {
        address = mspace.base + selected_offset;
        mspace.allocations[address] = {
            selected_offset,
            size,
            alignment,
        };
    }
    ReleaseSRWLockExclusive(&mspace.lock);

    if (address != 0 && zero_fill) {
        std::memset(reinterpret_cast<void*>(address), 0, size);
    }
    return reinterpret_cast<void*>(address);
}

bool libc_mspace_release(
    LibcMspace& mspace,
    std::uint64_t address,
    LibcMspaceAllocation* released = nullptr) {
    AcquireSRWLockExclusive(&mspace.lock);
    const auto entry = mspace.allocations.find(address);
    if (entry == mspace.allocations.end()) {
        ReleaseSRWLockExclusive(&mspace.lock);
        return false;
    }
    const auto allocation = entry->second;
    mspace.allocations.erase(entry);
    mspace.free_ranges.push_back({
        allocation.offset,
        allocation.size,
    });
    coalesce_mspace_free_ranges(mspace);
    ReleaseSRWLockExclusive(&mspace.lock);
    if (released != nullptr) {
        *released = allocation;
    }
    return true;
}

void* libc_heap_allocate(
    std::uint64_t requested_size,
    std::uint64_t requested_alignment,
    bool zero_fill,
    std::uint64_t caller = 0) {
    const auto alignment =
        normalize_libc_alignment(static_cast<std::size_t>(requested_alignment));
    if (!is_power_of_two(alignment)) {
        return nullptr;
    }

    const std::size_t size =
        requested_size == 0 ? 1 : static_cast<std::size_t>(requested_size);
    const std::size_t overhead = alignment - 1 + kLibcBlockPrefix;
    if (size > SIZE_MAX - overhead - kGuestHeapSafetySlack) {
        return nullptr;
    }

    std::size_t span = 0;
    AcquireSRWLockExclusive(&g_libc_heap_lock);
    void* base = libc_slab_take(size, alignment, span);
    ReleaseSRWLockExclusive(&g_libc_heap_lock);
    bool isolated = false;
    if (base == nullptr) {
        span = size + overhead + kGuestHeapSafetySlack;
        base = VirtualAlloc(
            nullptr,
            span,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE);
        isolated = true;
    }
    if (base == nullptr) {
        return nullptr;
    }

    const auto raw = reinterpret_cast<std::uint64_t>(base) + kLibcBlockPrefix;
    const auto aligned =
        (raw + alignment - 1) & ~(static_cast<std::uint64_t>(alignment) - 1);
    auto* header = static_cast<LibcBlockHeader*>(base);
    header->user = aligned;
    header->size = size;
    header->span = span;
    header->alignment = alignment;
    header->caller = caller;
    header->state = kLibcBlockLive;
    header->reserved = 0;
    *reinterpret_cast<std::uint64_t*>(aligned - sizeof(std::uint64_t)) =
        reinterpret_cast<std::uint64_t>(base);
    // Last, so a reader under the lock never sees a half-written header as
    // valid.
    header->magic = kLibcBlockMagic ^ aligned;

    AcquireSRWLockExclusive(&g_libc_heap_lock);
    if (isolated) {
        ++g_libc_isolated_count;
        g_libc_isolated_ranges[reinterpret_cast<std::uint64_t>(base)] =
            reinterpret_cast<std::uint64_t>(base) + span;
    }
    g_libc_heap_bytes += size;
    const auto live = ++g_libc_live_blocks;
    ReleaseSRWLockExclusive(&g_libc_heap_lock);
    {
        static std::atomic<std::uint64_t> announced{0};
        auto last = announced.load(std::memory_order_relaxed);
        if (live >= last + 100000 &&
            announced.compare_exchange_strong(last, live)) {
            std::fprintf(
                stderr,
                "ps5rt.libc_heap live=%llu requested_bytes=%llu "
                "slabs=%llu slab_bytes=%llu isolated=%llu\n",
                static_cast<unsigned long long>(live),
                static_cast<unsigned long long>(g_libc_heap_bytes),
                static_cast<unsigned long long>(g_libc_slab_count),
                static_cast<unsigned long long>(g_libc_slab_bytes),
                static_cast<unsigned long long>(g_libc_isolated_count));
            std::fflush(stderr);
        }
    }

    if (zero_fill) {
        std::memset(reinterpret_cast<void*>(aligned), 0, size);
    }
    if (g_trace_alloc) {
        trace_sync_error(
            "alloc ptr=0x%016llX size=0x%llX align=0x%llX zero=%d\n",
            static_cast<unsigned long long>(aligned),
            static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(alignment),
            zero_fill ? 1 : 0);
    }
    return reinterpret_cast<void*>(aligned);
}

bool libc_heap_lookup(std::uint64_t address, LibcHeapAllocation& allocation) {
    AcquireSRWLockShared(&g_libc_heap_lock);
    const auto* header = libc_block_of(address);
    const bool found =
        header != nullptr && header->state == kLibcBlockLive;
    if (found) {
        allocation = LibcHeapAllocation{
            const_cast<LibcBlockHeader*>(header),
            static_cast<std::size_t>(header->size),
            static_cast<std::size_t>(header->alignment),
            static_cast<std::size_t>(header->span),
            header->caller,
        };
    }
    ReleaseSRWLockShared(&g_libc_heap_lock);
    return found;
}

void libc_heap_free(void* pointer) {
    if (pointer == nullptr) {
        return;
    }

    const auto address = reinterpret_cast<std::uint64_t>(pointer);
    LibcHeapAllocation allocation{};
    bool found = false;

    AcquireSRWLockExclusive(&g_libc_heap_lock);
    if (auto* header = libc_block_of(address);
        header != nullptr && header->state == kLibcBlockLive) {
        header->state = kLibcBlockFreed;
        allocation.size = static_cast<std::size_t>(header->size);
        --g_libc_live_blocks;
        found = true;
    }
    ReleaseSRWLockExclusive(&g_libc_heap_lock);

    if (!found) {
        const auto hit =
            g_libc_unknown_free_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hit <= 32) {
            trace_sync_error(
                "libc_heap_free_foreign=%llu ptr=0x%016llX\n",
                static_cast<unsigned long long>(hit),
                static_cast<unsigned long long>(address));
        }
        return;
    }

    if (g_trace_alloc) {
        trace_sync_error(
            "free ptr=0x%016llX size=0x%llX\n",
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(allocation.size));
    }
    // Keep the backing region alive until process exit. Astro Bot can retain
    // allocator aliases across worker threads; immediately releasing the
    // address turns that race into a host AV inside memcpy/memmove.
}

extern "C" void ps5rt_describe_libc_address(
    std::uint64_t address,
    const char* label) {
    std::uint64_t user_address = 0;
    LibcHeapAllocation allocation{};
    bool found = false;

    // The block around an address: walk the region it is in from the start,
    // block to block by span. Only for crash reports.
    AcquireSRWLockShared(&g_libc_heap_lock);
    const auto region = libc_region_of(address);
    for (auto cursor = region.first;
         region.first != 0 && cursor + sizeof(LibcBlockHeader) <= region.second;) {
        const auto* header = reinterpret_cast<const LibcBlockHeader*>(cursor);
        if (header->magic != (kLibcBlockMagic ^ header->user) ||
            header->span == 0) {
            break;
        }
        if (address >= cursor && address - cursor < header->span) {
            user_address = header->user;
            allocation = LibcHeapAllocation{
                reinterpret_cast<void*>(cursor),
                static_cast<std::size_t>(header->size),
                static_cast<std::size_t>(header->alignment),
                static_cast<std::size_t>(header->span),
                header->caller,
            };
            found = true;
            break;
        }
        cursor += header->span;
    }
    ReleaseSRWLockShared(&g_libc_heap_lock);

    trace_sync_error(
        "libc_address label=%s address=0x%016llX found=%d "
        "user=0x%016llX base=0x%016llX requested=0x%llX "
        "span=0x%llX align=0x%llX caller=0x%016llX offset=0x%llX\n",
        label == nullptr ? "unknown" : label,
        static_cast<unsigned long long>(address),
        found ? 1 : 0,
        static_cast<unsigned long long>(user_address),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uint64_t>(allocation.base)),
        static_cast<unsigned long long>(allocation.size),
        static_cast<unsigned long long>(allocation.span),
        static_cast<unsigned long long>(allocation.alignment),
        static_cast<unsigned long long>(allocation.caller),
        static_cast<unsigned long long>(
            found ? address - user_address : 0));
}

void* libc_heap_reallocate(void* pointer, std::uint64_t requested_size) {
    if (pointer == nullptr) {
        return libc_heap_allocate(requested_size, kDefaultLibcHeapAlignment, false);
    }
    if (requested_size == 0) {
        libc_heap_free(pointer);
        return nullptr;
    }

    LibcHeapAllocation allocation{};
    if (!libc_heap_lookup(reinterpret_cast<std::uint64_t>(pointer), allocation)) {
        const auto hit =
            g_libc_unknown_free_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hit <= 32) {
            trace_sync_error(
                "libc_heap_realloc_foreign=%llu ptr=0x%016llX\n",
                static_cast<unsigned long long>(hit),
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uint64_t>(pointer)));
        }
        return nullptr;
    }

    auto* fresh = libc_heap_allocate(requested_size, allocation.alignment, false);
    if (fresh == nullptr) {
        return nullptr;
    }

    const auto copied = allocation.size < requested_size
        ? allocation.size
        : static_cast<std::size_t>(requested_size);
    std::memcpy(fresh, pointer, copied);
    libc_heap_free(pointer);
    return fresh;
}

// Which guest threads sit inside a blocking HLE wait, and since when.
// The thread that owes a command submit makes no bridge calls and no
// traced native calls while it is stuck, so recording entry and exit
// around the waits themselves is the only way to name what holds it.
// Which guest wait a thread is inside, and how long the guest has spent in
// each. The first answers a hang; the second answers a frame that is merely
// slow, because by the time anyone looks at a slow frame every wait in it
// has already ended.
//
// Both used to live in one std::map behind one global exclusive lock, taken
// twice per wait - once to record entry, once to erase it - with a node
// allocated and freed each time. That was affordable when the waits were
// assumed to be rare. Measured, they are not: this title makes 24 million
// semaphore waits and 18 million event-flag waits in two minutes, about
// 190,000 a second across forty threads, and the great majority return in
// under thirty microseconds. Forty threads serialising on one lock sixty
// million times is not instrumentation of the guest, it is a second guest.
//
// So each thread owns its own slot and writes only to it. The slot is
// deliberately never freed, because the reporting thread reads it and has
// no way to know the owner has gone.
enum class WaitKind : std::size_t {
    ThreadJoin,
    MutexLock,
    CondWait,
    WaitSema,
    WaitEventFlag,
    WaitVblank,
    Count,
};

constexpr std::size_t kWaitKindCount =
    static_cast<std::size_t>(WaitKind::Count);

const char* wait_kind_name(WaitKind kind) {
    switch (kind) {
    case WaitKind::ThreadJoin:
        return "thread_join";
    case WaitKind::MutexLock:
        return "mutex_lock";
    case WaitKind::CondWait:
        return "cond_wait";
    case WaitKind::WaitSema:
        return "wait_sema";
    case WaitKind::WaitEventFlag:
        return "wait_event_flag";
    case WaitKind::WaitVblank:
        return "wait_vblank";
    case WaitKind::Count:
        break;
    }
    return "?";
}

std::int64_t wait_performance_counter() {
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

std::int64_t wait_performance_frequency() {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value = {};
        QueryPerformanceFrequency(&value);
        return value.QuadPart != 0 ? value.QuadPart : 1;
    }();
    return frequency;
}

struct WaitSlot {
    std::uint32_t thread_id = 0;
    // -1 when the thread is running, otherwise the WaitKind it is inside.
    std::atomic<int> active_kind{-1};
    std::atomic<std::uint64_t> active_detail{0};
    std::atomic<std::uint64_t> active_since_ms{0};
    // Written only by the owning thread, read by the reporter, so relaxed
    // loads and stores rather than read-modify-write.
    std::array<std::atomic<std::uint64_t>, kWaitKindCount> ticks{};
    std::array<std::atomic<std::uint64_t>, kWaitKindCount> count{};
};

SRWLOCK g_wait_slots_lock = SRWLOCK_INIT;
std::vector<WaitSlot*> g_wait_slots;
thread_local WaitSlot* t_wait_slot = nullptr;

void start_stall_watchdog();

WaitSlot& current_wait_slot() {
    if (t_wait_slot == nullptr) {
        start_stall_watchdog();
        auto* slot = new WaitSlot();
        slot->thread_id = GetCurrentThreadId();
        AcquireSRWLockExclusive(&g_wait_slots_lock);
        g_wait_slots.push_back(slot);
        ReleaseSRWLockExclusive(&g_wait_slots_lock);
        t_wait_slot = slot;
    }
    return *t_wait_slot;
}

// Where a shader registration goes: translating it in the bridge, or
// handing the result to the GPU runtime. Both happen on the guest thread,
// in front of the dispatch that needs them, so both are frame time. This
// is a handful of counter reads per registration against a path that
// already costs milliseconds, so it does not need a thread-local slot.
enum class ShaderStage : std::size_t {
    TranslateGraphics,
    TranslateCompute,
    RegisterGraphics,
    RegisterCompute,
    Count,
};

constexpr std::size_t kShaderStageCount =
    static_cast<std::size_t>(ShaderStage::Count);

const char* shader_stage_name(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::TranslateGraphics:
        return "translate_gfx";
    case ShaderStage::TranslateCompute:
        return "translate_cs";
    case ShaderStage::RegisterGraphics:
        return "register_gfx";
    case ShaderStage::RegisterCompute:
        return "register_cs";
    case ShaderStage::Count:
        break;
    }
    return "?";
}

std::array<std::atomic<std::uint64_t>, kShaderStageCount>
    g_shader_stage_ticks{};
std::array<std::atomic<std::uint64_t>, kShaderStageCount>
    g_shader_stage_count{};

struct ShaderStageScope {
    explicit ShaderStageScope(ShaderStage stage)
        : stage(stage), started_ticks(wait_performance_counter()) {}

    ~ShaderStageScope() {
        const auto elapsed = wait_performance_counter() - started_ticks;
        const auto index = static_cast<std::size_t>(stage);
        g_shader_stage_ticks[index].fetch_add(
            elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0,
            std::memory_order_relaxed);
        g_shader_stage_count[index].fetch_add(
            1,
            std::memory_order_relaxed);
    }

    ShaderStage stage;
    std::int64_t started_ticks;

    ShaderStageScope(const ShaderStageScope&) = delete;
    ShaderStageScope& operator=(const ShaderStageScope&) = delete;
};

struct WaitScope {
    WaitScope(WaitKind kind, std::uint64_t detail)
        : slot(current_wait_slot()),
          kind(kind),
          started_ticks(wait_performance_counter()) {
        slot.active_detail.store(detail, std::memory_order_relaxed);
        slot.active_since_ms.store(
            GetTickCount64(),
            std::memory_order_relaxed);
        slot.active_kind.store(
            static_cast<int>(kind),
            std::memory_order_release);
    }

    ~WaitScope() {
        const auto elapsed =
            wait_performance_counter() - started_ticks;
        slot.active_kind.store(-1, std::memory_order_release);
        const auto index = static_cast<std::size_t>(kind);
        auto& ticks = slot.ticks[index];
        auto& count = slot.count[index];
        ticks.store(
            ticks.load(std::memory_order_relaxed) +
                (elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0),
            std::memory_order_relaxed);
        count.store(
            count.load(std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
    }

    WaitSlot& slot;
    WaitKind kind;
    std::int64_t started_ticks;

    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;
};

} // namespace

extern "C" PS5RT_GUEST_ABI void* ps5rt_mspace_malloc(std::uint64_t size) {
    return libc_heap_allocate(
        size,
        kDefaultLibcHeapAlignment,
        false,
        reinterpret_cast<std::uint64_t>(__builtin_return_address(0)));
}

extern "C" PS5RT_GUEST_ABI void ps5rt_mspace_free(void* ptr) {
    libc_heap_free(ptr);
}

extern "C" PS5RT_GUEST_ABI void* ps5rt_mspace_memalign(
    std::uint64_t align,
    std::uint64_t size) {
    if (!is_power_of_two(static_cast<std::size_t>(align))) {
        return nullptr;
    }
    return libc_heap_allocate(size, align, false);
}

extern "C" PS5RT_GUEST_ABI void* ps5rt_calloc(
    std::uint64_t count,
    std::uint64_t size) {
    if (count != 0 && size > UINT64_MAX / count) {
        return nullptr;
    }
    return libc_heap_allocate(count * size, kDefaultLibcHeapAlignment, true);
}

extern "C" PS5RT_GUEST_ABI void* ps5rt_realloc(void* ptr, std::uint64_t size) {
    return libc_heap_reallocate(ptr, size);
}

extern "C" PS5RT_GUEST_ABI void* ps5rt_aligned_alloc(
    std::uint64_t align,
    std::uint64_t size) {
    if (!is_power_of_two(static_cast<std::size_t>(align))) {
        return nullptr;
    }
    return libc_heap_allocate(size, align, false);
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_posix_memalign(
    void** out_pointer,
    std::uint64_t align,
    std::uint64_t size) {
    if (out_pointer == nullptr) {
        return 22; // EINVAL
    }
    if (!is_power_of_two(static_cast<std::size_t>(align)) ||
        align < sizeof(void*)) {
        return 22;
    }

    auto* allocated = libc_heap_allocate(size, align, false);
    if (allocated == nullptr) {
        return 12; // ENOMEM
    }
    *out_pointer = allocated;
    return 0;
}

// --- sceLibcMspace* ---
//
// These carry the mspace handle in the first argument slot. Aliasing them onto
// the plain libc entry points shifts every argument by one, which made
// sceLibcMspaceFree hand its mspace handle to free().

extern "C" bool ps5rt_libc_mspace_is_native(std::uint64_t mspace) {
    auto* state = find_libc_mspace(mspace);
    return state != nullptr && state->magic == kLibcMspaceMagic;
}

extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_libc_mspace_create(
    std::uint64_t name,
    std::uint64_t base,
    std::uint64_t capacity,
    std::uint64_t flags) {
    if (capacity == 0) {
        return 0;
    }
    if (base == 0) {
        base = reinterpret_cast<std::uint64_t>(VirtualAlloc(
            nullptr,
            static_cast<SIZE_T>(capacity),
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE));
        if (base == 0) {
            return 0;
        }
    }

    auto* mspace = new (std::nothrow) LibcMspace(base, capacity);
    if (mspace == nullptr) {
        return 0;
    }
    const auto handle = reinterpret_cast<std::uint64_t>(mspace);
    AcquireSRWLockExclusive(&g_libc_mspace_registry_lock);
    g_libc_mspaces[handle] = mspace;
    ReleaseSRWLockExclusive(&g_libc_mspace_registry_lock);
    trace_sync_error(
        "mspace_create handle=0x%016llX name=0x%016llX "
        "base=0x%016llX capacity=0x%016llX flags=0x%llX\n",
        static_cast<unsigned long long>(handle),
        static_cast<unsigned long long>(name),
        static_cast<unsigned long long>(base),
        static_cast<unsigned long long>(capacity),
        static_cast<unsigned long long>(flags));
    return handle;
}

extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_libc_mspace_destroy(
    std::uint64_t mspace) {
    auto* state = find_libc_mspace(mspace);
    if (state == nullptr || state->magic != kLibcMspaceMagic) {
        return 0;
    }
    // Keep the object alive until process exit. Worker threads can still
    // observe allocator aliases while teardown is in progress.
    return 0;
}

extern "C" PS5RT_GUEST_ABI void* ps5rt_libc_mspace_malloc(
    std::uint64_t mspace,
    std::uint64_t size) {
    auto* state = find_libc_mspace(mspace);
    if (state == nullptr || state->magic != kLibcMspaceMagic) {
        return libc_heap_allocate(
            size,
            kDefaultLibcHeapAlignment,
            false,
            reinterpret_cast<std::uint64_t>(__builtin_return_address(0)));
    }
    auto* result = libc_mspace_allocate(
        *state,
        size,
        kDefaultLibcHeapAlignment,
        false);
    const auto trace_index =
        g_libc_mspace_trace_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (trace_index <= 32) {
        trace_sync_error(
            "mspace_malloc count=%llu handle=0x%016llX "
            "size=0x%llX result=0x%016llX\n",
            static_cast<unsigned long long>(trace_index),
            static_cast<unsigned long long>(mspace),
            static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(
                reinterpret_cast<std::uint64_t>(result)));
    }
    return result;
}

extern "C" PS5RT_GUEST_ABI void ps5rt_libc_mspace_free(
    std::uint64_t mspace,
    void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    auto* state = find_libc_mspace(mspace);
    if (state == nullptr ||
        state->magic != kLibcMspaceMagic ||
        !libc_mspace_release(
            *state,
            reinterpret_cast<std::uint64_t>(ptr))) {
        libc_heap_free(ptr);
    }
}

extern "C" PS5RT_GUEST_ABI void* ps5rt_libc_mspace_calloc(
    std::uint64_t mspace,
    std::uint64_t count,
    std::uint64_t size) {
    if (count != 0 && size > UINT64_MAX / count) {
        return nullptr;
    }
    auto* state = find_libc_mspace(mspace);
    if (state == nullptr || state->magic != kLibcMspaceMagic) {
        return libc_heap_allocate(
            count * size,
            kDefaultLibcHeapAlignment,
            true,
            reinterpret_cast<std::uint64_t>(__builtin_return_address(0)));
    }
    return libc_mspace_allocate(
        *state,
        count * size,
        kDefaultLibcHeapAlignment,
        true);
}

extern "C" PS5RT_GUEST_ABI void* ps5rt_libc_mspace_realloc(
    std::uint64_t mspace,
    void* ptr,
    std::uint64_t size) {
    auto* state = find_libc_mspace(mspace);
    if (state == nullptr || state->magic != kLibcMspaceMagic) {
        return libc_heap_reallocate(ptr, size);
    }
    if (ptr == nullptr) {
        return libc_mspace_allocate(
            *state,
            size,
            kDefaultLibcHeapAlignment,
            false);
    }
    if (size == 0) {
        libc_mspace_release(
            *state,
            reinterpret_cast<std::uint64_t>(ptr));
        return nullptr;
    }

    LibcMspaceAllocation existing{};
    AcquireSRWLockShared(&state->lock);
    const auto entry = state->allocations.find(
        reinterpret_cast<std::uint64_t>(ptr));
    const bool found = entry != state->allocations.end();
    if (found) {
        existing = entry->second;
    }
    ReleaseSRWLockShared(&state->lock);
    if (!found) {
        return nullptr;
    }

    auto* result = libc_mspace_allocate(
        *state,
        size,
        existing.alignment,
        false);
    if (result == nullptr) {
        return nullptr;
    }
    std::memcpy(
        result,
        ptr,
        static_cast<std::size_t>(std::min(existing.size, size)));
    libc_mspace_release(
        *state,
        reinterpret_cast<std::uint64_t>(ptr));
    return result;
}

extern "C" PS5RT_GUEST_ABI void* ps5rt_libc_mspace_memalign(
    std::uint64_t mspace,
    std::uint64_t align,
    std::uint64_t size) {
    if (!is_power_of_two(static_cast<std::size_t>(align))) {
        return nullptr;
    }
    auto* state = find_libc_mspace(mspace);
    if (state == nullptr || state->magic != kLibcMspaceMagic) {
        return libc_heap_allocate(
            size,
            align,
            false,
            reinterpret_cast<std::uint64_t>(__builtin_return_address(0)));
    }
    return libc_mspace_allocate(*state, size, align, false);
}

extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_libc_mspace_malloc_stats(
    std::uint64_t mspace,
    std::uint64_t stats) {
    auto* state = find_libc_mspace(mspace);
    if (state == nullptr || state->magic != kLibcMspaceMagic) {
        return 0;
    }
    if (stats == 0) {
        return 0x80020016;
    }

    std::uint64_t used = 0;
    AcquireSRWLockShared(&state->lock);
    for (const auto& [_, allocation] : state->allocations) {
        used += allocation.size;
    }
    const auto high_water = state->high_water;
    ReleaseSRWLockShared(&state->lock);

    auto* output = reinterpret_cast<std::uint8_t*>(stats);
    const auto region_end = state->base + state->capacity - 1;
    std::memcpy(output + 0x08, &state->base, sizeof(state->base));
    std::memcpy(output + 0x10, &region_end, sizeof(region_end));
    std::memcpy(output + 0x18, &used, sizeof(used));
    const auto available = state->capacity - used;
    std::memcpy(output + 0x20, &available, sizeof(available));
    if (g_trace_alloc) {
        trace_sync_error(
            "mspace_stats handle=0x%016llX base=0x%016llX "
            "end=0x%016llX used=0x%llX high=0x%llX free=0x%llX\n",
            static_cast<unsigned long long>(mspace),
            static_cast<unsigned long long>(state->base),
            static_cast<unsigned long long>(region_end),
            static_cast<unsigned long long>(used),
            static_cast<unsigned long long>(high_water),
            static_cast<unsigned long long>(available));
    }
    return 0;
}

extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_libc_mspace_malloc_usable_size(
    std::uint64_t mspace,
    const void* pointer) {
    auto* state = find_libc_mspace(mspace);
    if (state == nullptr ||
        state->magic != kLibcMspaceMagic ||
        pointer == nullptr) {
        return 0;
    }

    AcquireSRWLockShared(&state->lock);
    const auto entry = state->allocations.find(
        reinterpret_cast<std::uint64_t>(pointer));
    const auto size = entry == state->allocations.end()
        ? 0ULL
        : entry->second.size;
    ReleaseSRWLockShared(&state->lock);
    return size;
}

extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_libc_mspace_is_heap_empty(std::uint64_t mspace) {
    auto* state = find_libc_mspace(mspace);
    if (state == nullptr || state->magic != kLibcMspaceMagic) {
        return 1;
    }

    AcquireSRWLockShared(&state->lock);
    const bool empty = state->allocations.empty();
    ReleaseSRWLockShared(&state->lock);
    return empty ? 1 : 0;
}

// Thread management
struct GuestThread {
    HANDLE host_handle;
    std::uint64_t guest_id;
    std::uint64_t guest_func;
    std::uint64_t guest_arg;
    std::atomic<bool> finished{false};
    std::uint64_t exit_code{0};
};

static std::atomic<std::uint64_t> g_next_thread_id{1000};
static thread_local std::uint64_t g_tls_tcb{0};
static thread_local GuestThread* g_current_guest_thread = nullptr;
static thread_local void* g_current_guest_tls_context = nullptr;
static std::map<std::uint64_t, GuestThread*> g_threads;
static SRWLOCK g_thread_lock = SRWLOCK_INIT;
static Ps5RtGuestTlsEnter g_guest_tls_enter = nullptr;
static Ps5RtGuestTlsLeave g_guest_tls_leave = nullptr;

// Host-only synchronization state stored in the unused tail of the
// 0x100-byte opaque pthread mutex object created by the runner.
struct GuestMutexHostState {
    SRWLOCK srw;
    std::uint32_t owner_thread_id;
    std::uint32_t lock_count;
};

static constexpr std::size_t kGuestMutexHostStateOffset = 0x80;
static constexpr std::size_t kGuestMutexTypeOffset = 0x20;

static std::uint8_t* allocate_guest_mutex_object(std::uint32_t type) {
    auto* object = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 0x100, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (object == nullptr) {
        return nullptr;
    }
    *reinterpret_cast<std::uint32_t*>(
        object + kGuestMutexTypeOffset) = type;
    auto* host = reinterpret_cast<GuestMutexHostState*>(
        object + kGuestMutexHostStateOffset);
    InitializeSRWLock(&host->srw);
    return object;
}

static std::uint8_t* resolve_guest_mutex_object(
    std::uint64_t mutex_ptr, bool create_if_static = true) {
    if (mutex_ptr == 0) {
        return nullptr;
    }

    auto* slot = reinterpret_cast<volatile LONG64*>(mutex_ptr);
    for (;;) {
        const auto handle = static_cast<std::uint64_t>(*slot);
        if (handle > 1) {
            return reinterpret_cast<std::uint8_t*>(handle);
        }
        if (!create_if_static) {
            return nullptr;
        }

        const auto type = handle == 1 ? std::uint32_t{4} : std::uint32_t{1};
        auto* object = allocate_guest_mutex_object(type);
        if (object == nullptr) {
            return nullptr;
        }

        const auto installed = InterlockedCompareExchange64(
            slot,
            static_cast<LONG64>(reinterpret_cast<std::uint64_t>(object)),
            static_cast<LONG64>(handle));
        if (static_cast<std::uint64_t>(installed) == handle) {
            return object;
        }

        VirtualFree(object, 0, MEM_RELEASE);
    }
}

static GuestMutexHostState* resolve_guest_mutex_host_state(
    std::uint8_t* object) {
    return object == nullptr
        ? nullptr
        : reinterpret_cast<GuestMutexHostState*>(
            object + kGuestMutexHostStateOffset);
}

static std::uint32_t guest_mutex_type(const std::uint8_t* object) {
    return object == nullptr
        ? 1
        : *reinterpret_cast<const std::uint32_t*>(
            object + kGuestMutexTypeOffset);
}

// Condition variable
struct GuestCondVar {
    CONDITION_VARIABLE cv;
    // Who woke it last, for PS5RT_TRACE_COND_WAITS.
    volatile LONG last_signaler;
};

// PS5RT_TRACE_COND_WAITS=ms: every condition wait longer than that, with
// the thread that woke it and where the title waited from - who a waiting
// thread is waiting for.
static std::uint32_t cond_wait_trace_ms() {
    static const std::uint32_t threshold = [] {
        const auto* value = std::getenv("PS5RT_TRACE_COND_WAITS");
        return value == nullptr ? 0u
                                : static_cast<std::uint32_t>(
                                      std::strtoul(value, nullptr, 0));
    }();
    return threshold;
}

static GuestCondVar* allocate_guest_cond_var() {
    auto* cv = new (std::nothrow) GuestCondVar{};
    if (cv != nullptr) {
        InitializeConditionVariable(&cv->cv);
    }
    return cv;
}

static GuestCondVar* resolve_guest_cond_var(
    std::uint64_t cond_ptr, bool create_if_zero = true) {
    if (cond_ptr == 0) {
        return nullptr;
    }

    auto* slot = reinterpret_cast<volatile LONG64*>(cond_ptr);
    for (;;) {
        const auto handle = static_cast<std::uint64_t>(*slot);
        if (handle > 1) {
            return reinterpret_cast<GuestCondVar*>(handle);
        }
        if (handle != 0 || !create_if_zero) {
            return nullptr;
        }

        auto* cv = allocate_guest_cond_var();
        if (cv == nullptr) {
            return nullptr;
        }

        const auto installed = InterlockedCompareExchange64(
            slot,
            static_cast<LONG64>(reinterpret_cast<std::uint64_t>(cv)),
            0);
        if (installed == 0) {
            return cv;
        }

        delete cv;
    }
}

// Semaphore
struct GuestSema {
    HANDLE handle;
};

// Event flag
struct GuestEventFlag {
    HANDLE handle;
};

// Equeue
struct GuestEqueue {
    HANDLE iocp;
};

// Global storage
static std::map<std::uint64_t, GuestCondVar*> g_condvars;
static std::map<std::uint64_t, GuestSema*> g_semas;
static std::map<std::uint64_t, GuestEventFlag*> g_events;
static std::map<std::uint64_t, GuestEqueue*> g_equeues;
static std::atomic<std::uint64_t> g_next_handle{0x10000};

// These maps are read and written by every guest thread at once - forty of
// them in a run - and were unguarded. A worker that loses its lookup to a
// concurrent insert gets ESRCH out of sceKernelWaitSema, returns straight
// away instead of waiting, and calls the callback its pool has not set
// yet. Deleting an object while another thread holds a pointer into it is
// the same hazard from the other end, so a destroyed object leaves the map
// and its handle closes, but the small struct itself is not freed.
// The title maps 10.2 GB of direct memory in six calls and touches a
// fraction of it, but every byte was committed up front. On this host that
// leaves under a gigabyte of commit free at the first flip and none at the
// sixty-first, which is where the run ends in std::bad_alloc. Those maps
// are reserved instead and committed as they are used: by the fault
// handler for the guest's own accesses, and by these helpers for the ones
// this side makes on its behalf, because a kernel-mode write to a reserved
// page fails rather than faulting.
struct GuestReservation {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

constexpr std::size_t kGuestReservationCount = 64;
static GuestReservation g_guest_reservations[kGuestReservationCount] = {};
static std::atomic<std::size_t> g_guest_reservation_used{0};
static std::atomic<std::uint64_t> g_guest_commit_bytes{0};
static std::atomic<std::uint64_t> g_guest_commit_failed{0};

extern "C" void ps5rt_guest_reservation_add(
    std::uint64_t begin, std::uint64_t length) {
    if (begin == 0 || length == 0) {
        return;
    }
    const auto slot =
        g_guest_reservation_used.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kGuestReservationCount) {
        return;
    }
    g_guest_reservations[slot].begin = begin;
    g_guest_reservations[slot].end = begin + length;
}

extern "C" bool ps5rt_guest_reservation_contains(std::uint64_t address) {
    const auto used = g_guest_reservation_used.load(
        std::memory_order_relaxed);
    const auto count = used < kGuestReservationCount
        ? used
        : kGuestReservationCount;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& reservation = g_guest_reservations[index];
        if (address >= reservation.begin && address < reservation.end) {
            return true;
        }
    }
    return false;
}

// Commits whatever part of [address, address + size) is reserved but not
// committed, and only where this runtime reserved it. False if any of the
// range could not be made usable.
extern "C" bool ps5rt_guest_commit_range(
    std::uint64_t address, std::uint64_t size) {
    if (address == 0 || size == 0 ||
        address > std::numeric_limits<std::uint64_t>::max() - size) {
        return false;
    }
    auto current = address;
    const auto end = address + size;
    while (current < end) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (VirtualQuery(
                reinterpret_cast<const void*>(current),
                &memory,
                sizeof(memory)) != sizeof(memory)) {
            return false;
        }
        const auto region_start = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress));
        const auto region_end =
            region_start + static_cast<std::uint64_t>(memory.RegionSize);
        if (region_end <= current) {
            return false;
        }
        const auto span = (region_end < end ? region_end : end) - current;
        if (memory.State == MEM_RESERVE) {
            // Rounded out to four megabytes for the same reason as the
            // fault path: a reservation cut into thousands of committed
            // fragments makes every later walk over it expensive.
            constexpr std::uint64_t kGranule = 4ULL * 1024ULL * 1024ULL;
            const auto commit_begin = std::max(
                region_start, current & ~(kGranule - 1));
            const auto commit_end = std::min(
                region_end,
                std::max(current + span, commit_begin + kGranule));
            if (!ps5rt_guest_reservation_contains(current) ||
                commit_end <= commit_begin ||
                VirtualAlloc(
                    reinterpret_cast<void*>(commit_begin),
                    static_cast<SIZE_T>(commit_end - commit_begin),
                    MEM_COMMIT,
                    PAGE_READWRITE) == nullptr) {
                g_guest_commit_failed.fetch_add(
                    1, std::memory_order_relaxed);
                return false;
            }
            g_guest_commit_bytes.fetch_add(span, std::memory_order_relaxed);
        } else if (memory.State != MEM_COMMIT) {
            return false;
        }
        current += span;
    }
    return true;
}

extern "C" std::uint64_t ps5rt_guest_commit_megabytes() {
    return g_guest_commit_bytes.load(std::memory_order_relaxed) >> 20;
}

static SRWLOCK g_object_lock = SRWLOCK_INIT;

template <typename Map>
static typename Map::mapped_type object_lookup(
    const Map& map, std::uint64_t id) {
    AcquireSRWLockShared(&g_object_lock);
    const auto entry = map.find(id);
    auto value = entry == map.end()
        ? typename Map::mapped_type{}
        : entry->second;
    ReleaseSRWLockShared(&g_object_lock);
    return value;
}

template <typename Map>
static void object_insert(
    Map& map, std::uint64_t id, typename Map::mapped_type value) {
    AcquireSRWLockExclusive(&g_object_lock);
    map[id] = value;
    ReleaseSRWLockExclusive(&g_object_lock);
}

template <typename Map>
static typename Map::mapped_type object_take(Map& map, std::uint64_t id) {
    AcquireSRWLockExclusive(&g_object_lock);
    const auto entry = map.find(id);
    auto value = entry == map.end()
        ? typename Map::mapped_type{}
        : entry->second;
    if (entry != map.end()) {
        map.erase(entry);
    }
    ReleaseSRWLockExclusive(&g_object_lock);
    return value;
}

extern "C" {

// --- THREAD MANAGEMENT ---

void ps5rt_set_guest_tls_hooks(
    Ps5RtGuestTlsEnter enter,
    Ps5RtGuestTlsLeave leave,
    Ps5RtGuestTlsRestore restore) {
    g_guest_tls_enter = enter;
    g_guest_tls_leave = leave;
    g_guest_tls_restore = restore;
}

bool ps5rt_is_guest_worker_thread() {
    return g_current_guest_thread != nullptr;
}

[[noreturn]] void ps5rt_abort_current_guest_thread() {
    constexpr std::uint64_t kWorkerAbortExitCode = 0x80020001;
    auto* gt = g_current_guest_thread;
    auto* tls_context = g_current_guest_tls_context;
    g_current_guest_thread = nullptr;
    g_current_guest_tls_context = nullptr;
    if (gt != nullptr) {
        gt->exit_code = kWorkerAbortExitCode;
        gt->finished.store(true, std::memory_order_release);
    }
    if (g_guest_tls_leave != nullptr && tls_context != nullptr) {
        g_guest_tls_leave(tls_context);
    }
    g_tls_tcb = 0;
    trace_sync_error(
        "thread_abort host_id=%lu guest_id=%llu code=0x%08llX\n",
        static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<unsigned long long>(gt == nullptr ? 0 : gt->guest_id),
        static_cast<unsigned long long>(kWorkerAbortExitCode));
    ExitThread(static_cast<DWORD>(kWorkerAbortExitCode));
    __builtin_unreachable();
}

static void trace_guest_thread_start(const GuestThread* gt) {
    // The offsets here were guesses and read a name string out of the
    // middle of the descriptor. The pool trampoline at 0x8003CE260 does
    // `mov rdi,[rbx+0x20]` then `call [rbx+0x18]`, so those two are the
    // entry point and its argument. That argument is the pool object whose
    // +0x128 and +0x130 the worker later calls through, which is where the
    // access violation at 0x800410CD6 comes from - so read them here too,
    // while the thread is still starting.
    std::uint64_t entry = 0;
    std::uint64_t pool = 0;
    std::uint64_t pool_callback = 0;
    std::uint64_t pool_argument = 0;
    const auto read_guest = [](std::uint64_t address, std::uint64_t& value) {
        SIZE_T taken = 0;
        return address != 0 &&
            ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(address),
                &value,
                sizeof(value),
                &taken) != FALSE &&
            taken == sizeof(value);
    };
    // The entry at +0x18 for these threads is the thunk at 0x8004119A0,
    // which does `rsi=[rdi]; rdi=[rsi]; jmp [[rdi]+0x70]`. So the object
    // the worker runs on is two dereferences past the argument, and it is
    // that object's +0x128 the worker calls through.
    std::uint64_t owner = 0;
    std::uint64_t object = 0;
    std::uint64_t vtable = 0;
    const bool has_entry = read_guest(gt->guest_arg + 0x18, entry);
    const bool has_pool = read_guest(gt->guest_arg + 0x20, pool);
    const bool has_owner = has_pool && read_guest(pool, owner);
    const bool has_object = has_owner && read_guest(owner, object);
    const bool has_vtable = has_object && read_guest(object, vtable);
    const bool has_pool_callback =
        has_object && read_guest(object + 0x128, pool_callback);
    const bool has_pool_argument =
        has_object && read_guest(object + 0x130, pool_argument);

    char buffer[448] = {};
    const int length = std::snprintf(
        buffer,
        sizeof(buffer),
        "thread_start guest_id=%llu host_id=%lu func=0x%016llX "
        "arg=0x%016llX entry=0x%016llX pool=0x%016llX "
        "object=0x%016llX vtable=0x%016llX "
        "callback=0x%016llX argument=0x%016llX "
        "read=%d%d%d%d%d%d\n",
        static_cast<unsigned long long>(gt->guest_id),
        static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<unsigned long long>(gt->guest_func),
        static_cast<unsigned long long>(gt->guest_arg),
        static_cast<unsigned long long>(entry),
        static_cast<unsigned long long>(pool),
        static_cast<unsigned long long>(object),
        static_cast<unsigned long long>(vtable),
        static_cast<unsigned long long>(pool_callback),
        static_cast<unsigned long long>(pool_argument),
        has_entry ? 1 : 0,
        has_pool ? 1 : 0,
        has_owner ? 1 : 0,
        has_object ? 1 : 0,
        has_vtable ? 1 : 0,
        has_pool_callback ? 1 : 0);
    if (length <= 0) {
        return;
    }
    const auto bytes = static_cast<DWORD>(
        length < static_cast<int>(sizeof(buffer))
            ? length
            : static_cast<int>(sizeof(buffer) - 1));
    DWORD written = 0;
    WriteFile(
        GetStdHandle(STD_ERROR_HANDLE),
        buffer,
        bytes,
        &written,
        nullptr);
}

static DWORD WINAPI guest_thread_entry(LPVOID param) {
    auto* gt = static_cast<GuestThread*>(param);
    trace_guest_thread_start(gt);
    if (gt->guest_func == 0) {
        gt->finished.store(true, std::memory_order_release);
        return 0;
    }
    g_tls_tcb = gt->guest_id;
    void* tls_context =
        g_guest_tls_enter == nullptr ? nullptr : g_guest_tls_enter();
    if (g_guest_tls_enter != nullptr && tls_context == nullptr) {
        g_tls_tcb = 0;
        gt->exit_code = 0x8002000C;
        gt->finished.store(true, std::memory_order_release);
        return 0;
    }
    trace_stderr("thread_start id=%llu func=0x%llX arg=0x%llX\n",
        static_cast<unsigned long long>(gt->host_handle != nullptr ? GetCurrentThreadId() : 0),
        static_cast<unsigned long long>(gt->guest_func),
        static_cast<unsigned long long>(gt->guest_arg));
    restore_guest_fs();
    g_current_guest_thread = gt;
    g_current_guest_tls_context = tls_context;
    using GuestFunc = std::uint64_t(PS5_GUEST_ABI*)(std::uint64_t);
    auto* func = reinterpret_cast<GuestFunc>(gt->guest_func);
    gt->exit_code = func(gt->guest_arg);
    g_current_guest_thread = nullptr;
    g_current_guest_tls_context = nullptr;
    gt->finished.store(true, std::memory_order_release);
    if (g_guest_tls_leave != nullptr) {
        g_guest_tls_leave(tls_context);
    }
    g_tls_tcb = 0;
    trace_stderr("thread_exit id=%llu code=%llu\n",
        static_cast<unsigned long long>(GetCurrentThreadId()),
        static_cast<unsigned long long>(gt->exit_code));
    return 0;
}

std::uint64_t ps5rt_pthread_create_real(
    std::uint64_t thread_ptr, std::uint64_t attr,
    std::uint64_t func, std::uint64_t arg) {
    PS5_HLE_GUARD();
    auto id = g_next_thread_id.fetch_add(1, std::memory_order_relaxed);
    auto* gt = new GuestThread{};
    gt->guest_id = id;
    gt->guest_func = func;
    gt->guest_arg = arg;
    auto handle = CreateThread(
        nullptr,
        kGuestThreadStackCommit,
        guest_thread_entry,
        gt,
        0,
        nullptr);
    if (handle == nullptr) {
        delete gt;
        restore_guest_fs();
        return 0x8002000C;
    }
    gt->host_handle = handle;
    if (thread_ptr != 0) {
        *reinterpret_cast<std::uint64_t*>(thread_ptr) = id;
    }
    AcquireSRWLockExclusive(&g_thread_lock);
    g_threads[id] = gt;
    ReleaseSRWLockExclusive(&g_thread_lock);
    trace_stderr("hle=scePthreadCreate id=%llu func=0x%llX\n",
        static_cast<unsigned long long>(id),
        static_cast<unsigned long long>(func));
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_pthread_join_real(
    std::uint64_t thread_id, std::uint64_t retval_ptr) {
    PS5_HLE_GUARD();
    const WaitScope wait_scope(WaitKind::ThreadJoin, thread_id);
    GuestThread* gt = nullptr;
    AcquireSRWLockShared(&g_thread_lock);
    auto it = g_threads.find(thread_id);
    if (it != g_threads.end()) gt = it->second;
    ReleaseSRWLockShared(&g_thread_lock);
    if (!gt) { restore_guest_fs(); return 0x80020003; }
    WaitForSingleObject(gt->host_handle, INFINITE);
    if (retval_ptr) {
        *reinterpret_cast<std::uint64_t*>(retval_ptr) = gt->exit_code;
    }
    CloseHandle(gt->host_handle);
    AcquireSRWLockExclusive(&g_thread_lock);
    g_threads.erase(thread_id);
    ReleaseSRWLockExclusive(&g_thread_lock);
    delete gt;
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_pthread_exit_real(std::uint64_t value) {
    PS5_HLE_GUARD();
    trace_stderr("hle=scePthreadExit value=%llu\n", static_cast<unsigned long long>(value));
    ExitThread(static_cast<DWORD>(value));
    return 0;
}

std::uint64_t ps5rt_pthread_self_real() {
    PS5_HLE_GUARD();
    auto result = g_tls_tcb != 0 ? g_tls_tcb :
        static_cast<std::uint64_t>(GetCurrentThreadId());
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_pthread_setprio_real(
    std::uint64_t thread_id, std::int32_t prio) {
    PS5_HLE_GUARD();
    int win_prio = prio <= 100 ? THREAD_PRIORITY_HIGHEST :
                   prio <= 200 ? THREAD_PRIORITY_ABOVE_NORMAL :
                   prio <= 300 ? THREAD_PRIORITY_NORMAL :
                   THREAD_PRIORITY_BELOW_NORMAL;
    SetThreadPriority(GetCurrentThread(), win_prio);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_pthread_getprio_real(std::uint64_t) {
    PS5_HLE_GUARD();
    int p = GetThreadPriority(GetCurrentThread());
    std::int32_t result = p == THREAD_PRIORITY_HIGHEST ? 100 :
                          p == THREAD_PRIORITY_ABOVE_NORMAL ? 200 : 300;
    restore_guest_fs();
    return static_cast<std::uint64_t>(result);
}

// --- MUTEX ---

// Guest memory is this process's memory - the whole point of mapping the
// title at its own addresses - so reading a guest dword should be a load.
// It was a ReadProcessMemory call, which is a system call, and the callers
// that matter read and write one dword at a time: the AGC command-buffer
// helpers assemble a packet field by field. Sampling the opening frame found
// whole threads inside NtQueryVirtualMemory and NtReadVirtualMemory for that
// reason.
//
// The system call was doing one useful thing: returning false instead of
// faulting on an address the guest got wrong. That is kept, by asking
// VirtualQuery once per region and remembering the answer, and by falling
// back to the original call whenever the fast path is not certain. The cache
// is per thread, so checking it costs no atomic and no lock; the working set
static std::atomic<std::uint64_t> g_guest_protect_generation{0};

// The last protection changes and the ranges they touched, so a cached
// region older than the current generation is dropped only when one of
// the changes since overlaps it. Slot g % size holds change g; a reader
// that finds a slot rewritten under it, or more changes than the ring
// holds, falls back to dropping the entry.
constexpr std::uint64_t kProtectChangeRing = 256;
struct ProtectChange {
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> begin{0};
    std::atomic<std::uint64_t> end{0};
};
static ProtectChange g_protect_changes[kProtectChangeRing];
static SRWLOCK g_protect_change_lock = SRWLOCK_INIT;

extern "C" __declspec(dllexport) void ps5rt_guest_protect_changed(
    std::uint64_t begin, std::uint64_t end) {
    AcquireSRWLockExclusive(&g_protect_change_lock);
    const auto generation =
        g_guest_protect_generation.load(std::memory_order_relaxed) + 1;
    auto& slot = g_protect_changes[generation % kProtectChangeRing];
    slot.generation.store(0, std::memory_order_relaxed);
    slot.begin.store(begin, std::memory_order_relaxed);
    slot.end.store(end, std::memory_order_relaxed);
    slot.generation.store(generation, std::memory_order_release);
    g_guest_protect_generation.store(generation, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_protect_change_lock);
}

extern "C" __declspec(dllexport) void ps5rt_guest_protect_generation_bump() {
    ps5rt_guest_protect_changed(0, ~std::uint64_t{0});
}

// Whether [start, end) is untouched by every change after `since`, up to
// `current`.
static bool protect_unchanged_since(
    std::uint64_t start, std::uint64_t end,
    std::uint64_t since, std::uint64_t current) {
    static const bool strict = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_REGION_CACHE_STRICT");
        return value != nullptr && value[0] == '1';
    }();
    if (strict || current - since > kProtectChangeRing) {
        return false;
    }
    for (auto generation = since + 1; generation <= current; ++generation) {
        auto& slot = g_protect_changes[generation % kProtectChangeRing];
        if (slot.generation.load(std::memory_order_acquire) != generation) {
            return false;
        }
        const auto begin = slot.begin.load(std::memory_order_relaxed);
        const auto finish = slot.end.load(std::memory_order_relaxed);
        if (slot.generation.load(std::memory_order_acquire) != generation) {
            return false;
        }
        if (begin < end && start < finish) {
            return false;
        }
    }
    return true;
}

// is a handful of large allocations made once by the loader.
namespace {

constexpr int kGuestRegionCacheCount = 32;

struct GuestRegionEntry {
    std::uint64_t start;
    std::uint64_t end;
    bool writable;
    std::uint64_t generation;
};

// One thread-local for all of it. MinGW has no native TLS: every access
// to a thread_local is a call through __emutls_get_address into
// pthread_getspecific, and with the table and its counters as seven
// separate variables the lookup below made two such calls per entry it
// looked at. It was 43% of the title's main thread during the intro.
// Functions here take the reference once.
struct GuestRegionCache {
    GuestRegionEntry regions[kGuestRegionCacheCount] = {};
    int count = 0;
    int next = 0;
    // Whether the thirty-two entries are enough. A round-robin table
    // smaller than the working set misses every time and the system call
    // it was built to avoid comes back, so the number worth knowing is not
    // how often the cache is asked but how often it answers.
    std::uint64_t asked = 0;
    std::uint64_t hit = 0;
    std::uint64_t queried = 0;
    std::uint64_t evicted = 0;
    std::uint64_t writable_asked = 0;
    // The entry that answered last, asked first: the title's calls come in
    // runs against the same region.
    int last_hit = 0;
};

// Through the Windows TLS slot rather than thread_local: even one
// __emutls_get_address a call was a seventh of the main thread, at a
// hundred thousand calls a frame. TlsGetValue reads the thread block.
// Each thread's cache is allocated once and never freed; there are a few
// dozen threads.
GuestRegionCache& guest_region_cache() {
    static const DWORD slot = TlsAlloc();
    auto* cache = static_cast<GuestRegionCache*>(TlsGetValue(slot));
    if (cache == nullptr) {
        cache = new GuestRegionCache();
        TlsSetValue(slot, cache);
    }
    return *cache;
}

// What the AGC timeline costs the thread it is timing. Every traced packet
// is one unbuffered WriteFile to a redirected stderr, and the title traces
// eighty-five thousand of them in two minutes, so this wants to be a
// measured number rather than an assumed-small one.
thread_local std::uint64_t g_agc_trace_ticks = 0;
thread_local std::uint64_t g_agc_trace_count = 0;

// The AGC timeline goes out a line at a time through an unbuffered
// WriteFile, and it measured at 67.7 seconds of a 120 second run - 1.1ms
// for each of sixty-one thousand lines, on the thread that owes the frame.
// A hundred bytes should not cost a millisecond; a filter driver on the
// path evidently thinks otherwise, and the only cure that does not depend
// on the machine is to make far fewer calls.
//
// So each thread fills its own block and writes that. Lines stay whole and
// stay ordered within a thread, which is what reading the timeline needs;
// what is lost is ordering between threads, and every line already carries
// its own timestamp. The block is flushed on the flip as well, so the tail
// is on disk once a frame rather than whenever the buffer happens to fill,
// and a thread-local destructor catches whatever is left when a thread
// ends.
constexpr std::size_t kAgcTraceBufferBytes = 16384;
thread_local char g_agc_trace_buffer[kAgcTraceBufferBytes] = {};
thread_local std::size_t g_agc_trace_used = 0;

void agc_trace_flush() {
    if (g_agc_trace_used == 0) {
        return;
    }
    DWORD written = 0;
    WriteFile(
        GetStdHandle(STD_ERROR_HANDLE),
        g_agc_trace_buffer,
        static_cast<DWORD>(g_agc_trace_used),
        &written,
        nullptr);
    g_agc_trace_used = 0;
}

struct AgcTraceFlushOnExit {
    ~AgcTraceFlushOnExit() { agc_trace_flush(); }
};

thread_local AgcTraceFlushOnExit g_agc_trace_flush_on_exit;

// The GPU module arms and disarms texture pages read-only from its own
// threads. This cache had no way to hear about that, so a remembered
// "writable" could be a memcpy into a page that had since become
// read-only - an access violation inside ucrtbase with no guest frame to
// explain it. Dropping the cache for writes was correct and cost twenty
// times the frame rate, so instead the other module bumps this counter
// whenever it changes protection and every entry older than the current
// value is ignored.
bool guest_region_remembered_in(
    GuestRegionCache& cache,
    std::uint64_t start, std::uint64_t end, bool write_access) {
    const auto generation =
        g_guest_protect_generation.load(std::memory_order_acquire);
    const auto answers = [&](GuestRegionEntry& region) {
        if (start >= region.start && end <= region.end &&
            (!write_access || region.writable)) {
            // A read does not care what protection changed since: the only
            // changes that ring records are the GPU runtime's write watches,
            // which move pages between read-write and read-only, and both
            // read. The watches change protection hundreds of times a frame,
            // the ring of 256 overflowed, and every region was asked of
            // VirtualQuery again - an eighth of the main thread.
            if (!write_access || region.generation == generation) {
                return true;
            }
            if (protect_unchanged_since(
                    region.start, region.end, region.generation,
                    generation)) {
                region.generation = generation;
                return true;
            }
        }
        return false;
    };
    if (cache.last_hit < cache.count &&
        answers(cache.regions[cache.last_hit])) {
        return true;
    }
    for (int index = 0; index < cache.count; ++index) {
        if (answers(cache.regions[index])) {
            cache.last_hit = index;
            return true;
        }
    }
    return false;
}

bool guest_region_remembered(
    std::uint64_t start, std::uint64_t end, bool write_access) {
    return guest_region_remembered_in(
        guest_region_cache(), start, end, write_access);
}

void guest_region_remember(
    std::uint64_t start, std::uint64_t end, bool writable) {
    auto& cache = guest_region_cache();
    if (end <= start) {
        return;
    }
    const auto generation =
        g_guest_protect_generation.load(std::memory_order_acquire);
    for (int index = 0; index < cache.count; ++index) {
        if (cache.regions[index].start == start &&
            cache.regions[index].end == end) {
            cache.regions[index].writable = writable;
            cache.regions[index].generation = generation;
            return;
        }
    }
    if (cache.count < kGuestRegionCacheCount) {
        cache.regions[cache.count++] =
            {start, end, writable, generation};
        return;
    }
    ++cache.evicted;
    // Round-robin: a wrong eviction costs one system call, not a wrong answer.
    cache.regions[cache.next] =
        {start, end, writable, generation};
    cache.next =
        (cache.next + 1) % kGuestRegionCacheCount;
}

bool guest_range_direct(
    std::uint64_t address, std::size_t size, bool write_access) {
    auto& cache = guest_region_cache();
    if (address < 0x10000 || size == 0 ||
        address >
            std::numeric_limits<std::uint64_t>::max() - size) {
        return false;
    }

    const auto end = address + size;
    ++cache.asked;
    if (guest_region_remembered_in(cache, address, end, write_access)) {
        ++cache.hit;
        return true;
    }

    auto current = address;
    while (current < end) {
        MEMORY_BASIC_INFORMATION memory = {};
        ++cache.queried;
        if (VirtualQuery(
                reinterpret_cast<const void*>(current),
                &memory,
                sizeof(memory)) != sizeof(memory)) {
            return false;
        }
        if (memory.State == MEM_RESERVE &&
            ps5rt_guest_reservation_contains(current)) {
            const auto reserved_end =
                reinterpret_cast<std::uint64_t>(memory.BaseAddress) +
                memory.RegionSize;
            const auto span =
                (reserved_end < end ? reserved_end : end) - current;
            if (!ps5rt_guest_commit_range(current, span) ||
                VirtualQuery(
                    reinterpret_cast<const void*>(current),
                    &memory,
                    sizeof(memory)) != sizeof(memory)) {
                return false;
            }
        }
        if (memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }

        const auto region_start =
            static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(memory.BaseAddress));
        const auto region_end =
            region_start + static_cast<std::uint64_t>(memory.RegionSize);
        if (region_end <= current) {
            return false;
        }

        const DWORD protection = memory.Protect & 0xFFU;
        const bool writable =
            protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE ||
            protection == PAGE_EXECUTE_WRITECOPY;
        guest_region_remember(region_start, region_end, writable);
        if (write_access && !writable) {
            return false;
        }
        current = region_end < end ? region_end : end;
    }
    return true;
}

void agc_trace(const char* format, ...);
bool agc_packet_trace_enabled();

// trace_stderr is a stub in this file and agc_trace is defined a long
// way below the file entry points, so nothing in the open or read path
// could report anything at all. Kept for whatever calls these: Astro
// Bot is not one of them. It imports sceKernelRead as Cg4srZ6TKbU and
// calls it once in a run, while the NIDs wired to these two, wuCroIGjt2g
// and AqBioC2vF3I, are not in its import table at all.
std::atomic<std::uint64_t> g_file_read_calls{0};
std::atomic<std::uint64_t> g_file_read_bytes{0};
std::atomic<std::uint64_t> g_file_read_failures{0};

// Whether the native translator produces the module the runtime runs,
// rather than only being measured against the shader. Off by default: the
// bridge's producer is what the title has been drawing with, and this is
// the thing being proved, not the thing being trusted.
bool native_shader_producer_enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("PS5RT_NATIVE_SHADER_PRODUCER");
        return value != nullptr &&
            (std::strcmp(value, "1") == 0 ||
             std::strcmp(value, "true") == 0 ||
             std::strcmp(value, "on") == 0 ||
             // The modes combine: "readonly,both" is store-free modules
             // with the bridge still called, which is how the two were
             // told apart.
             std::strstr(value, "trace") != nullptr ||
             std::strstr(value, "readonly") != nullptr ||
             std::strstr(value, "both") != nullptr ||
             std::strstr(value, "noimages") != nullptr ||
             std::strstr(value, "nobuffers") != nullptr ||
             std::strstr(value, "graphics") != nullptr);
    }();
    return enabled;
}

// Translate, but let the bridge's module be the one that runs. This is
// what separates a fault caused by producing the module from one caused by
// using it - two failures that look identical from outside.
// Emit everything but the stores, so a module runs against the title
// without being able to write to it.
// Call the bridge as well, throw its result away, and still run the
// native module. The bridge is not only a producer - if it also sets
// something up that the runtime later depends on, then skipping it is the
// difference, not the module, and these two modes tell which.
bool native_shader_producer_also_calls_bridge() {
    static const bool both = [] {
        const auto* value = std::getenv("PS5RT_NATIVE_SHADER_PRODUCER");
        return value != nullptr && std::strstr(value, "both") != nullptr;
    }();
    return both;
}

// The graphics stages, which are separate from compute in every way
// that matters here: a different entry point, different declarations, and
// a runtime path that builds two stages into one pipeline. Opt-in on top
// of the producer flag.
// An upper bound on how much control flow a native module may have, so
// that a hang can be bisected by shape instead of guessed at. Zero means
// no bound.
std::uint32_t native_shader_block_limit() {
    static const std::uint32_t limit = [] {
        const auto* value =
            std::getenv("PS5RT_NATIVE_SHADER_MAX_BLOCKS");
        if (value == nullptr || value[0] == ' ') {
            return 0u;
        }
        char* end = nullptr;
        const auto parsed = std::strtoul(value, &end, 0);
        return end == value ? 0u : static_cast<std::uint32_t>(parsed);
    }();
    return limit;
}

bool native_graphics_producer_enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("PS5RT_NATIVE_SHADER_PRODUCER");
        return value != nullptr &&
            std::strstr(value, "graphics") != nullptr;
    }();
    return enabled;
}

bool native_shader_producer_omits_images() {
    static const bool omit = [] {
        const auto* value = std::getenv("PS5RT_NATIVE_SHADER_PRODUCER");
        return value != nullptr &&
            std::strstr(value, "noimages") != nullptr;
    }();
    return omit;
}

bool native_shader_producer_omits_buffers() {
    static const bool omit = [] {
        const auto* value = std::getenv("PS5RT_NATIVE_SHADER_PRODUCER");
        return value != nullptr &&
            std::strstr(value, "nobuffers") != nullptr;
    }();
    return omit;
}

bool native_shader_producer_omits_stores() {
    static const bool omit = [] {
        const auto* value = std::getenv("PS5RT_NATIVE_SHADER_PRODUCER");
        return value != nullptr &&
            std::strstr(value, "readonly") != nullptr;
    }();
    return omit;
}

bool native_shader_producer_traces_only() {
    static const bool trace_only = [] {
        const auto* value = std::getenv("PS5RT_NATIVE_SHADER_PRODUCER");
        return value != nullptr && std::strstr(value, "trace") != nullptr;
    }();
    return trace_only;
}

bool analyse_shader_cfg_enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("PS5RT_ANALYSE_SHADER_CFG");
        return value != nullptr &&
            (std::strcmp(value, "1") == 0 ||
             std::strcmp(value, "true") == 0 ||
             std::strcmp(value, "on") == 0);
    }();
    return enabled;
}

bool io_trace_enabled() {
    static const auto enabled = [] {
        const auto* value = std::getenv("PS5RT_TRACE_IO");
        return value != nullptr && value[0] != '0';
    }();
    return enabled;
}

}  // namespace

static bool try_read_process_bytes(
    std::uint64_t address,
    void* destination,
    std::size_t size) {
    if (address < 0x10000 || destination == nullptr || size == 0) {
        return false;
    }
    if (guest_range_direct(address, size, false)) {
        std::memcpy(
            destination, reinterpret_cast<const void*>(address), size);
        return true;
    }
    SIZE_T bytes_read = 0;
    return ReadProcessMemory(
               GetCurrentProcess(),
               reinterpret_cast<const void*>(address),
               destination,
               size,
               &bytes_read) &&
        bytes_read == size;
}

extern "C" void ps5rt_native_gpu_guest_written(
    std::uint64_t address, std::uint64_t size);

static bool try_write_process_bytes(
    std::uint64_t address,
    const void* source,
    std::size_t size) {
    if (address < 0x10000 || source == nullptr || size == 0) {
        return false;
    }
    if (guest_range_direct(address, size, true)) {
        std::memcpy(reinterpret_cast<void*>(address), source, size);
        return true;
    }
    // WriteProcessMemory can write a page the guest left read-only, which is
    // how code patching works; the direct path deliberately refuses those.
    // It bypasses the GPU runtime's write watch, so tell the runtime first:
    // otherwise a texture written this way keeps its stale upload.
    ps5rt_native_gpu_guest_written(address, size);
    SIZE_T bytes_written = 0;
    return WriteProcessMemory(
               GetCurrentProcess(),
               reinterpret_cast<void*>(address),
               source,
               size,
               &bytes_written) &&
        bytes_written == size;
}

// Whether the guest may be written at this address. This asks the kernel
// every time, which looks like the duplicate of the cached reader beside
// it - same test, committed and not guarded and one of the four writable
// protections - and routing it through that cache did compile and did
// measure the same. It also stalled the title on two runs out of three,
// where the same build without it stalled on none.
//
// The cache holds thirty-one regions against a table of thirty-two, so a
// new caller pushes it into eviction, and a remembered region that the
// guest has since released answers readable when it is not. The reader
// already carries that risk; the counter below says this path is asked
// 5763 times in two minutes, so there is nothing here worth widening it
// for.
static bool is_writable_process_range(
    std::uint64_t address,
    std::size_t size) {
    auto& cache = guest_region_cache();
    ++cache.writable_asked;
    if (address < 0x10000 || size == 0 ||
        address > std::numeric_limits<std::uint64_t>::max() - size) {
        return false;
    }

    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(
            reinterpret_cast<const void*>(address),
            &memory,
            sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const DWORD protection = memory.Protect & 0xFFU;
    if (protection != PAGE_READWRITE &&
        protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE &&
        protection != PAGE_EXECUTE_WRITECOPY) {
        return false;
    }

    const auto region_start =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    if (memory.RegionSize >
        std::numeric_limits<std::uintptr_t>::max() - region_start) {
        return false;
    }
    const auto region_end = region_start + memory.RegionSize;
    return address >= region_start && address + size <= region_end;
}

std::uint64_t ps5rt_pthread_mutex_init_real(
    std::uint64_t mutex_ptr, std::uint64_t attr_ptr) {
    PS5_HLE_GUARD();
    if (mutex_ptr == 0) {
        restore_guest_fs();
        return 0x80020016;
    }
    auto type = std::uint32_t{1};
    if (attr_ptr) {
        std::uint64_t attr_handle = 0;
        std::uint32_t attr_type = 0;
        if (try_read_process_bytes(
                attr_ptr,
                &attr_handle,
                sizeof(attr_handle)) &&
            attr_handle != 0 &&
            try_read_process_bytes(
                attr_handle,
                &attr_type,
                sizeof(attr_type)) &&
            attr_type >= 1 &&
            attr_type <= 4) {
            type = attr_type;
        }
    }
    auto* object = allocate_guest_mutex_object(type);
    if (object == nullptr) {
        restore_guest_fs();
        return 0x8002000C;
    }
    *reinterpret_cast<std::uint64_t*>(mutex_ptr) =
        reinterpret_cast<std::uint64_t>(object);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_pthread_mutex_destroy_real(std::uint64_t mutex_ptr) {
    PS5_HLE_GUARD();
    if (mutex_ptr) {
        const auto handle = static_cast<std::uint64_t>(
            InterlockedExchange64(
                reinterpret_cast<volatile LONG64*>(mutex_ptr), 0));
        if (handle > 1) {
            VirtualFree(reinterpret_cast<void*>(handle), 0, MEM_RELEASE);
        }
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_pthread_mutex_lock_real(std::uint64_t mutex_ptr) {
    PS5_HLE_GUARD();
    const WaitScope wait_scope(WaitKind::MutexLock, mutex_ptr);
    auto* object = resolve_guest_mutex_object(mutex_ptr);
    auto* host = resolve_guest_mutex_host_state(object);
    if (host == nullptr) {
        trace_sync_error(
            "sync_error op=mutex_lock mutex=0x%016llX result=0x%08X "
            "reason=resolve\n",
            static_cast<unsigned long long>(mutex_ptr),
            mutex_ptr == 0 ? 0x80020016U : 0x8002000CU);
        restore_guest_fs();
        return mutex_ptr == 0 ? 0x80020016 : 0x8002000C;
    }
    {
            const auto type = guest_mutex_type(object);
            const auto current_thread_id = GetCurrentThreadId();
            if (host->owner_thread_id == current_thread_id) {
                if (type == 2 || type == 3) {
                    host->lock_count++;
                    trace_stderr(
                        "hle=scePthreadMutexLock recursive mutex=0x%016llX "
                        "type=%u count=%u\n",
                        static_cast<unsigned long long>(mutex_ptr),
                        type,
                        host->lock_count);
                    restore_guest_fs();
                    return 0;
                }
                if (type == 4) {
                    restore_guest_fs();
                    return 0;
                }
                restore_guest_fs();
                trace_sync_error(
                    "sync_error op=mutex_lock mutex=0x%016llX "
                    "type=%u result=0x8002000B reason=self_lock\n",
                    static_cast<unsigned long long>(mutex_ptr),
                    type);
                return 0x8002000B;
            }
            AcquireSRWLockExclusive(&host->srw);
            host->owner_thread_id = current_thread_id;
            host->lock_count = 1;
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_pthread_mutex_unlock_real(std::uint64_t mutex_ptr) {
    PS5_HLE_GUARD();
    auto* object = resolve_guest_mutex_object(mutex_ptr);
    auto* host = resolve_guest_mutex_host_state(object);
    if (host == nullptr) {
        trace_sync_error(
            "sync_error op=mutex_unlock mutex=0x%016llX result=0x%08X "
            "reason=resolve\n",
            static_cast<unsigned long long>(mutex_ptr),
            mutex_ptr == 0 ? 0x80020016U : 0x8002000CU);
        restore_guest_fs();
        return mutex_ptr == 0 ? 0x80020016 : 0x8002000C;
    }
    {
            if (host->owner_thread_id != GetCurrentThreadId() ||
                host->lock_count == 0) {
                trace_sync_error(
                    "sync_error op=mutex_unlock mutex=0x%016llX "
                    "owner=%u current=%u count=%u result=0x80020001\n",
                    static_cast<unsigned long long>(mutex_ptr),
                    host->owner_thread_id,
                    GetCurrentThreadId(),
                    host->lock_count);
                restore_guest_fs();
                return 0x80020001;
            }
            if (host->lock_count > 1) {
                host->lock_count--;
                restore_guest_fs();
                return 0;
            }
            host->owner_thread_id = 0;
            host->lock_count = 0;
            ReleaseSRWLockExclusive(&host->srw);
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_pthread_mutex_trylock_real(std::uint64_t mutex_ptr) {
    PS5_HLE_GUARD();
    auto* object = resolve_guest_mutex_object(mutex_ptr);
    auto* host = resolve_guest_mutex_host_state(object);
    if (host == nullptr) {
        trace_sync_error(
            "sync_error op=mutex_trylock mutex=0x%016llX result=0x%08X "
            "reason=resolve\n",
            static_cast<unsigned long long>(mutex_ptr),
            mutex_ptr == 0 ? 0x80020016U : 0x8002000CU);
        restore_guest_fs();
        return mutex_ptr == 0 ? 0x80020016 : 0x8002000C;
    }
    {
            const auto type = guest_mutex_type(object);
            const auto current_thread_id = GetCurrentThreadId();
            if (type == 2 &&
                host->owner_thread_id == current_thread_id) {
                host->lock_count++;
                restore_guest_fs();
                return 0;
            }
            if (host->owner_thread_id == current_thread_id) {
                restore_guest_fs();
                return 0x80020010;
            }
            if (TryAcquireSRWLockExclusive(&host->srw)) {
                host->owner_thread_id = current_thread_id;
                host->lock_count = 1;
                restore_guest_fs();
                return 0;
            }
            restore_guest_fs();
            return 0x80020010;
    }
    restore_guest_fs();
    return 0;
}

// --- CONDITION VARIABLES ---

std::uint64_t ps5rt_pthread_cond_init_real(
    std::uint64_t cond_ptr, std::uint64_t) {
    PS5_HLE_GUARD();
    if (cond_ptr == 0) {
        restore_guest_fs();
        return 0x80020016;
    }
    auto* cv = allocate_guest_cond_var();
    if (cv == nullptr) {
        restore_guest_fs();
        return 0x8002000C;
    }
    *reinterpret_cast<std::uint64_t*>(cond_ptr) =
        reinterpret_cast<std::uint64_t>(cv);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_pthread_cond_destroy_real(std::uint64_t cond_ptr) {
    PS5_HLE_GUARD();
    if (cond_ptr) {
        const auto handle = static_cast<std::uint64_t>(
            InterlockedExchange64(
                reinterpret_cast<volatile LONG64*>(cond_ptr), 0));
        if (handle > 1) {
            delete reinterpret_cast<GuestCondVar*>(handle);
        }
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_pthread_cond_wait_real(
    std::uint64_t cond_ptr, std::uint64_t mutex_ptr) {
    PS5_HLE_GUARD();
    const WaitScope wait_scope(WaitKind::CondWait, cond_ptr);
    if (cond_ptr == 0 || mutex_ptr == 0) {
        trace_sync_error(
            "sync_error op=cond_wait cond=0x%016llX mutex=0x%016llX "
            "result=0x80020016 reason=null\n",
            static_cast<unsigned long long>(cond_ptr),
            static_cast<unsigned long long>(mutex_ptr));
        restore_guest_fs();
        return 0x80020016;
    }

    auto* cv = resolve_guest_cond_var(cond_ptr);
    auto* object = resolve_guest_mutex_object(mutex_ptr);
    auto* host = resolve_guest_mutex_host_state(object);
    if (cv == nullptr || host == nullptr) {
        trace_sync_error(
            "sync_error op=cond_wait cond=0x%016llX mutex=0x%016llX "
            "result=0x8002000C reason=resolve\n",
            static_cast<unsigned long long>(cond_ptr),
            static_cast<unsigned long long>(mutex_ptr));
        restore_guest_fs();
        return 0x8002000C;
    }

    const auto current_thread_id = GetCurrentThreadId();
    if (host->owner_thread_id != current_thread_id) {
        trace_sync_error(
            "sync_error op=cond_wait cond=0x%016llX mutex=0x%016llX "
            "owner=%u current=%u count=%u result=0x80020001\n",
            static_cast<unsigned long long>(cond_ptr),
            static_cast<unsigned long long>(mutex_ptr),
            host->owner_thread_id,
            current_thread_id,
            host->lock_count);
        restore_guest_fs();
        return 0x80020001;
    }
    if (host->lock_count != 1) {
        trace_sync_error(
            "sync_error op=cond_wait cond=0x%016llX mutex=0x%016llX "
            "owner=%u current=%u count=%u result=0x80020016\n",
            static_cast<unsigned long long>(cond_ptr),
            static_cast<unsigned long long>(mutex_ptr),
            host->owner_thread_id,
            current_thread_id,
            host->lock_count);
        restore_guest_fs();
        return 0x80020016;
    }

    host->owner_thread_id = 0;
    host->lock_count = 0;
    const auto trace_ms = cond_wait_trace_ms();
    const auto wait_started = trace_ms != 0 ? GetTickCount64() : 0;
    const auto waited = SleepConditionVariableSRW(
        &cv->cv, &host->srw, INFINITE, 0);
    if (trace_ms != 0) {
        const auto elapsed = GetTickCount64() - wait_started;
        if (elapsed >= trace_ms) {
            agc_trace(
                "cond_wait_long cond=0x%016llX waiter=%lu ms=%llu "
                "woken_by=%ld from=0x%016llX\n",
                static_cast<unsigned long long>(cond_ptr),
                GetCurrentThreadId(),
                static_cast<unsigned long long>(elapsed),
                static_cast<long>(cv->last_signaler),
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uint64_t>(
                        __builtin_return_address(0))));
        }
    }
    host->owner_thread_id = current_thread_id;
    host->lock_count = 1;
    if (!waited) {
        trace_sync_error(
            "sync_error op=cond_wait cond=0x%016llX mutex=0x%016llX "
            "win32=%lu result=0x80020001 reason=wait_failed\n",
            static_cast<unsigned long long>(cond_ptr),
            static_cast<unsigned long long>(mutex_ptr),
            GetLastError());
    }
    restore_guest_fs();
    return waited ? 0 : 0x80020001;
}

std::uint64_t ps5rt_pthread_cond_signal_real(std::uint64_t cond_ptr) {
    PS5_HLE_GUARD();
    auto* cv = resolve_guest_cond_var(cond_ptr);
    if (cv == nullptr) {
        restore_guest_fs();
        return cond_ptr == 0 ? 0x80020016 : 0x8002000C;
    }
    cv->last_signaler = static_cast<LONG>(GetCurrentThreadId());
    WakeConditionVariable(&cv->cv);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_pthread_cond_broadcast_real(std::uint64_t cond_ptr) {
    PS5_HLE_GUARD();
    auto* cv = resolve_guest_cond_var(cond_ptr);
    if (cv == nullptr) {
        restore_guest_fs();
        return cond_ptr == 0 ? 0x80020016 : 0x8002000C;
    }
    cv->last_signaler = static_cast<LONG>(GetCurrentThreadId());
    WakeAllConditionVariable(&cv->cv);
    restore_guest_fs();
    return 0;
}

// --- SEMAPHORES ---

std::uint64_t ps5rt_kernel_create_sema_real(
    std::uint64_t semaphore_address, std::uint64_t name_address,
    std::uint32_t attr, std::int32_t initial_count,
    std::int32_t max_count, std::uint64_t option) {
    PS5_HLE_GUARD();
    (void)name_address;
    (void)attr;
    (void)option;
    if (semaphore_address == 0 ||
        initial_count < 0 ||
        max_count <= 0 ||
        initial_count > max_count) {
        restore_guest_fs();
        return 0x80020016;
    }

    auto id = g_next_handle.fetch_add(1, std::memory_order_relaxed);
    auto* s = new GuestSema{};
    s->handle = CreateSemaphoreA(
        nullptr,
        initial_count,
        max_count,
        nullptr);
    if (s->handle == nullptr) {
        delete s;
        restore_guest_fs();
        return 0x8002000C;
    }
    object_insert(g_semas, id, s);
    {
        static std::atomic<std::uint64_t> created{0};
        const auto count = created.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 64) {
            agc_trace(
                "sema.created id=0x%016llX init=%d max=%d count=%llu\n",
                static_cast<unsigned long long>(id),
                initial_count,
                max_count,
                static_cast<unsigned long long>(count));
        }
    }
    *reinterpret_cast<std::uint32_t*>(semaphore_address) =
        static_cast<std::uint32_t>(id);
    trace_stderr("hle=sceKernelCreateSema id=%llu init=%d max=%d\n",
        static_cast<unsigned long long>(id), initial_count, max_count);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_kernel_wait_sema_real(
    std::uint64_t sema_id_raw, std::int32_t need_count,
    std::uint64_t timeout_address) {
    PS5_HLE_GUARD();
    const std::uint64_t sema_id =
        static_cast<std::uint32_t>(sema_id_raw);
    {
        static std::atomic<std::uint64_t> calls{0};
        const auto count = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 8 || (count % 8192) == 0) {
            agc_trace(
                "sema.wait raw=0x%016llX id=0x%08llX count=%llu\n",
                static_cast<unsigned long long>(sema_id_raw),
                static_cast<unsigned long long>(sema_id),
                static_cast<unsigned long long>(count));
        }
    }
    const WaitScope wait_scope(WaitKind::WaitSema, sema_id);
    auto* sema = object_lookup(g_semas, sema_id);
    if (sema == nullptr) {
        static std::atomic<std::uint64_t> missing{0};
        const auto count = missing.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 32 || (count % 4096) == 0) {
            agc_trace(
                "sema.wait_unknown id=0x%016llX count=%llu\n",
                static_cast<unsigned long long>(sema_id),
                static_cast<unsigned long long>(count));
        }
        restore_guest_fs();
        return 0x80020003;
    }
    if (need_count <= 0) {
        restore_guest_fs();
        return 0x80020016;
    }

    const auto timeout_usec = timeout_address == 0
        ? 0U
        : *reinterpret_cast<const std::uint32_t*>(timeout_address);
    const auto deadline = timeout_address == 0
        ? 0ULL
        : GetTickCount64() +
            (timeout_usec == 0
                ? 0ULL
                : std::max<std::uint64_t>(
                    1,
                    (static_cast<std::uint64_t>(timeout_usec) + 999) / 1000));

    std::int32_t acquired_count = 0;
    DWORD result = WAIT_OBJECT_0;
    while (acquired_count < need_count) {
        DWORD timeout_ms = INFINITE;
        if (timeout_address != 0) {
            const auto now = GetTickCount64();
            timeout_ms = now >= deadline
                ? 0
                : static_cast<DWORD>(std::min<std::uint64_t>(
                    deadline - now,
                    MAXDWORD - 1ULL));
        }
        result = WaitForSingleObject(sema->handle, timeout_ms);
        if (result != WAIT_OBJECT_0) {
            break;
        }
        ++acquired_count;
    }

    if (result != WAIT_OBJECT_0 && acquired_count != 0) {
        ReleaseSemaphore(
            sema->handle,
            acquired_count,
            nullptr);
    }
    if (result == WAIT_OBJECT_0 && timeout_address != 0) {
        *reinterpret_cast<std::uint32_t*>(timeout_address) = 0;
    }
    restore_guest_fs();
    if (result == WAIT_OBJECT_0) {
        return 0;
    }
    return result == WAIT_TIMEOUT ? 0x80020023 : 0x80020001;
}

std::uint64_t ps5rt_kernel_signal_sema_real(
    std::uint64_t sema_id_raw, std::int32_t count) {
    PS5_HLE_GUARD();
    const std::uint64_t sema_id =
        static_cast<std::uint32_t>(sema_id_raw);
    auto* sema = object_lookup(g_semas, sema_id);
    if (sema == nullptr) { restore_guest_fs(); return 0x80020003; }
    ReleaseSemaphore(sema->handle, count, nullptr);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_kernel_delete_sema_real(std::uint64_t sema_id_raw) {
    PS5_HLE_GUARD();
    const std::uint64_t sema_id =
        static_cast<std::uint32_t>(sema_id_raw);
    auto* sema = object_take(g_semas, sema_id);
    if (sema != nullptr) {
        CloseHandle(sema->handle);
        sema->handle = nullptr;
    }
    restore_guest_fs();
    return 0;
}

// --- POSIX SEMAPHORES ---

bool try_read_u32(std::uint64_t address, std::uint32_t& value);
//
// sem_init and the rest, and their scePthreadSem twins. They went through
// the managed bridge, which checks every guest word it reads with
// VirtualQuery: with the intro running, sem_post alone was a sixth of the
// title's main thread. A counter and a condition variable each, the id
// kept in the sem_t's first word the way the bridge kept it, and the same
// return codes it gave, so nothing the title sees changes but the cost.

struct PosixSemaphore {
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE changed = CONDITION_VARIABLE_INIT;
    std::int64_t count = 0;
    // Who posted last, for PS5RT_TRACE_COND_WAITS.
    DWORD last_poster = 0;
};

static SRWLOCK g_posix_sem_lock = SRWLOCK_INIT;
static std::unordered_map<std::uint32_t, PosixSemaphore*> g_posix_sems;

constexpr std::uint64_t kSemInvalid = 0x80020016;   // EINVAL
constexpr std::uint64_t kSemFault = 0x8002000E;     // EFAULT
constexpr std::uint64_t kSemBusy = 0x80020010;      // EBUSY
constexpr std::uint64_t kSemAgain = 0x80020023;     // EAGAIN
constexpr std::uint64_t kSemTimedOut = 0x8002003C;  // ETIMEDOUT

static PosixSemaphore* posix_sem_of(std::uint64_t sem_address,
                                    std::uint32_t* id_out = nullptr) {
    if (sem_address == 0) {
        return nullptr;
    }
    std::uint32_t id = 0;
    if (!try_read_u32(sem_address, id) || id == 0) {
        return nullptr;
    }
    if (id_out != nullptr) {
        *id_out = id;
    }
    AcquireSRWLockShared(&g_posix_sem_lock);
    const auto found = g_posix_sems.find(id);
    auto* semaphore = found == g_posix_sems.end() ? nullptr : found->second;
    ReleaseSRWLockShared(&g_posix_sem_lock);
    return semaphore;
}

// Waits for the count, until deadline_ms on GetTickCount64 (0: no limit,
// ~0: do not wait at all). Returns 0, kSemBusy or kSemTimedOut.
static std::uint64_t posix_sem_take(
    PosixSemaphore& semaphore, std::uint32_t id, std::uint64_t deadline_ms,
    std::uint64_t caller = 0) {
    const WaitScope wait_scope(WaitKind::WaitSema, id);
    const auto trace_ms = cond_wait_trace_ms();
    const auto wait_started = trace_ms != 0 ? GetTickCount64() : 0;
    struct LongWait {
        std::uint32_t threshold;
        std::uint64_t started;
        PosixSemaphore& semaphore;
        std::uint32_t id;
        std::uint64_t caller;
        ~LongWait() {
            if (threshold == 0) {
                return;
            }
            const auto elapsed = GetTickCount64() - started;
            if (elapsed >= threshold) {
                agc_trace(
                    "sem_wait_long sem=%u waiter=%lu ms=%llu posted_by=%lu "
                    "from=0x%016llX\n",
                    id, GetCurrentThreadId(),
                    static_cast<unsigned long long>(elapsed),
                    semaphore.last_poster,
                    static_cast<unsigned long long>(caller));
            }
        }
    } long_wait{trace_ms, wait_started, semaphore, id, caller};
    AcquireSRWLockExclusive(&semaphore.lock);
    while (semaphore.count <= 0) {
        if (deadline_ms == ~std::uint64_t{0}) {
            ReleaseSRWLockExclusive(&semaphore.lock);
            return kSemBusy;
        }
        DWORD wait_ms = INFINITE;
        if (deadline_ms != 0) {
            const auto now = GetTickCount64();
            if (now >= deadline_ms) {
                ReleaseSRWLockExclusive(&semaphore.lock);
                return kSemTimedOut;
            }
            wait_ms = static_cast<DWORD>(
                std::min<std::uint64_t>(deadline_ms - now, MAXDWORD - 1ULL));
        }
        SleepConditionVariableSRW(
            &semaphore.changed, &semaphore.lock, wait_ms, 0);
    }
    --semaphore.count;
    ReleaseSRWLockExclusive(&semaphore.lock);
    return 0;
}

std::uint64_t ps5rt_posix_sem_init_real(
    std::uint64_t sem_address, std::uint64_t pshared, std::uint64_t value) {
    PS5_HLE_GUARD();
    (void)pshared;
    if (sem_address == 0 ||
        value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int32_t>::max())) {
        restore_guest_fs();
        return kSemInvalid;
    }
    auto id = static_cast<std::uint32_t>(
        g_next_handle.fetch_add(1, std::memory_order_relaxed));
    if (id == 0) {
        id = static_cast<std::uint32_t>(
            g_next_handle.fetch_add(1, std::memory_order_relaxed));
    }
    auto* semaphore = new PosixSemaphore{};
    semaphore->count = static_cast<std::int64_t>(value);
    AcquireSRWLockExclusive(&g_posix_sem_lock);
    g_posix_sems[id] = semaphore;
    ReleaseSRWLockExclusive(&g_posix_sem_lock);
    if (!try_write_process_bytes(sem_address, &id, sizeof(id))) {
        AcquireSRWLockExclusive(&g_posix_sem_lock);
        g_posix_sems.erase(id);
        ReleaseSRWLockExclusive(&g_posix_sem_lock);
        delete semaphore;
        restore_guest_fs();
        return kSemFault;
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_pthread_sem_init_real(
    std::uint64_t sem_address, std::uint64_t flag, std::uint64_t value,
    std::uint64_t name) {
    (void)name;
    // Private semaphores only, as the bridge had it.
    if (flag != 0) {
        return kSemInvalid;
    }
    return ps5rt_posix_sem_init_real(sem_address, 0, value);
}

std::uint64_t ps5rt_posix_sem_wait_real(std::uint64_t sem_address) {
    PS5_HLE_GUARD();
    std::uint32_t id = 0;
    auto* semaphore = posix_sem_of(sem_address, &id);
    const auto result = semaphore == nullptr
        ? kSemInvalid
        : posix_sem_take(
              *semaphore, id, 0,
              reinterpret_cast<std::uint64_t>(__builtin_return_address(0)));
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_posix_sem_trywait_real(std::uint64_t sem_address) {
    PS5_HLE_GUARD();
    std::uint32_t id = 0;
    auto* semaphore = posix_sem_of(sem_address, &id);
    const auto result = semaphore == nullptr
        ? kSemInvalid
        : posix_sem_take(*semaphore, id, ~std::uint64_t{0});
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_pthread_sem_trywait_real(std::uint64_t sem_address) {
    const auto result = ps5rt_posix_sem_trywait_real(sem_address);
    return result == kSemBusy ? kSemAgain : result;
}

// The deadline is a struct timespec on the realtime clock.
std::uint64_t ps5rt_posix_sem_timedwait_real(
    std::uint64_t sem_address, std::uint64_t abstime_address) {
    PS5_HLE_GUARD();
    std::uint32_t id = 0;
    auto* semaphore = posix_sem_of(sem_address, &id);
    if (semaphore == nullptr || abstime_address == 0) {
        restore_guest_fs();
        return kSemInvalid;
    }
    std::uint64_t seconds = 0;
    std::uint64_t nanoseconds = 0;
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    if (!try_read_u32(abstime_address, low) ||
        !try_read_u32(abstime_address + 4, high)) {
        restore_guest_fs();
        return kSemFault;
    }
    seconds = (static_cast<std::uint64_t>(high) << 32) | low;
    if (!try_read_u32(abstime_address + 8, low) ||
        !try_read_u32(abstime_address + 12, high)) {
        restore_guest_fs();
        return kSemFault;
    }
    nanoseconds = (static_cast<std::uint64_t>(high) << 32) | low;
    FILETIME now_file = {};
    GetSystemTimeAsFileTime(&now_file);
    const auto now_100ns =
        (static_cast<std::uint64_t>(now_file.dwHighDateTime) << 32) |
        now_file.dwLowDateTime;
    constexpr std::uint64_t kUnixEpochIn100ns = 116444736000000000ULL;
    const auto now_ms = (now_100ns - kUnixEpochIn100ns) / 10000ULL;
    const auto deadline_ms = seconds * 1000ULL + nanoseconds / 1000000ULL;
    const auto remaining = deadline_ms > now_ms ? deadline_ms - now_ms : 0;
    // 0 would mean no limit to posix_sem_take; one millisecond is the least.
    const auto result = posix_sem_take(
        *semaphore, id, GetTickCount64() + std::max<std::uint64_t>(1, remaining));
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_posix_sem_post_real(std::uint64_t sem_address) {
    PS5_HLE_GUARD();
    auto* semaphore = posix_sem_of(sem_address);
    if (semaphore == nullptr) {
        restore_guest_fs();
        return kSemInvalid;
    }
    AcquireSRWLockExclusive(&semaphore->lock);
    ++semaphore->count;
    semaphore->last_poster = GetCurrentThreadId();
    ReleaseSRWLockExclusive(&semaphore->lock);
    WakeConditionVariable(&semaphore->changed);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_posix_sem_getvalue_real(
    std::uint64_t sem_address, std::uint64_t value_address) {
    PS5_HLE_GUARD();
    auto* semaphore = posix_sem_of(sem_address);
    if (semaphore == nullptr || value_address == 0) {
        restore_guest_fs();
        return kSemInvalid;
    }
    AcquireSRWLockShared(&semaphore->lock);
    const auto count = static_cast<std::uint32_t>(
        static_cast<std::int32_t>(semaphore->count));
    ReleaseSRWLockShared(&semaphore->lock);
    const auto written =
        try_write_process_bytes(value_address, &count, sizeof(count));
    restore_guest_fs();
    return written ? 0 : kSemFault;
}

std::uint64_t ps5rt_posix_sem_destroy_real(std::uint64_t sem_address) {
    PS5_HLE_GUARD();
    std::uint32_t id = 0;
    auto* semaphore = posix_sem_of(sem_address, &id);
    if (semaphore == nullptr) {
        restore_guest_fs();
        return kSemInvalid;
    }
    AcquireSRWLockExclusive(&g_posix_sem_lock);
    g_posix_sems.erase(id);
    ReleaseSRWLockExclusive(&g_posix_sem_lock);
    // Left allocated: a thread still inside a wait holds a pointer to it.
    const std::uint32_t zero = 0;
    try_write_process_bytes(sem_address, &zero, sizeof(zero));
    restore_guest_fs();
    return 0;
}

// --- EVENT FLAGS ---

std::uint64_t ps5rt_kernel_create_event_flag_real(
    std::uint64_t name, std::uint32_t attr, std::uint64_t init_pattern) {
    PS5_HLE_GUARD();
    auto id = g_next_handle.fetch_add(1, std::memory_order_relaxed);
    auto* e = new GuestEventFlag{};
    e->handle = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    object_insert(g_events, id, e);
    if (name) *reinterpret_cast<std::uint64_t*>(name) = id;
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_kernel_delete_event_flag_real(std::uint64_t id) {
    PS5_HLE_GUARD();
    auto* flag = object_take(g_events, id);
    if (flag != nullptr) {
        CloseHandle(flag->handle);
        flag->handle = nullptr;
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_kernel_set_event_flag_real(
    std::uint64_t id, std::uint64_t pattern) {
    PS5_HLE_GUARD();
    auto* flag = object_lookup(g_events, id);
    if (flag != nullptr && flag->handle != nullptr) {
        SetEvent(flag->handle);
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_kernel_wait_event_flag_real(
    std::uint64_t id, std::uint64_t bits, std::uint32_t mode,
    std::uint64_t* out, std::uint32_t timeout_ms) {
    PS5_HLE_GUARD();
    const WaitScope wait_scope(WaitKind::WaitEventFlag, id);
    auto* flag = object_lookup(g_events, id);
    if (flag == nullptr || flag->handle == nullptr) {
        restore_guest_fs();
        return 0x80020003;
    }
    DWORD t = timeout_ms == 0 ? INFINITE : timeout_ms;
    DWORD result = WaitForSingleObject(flag->handle, t);
    if (out) *out = bits;
    restore_guest_fs();
    return result == WAIT_TIMEOUT ? 0x80020023 : 0;
}

// --- FILE I/O ---

std::uint64_t ps5rt_kernel_check_reachability_real(
    std::uint64_t path_address) {
    PS5_HLE_GUARD();
    if (path_address == 0) {
        return 0x80020016;
    }
    std::string guest_path;
    if (!read_process_c_string(path_address, 256, guest_path)) {
        return 0x8002003F;
    }
    if (guest_path == "/dev/random" ||
        guest_path == "/dev/urandom" ||
        guest_path == "/dev/null") {
        return 0;
    }
    const auto mapped = map_ps5_path(guest_path.c_str());
    if (!mapped.empty() &&
        GetFileAttributesA(mapped.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    return 0x80020002;
}

std::uint64_t ps5rt_kernel_open_real(
    std::uint64_t path, std::int32_t flags, std::int32_t mode) {
    PS5_HLE_GUARD();
    const char* p = reinterpret_cast<const char*>(path);
    if (!p) { restore_guest_fs(); return 0x80020016; }
    auto mapped = map_ps5_path(p);
    DWORD access = GENERIC_READ;
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    DWORD creation = OPEN_EXISTING;
    DWORD flags_attr = FILE_ATTRIBUTE_NORMAL;
    if ((flags & 0x0003) == 0) access = GENERIC_READ;
    else if ((flags & 0x0003) == 1) access = GENERIC_WRITE;
    else if ((flags & 0x0003) == 2) access = GENERIC_READ | GENERIC_WRITE;
    if ((flags & 0x0200) != 0) creation = CREATE_ALWAYS;
    else if ((flags & 0x0400) != 0) creation = CREATE_NEW;
    else if ((flags & 0x0800) != 0) { access |= DELETE; }
    HANDLE h = CreateFileA(mapped.c_str(), access, share, nullptr, creation, flags_attr, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        agc_trace(
            "io.open_failed path=%s host=%s flags=%d err=%lu\n",
            p,
            mapped.c_str(),
            flags,
            GetLastError());
        restore_guest_fs();
        return 0x80020002;
    }
    auto id = g_next_handle.fetch_add(1, std::memory_order_relaxed);
    g_open_files[id] = mapped;
    // Store HANDLE directly via equeue map (reusing map)
    object_insert(g_equeues, id, reinterpret_cast<GuestEqueue*>(h));
    agc_trace(
        "io.open id=%llu path=%s host=%s\n",
        static_cast<unsigned long long>(id),
        p,
        mapped.c_str());
    restore_guest_fs();
    return id;
}

std::uint64_t ps5rt_kernel_close_real(std::uint64_t fd) {
    PS5_HLE_GUARD();
    auto* file = object_take(g_equeues, fd);
    if (file != nullptr) {
        CloseHandle(reinterpret_cast<HANDLE>(file));
    }
    restore_guest_fs();
    return 0;
}

// Defined with the runner: tells the GPU runtime a guest range is about to
// change, which also lifts any read-only watch it put there.
extern "C" void ps5rt_native_gpu_guest_written(
    std::uint64_t address, std::uint64_t size);

std::uint64_t ps5rt_kernel_read_real(
    std::uint64_t fd, std::uint64_t buf, std::size_t len, std::size_t nread) {
    PS5_HLE_GUARD();
    auto* file = object_lookup(g_equeues, fd);
    if (file == nullptr) { restore_guest_fs(); return 0x80020002; }
    // The kernel writes the file straight into guest memory and does not
    // fault into anyone's handler on the way: a page the GPU runtime has
    // made read-only to watch a texture just fails the read. Say so first.
    ps5rt_native_gpu_guest_written(buf, len);
    DWORD read_bytes = 0;
    const auto succeeded = ReadFile(
        reinterpret_cast<HANDLE>(file),
        reinterpret_cast<void*>(buf),
        static_cast<DWORD>(len),
        &read_bytes,
        nullptr) != FALSE;
    const auto error = succeeded ? 0UL : GetLastError();
    g_file_read_calls.fetch_add(1, std::memory_order_relaxed);
    g_file_read_bytes.fetch_add(read_bytes, std::memory_order_relaxed);
    if (!succeeded) {
        g_file_read_failures.fetch_add(1, std::memory_order_relaxed);
    }
    if (io_trace_enabled()) {
        agc_trace(
            "io.read fd=%llu dst=0x%016llX len=%llu got=%lu err=%lu "
            "calls=%llu total=%llu failed=%llu\n",
            static_cast<unsigned long long>(fd),
            static_cast<unsigned long long>(buf),
            static_cast<unsigned long long>(len),
            static_cast<unsigned long>(read_bytes),
            static_cast<unsigned long>(error),
            static_cast<unsigned long long>(
                g_file_read_calls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_file_read_bytes.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_file_read_failures.load(std::memory_order_relaxed)));
    }
    if (nread) *reinterpret_cast<std::size_t*>(nread) = read_bytes;
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_kernel_write_real(
    std::uint64_t fd, std::uint64_t buf, std::size_t len) {
    PS5_HLE_GUARD();
    if (fd == 1 || fd == 2) {
        DWORD written = 0;
        auto handle = GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
        WriteFile(handle, reinterpret_cast<void*>(buf), static_cast<DWORD>(len), &written, nullptr);
        restore_guest_fs();
        return written;
    }
    auto* file = object_lookup(g_equeues, fd);
    if (file == nullptr) { restore_guest_fs(); return 0x80020002; }
    DWORD written = 0;
    WriteFile(reinterpret_cast<HANDLE>(file),
        reinterpret_cast<void*>(buf), static_cast<DWORD>(len), &written, nullptr);
    restore_guest_fs();
    return written;
}

std::uint64_t ps5rt_kernel_lseek_real(
    std::uint64_t fd, std::int64_t offset, std::int32_t whence) {
    PS5_HLE_GUARD();
    auto* file = object_lookup(g_equeues, fd);
    if (file == nullptr) { restore_guest_fs(); return 0x80020002; }
    DWORD method = whence == 0 ? FILE_BEGIN : whence == 1 ? FILE_CURRENT : FILE_END;
    LARGE_INTEGER li; li.QuadPart = offset;
    SetFilePointerEx(reinterpret_cast<HANDLE>(file), li, &li, method);
    restore_guest_fs();
    return static_cast<std::uint64_t>(li.QuadPart);
}

} // extern "C"
// --- VideoOut ---

namespace {

constexpr std::size_t kVideoOutShadowMaximumPorts = 4;
constexpr std::size_t kVideoOutShadowMaximumGroups = 4;
constexpr std::size_t kVideoOutShadowMaximumBuffers = 16;
constexpr std::uint64_t kVideoOutErrorInvalidValue = 0x80290001ULL;
constexpr std::uint64_t kVideoOutErrorInvalidAddress = 0x80290002ULL;
constexpr std::uint64_t kVideoOutErrorResourceBusy = 0x80290009ULL;
constexpr std::uint64_t kVideoOutErrorInvalidIndex = 0x8029000AULL;
constexpr std::uint64_t kVideoOutErrorInvalidHandle = 0x8029000BULL;
constexpr std::uint64_t kVideoOutErrorInvalidEventQueue = 0x8029000CULL;
constexpr std::uint64_t kVideoOutErrorUnsupportedOutputMode = 0x80290016ULL;
constexpr std::uint64_t kVideoOutErrorInvalidOption = 0x8029001AULL;
constexpr std::uint64_t kVideoOutMemoryFault = 0x8002000DULL;
constexpr std::uint64_t kVideoOutOutputModeDefault = 1;
constexpr std::uint64_t kVideoOutOutputMode119Hz = 0xF;
constexpr std::uint64_t kVideoOutFormat2B10G10R10A2Srgb =
    0x8100000000000000ULL;

struct VideoOutShadowAttribute {
    std::uint64_t pixel_format = 0;
    std::uint32_t tiling_mode = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch_in_pixel = 0;
    std::uint64_t option = 0;
};

struct VideoOutShadowGroup {
    bool active = false;
    VideoOutShadowAttribute attribute;
};

struct VideoOutShadowBuffer {
    std::int32_t group_index = -1;
    std::uint64_t address = 0;
};

struct VideoOutShadowPort {
    bool active = false;
    std::uint32_t handle = 0;
    std::int32_t flip_rate = 0;
    std::int32_t current_buffer = -1;
    std::uint32_t output_width = 1920;
    std::uint32_t output_height = 1080;
    std::uint32_t refresh_rate = 60;
    float gamma = 1.0f;
    std::uint64_t vblank_count = 0;
    std::uint64_t flip_count = 0;
    std::int64_t opened_at = 0;
    std::int64_t last_vblank = 0;
    std::array<VideoOutShadowGroup, kVideoOutShadowMaximumGroups> groups;
    std::array<VideoOutShadowBuffer, kVideoOutShadowMaximumBuffers> buffers;
};

SRWLOCK g_videoout_shadow_lock = SRWLOCK_INIT;
std::array<VideoOutShadowPort, kVideoOutShadowMaximumPorts>
    g_videoout_shadow_ports;
SRWLOCK g_videoout_native_lock = SRWLOCK_INIT;
std::array<VideoOutShadowPort, kVideoOutShadowMaximumPorts>
    g_videoout_native_ports;
std::atomic<std::uint32_t> g_videoout_next_native_handle{1};

void videoout_shadow_trace(const char* format, ...) {
    char buffer[1024] = {};
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(
        buffer,
        sizeof(buffer),
        format,
        arguments);
    va_end(arguments);
    if (length <= 0) {
        return;
    }

    const auto byte_count = static_cast<DWORD>(
        length < static_cast<int>(sizeof(buffer))
            ? length
            : sizeof(buffer) - 1);
    DWORD written = 0;
    WriteFile(
        GetStdHandle(STD_ERROR_HANDLE),
        buffer,
        byte_count,
        &written,
        nullptr);
}

VideoOutShadowPort* find_videoout_shadow_port(std::uint32_t handle) {
    for (auto& port : g_videoout_shadow_ports) {
        if (port.active && port.handle == handle) {
            return &port;
        }
    }
    return nullptr;
}

VideoOutShadowPort* open_videoout_shadow_port(std::uint32_t handle) {
    if (auto* existing = find_videoout_shadow_port(handle)) {
        return existing;
    }
    for (auto& port : g_videoout_shadow_ports) {
        if (!port.active) {
            port = {};
            port.active = true;
            port.handle = handle;
            return &port;
        }
    }
    return nullptr;
}

bool read_videoout_shadow_attribute(
    std::uint64_t address,
    bool attribute2,
    VideoOutShadowAttribute& attribute) {
    if (address == 0 ||
        !try_read_process_bytes(
            address + 0x04,
            &attribute.tiling_mode,
            sizeof(attribute.tiling_mode)) ||
        !try_read_process_bytes(
            address + 0x0C,
            &attribute.width,
            sizeof(attribute.width)) ||
        !try_read_process_bytes(
            address + 0x10,
            &attribute.height,
            sizeof(attribute.height))) {
        return false;
    }

    if (attribute2) {
        if (!try_read_process_bytes(
                address + 0x18,
                &attribute.option,
                sizeof(attribute.option)) ||
            !try_read_process_bytes(
                address + 0x20,
                &attribute.pixel_format,
                sizeof(attribute.pixel_format))) {
            return false;
        }
        attribute.pitch_in_pixel = attribute.width;
        return true;
    }

    std::uint32_t pixel_format = 0;
    std::uint32_t option = 0;
    if (!try_read_process_bytes(
            address,
            &pixel_format,
            sizeof(pixel_format)) ||
        !try_read_process_bytes(
            address + 0x14,
            &attribute.pitch_in_pixel,
            sizeof(attribute.pitch_in_pixel)) ||
        !try_read_process_bytes(
            address + 0x18,
            &option,
            sizeof(option))) {
        return false;
    }
    attribute.pixel_format = pixel_format;
    attribute.option = option;
    return true;
}

bool valid_videoout_shadow_range(
    std::uint32_t start_index,
    std::uint32_t count) {
    return count != 0 &&
        start_index < kVideoOutShadowMaximumBuffers &&
        count <= kVideoOutShadowMaximumBuffers &&
        start_index + count <= kVideoOutShadowMaximumBuffers;
}

std::int64_t videoout_performance_counter() {
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

std::int64_t videoout_performance_frequency() {
    static const std::int64_t frequency = []() {
        LARGE_INTEGER value = {};
        QueryPerformanceFrequency(&value);
        return std::max<std::int64_t>(value.QuadPart, 1);
    }();
    return frequency;
}

VideoOutShadowPort* find_videoout_native_port(std::uint32_t handle) {
    for (auto& port : g_videoout_native_ports) {
        if (port.active && port.handle == handle) {
            return &port;
        }
    }
    return nullptr;
}

VideoOutShadowPort* allocate_videoout_native_port() {
    for (auto& port : g_videoout_native_ports) {
        if (!port.active) {
            port = {};
            port.active = true;
            port.handle = g_videoout_next_native_handle.fetch_add(
                1,
                std::memory_order_relaxed);
            port.opened_at = videoout_performance_counter();
            port.last_vblank = port.opened_at;
            return &port;
        }
    }
    return nullptr;
}

std::int32_t find_free_videoout_group(const VideoOutShadowPort& port) {
    for (std::size_t index = 0; index < port.groups.size(); ++index) {
        if (!port.groups[index].active) {
            return static_cast<std::int32_t>(index);
        }
    }
    return -1;
}

std::uint64_t register_videoout_native_buffers(
    std::uint32_t handle,
    std::int32_t requested_group,
    std::uint32_t start_index,
    std::uint64_t buffers_address,
    std::uint32_t buffer_count,
    std::uint64_t attribute_address,
    bool attribute2) {
    if (buffers_address == 0) {
        return kVideoOutErrorInvalidAddress;
    }
    if (attribute_address == 0) {
        return kVideoOutErrorInvalidOption;
    }
    if (!valid_videoout_shadow_range(start_index, buffer_count)) {
        return kVideoOutErrorInvalidValue;
    }

    VideoOutShadowAttribute attribute;
    if (!read_videoout_shadow_attribute(
            attribute_address,
            attribute2,
            attribute)) {
        return kVideoOutMemoryFault;
    }

    std::array<std::uint64_t, kVideoOutShadowMaximumBuffers> addresses = {};
    for (std::uint32_t index = 0; index < buffer_count; ++index) {
        const auto entry_address = attribute2
            ? buffers_address + static_cast<std::uint64_t>(index) * 0x20
            : buffers_address + static_cast<std::uint64_t>(index) * 8;
        if (!try_read_process_bytes(
                entry_address,
                &addresses[index],
                sizeof(addresses[index]))) {
            return kVideoOutMemoryFault;
        }
    }

    std::int32_t group_index = -1;
    AcquireSRWLockExclusive(&g_videoout_native_lock);
    auto* port = find_videoout_native_port(handle);
    if (port == nullptr) {
        ReleaseSRWLockExclusive(&g_videoout_native_lock);
        return kVideoOutErrorInvalidHandle;
    }
    group_index = requested_group >= 0
        ? requested_group
        : find_free_videoout_group(*port);
    if (group_index < 0 ||
        static_cast<std::size_t>(group_index) >= port->groups.size()) {
        ReleaseSRWLockExclusive(&g_videoout_native_lock);
        return kVideoOutErrorInvalidValue;
    }
    if (port->groups[group_index].active) {
        ReleaseSRWLockExclusive(&g_videoout_native_lock);
        return kVideoOutErrorResourceBusy;
    }
    for (std::uint32_t index = 0; index < buffer_count; ++index) {
        if (port->buffers[start_index + index].group_index >= 0) {
            ReleaseSRWLockExclusive(&g_videoout_native_lock);
            return kVideoOutErrorResourceBusy;
        }
    }
    auto& group = port->groups[group_index];
    group.active = true;
    group.attribute = attribute;
    port->output_width = attribute.width;
    port->output_height = attribute.height;
    for (std::uint32_t index = 0; index < buffer_count; ++index) {
        auto& buffer = port->buffers[start_index + index];
        buffer.group_index = group_index;
        buffer.address = addresses[index];
    }
    ReleaseSRWLockExclusive(&g_videoout_native_lock);

    videoout_shadow_trace(
        "native_videoout.register handle=%u group=%d start=%u count=%u "
        "first=0x%016llX second=0x%016llX fmt=0x%016llX "
        "tile=%u size=%ux%u pitch=%u\n",
        handle,
        group_index,
        start_index,
        buffer_count,
        static_cast<unsigned long long>(addresses[0]),
        static_cast<unsigned long long>(
            buffer_count > 1 ? addresses[1] : 0),
        static_cast<unsigned long long>(attribute.pixel_format),
        attribute.tiling_mode,
        attribute.width,
        attribute.height,
        attribute.pitch_in_pixel);
    return static_cast<std::uint32_t>(group_index);
}

// Printed from the flip, because the flip is the only thing in this
// process that happens once per guest frame on the thread that owes the
// frame. The bridge heartbeat that used to carry reports like this fires
// once every ten thousand crossings into the managed side, and now that
// nothing crosses it does not fire at all.
// What one guest frame is made of, from the point of view of the thread
// that presents it. The totals below say what the whole process waits on;
// they cannot say where a frame goes, because they are summed over forty
// threads of which thirty-nine are parked job workers. This is the same
// counters read on one thread and differenced across one flip, so the
// numbers add up to the frame rather than to the process.
void report_frame_budget(std::uint64_t flip_count) {
    thread_local std::int64_t previous_ticks = 0;
    thread_local std::array<std::uint64_t, kWaitKindCount> previous_wait = {};
    thread_local std::array<std::uint64_t, kWaitKindCount> previous_count = {};

    const auto now = wait_performance_counter();
    const auto& slot = current_wait_slot();
    std::array<std::uint64_t, kWaitKindCount> wait = {};
    std::array<std::uint64_t, kWaitKindCount> count = {};
    for (std::size_t kind = 0; kind < kWaitKindCount; ++kind) {
        wait[kind] = slot.ticks[kind].load(std::memory_order_relaxed);
        count[kind] = slot.count[kind].load(std::memory_order_relaxed);
    }
    if (previous_ticks == 0) {
        previous_ticks = now;
        previous_wait = wait;
        previous_count = count;
        return;
    }

    const auto frequency = wait_performance_frequency();
    const auto frame_us =
        (now - previous_ticks) * 1'000'000 / frequency;
    std::uint64_t waited_us = 0;
    std::string text;
    for (std::size_t kind = 0; kind < kWaitKindCount; ++kind) {
        const auto ticks = wait[kind] - previous_wait[kind];
        const auto calls = count[kind] - previous_count[kind];
        if (calls == 0) {
            continue;
        }
        const auto microseconds =
            static_cast<std::uint64_t>(ticks) * 1'000'000 / frequency;
        waited_us += microseconds;
        char entry[96] = {};
        std::snprintf(
            entry,
            sizeof(entry),
            " %s=%lluus/%llu",
            wait_kind_name(static_cast<WaitKind>(kind)),
            static_cast<unsigned long long>(microseconds),
            static_cast<unsigned long long>(calls));
        text += entry;
    }
    videoout_shadow_trace(
        // The thread id lets the frame be matched against the sampler,
        // which already says per thread whether time went to guest code
        // or to a wait, and costs nothing extra to read.
        "hle.frame_budget flip=%llu tid=%u frame=%lluus waited=%lluus "
        "elsewhere=%lldus%s\n",
        static_cast<unsigned long long>(flip_count),
        slot.thread_id,
        static_cast<unsigned long long>(frame_us),
        static_cast<unsigned long long>(waited_us),
        static_cast<long long>(frame_us) -
            static_cast<long long>(waited_us),
        text.c_str());

    previous_ticks = now;
    previous_wait = wait;
    previous_count = count;
}

// Roughly one run in three stops early: a handful of flips, sometimes
// none, and then nothing for the rest of the probe, with fatal=0 and the
// process still alive. Every instrument in here is driven by the flip, so
// when the flip stops so does the reporting, and the run says nothing
// about why it stopped.
//
// This is the one thing that must not be. It watches the flip counter from
// its own thread and, when that counter has not moved, says what every
// guest thread is doing - which wait it is inside and for how long, or
// that it is running, because a guest spinning on a memory poll looks
// nothing like a guest deadlocked on a semaphore and the difference is the
// whole answer.
std::atomic<std::uint64_t> g_flip_progress{0};
std::atomic<bool> g_stall_watchdog_started{false};

void report_thread_states(std::uint64_t stalled_ms) {
    const auto now = GetTickCount64();
    std::size_t running = 0;
    std::string text;
    AcquireSRWLockShared(&g_wait_slots_lock);
    const auto threads = g_wait_slots.size();
    for (const auto* slot : g_wait_slots) {
        const auto kind = slot->active_kind.load(std::memory_order_acquire);
        if (kind < 0) {
            ++running;
            if (text.size() < 700) {
                char line[64] = {};
                std::snprintf(
                    line,
                    sizeof(line),
                    " tid=%u:running",
                    slot->thread_id);
                text += line;
            }
            continue;
        }
        if (text.size() > 700) {
            continue;
        }
        const auto since =
            slot->active_since_ms.load(std::memory_order_relaxed);
        char line[112] = {};
        std::snprintf(
            line,
            sizeof(line),
            " tid=%u:%s(0x%llX):%llums",
            slot->thread_id,
            wait_kind_name(static_cast<WaitKind>(kind)),
            static_cast<unsigned long long>(
                slot->active_detail.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                now > since ? now - since : 0));
        text += line;
    }
    ReleaseSRWLockShared(&g_wait_slots_lock);
    videoout_shadow_trace(
        "hle.stalled for=%llums flips=%llu threads=%zu running=%zu%s\n",
        static_cast<unsigned long long>(stalled_ms),
        static_cast<unsigned long long>(
            g_flip_progress.load(std::memory_order_relaxed)),
        threads,
        running,
        text.c_str());
}

void stall_watchdog_main() {
    constexpr std::uint64_t kTickMs = 2000;
    constexpr std::uint64_t kStallMs = 8000;
    // Said once, so a run that never reports a stall is distinguishable
    // from a run where this thread never started.
    videoout_shadow_trace(
        "hle.stall_watchdog started tick=%llums stall=%llums\n",
        static_cast<unsigned long long>(kTickMs),
        static_cast<unsigned long long>(kStallMs));
    std::uint64_t last_seen = 0;
    std::uint64_t stalled_ms = 0;
    for (;;) {
        Sleep(static_cast<DWORD>(kTickMs));
        const auto seen = g_flip_progress.load(std::memory_order_relaxed);
        if (seen != last_seen) {
            last_seen = seen;
            stalled_ms = 0;
            continue;
        }
        stalled_ms += kTickMs;
        if (stalled_ms < kStallMs || (stalled_ms % kStallMs) != 0) {
            continue;
        }
        report_thread_states(stalled_ms);
        agc_trace_flush();
    }
}

void start_stall_watchdog() {
    auto expected = false;
    if (g_stall_watchdog_started.compare_exchange_strong(expected, true)) {
        std::thread(stall_watchdog_main).detach();
    }
}

void report_wait_totals(std::uint64_t flip_count) {
    auto& cache = guest_region_cache();
    constexpr std::uint64_t kFlipsPerReport = 16;
    if (flip_count == 0 || (flip_count % kFlipsPerReport) != 0) {
        return;
    }
    std::array<std::uint64_t, kWaitKindCount> ticks = {};
    std::array<std::uint64_t, kWaitKindCount> count = {};
    std::size_t threads = 0;
    std::size_t blocked = 0;
    AcquireSRWLockShared(&g_wait_slots_lock);
    threads = g_wait_slots.size();
    for (const auto* slot : g_wait_slots) {
        if (slot->active_kind.load(std::memory_order_acquire) >= 0) {
            ++blocked;
        }
        for (std::size_t kind = 0; kind < kWaitKindCount; ++kind) {
            ticks[kind] +=
                slot->ticks[kind].load(std::memory_order_relaxed);
            count[kind] +=
                slot->count[kind].load(std::memory_order_relaxed);
        }
    }
    ReleaseSRWLockShared(&g_wait_slots_lock);

    const auto frequency = wait_performance_frequency();
    std::string text;
    for (std::size_t kind = 0; kind < kWaitKindCount; ++kind) {
        if (count[kind] == 0) {
            continue;
        }
        char entry[96] = {};
        std::snprintf(
            entry,
            sizeof(entry),
            " %s=%llums/%llu",
            wait_kind_name(static_cast<WaitKind>(kind)),
            static_cast<unsigned long long>(
                ticks[kind] * 1000 / frequency),
            static_cast<unsigned long long>(count[kind]));
        text += entry;
    }
    std::string stages;
    for (std::size_t stage = 0; stage < kShaderStageCount; ++stage) {
        const auto calls =
            g_shader_stage_count[stage].load(std::memory_order_relaxed);
        if (calls == 0) {
            continue;
        }
        char entry[96] = {};
        std::snprintf(
            entry,
            sizeof(entry),
            " %s=%llums/%llu",
            shader_stage_name(static_cast<ShaderStage>(stage)),
            static_cast<unsigned long long>(
                g_shader_stage_ticks[stage].load(
                    std::memory_order_relaxed) *
                1000 / frequency),
            static_cast<unsigned long long>(calls));
        stages += entry;
    }
    videoout_shadow_trace(
        "hle.trace_cost flips=%llu agc=%llums/%llu\n",
        static_cast<unsigned long long>(flip_count),
        static_cast<unsigned long long>(
            g_agc_trace_ticks * 1000 / frequency),
        static_cast<unsigned long long>(g_agc_trace_count));
    videoout_shadow_trace(
        "hle.guest_regions flips=%llu asked=%llu hit=%llu queried=%llu "
        "evicted=%llu cached=%d writable=%llu\n",
        static_cast<unsigned long long>(flip_count),
        static_cast<unsigned long long>(cache.asked),
        static_cast<unsigned long long>(cache.hit),
        static_cast<unsigned long long>(cache.queried),
        static_cast<unsigned long long>(cache.evicted),
        cache.count,
        static_cast<unsigned long long>(cache.writable_asked));
    videoout_shadow_trace(
        "hle.shader_totals flips=%llu%s\n",
        static_cast<unsigned long long>(flip_count),
        stages.c_str());
    videoout_shadow_trace(
        "hle.wait_totals flips=%llu threads=%zu blocked=%zu%s\n",
        static_cast<unsigned long long>(flip_count),
        threads,
        blocked,
        text.c_str());
}

std::uint64_t submit_videoout_native_flip(
    std::uint32_t handle,
    std::int32_t buffer_index,
    std::uint32_t flip_mode,
    std::uint64_t flip_argument,
    const char* source) {
    std::uint64_t address = 0;
    std::uint64_t flip_count = 0;
    VideoOutShadowAttribute attribute = {};
    AcquireSRWLockExclusive(&g_videoout_native_lock);
    auto* port = find_videoout_native_port(handle);
    if (port == nullptr) {
        ReleaseSRWLockExclusive(&g_videoout_native_lock);
        return kVideoOutErrorInvalidHandle;
    }
    if (buffer_index < -1 ||
        buffer_index >=
            static_cast<std::int32_t>(port->buffers.size()) ||
        (buffer_index >= 0 &&
         port->buffers[buffer_index].group_index < 0)) {
        ReleaseSRWLockExclusive(&g_videoout_native_lock);
        return kVideoOutErrorInvalidIndex;
    }
    port->current_buffer = buffer_index;
    flip_count = ++port->flip_count;
    if (buffer_index >= 0) {
        const auto& buffer = port->buffers[buffer_index];
        address = buffer.address;
        if (buffer.group_index >= 0 &&
            static_cast<std::size_t>(buffer.group_index) <
                port->groups.size()) {
            attribute = port->groups[buffer.group_index].attribute;
        }
    }
    ReleaseSRWLockExclusive(&g_videoout_native_lock);
    videoout_shadow_trace(
        "native_videoout.flip source=%s handle=%u index=%d mode=%u "
        "arg=%llu count=%llu address=0x%016llX\n",
        source,
        handle,
        buffer_index,
        flip_mode,
        static_cast<unsigned long long>(flip_argument),
        static_cast<unsigned long long>(flip_count),
        static_cast<unsigned long long>(address));
    g_flip_progress.store(flip_count, std::memory_order_relaxed);
    agc_trace_flush();
    report_frame_budget(flip_count);
    report_wait_totals(flip_count);

    Ps5GpuNativeFlip native_flip = {};
    native_flip.struct_size = sizeof(native_flip);
    native_flip.abi_version = PS5GPU_NATIVE_ABI_VERSION;
    native_flip.flip_id = flip_count;
    native_flip.display_address = address;
    native_flip.flip_argument = flip_argument;
    native_flip.pixel_format = attribute.pixel_format;
    native_flip.video_handle = handle;
    native_flip.buffer_index = buffer_index;
    native_flip.flip_mode = flip_mode;
    native_flip.tiling_mode = attribute.tiling_mode;
    native_flip.width = attribute.width;
    native_flip.height = attribute.height;
    native_flip.pitch_in_pixels = attribute.pitch_in_pixel;
    if (!ps5rt_native_gpu_submit_flip(&native_flip)) {
        videoout_shadow_trace(
            "native_gpu.submit_flip_failed source=%s handle=%u index=%d\n",
            source,
            handle,
            buffer_index);
    }
    return 0;
}

void observe_videoout_shadow_register(
    std::uint32_t handle,
    std::uint32_t group_index,
    std::uint32_t start_index,
    std::uint64_t buffers_address,
    std::uint32_t buffer_count,
    std::uint64_t attribute_address,
    bool attribute2) {
    VideoOutShadowAttribute attribute;
    if (group_index >= kVideoOutShadowMaximumGroups ||
        !valid_videoout_shadow_range(start_index, buffer_count) ||
        !read_videoout_shadow_attribute(
            attribute_address,
            attribute2,
            attribute)) {
        videoout_shadow_trace(
            "native_shadow.videoout.register_rejected handle=%u "
            "group=%u start=%u count=%u attr=0x%016llX\n",
            handle,
            group_index,
            start_index,
            buffer_count,
            static_cast<unsigned long long>(attribute_address));
        return;
    }

    std::array<std::uint64_t, kVideoOutShadowMaximumBuffers> addresses = {};
    for (std::uint32_t index = 0; index < buffer_count; ++index) {
        const auto entry_address = attribute2
            ? buffers_address + (static_cast<std::uint64_t>(index) * 0x20)
            : buffers_address + (static_cast<std::uint64_t>(index) * 8);
        if (!try_read_process_bytes(
                entry_address,
                &addresses[index],
                sizeof(addresses[index]))) {
            videoout_shadow_trace(
                "native_shadow.videoout.register_memory_fault "
                "handle=%u entry=0x%016llX\n",
                handle,
                static_cast<unsigned long long>(entry_address));
            return;
        }
    }

    AcquireSRWLockExclusive(&g_videoout_shadow_lock);
    auto* port = open_videoout_shadow_port(handle);
    if (port == nullptr) {
        ReleaseSRWLockExclusive(&g_videoout_shadow_lock);
        videoout_shadow_trace(
            "native_shadow.videoout.register_no_port handle=%u\n",
            handle);
        return;
    }
    auto& group = port->groups[group_index];
    group.active = true;
    group.attribute = attribute;
    for (std::uint32_t index = 0; index < buffer_count; ++index) {
        auto& buffer = port->buffers[start_index + index];
        buffer.group_index = static_cast<std::int32_t>(group_index);
        buffer.address = addresses[index];
    }
    ReleaseSRWLockExclusive(&g_videoout_shadow_lock);

    videoout_shadow_trace(
        "native_shadow.videoout.register handle=%u group=%u start=%u "
        "count=%u first=0x%016llX second=0x%016llX "
        "fmt=0x%016llX tile=%u width=%u height=%u pitch=%u "
        "expected_astro=%d\n",
        handle,
        group_index,
        start_index,
        buffer_count,
        static_cast<unsigned long long>(addresses[0]),
        static_cast<unsigned long long>(
            buffer_count > 1 ? addresses[1] : 0),
        static_cast<unsigned long long>(attribute.pixel_format),
        attribute.tiling_mode,
        attribute.width,
        attribute.height,
        attribute.pitch_in_pixel,
        attribute.pixel_format == kVideoOutFormat2B10G10R10A2Srgb &&
                attribute.width == 3840 &&
                attribute.height == 2160
            ? 1
            : 0);
}

void observe_videoout_shadow_flip(
    std::uint32_t handle,
    std::int32_t buffer_index,
    std::uint32_t flip_mode,
    std::uint64_t flip_argument,
    std::uint64_t submission) {
    std::uint64_t address = 0;
    std::uint64_t flip_count = 0;
    VideoOutShadowAttribute attribute = {};
    AcquireSRWLockExclusive(&g_videoout_shadow_lock);
    if (auto* port = find_videoout_shadow_port(handle)) {
        port->current_buffer = buffer_index;
        flip_count = ++port->flip_count;
        if (buffer_index >= 0 &&
            static_cast<std::size_t>(buffer_index) <
                port->buffers.size()) {
            const auto& buffer = port->buffers[buffer_index];
            address = buffer.address;
            if (buffer.group_index >= 0 &&
                static_cast<std::size_t>(buffer.group_index) <
                    port->groups.size()) {
                attribute =
                    port->groups[buffer.group_index].attribute;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_videoout_shadow_lock);
    videoout_shadow_trace(
        "native_shadow.agc.flip submission=%llu handle=%u index=%d "
        "mode=%u arg=%llu count=%llu address=0x%016llX known=%d\n",
        static_cast<unsigned long long>(submission),
        handle,
        buffer_index,
        flip_mode,
        static_cast<unsigned long long>(flip_argument),
        static_cast<unsigned long long>(flip_count),
        static_cast<unsigned long long>(address),
        address != 0 ? 1 : 0);
    const auto native_videoout_result = submit_videoout_native_flip(
        handle,
        buffer_index,
        flip_mode,
        flip_argument,
        "agc");
    if (native_videoout_result != 0 && address != 0) {
        Ps5GpuNativeFlip native_flip = {};
        native_flip.struct_size = sizeof(native_flip);
        native_flip.abi_version = PS5GPU_NATIVE_ABI_VERSION;
        native_flip.flip_id = flip_count;
        native_flip.display_address = address;
        native_flip.flip_argument = flip_argument;
        native_flip.pixel_format = attribute.pixel_format;
        native_flip.video_handle = handle;
        native_flip.buffer_index = buffer_index;
        native_flip.flip_mode = flip_mode;
        native_flip.tiling_mode = attribute.tiling_mode;
        native_flip.width = attribute.width;
        native_flip.height = attribute.height;
        native_flip.pitch_in_pixels = attribute.pitch_in_pixel;
        if (ps5rt_native_gpu_submit_flip(&native_flip)) {
            videoout_shadow_trace(
                "native_shadow.agc.native_flip_submit "
                "submission=%llu flip=%llu address=0x%016llX "
                "size=%ux%u\n",
                static_cast<unsigned long long>(submission),
                static_cast<unsigned long long>(flip_count),
                static_cast<unsigned long long>(address),
                attribute.width,
                attribute.height);
        } else {
            videoout_shadow_trace(
                "native_gpu.submit_flip_failed "
                "source=agc-shadow handle=%u index=%d\n",
                handle,
                buffer_index);
        }
    }
}

} // namespace

extern "C" {

std::uint64_t ps5rt_videoout_open_real(
    std::uint64_t user_id,
    std::uint64_t bus_type,
    std::uint64_t index,
    std::uint64_t) {
    PS5_HLE_GUARD();
    if (static_cast<std::int32_t>(bus_type) != 0 ||
        static_cast<std::int32_t>(index) != 0 ||
        (static_cast<std::int32_t>(user_id) != 0 &&
         static_cast<std::int32_t>(user_id) != 255)) {
        restore_guest_fs();
        return kVideoOutErrorInvalidValue;
    }

    AcquireSRWLockExclusive(&g_videoout_native_lock);
    auto* port = allocate_videoout_native_port();
    const auto result = port == nullptr
        ? kVideoOutErrorResourceBusy
        : static_cast<std::uint64_t>(port->handle);
    ReleaseSRWLockExclusive(&g_videoout_native_lock);
    if (port != nullptr) {
        videoout_shadow_trace(
            "native_videoout.open handle=%u user=%d bus=%d index=%d\n",
            port->handle,
            static_cast<std::int32_t>(user_id),
            static_cast<std::int32_t>(bus_type),
            static_cast<std::int32_t>(index));
    }
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_videoout_close_real(std::uint64_t handle) {
    PS5_HLE_GUARD();
    AcquireSRWLockExclusive(&g_videoout_native_lock);
    if (auto* port = find_videoout_native_port(
            static_cast<std::uint32_t>(handle))) {
        *port = {};
    }
    ReleaseSRWLockExclusive(&g_videoout_native_lock);
    videoout_shadow_trace(
        "native_videoout.close handle=%u\n",
        static_cast<std::uint32_t>(handle));
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_videoout_register_buffers_real(
    std::uint64_t handle,
    std::uint64_t start_index,
    std::uint64_t addresses_address,
    std::uint64_t buffer_count,
    std::uint64_t attribute_address) {
    PS5_HLE_GUARD();
    const auto result = register_videoout_native_buffers(
        static_cast<std::uint32_t>(handle),
        -1,
        static_cast<std::uint32_t>(start_index),
        addresses_address,
        static_cast<std::uint32_t>(buffer_count),
        attribute_address,
        false);
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_videoout_register_buffers2_real(
    std::uint64_t handle,
    std::uint64_t set_index,
    std::uint64_t start_index,
    std::uint64_t buffers_address,
    std::uint64_t buffer_count,
    std::uint64_t attribute_address,
    std::uint64_t category_raw,
    std::uint64_t option) {
    PS5_HLE_GUARD();
    const auto category = static_cast<std::uint32_t>(category_raw);
    if (category > 1 || option != 0) {
        restore_guest_fs();
        return kVideoOutErrorInvalidValue;
    }
    const auto result = register_videoout_native_buffers(
        static_cast<std::uint32_t>(handle),
        static_cast<std::int32_t>(set_index),
        static_cast<std::uint32_t>(start_index),
        buffers_address,
        static_cast<std::uint32_t>(buffer_count),
        attribute_address,
        true);
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_videoout_submit_flip_real(
    std::uint64_t handle,
    std::uint64_t buffer_index,
    std::uint64_t flip_mode,
    std::uint64_t flip_argument) {
    PS5_HLE_GUARD();
    const auto result = submit_videoout_native_flip(
        static_cast<std::uint32_t>(handle),
        static_cast<std::int32_t>(buffer_index),
        static_cast<std::uint32_t>(flip_mode),
        flip_argument,
        "api");
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_videoout_set_flip_rate_real(
    std::uint64_t handle, std::int32_t rate) {
    PS5_HLE_GUARD();
    if (rate < 0 || rate > 2) {
        restore_guest_fs();
        return kVideoOutErrorInvalidValue;
    }
    AcquireSRWLockExclusive(&g_videoout_native_lock);
    auto* port = find_videoout_native_port(
        static_cast<std::uint32_t>(handle));
    if (port != nullptr) {
        port->flip_rate = rate;
    }
    ReleaseSRWLockExclusive(&g_videoout_native_lock);
    restore_guest_fs();
    return port == nullptr ? kVideoOutErrorInvalidHandle : 0;
}

std::uint64_t ps5rt_videoout_is_flip_pending_real(std::uint64_t handle) {
    PS5_HLE_GUARD();
    AcquireSRWLockShared(&g_videoout_native_lock);
    const bool valid = find_videoout_native_port(
        static_cast<std::uint32_t>(handle)) != nullptr;
    ReleaseSRWLockShared(&g_videoout_native_lock);
    restore_guest_fs();
    return valid ? 0 : kVideoOutErrorInvalidHandle;
}

std::uint64_t ps5rt_videoout_get_vblank_status_real(
    std::uint64_t handle, std::uint64_t status_addr) {
    PS5_HLE_GUARD();
    if (status_addr == 0) {
        restore_guest_fs();
        return kVideoOutErrorInvalidAddress;
    }
    std::array<std::uint8_t, 0x28> status = {};
    const auto now = videoout_performance_counter();
    std::uint64_t count = 0;
    std::int64_t opened_at = 0;
    AcquireSRWLockExclusive(&g_videoout_native_lock);
    auto* port = find_videoout_native_port(
        static_cast<std::uint32_t>(handle));
    if (port != nullptr) {
        opened_at = port->opened_at;
        const auto elapsed = std::max<std::int64_t>(
            now - opened_at,
            0);
        const auto elapsed_count = static_cast<std::uint64_t>(
            elapsed * std::max<std::uint32_t>(port->refresh_rate, 1) /
            videoout_performance_frequency());
        port->vblank_count =
            std::max(port->vblank_count, elapsed_count);
        count = port->vblank_count;
    }
    ReleaseSRWLockExclusive(&g_videoout_native_lock);
    if (port == nullptr) {
        restore_guest_fs();
        return kVideoOutErrorInvalidHandle;
    }
    const auto elapsed_microseconds = static_cast<std::uint64_t>(
        std::max<std::int64_t>(now - opened_at, 0) * 1'000'000 /
        videoout_performance_frequency());
    std::memcpy(status.data() + 0x00, &count, sizeof(count));
    std::memcpy(
        status.data() + 0x08,
        &elapsed_microseconds,
        sizeof(elapsed_microseconds));
    const auto counter = static_cast<std::uint64_t>(now);
    std::memcpy(status.data() + 0x10, &counter, sizeof(counter));
    const auto wrote = try_write_process_bytes(
        status_addr,
        status.data(),
        status.size());
    restore_guest_fs();
    return wrote ? 0 : kVideoOutMemoryFault;
}

std::uint64_t ps5rt_videoout_add_flip_event_real(
    std::uint64_t equeue,
    std::uint64_t handle,
    std::uint64_t) {
    PS5_HLE_GUARD();
    AcquireSRWLockShared(&g_videoout_native_lock);
    const bool valid_handle = find_videoout_native_port(
        static_cast<std::uint32_t>(handle)) != nullptr;
    ReleaseSRWLockShared(&g_videoout_native_lock);
    if (!valid_handle) {
        restore_guest_fs();
        return kVideoOutErrorInvalidHandle;
    }
    if (object_lookup(g_equeues, equeue) == nullptr) {
        restore_guest_fs();
        return kVideoOutErrorInvalidEventQueue;
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_videoout_set_buffer_attribute_real(
    std::uint64_t attribute_address,
    std::uint32_t pixel_format,
    std::uint32_t tiling_mode,
    std::uint32_t aspect_ratio,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t pitch_in_pixel) {
    PS5_HLE_GUARD();
    if (attribute_address == 0) {
        restore_guest_fs();
        return kVideoOutErrorInvalidAddress;
    }
    std::array<std::uint8_t, 0x28> attribute = {};
    std::memcpy(attribute.data() + 0x00, &pixel_format, sizeof(pixel_format));
    std::memcpy(attribute.data() + 0x04, &tiling_mode, sizeof(tiling_mode));
    std::memcpy(attribute.data() + 0x08, &aspect_ratio, sizeof(aspect_ratio));
    std::memcpy(attribute.data() + 0x0C, &width, sizeof(width));
    std::memcpy(attribute.data() + 0x10, &height, sizeof(height));
    std::memcpy(
        attribute.data() + 0x14,
        &pitch_in_pixel,
        sizeof(pitch_in_pixel));
    const auto wrote = try_write_process_bytes(
        attribute_address,
        attribute.data(),
        attribute.size());
    restore_guest_fs();
    return wrote ? 0 : kVideoOutMemoryFault;
}

std::uint64_t ps5rt_videoout_set_buffer_attribute2_real(
    std::uint64_t attribute_address,
    std::uint64_t pixel_format,
    std::uint32_t tiling_mode,
    std::uint32_t width,
    std::uint32_t height,
    std::uint64_t option,
    std::uint32_t dcc_control,
    std::uint64_t dcc_clear_color) {
    PS5_HLE_GUARD();
    if (attribute_address == 0) {
        restore_guest_fs();
        return kVideoOutErrorInvalidAddress;
    }
    std::array<std::uint8_t, 0x50> attribute = {};
    std::memcpy(attribute.data() + 0x04, &tiling_mode, sizeof(tiling_mode));
    std::memcpy(attribute.data() + 0x0C, &width, sizeof(width));
    std::memcpy(attribute.data() + 0x10, &height, sizeof(height));
    std::memcpy(attribute.data() + 0x18, &option, sizeof(option));
    std::memcpy(
        attribute.data() + 0x20,
        &pixel_format,
        sizeof(pixel_format));
    std::memcpy(
        attribute.data() + 0x28,
        &dcc_clear_color,
        sizeof(dcc_clear_color));
    std::memcpy(
        attribute.data() + 0x30,
        &dcc_control,
        sizeof(dcc_control));
    const auto wrote = try_write_process_bytes(
        attribute_address,
        attribute.data(),
        attribute.size());
    restore_guest_fs();
    return wrote ? 0 : kVideoOutMemoryFault;
}

std::uint64_t ps5rt_videoout_get_output_status_real(
    std::uint64_t handle,
    std::uint64_t status_address) {
    PS5_HLE_GUARD();
    if (status_address == 0) {
        restore_guest_fs();
        return kVideoOutErrorInvalidAddress;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refresh = 0;
    AcquireSRWLockShared(&g_videoout_native_lock);
    auto* port = find_videoout_native_port(
        static_cast<std::uint32_t>(handle));
    if (port != nullptr) {
        width = port->output_width;
        height = port->output_height;
        refresh = port->refresh_rate;
    }
    ReleaseSRWLockShared(&g_videoout_native_lock);
    if (port == nullptr) {
        restore_guest_fs();
        return kVideoOutErrorInvalidHandle;
    }
    std::array<std::uint8_t, 0x30> status = {};
    const std::int32_t resolution_class =
        width >= 3840 || height >= 2160 ? 2 : 1;
    const std::int32_t connected = 1;
    const auto refresh64 = static_cast<std::uint64_t>(refresh);
    std::memcpy(
        status.data() + 0x00,
        &resolution_class,
        sizeof(resolution_class));
    std::memcpy(
        status.data() + 0x04,
        &connected,
        sizeof(connected));
    std::memcpy(status.data() + 0x08, &refresh64, sizeof(refresh64));
    const auto wrote = try_write_process_bytes(
        status_address,
        status.data(),
        status.size());
    restore_guest_fs();
    return wrote ? 0 : kVideoOutMemoryFault;
}

std::uint64_t ps5rt_videoout_color_settings_real(
    std::uint64_t settings_address,
    float gamma) {
    PS5_HLE_GUARD();
    if (settings_address == 0) {
        restore_guest_fs();
        return kVideoOutErrorInvalidAddress;
    }
    if (!std::isfinite(gamma) || gamma < 0.1f || gamma > 2.0f) {
        restore_guest_fs();
        return kVideoOutErrorInvalidValue;
    }
    const auto wrote = try_write_process_bytes(
        settings_address,
        &gamma,
        sizeof(gamma));
    restore_guest_fs();
    return wrote ? 0 : kVideoOutMemoryFault;
}

std::uint64_t ps5rt_videoout_adjust_color_real(
    std::uint64_t handle,
    std::uint64_t settings_address) {
    PS5_HLE_GUARD();
    if (settings_address == 0) {
        restore_guest_fs();
        return kVideoOutErrorInvalidAddress;
    }
    float gamma = 0.0f;
    if (!try_read_process_bytes(
            settings_address,
            &gamma,
            sizeof(gamma))) {
        restore_guest_fs();
        return kVideoOutMemoryFault;
    }
    AcquireSRWLockExclusive(&g_videoout_native_lock);
    auto* port = find_videoout_native_port(
        static_cast<std::uint32_t>(handle));
    if (port != nullptr) {
        port->gamma = gamma;
    }
    ReleaseSRWLockExclusive(&g_videoout_native_lock);
    restore_guest_fs();
    return port == nullptr ? kVideoOutErrorInvalidHandle : 0;
}

std::uint64_t ps5rt_videoout_is_output_supported_real(
    std::uint64_t handle,
    std::uint64_t mode,
    std::uint64_t options_address,
    std::uint64_t reserved_pointer,
    std::uint64_t reserved) {
    PS5_HLE_GUARD();
    AcquireSRWLockShared(&g_videoout_native_lock);
    auto* port = find_videoout_native_port(
        static_cast<std::uint32_t>(handle));
    const auto refresh = port == nullptr ? 0 : port->refresh_rate;
    ReleaseSRWLockShared(&g_videoout_native_lock);
    if (port == nullptr) {
        restore_guest_fs();
        return kVideoOutErrorInvalidHandle;
    }
    if (reserved_pointer != 0 || reserved != 0) {
        restore_guest_fs();
        return kVideoOutErrorInvalidValue;
    }
    if (options_address != 0) {
        std::array<std::uint8_t, 0x40> options = {};
        if (!try_read_process_bytes(
                options_address,
                options.data(),
                options.size())) {
            restore_guest_fs();
            return kVideoOutMemoryFault;
        }
        if (std::any_of(
                options.begin(),
                options.end(),
                [](std::uint8_t value) { return value != 0; })) {
            restore_guest_fs();
            return kVideoOutErrorInvalidOption;
        }
    }
    if (mode != kVideoOutOutputModeDefault &&
        mode != kVideoOutOutputMode119Hz) {
        restore_guest_fs();
        return kVideoOutErrorUnsupportedOutputMode;
    }
    restore_guest_fs();
    return mode == kVideoOutOutputModeDefault || refresh >= 119 ? 1 : 0;
}

std::uint64_t ps5rt_videoout_wait_vblank_real(std::uint64_t handle) {
    PS5_HLE_GUARD();
    const WaitScope wait_scope(WaitKind::WaitVblank, handle);
    std::int64_t target = 0;
    AcquireSRWLockExclusive(&g_videoout_native_lock);
    auto* port = find_videoout_native_port(
        static_cast<std::uint32_t>(handle));
    if (port != nullptr) {
        const auto now = videoout_performance_counter();
        const auto interval =
            videoout_performance_frequency() /
            std::max<std::uint32_t>(port->refresh_rate, 1);
        target = port->last_vblank + interval;
        if (target <= now || target > now + interval) {
            target = now;
        }
        port->last_vblank = target;
        ++port->vblank_count;
    }
    ReleaseSRWLockExclusive(&g_videoout_native_lock);
    if (port == nullptr) {
        restore_guest_fs();
        return kVideoOutErrorInvalidHandle;
    }
    while (videoout_performance_counter() < target) {
        Sleep(1);
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_videoout_unregister_buffers_real(
    std::uint64_t handle,
    std::int32_t group_index) {
    PS5_HLE_GUARD();
    if (group_index < 0 ||
        group_index >=
            static_cast<std::int32_t>(kVideoOutShadowMaximumGroups)) {
        restore_guest_fs();
        return kVideoOutErrorInvalidValue;
    }
    AcquireSRWLockExclusive(&g_videoout_native_lock);
    auto* port = find_videoout_native_port(
        static_cast<std::uint32_t>(handle));
    if (port == nullptr) {
        ReleaseSRWLockExclusive(&g_videoout_native_lock);
        restore_guest_fs();
        return kVideoOutErrorInvalidHandle;
    }
    if (!port->groups[group_index].active) {
        ReleaseSRWLockExclusive(&g_videoout_native_lock);
        restore_guest_fs();
        return kVideoOutErrorInvalidValue;
    }
    port->groups[group_index] = {};
    for (auto& buffer : port->buffers) {
        if (buffer.group_index == group_index) {
            buffer = {};
        }
    }
    ReleaseSRWLockExclusive(&g_videoout_native_lock);
    restore_guest_fs();
    return 0;
}

bool ps5rt_videoout_authority_dispatch(
    const char* nid,
    std::uint64_t arg0,
    std::uint64_t arg1,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4,
    std::uint64_t arg5,
    std::uint64_t guest_rsp,
    std::uint64_t xmm0_low,
    std::uint64_t* result) {
    if (nid == nullptr || result == nullptr) {
        return false;
    }
    if (std::strcmp(nid, "Up36PTk687E") == 0) {
        *result = ps5rt_videoout_open_real(arg0, arg1, arg2, arg3);
    } else if (std::strcmp(nid, "uquVH4-Du78") == 0) {
        *result = ps5rt_videoout_close_real(arg0);
    } else if (std::strcmp(nid, "w3BY+tAEiQY") == 0) {
        *result = ps5rt_videoout_register_buffers_real(
            arg0, arg1, arg2, arg3, arg4);
    } else if (std::strcmp(nid, "rKBUtgRrtbk") == 0) {
        std::uint64_t category = 0;
        std::uint64_t option = 0;
        if (!try_read_process_bytes(
                guest_rsp + 0x08,
                &category,
                sizeof(category)) ||
            !try_read_process_bytes(
                guest_rsp + 0x10,
                &option,
                sizeof(option))) {
            *result = kVideoOutMemoryFault;
        } else {
            *result = ps5rt_videoout_register_buffers2_real(
                arg0,
                arg1,
                arg2,
                arg3,
                arg4,
                arg5,
                category,
                option);
        }
    } else if (std::strcmp(nid, "U46NwOiJpys") == 0) {
        *result = ps5rt_videoout_submit_flip_real(
            arg0, arg1, arg2, arg3);
    } else if (std::strcmp(nid, "CBiu4mCE1DA") == 0) {
        *result = ps5rt_videoout_set_flip_rate_real(
            arg0,
            static_cast<std::int32_t>(arg1));
    } else if (std::strcmp(nid, "zgXifHT9ErY") == 0) {
        *result = ps5rt_videoout_is_flip_pending_real(arg0);
    } else if (std::strcmp(nid, "1FZBKy8HeNU") == 0) {
        *result = ps5rt_videoout_get_vblank_status_real(arg0, arg1);
    } else if (std::strcmp(nid, "HXzjK9yI30k") == 0) {
        *result = ps5rt_videoout_add_flip_event_real(
            arg0, arg1, arg2);
    } else if (std::strcmp(nid, "i6-sR91Wt-4") == 0) {
        std::uint32_t pitch = 0;
        if (!try_read_process_bytes(
                guest_rsp + 0x08,
                &pitch,
                sizeof(pitch))) {
            *result = kVideoOutMemoryFault;
        } else {
            *result = ps5rt_videoout_set_buffer_attribute_real(
                arg0,
                static_cast<std::uint32_t>(arg1),
                static_cast<std::uint32_t>(arg2),
                static_cast<std::uint32_t>(arg3),
                static_cast<std::uint32_t>(arg4),
                static_cast<std::uint32_t>(arg5),
                pitch);
        }
    } else if (std::strcmp(nid, "PjS5uASwcV8") == 0) {
        std::uint32_t dcc_control = 0;
        std::uint64_t dcc_clear_color = 0;
        if (!try_read_process_bytes(
                guest_rsp + 0x08,
                &dcc_control,
                sizeof(dcc_control)) ||
            !try_read_process_bytes(
                guest_rsp + 0x10,
                &dcc_clear_color,
                sizeof(dcc_clear_color))) {
            *result = kVideoOutMemoryFault;
        } else {
            *result = ps5rt_videoout_set_buffer_attribute2_real(
                arg0,
                arg1,
                static_cast<std::uint32_t>(arg2),
                static_cast<std::uint32_t>(arg3),
                static_cast<std::uint32_t>(arg4),
                arg5,
                dcc_control,
                dcc_clear_color);
        }
    } else if (std::strcmp(nid, "HuViW4HnrOw") == 0) {
        *result = 0;
    } else if (std::strcmp(nid, "utPrVdxio-8") == 0) {
        *result = ps5rt_videoout_get_output_status_real(arg0, arg1);
    } else if (std::strcmp(nid, "DYhhWbJSeRg") == 0) {
        const auto gamma_bits = static_cast<std::uint32_t>(xmm0_low);
        float gamma = 0.0f;
        std::memcpy(&gamma, &gamma_bits, sizeof(gamma));
        *result = ps5rt_videoout_color_settings_real(arg0, gamma);
    } else if (std::strcmp(nid, "pv9CI5VC+R0") == 0) {
        *result = ps5rt_videoout_adjust_color_real(arg0, arg1);
    } else if (std::strcmp(nid, "Nv8c-Kb+DUM") == 0) {
        *result = ps5rt_videoout_is_output_supported_real(
            arg0, arg1, arg2, arg3, arg4);
    } else if (std::strcmp(nid, "j6RaAUlaLv0") == 0) {
        *result = ps5rt_videoout_wait_vblank_real(arg0);
    } else if (std::strcmp(nid, "N5KDtkIjjJ4") == 0) {
        *result = ps5rt_videoout_unregister_buffers_real(
            arg0,
            static_cast<std::int32_t>(arg1));
    } else {
        return false;
    }
    return true;
}

void ps5rt_videoout_shadow_observe(
    const char* nid,
    std::uint64_t arg0,
    std::uint64_t arg1,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4,
    std::uint64_t arg5,
    std::uint64_t,
    std::uint64_t managed_result) {
    if (nid == nullptr) {
        return;
    }

    if (std::strcmp(nid, "Up36PTk687E") == 0) {
        if (managed_result == 0 ||
            (managed_result & 0x80000000ULL) != 0) {
            videoout_shadow_trace(
                "native_shadow.videoout.open_rejected "
                "result=0x%016llX\n",
                static_cast<unsigned long long>(managed_result));
            return;
        }
        AcquireSRWLockExclusive(&g_videoout_shadow_lock);
        open_videoout_shadow_port(
            static_cast<std::uint32_t>(managed_result));
        ReleaseSRWLockExclusive(&g_videoout_shadow_lock);
        videoout_shadow_trace(
            "native_shadow.videoout.open handle=%u user=%llu "
            "bus=%llu index=%llu\n",
            static_cast<std::uint32_t>(managed_result),
            static_cast<unsigned long long>(arg0),
            static_cast<unsigned long long>(arg1),
            static_cast<unsigned long long>(arg2));
        return;
    }

    if (std::strcmp(nid, "uquVH4-Du78") == 0) {
        AcquireSRWLockExclusive(&g_videoout_shadow_lock);
        if (auto* port = find_videoout_shadow_port(
                static_cast<std::uint32_t>(arg0))) {
            *port = {};
        }
        ReleaseSRWLockExclusive(&g_videoout_shadow_lock);
        videoout_shadow_trace(
            "native_shadow.videoout.close handle=%u\n",
            static_cast<std::uint32_t>(arg0));
        return;
    }

    if (managed_result != 0) {
        return;
    }

    if (std::strcmp(nid, "rKBUtgRrtbk") == 0) {
        observe_videoout_shadow_register(
            static_cast<std::uint32_t>(arg0),
            static_cast<std::uint32_t>(arg1),
            static_cast<std::uint32_t>(arg2),
            arg3,
            static_cast<std::uint32_t>(arg4),
            arg5,
            true);
        return;
    }

    if (std::strcmp(nid, "w3BY+tAEiQY") == 0) {
        observe_videoout_shadow_register(
            static_cast<std::uint32_t>(arg0),
            0,
            static_cast<std::uint32_t>(arg1),
            arg2,
            static_cast<std::uint32_t>(arg3),
            arg4,
            false);
        return;
    }

    if (std::strcmp(nid, "U46NwOiJpys") != 0) {
        return;
    }

    std::uint64_t address = 0;
    std::uint64_t flip_count = 0;
    AcquireSRWLockExclusive(&g_videoout_shadow_lock);
    if (auto* port = find_videoout_shadow_port(
            static_cast<std::uint32_t>(arg0))) {
        const auto buffer_index = static_cast<std::int32_t>(arg1);
        port->current_buffer = buffer_index;
        flip_count = ++port->flip_count;
        if (buffer_index >= 0 &&
            static_cast<std::size_t>(buffer_index) <
                port->buffers.size()) {
            address = port->buffers[buffer_index].address;
        }
    }
    ReleaseSRWLockExclusive(&g_videoout_shadow_lock);
    videoout_shadow_trace(
        "native_shadow.videoout.flip handle=%u index=%d mode=%u "
        "arg=%llu count=%llu address=0x%016llX\n",
        static_cast<std::uint32_t>(arg0),
        static_cast<std::int32_t>(arg1),
        static_cast<std::uint32_t>(arg2),
        static_cast<unsigned long long>(arg3),
        static_cast<unsigned long long>(flip_count),
        static_cast<unsigned long long>(address));
}

// --- AGC stubs ---
// The guest calls sceAgcGetRegisterDefaults2 exactly once, and what it does
// with the answer is worth writing down, because it is the whole reason
// CB_TARGET_MASK never reached the command stream:
//
//     call   sceAgcGetRegisterDefaults2
//     mov    ecx,[rax+0x38]          ; group count
//     test   rcx,rcx
//     je     .none                   ; -> writes -1 into every cached slot
//     mov    rdx,[rax+0x30]          ; group table
//   .search:
//     cmp    DWORD [rdx+rsi],0xE24F806D
//     je     .found                  ; twelve bytes per entry
//   .found:
//     mov    esi,[rdx+rsi+4]         ; the id
//     bextr  edi,esi,0x802           ; index  = (id >> 2) & 0xFF
//     and    esi,3                   ; space  = id & 3
//     mov    rsi,[rax+rsi*8]         ; the descriptor's pointer for it
//     mov    rsi,[rsi+rdi*8]         ; that space's slot
//     mov    rsi,[rsi]               ; the (offset, value) pair itself
//
// So a group names one register, by hash, and the descriptor answers with a
// pointer into the default table. Handing back a zero group count told the
// game every group it asked for was missing, and it cached -1 for each -
// which is why a register it never writes explicitly, the render target
// mask, was absent from every draw.
struct alignas(16) AgcRegisterDefaults {
    const void* space_slots[4];
    std::uint32_t cx_count;
    std::uint32_t sh_count;
    std::uint32_t uc_count;
    std::uint32_t reserved_2c;
    const void* groups;
    std::uint32_t group_count;
    std::uint32_t reserved_3c;
};

static_assert(offsetof(AgcRegisterDefaults, cx_count) == 0x20);
static_assert(offsetof(AgcRegisterDefaults, uc_count) == 0x28);
static_assert(offsetof(AgcRegisterDefaults, groups) == 0x30);
static_assert(offsetof(AgcRegisterDefaults, group_count) == 0x38);

// Twelve bytes, because that is the stride the guest's search loop uses.
struct AgcRegisterGroup {
    std::uint32_t hash;
    std::uint32_t id;
    std::uint32_t reserved;
};
static_assert(sizeof(AgcRegisterGroup) == 12);

static AgcRegisterDefaults g_agc_register_defaults = {};
static AgcRegisterDefaults g_agc_register_defaults_internal = {};

static AgcRegisterGroup
    g_agc_groups[sizeof(agc_defaults::kGroups) /
                 sizeof(agc_defaults::kGroups[0])];
static const agc_defaults::RegisterDefault*
    g_agc_slots_cx[sizeof(agc_defaults::kSlots0) /
                   sizeof(agc_defaults::kSlots0[0])];
static const agc_defaults::RegisterDefault*
    g_agc_slots_sh[sizeof(agc_defaults::kSlots1) /
                   sizeof(agc_defaults::kSlots1[0])];
static const agc_defaults::RegisterDefault*
    g_agc_slots_uc[sizeof(agc_defaults::kSlots2) /
                   sizeof(agc_defaults::kSlots2[0])];

void build_agc_register_defaults() {
    for (std::size_t index = 0;
         index < sizeof(g_agc_groups) / sizeof(g_agc_groups[0]);
         ++index) {
        g_agc_groups[index].hash = agc_defaults::kGroups[index].hash;
        g_agc_groups[index].id = agc_defaults::kGroups[index].id;
        g_agc_groups[index].reserved = 0;
    }
    const auto fill_slots = [](auto& slots, const auto& indices,
                               const auto& table) {
        for (std::size_t index = 0;
             index < sizeof(slots) / sizeof(slots[0]);
             ++index) {
            slots[index] = &table[indices[index]];
        }
    };
    fill_slots(g_agc_slots_cx, agc_defaults::kSlots0,
               agc_defaults::kContextFull);
    fill_slots(g_agc_slots_sh, agc_defaults::kSlots1,
               agc_defaults::kShaderFull);
    fill_slots(g_agc_slots_uc, agc_defaults::kSlots2,
               agc_defaults::kUconfigFull);

    for (auto* descriptor :
         {&g_agc_register_defaults, &g_agc_register_defaults_internal}) {
        descriptor->space_slots[0] = g_agc_slots_cx;
        descriptor->space_slots[1] = g_agc_slots_sh;
        descriptor->space_slots[2] = g_agc_slots_uc;
        // No group in the table names a fourth space.
        descriptor->space_slots[3] = nullptr;
        descriptor->cx_count = static_cast<std::uint32_t>(
            sizeof(agc_defaults::kContextFull) /
            sizeof(agc_defaults::kContextFull[0]));
        descriptor->sh_count = static_cast<std::uint32_t>(
            sizeof(agc_defaults::kShaderFull) /
            sizeof(agc_defaults::kShaderFull[0]));
        descriptor->uc_count = static_cast<std::uint32_t>(
            sizeof(agc_defaults::kUconfigFull) /
            sizeof(agc_defaults::kUconfigFull[0]));
        descriptor->groups = g_agc_groups;
        descriptor->group_count = static_cast<std::uint32_t>(
            sizeof(g_agc_groups) / sizeof(g_agc_groups[0]));
    }
}

static bool is_supported_agc_defaults_version(std::uint64_t version) {
    return version == 7 || version == 8 || version == 10 || version == 13;
}

namespace {

constexpr std::uint32_t kAgcItNop = 0x10;
constexpr std::uint32_t kAgcItSetBase = 0x11;
constexpr std::uint32_t kAgcItDispatchDirect = 0x15;
constexpr std::uint32_t kAgcItDispatchIndirect = 0x16;
constexpr std::uint32_t kAgcItWaitRegMem = 0x3C;
// The 64-bit wait has its own opcode rather than a width field,
// which is how AGC emits it: a nine-dword packet against the
// 32-bit form's seven.
constexpr std::uint32_t kAgcItWaitRegMem64 = 0x93;
constexpr std::uint32_t kAgcItWriteData = 0x37;
constexpr std::uint32_t kAgcItEventWriteEop = 0x47;
constexpr std::uint32_t kAgcItReleaseMem = 0x49;
// NOP-aliased forms of the same two packets, the way AGC emits them.
constexpr std::uint32_t kAgcRWriteData = 0x15;
constexpr std::uint32_t kAgcRReleaseMem = 0x18;
constexpr std::uint32_t kAgcItDrawIndirect = 0x24;
constexpr std::uint32_t kAgcItDrawIndexIndirect = 0x25;
constexpr std::uint32_t kAgcItDrawIndex2 = 0x27;
constexpr std::uint32_t kAgcItDrawIndexAuto = 0x2D;
constexpr std::uint32_t kAgcItDrawIndexMultiAuto = 0x30;
constexpr std::uint32_t kAgcItDrawIndexOffset2 = 0x35;
// Where the indices live, and how wide they are. Every geometry draw this
// title issues is a DRAW_INDEX_OFFSET_2, which carries an offset and no
// base: the base arrives in its own packet and holds until the next one.
constexpr std::uint32_t kAgcItIndexBaseOp = 0x26;
constexpr std::uint32_t kAgcItIndexTypeOp = 0x2A;
constexpr std::uint32_t kAgcItSetShRegIndirect = 0x63;
constexpr std::uint32_t kAgcItSetUconfigRegIndirect = 0x64;
constexpr std::uint32_t kAgcItSetContextReg = 0x69;
constexpr std::uint32_t kAgcItSetShReg = 0x76;
constexpr std::uint32_t kAgcItSetUconfigReg = 0x79;
constexpr std::uint32_t kAgcItSetContextRegIndirect = 0x9F;
constexpr std::uint32_t kAgcRDrawIndexAuto = 0x04;
constexpr std::uint32_t kAgcRDrawReset = 0x05;
constexpr std::uint32_t kAgcRAcbReset = 0x09;
constexpr std::uint32_t kAgcRWaitMem32 = 0x0A;
constexpr std::uint32_t kAgcRShRegsIndirect = 0x11;
constexpr std::uint32_t kAgcRCxRegsIndirect = 0x12;
constexpr std::uint32_t kAgcRUcRegsIndirect = 0x13;
constexpr std::uint32_t kAgcRWaitMem64 = 0x16;
constexpr std::uint32_t kAgcRFlip = 0x17;
constexpr std::uint32_t kAgcSpiShaderPgmLoPs = 0x08;
constexpr std::uint32_t kAgcSpiShaderPgmHiPs = 0x09;
constexpr std::uint32_t kAgcSpiShaderPgmLoVs = 0x48;
constexpr std::uint32_t kAgcSpiShaderPgmHiVs = 0x49;
constexpr std::uint32_t kAgcSpiShaderPgmLoEs = 0xC8;
constexpr std::uint32_t kAgcSpiShaderPgmHiEs = 0xC9;
constexpr std::uint32_t kAgcSpiShaderPgmLoHs = 0x108;
constexpr std::uint32_t kAgcSpiShaderPgmHiHs = 0x109;
constexpr std::uint32_t kAgcSpiShaderPgmRsrc1Hs = 0x10A;
constexpr std::uint32_t kAgcSpiShaderPgmLoLs = 0x148;
constexpr std::uint32_t kAgcSpiShaderPgmHiLs = 0x149;
constexpr std::uint32_t kAgcSpiShaderPgmLoGs = 0x88;
constexpr std::uint32_t kAgcSpiShaderPgmHiGs = 0x89;
constexpr std::uint32_t kAgcSpiShaderPgmRsrc1Gs = 0x8A;
constexpr std::uint32_t kAgcSpiPsInputEna = 0x1B3;
constexpr std::uint32_t kAgcSpiPsInputAddr = 0x1B4;
constexpr std::uint32_t kAgcComputePgmLo = 0x20C;
constexpr std::uint32_t kAgcComputePgmHi = 0x20D;
constexpr std::uint32_t kAgcComputePgmRsrc2 = 0x213;
constexpr std::uint32_t kAgcComputeStartX = 0x204;
constexpr std::uint32_t kAgcComputeStartY = 0x205;
constexpr std::uint32_t kAgcComputeStartZ = 0x206;
constexpr std::uint32_t kAgcComputeNumThreadX = 0x207;
constexpr std::uint32_t kAgcComputeNumThreadY = 0x208;
constexpr std::uint32_t kAgcComputeNumThreadZ = 0x209;
constexpr std::uint32_t kAgcComputeUserDataRegister = 0x240;
constexpr std::uint32_t kAgcPsUserDataRegister = 0x0C;
constexpr std::uint32_t kAgcVsUserDataRegister = 0x4C;
constexpr std::uint32_t kAgcGsUserDataRegister = 0x8C;
constexpr std::uint32_t kAgcEsUserDataRegister = 0xCC;
constexpr std::uint32_t kAgcVgtPrimitiveType = 0x242;
constexpr std::uint32_t kAgcGeIndexOffset = 0x24A;
// Register offsets do not arrive bare. The top nibble carries a
// selector, and storing the offset with it still attached files the
// write under a key no reader ever looks up - which is why the register
// map held no CB_TARGET_MASK at all while the stream was plainly
// setting it. KytyPS5 strips the same field.
constexpr std::uint32_t kAgcRegisterSelectorMask = 0x70000000u;
// One selector is not noise: it names the 32-entry bank of pixel-shader
// input controls, and its slot index is an index into that bank rather
// than a register offset. AGC's interpolant mapping writes through it,
// so dropping the selector without this remap would scatter the mapping
// over the first 32 context registers.
constexpr std::uint32_t kAgcCxPsShaderUsageBase = 0x10000000u;
constexpr std::uint32_t kAgcSpiPsInputCntl0 = 0x191;
constexpr std::uint32_t kAgcSpiPsInputCntlCount = 32;

std::uint32_t normalize_agc_register_offset(std::uint32_t raw_offset) {
    const auto selector = raw_offset & kAgcRegisterSelectorMask;
    const auto offset = raw_offset & ~kAgcRegisterSelectorMask;
    if (selector == kAgcCxPsShaderUsageBase &&
        offset < kAgcSpiPsInputCntlCount) {
        return kAgcSpiPsInputCntl0 + offset;
    }
    return offset;
}

// How often a selector was seen, so the remap above is answerable from a
// run rather than from another emulator's reading of a different ABI.
std::atomic<std::uint64_t> g_register_selector_seen{0};
std::atomic<std::uint64_t> g_register_selector_ps_input{0};

constexpr std::uint32_t kAgcCbTargetMask = 0x08E;
// Which slots the pixel shader actually exports to. A draw whose
// shader mask and target mask do not intersect writes no colour
// at all, however complete the rest of its state looks.
constexpr std::uint32_t kAgcCbShaderMask = 0x08F;
constexpr std::uint32_t kAgcPaScScreenScissorTl = 0x00C;
constexpr std::uint32_t kAgcPaScScreenScissorBr = 0x00D;
constexpr std::uint32_t kAgcPaScWindowOffset = 0x080;
constexpr std::uint32_t kAgcPaScWindowScissorTl = 0x081;
constexpr std::uint32_t kAgcPaScWindowScissorBr = 0x082;
constexpr std::uint32_t kAgcPaScGenericScissorTl = 0x090;
constexpr std::uint32_t kAgcPaScGenericScissorBr = 0x091;
constexpr std::uint32_t kAgcPaScVportScissor0Tl = 0x094;
constexpr std::uint32_t kAgcPaScVportScissor0Br = 0x095;
constexpr std::uint32_t kAgcPaScVportZMin0 = 0x0B4;
constexpr std::uint32_t kAgcPaScVportZMax0 = 0x0B5;
constexpr std::uint32_t kAgcPaClVportXScale = 0x10F;
constexpr std::uint32_t kAgcPaClVportXOffset = 0x110;
constexpr std::uint32_t kAgcPaClVportYScale = 0x111;
constexpr std::uint32_t kAgcPaClVportYOffset = 0x112;
constexpr std::uint32_t kAgcPaScModeCntl0 = 0x292;
constexpr std::uint32_t kAgcCbColor0Base = 0x318;
constexpr std::uint32_t kAgcCbColorRegisterStride = 15;
constexpr std::uint32_t kAgcCbColor0Info = 0x31C;
constexpr std::uint32_t kAgcCbColor0BaseExt = 0x390;
constexpr std::uint32_t kAgcCbColor0Attrib2 = 0x3B0;
constexpr std::uint32_t kAgcCbColor0Attrib3 = 0x3B8;
constexpr std::uint32_t kAgcColorTargetCount = 8;
// The colour target record is sixteen consecutive values. An attempt to write
// only the four the validator reads - on the grounds that the others decode to
// implausible register state - broke the frame: decode_agc_render_targets
// reads CB_COLOR0_ATTRIB3 at slot 15 for the swizzle mode, and COLOR_SW_MODE=27
// is a perfectly legal gfx10 value that I had written off as garbage. Several
// of the remaining slots still decode to states no game would program
// (SLICE_START=8191 on a 2D target, 0xFFFFFF00 in BASE_EXT), so the mapping is
// not fully understood - but "looks implausible" is not evidence enough to stop
// writing a register, as this cost a probe to learn.
constexpr std::array<std::uint32_t, 16> kAgcCbColor0SentinelOffsets = {
    0x318, 0x31B, 0x31C, 0x31D,
    0x31E, 0x31F, 0x321, 0x323,
    0x324, 0x325, 0x390, 0x398,
    0x3A0, 0x3A8, 0x3B0, 0x3B8,
};
constexpr std::size_t kAgcCbColor0SentinelLength =
    kAgcCbColor0SentinelOffsets.size();
constexpr std::array<std::uint32_t, 10> kAgcViewportSentinelOffsets = {
    kAgcPaClVportXScale,
    kAgcPaClVportYScale,
    0x113,
    kAgcPaClVportXOffset,
    kAgcPaClVportYOffset,
    0x114,
    kAgcPaScVportScissor0Tl,
    kAgcPaScVportScissor0Br,
    kAgcPaScVportZMin0,
    kAgcPaScVportZMax0,
};
constexpr std::uint32_t kAgcMaximumDwords = 1'000'000;
constexpr std::uint32_t kAgcShaderFileHeader = 0x34333231;
constexpr std::uint32_t kAgcShaderVersion = 0x18;
constexpr std::uint64_t kAgcShaderUserDataOffset = 0x08;
constexpr std::uint64_t kAgcShaderCodeOffset = 0x10;
constexpr std::uint64_t kAgcShaderCxRegistersOffset = 0x18;
constexpr std::uint64_t kAgcShaderShRegistersOffset = 0x20;
constexpr std::uint64_t kAgcShaderSpecialsOffset = 0x28;
constexpr std::uint64_t kAgcShaderInputSemanticsOffset = 0x30;
constexpr std::uint64_t kAgcShaderOutputSemanticsOffset = 0x38;
constexpr std::uint64_t kAgcShaderTypeOffset = 0x5A;
constexpr std::uint64_t kAgcShaderNumShRegistersOffset = 0x5C;

struct AgcSubmittedState {
    std::map<std::uint32_t, std::uint32_t> sh_registers;
    std::map<std::uint32_t, std::uint32_t> cx_registers;
    std::map<std::uint32_t, std::uint32_t> uc_registers;
    std::map<std::uint64_t, std::uint32_t> shader_attempts;
    std::map<std::uint32_t, std::uint32_t> graphics_attempts;
    std::map<std::uint64_t, std::uint32_t> compute_attempts;
    std::map<std::uint64_t, std::uint32_t> compute_state_ids;
    std::uint64_t indirect_args_address = 0;
    // INDEX_BASE and VGT_DMA_INDEX_TYPE, which persist across draws.
    std::uint64_t index_base = 0;
    std::uint32_t index_bytes = 2;
    std::uint64_t submission_count = 0;
    std::uint64_t draw_count = 0;
    std::uint64_t dispatch_count = 0;
    bool seeded = false;
};

// A reset returns the hardware to the state libSceAgc programmed when the
// context was created, which is not all zeroes: 147 context registers have a
// non-zero default, among them CB_COLOR_CONTROL (zero reads as CB_DISABLE) and
// every scissor rectangle (zero reads as an empty region). The table comes from
// Sony's own library via tools/agc_extract_defaults.py.
//
// Only the non-zero defaults are seeded. This decoder reads an absent register
// as zero and elsewhere uses presence to decide whether a resource is bound, so
// seeding the zeroes would make every unbound resource look bound.
void seed_agc_default_state(AgcSubmittedState& state) {
    static const auto disabled = [] {
        const auto* value = std::getenv("PS5RT_AGC_NO_DEFAULT_STATE");
        return value != nullptr && value[0] == '1';
    }();
    if (disabled) {
        return;
    }
    state.seeded = true;
    for (const auto& entry : agc_defaults::kContext) {
        state.cx_registers[entry.offset] = entry.value;
    }
    for (const auto& entry : agc_defaults::kShader) {
        state.sh_registers[entry.offset] = entry.value;
    }
    for (const auto& entry : agc_defaults::kUconfig) {
        state.uc_registers[entry.offset] = entry.value;
    }
}

// Which context registers the command stream itself writes. A seeded
// default only matters for a register the stream never touches, so this
// is what separates "the seed filled a hole" from "the seed was
// redundant". A bitset and not a set: this is marked on every register
// write in the parse, and the parse is a measured hot path.
constexpr std::size_t kAgcContextRegisterSlots = 0x400;
std::bitset<kAgcContextRegisterSlots> g_stream_written_cx;

void mark_stream_written_cx(std::uint32_t offset) {
    if (offset < kAgcContextRegisterSlots) {
        g_stream_written_cx.set(offset);
    }
}

AgcSubmittedState g_agc_submitted_state;
AgcSubmittedState g_agc_shadow_state;
std::map<std::uint32_t, AgcSubmittedState> g_agc_compute_states;
std::map<std::uint32_t, AgcSubmittedState> g_agc_shadow_compute_states;
SRWLOCK g_agc_submit_lock = SRWLOCK_INIT;
SRWLOCK g_agc_shadow_lock = SRWLOCK_INIT;
SRWLOCK g_agc_compute_lock = SRWLOCK_INIT;
SRWLOCK g_agc_shader_header_lock = SRWLOCK_INIT;
std::map<std::uint64_t, std::uint64_t> g_agc_shader_headers;

// Whether the per-packet traces are wanted. They are the bulk of a run's
// output and cost the guest's submission path to produce, so they are off
// unless asked for.
bool agc_packet_trace_enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("PS5RT_TRACE_DRAW_PACKETS");
        return value != nullptr &&
            (std::strcmp(value, "1") == 0 ||
             std::strcmp(value, "true") == 0 ||
             std::strcmp(value, "on") == 0);
    }();
    return enabled;
}

void agc_trace(const char* format, ...) {
    char buffer[1152] = {};
    // Same opt-in timeline as trace_stderr; see the note there.
    static const bool trace_time = [] {
        const auto* value = std::getenv("PS5RT_TRACE_TIME");
        return value != nullptr &&
            (std::strcmp(value, "1") == 0 ||
             std::strcmp(value, "true") == 0 ||
             std::strcmp(value, "on") == 0);
    }();
    int offset = 0;
    if (trace_time) {
        const auto now = GetTickCount64();
        const int prefix = std::snprintf(
            buffer,
            sizeof(buffer),
            "[t=%llu.%03llu] ",
            static_cast<unsigned long long>(now / 1000),
            static_cast<unsigned long long>(now % 1000));
        if (prefix > 0) {
            offset = prefix;
        }
    }
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(
        buffer + offset,
        sizeof(buffer) - static_cast<std::size_t>(offset),
        format,
        arguments);
    va_end(arguments);
    if (length <= 0) {
        return;
    }

    const auto byte_count = static_cast<DWORD>(
        offset + length < static_cast<int>(sizeof(buffer))
            ? offset + length
            : sizeof(buffer) - 1);
    const auto started = wait_performance_counter();
    if (g_agc_trace_used + byte_count > kAgcTraceBufferBytes) {
        agc_trace_flush();
    }
    if (byte_count > kAgcTraceBufferBytes) {
        DWORD written = 0;
        WriteFile(
            GetStdHandle(STD_ERROR_HANDLE),
            buffer,
            byte_count,
            &written,
            nullptr);
    } else {
        std::memcpy(g_agc_trace_buffer + g_agc_trace_used, buffer, byte_count);
        g_agc_trace_used += byte_count;
        // Touching the exit flusher is what makes the thread-local object
        // exist at all, and so what gets its destructor registered.
        (void)&g_agc_trace_flush_on_exit;
    }
    const auto elapsed = wait_performance_counter() - started;
    g_agc_trace_ticks += elapsed > 0
        ? static_cast<std::uint64_t>(elapsed)
        : 0;
    ++g_agc_trace_count;
}

bool try_read_u32(std::uint64_t address, std::uint32_t& value) {
    return try_read_process_bytes(address, &value, sizeof(value));
}

bool try_read_u64(std::uint64_t address, std::uint64_t& value) {
    return try_read_process_bytes(address, &value, sizeof(value));
}

bool try_read_u8(std::uint64_t address, std::uint8_t& value) {
    return try_read_process_bytes(address, &value, sizeof(value));
}

bool try_write_u32(std::uint64_t address, std::uint32_t value) {
    return try_write_process_bytes(address, &value, sizeof(value));
}

bool try_write_u64(std::uint64_t address, std::uint64_t value) {
    return try_write_process_bytes(address, &value, sizeof(value));
}

bool relocate_agc_pointer_field(std::uint64_t field_address) {
    std::uint64_t relative_address = 0;
    if (!try_read_u64(field_address, relative_address)) {
        return false;
    }
    if (relative_address == 0) {
        return true;
    }
    if (relative_address >
        std::numeric_limits<std::uint64_t>::max() - field_address) {
        return false;
    }
    return try_write_u64(field_address, field_address + relative_address);
}

struct AgcShaderRegisterPair {
    std::uint32_t lo;
    std::uint32_t hi;
};

constexpr std::array<AgcShaderRegisterPair, 7>
    kAgcShaderRegisterPairs = {{
        {kAgcComputePgmLo, kAgcComputePgmHi},
        {kAgcSpiShaderPgmLoPs, kAgcSpiShaderPgmHiPs},
        {kAgcSpiShaderPgmLoVs, kAgcSpiShaderPgmHiVs},
        {kAgcSpiShaderPgmLoEs, kAgcSpiShaderPgmHiEs},
        {kAgcSpiShaderPgmLoGs, kAgcSpiShaderPgmHiGs},
        {kAgcSpiShaderPgmLoHs, kAgcSpiShaderPgmHiHs},
        {kAgcSpiShaderPgmLoLs, kAgcSpiShaderPgmHiLs},
    }};

bool find_agc_shader_program_registers(
    std::uint64_t registers_address,
    std::uint8_t register_count,
    std::uint32_t preferred_lo,
    std::uint32_t preferred_hi,
    std::uint64_t& lo_entry_address,
    std::uint64_t& hi_entry_address) {
    lo_entry_address = 0;
    hi_entry_address = 0;
    std::uint64_t fallback_lo = 0;
    std::uint64_t fallback_hi = 0;

    for (std::uint32_t index = 0; index < register_count; ++index) {
        const auto entry_address =
            registers_address +
            static_cast<std::uint64_t>(index) * 8;
        std::uint32_t register_offset = 0;
        if (!try_read_u32(entry_address, register_offset)) {
            return false;
        }
        if (register_offset == preferred_lo) {
            lo_entry_address = entry_address;
        } else if (register_offset == preferred_hi) {
            hi_entry_address = entry_address;
        }
        if (fallback_lo != 0) {
            continue;
        }
        for (const auto& pair : kAgcShaderRegisterPairs) {
            if (register_offset != pair.lo) {
                continue;
            }
            for (std::uint32_t hi_index = 0;
                 hi_index < register_count;
                 ++hi_index) {
                if (hi_index == index) {
                    continue;
                }
                const auto candidate =
                    registers_address +
                    static_cast<std::uint64_t>(hi_index) * 8;
                std::uint32_t hi_offset = 0;
                if (try_read_u32(candidate, hi_offset) &&
                    hi_offset == pair.hi) {
                    fallback_lo = entry_address;
                    fallback_hi = candidate;
                    break;
                }
            }
            break;
        }
    }

    if (lo_entry_address != 0 && hi_entry_address != 0) {
        return true;
    }
    lo_entry_address = fallback_lo;
    hi_entry_address = fallback_hi;
    return lo_entry_address != 0 && hi_entry_address != 0;
}

bool patch_agc_shader_program_registers(
    std::uint64_t header_address,
    std::uint64_t code_address) {
    std::uint64_t registers_address = 0;
    std::uint8_t shader_type = 0;
    std::uint8_t register_count = 0;
    if (!try_read_u64(
            header_address + kAgcShaderShRegistersOffset,
            registers_address) ||
        !try_read_u8(
            header_address + kAgcShaderTypeOffset,
            shader_type) ||
        !try_read_u8(
            header_address + kAgcShaderNumShRegistersOffset,
            register_count) ||
        registers_address == 0 ||
        register_count < 2) {
        return false;
    }

    std::uint32_t expected_lo = 0;
    std::uint32_t expected_hi = 0;
    switch (shader_type) {
    case 0:
        expected_lo = kAgcComputePgmLo;
        expected_hi = kAgcComputePgmHi;
        break;
    case 1:
        expected_lo = kAgcSpiShaderPgmLoPs;
        expected_hi = kAgcSpiShaderPgmHiPs;
        break;
    case 2:
    case 6:
        expected_lo = kAgcSpiShaderPgmLoEs;
        expected_hi = kAgcSpiShaderPgmHiEs;
        break;
    case 3:
        expected_lo = kAgcSpiShaderPgmLoVs;
        expected_hi = kAgcSpiShaderPgmHiVs;
        break;
    case 4:
        expected_lo = kAgcSpiShaderPgmLoGs;
        expected_hi = kAgcSpiShaderPgmHiGs;
        break;
    case 5:
        expected_lo = kAgcSpiShaderPgmLoHs;
        expected_hi = kAgcSpiShaderPgmHiHs;
        break;
    case 7:
        expected_lo = kAgcSpiShaderPgmLoLs;
        expected_hi = kAgcSpiShaderPgmHiLs;
        break;
    default:
        return false;
    }

    std::uint64_t lo_entry_address = 0;
    std::uint64_t hi_entry_address = 0;
    if (!find_agc_shader_program_registers(
            registers_address,
            register_count,
            expected_lo,
            expected_hi,
            lo_entry_address,
            hi_entry_address)) {
        std::uint32_t first_register = 0;
        if (!try_read_u32(registers_address, first_register)) {
            return false;
        }
        if ((shader_type == 4 &&
             (first_register == kAgcSpiShaderPgmRsrc1Gs ||
              first_register == kAgcSpiShaderPgmLoGs)) ||
            (shader_type == 5 &&
             (first_register == kAgcSpiShaderPgmRsrc1Hs ||
              first_register == kAgcSpiShaderPgmLoHs))) {
            return true;
        }
        return false;
    }

    const auto lo_value =
        static_cast<std::uint32_t>((code_address >> 8) & 0xFFFFFFFFULL);
    const auto hi_value =
        static_cast<std::uint32_t>((code_address >> 40) & 0xFFULL);
    return try_write_u32(lo_entry_address + 4, lo_value) &&
        try_write_u32(hi_entry_address + 4, hi_value);
}

std::uint32_t agc_pm4_length(std::uint32_t header) {
    return ((header >> 16) & 0x3FFFu) + 2u;
}

bool try_get_shader_address(
    const std::map<std::uint32_t, std::uint32_t>& registers,
    std::uint32_t lo_register,
    std::uint32_t hi_register,
    std::uint64_t& address) {
    const auto lo = registers.find(lo_register);
    const auto hi = registers.find(hi_register);
    if (lo == registers.end() || hi == registers.end()) {
        address = 0;
        return false;
    }

    address =
        (static_cast<std::uint64_t>(hi->second) << 40) |
        (static_cast<std::uint64_t>(lo->second) << 8);
    return address != 0;
}

bool try_get_agc_register(
    const std::map<std::uint32_t, std::uint32_t>& registers,
    std::uint32_t address,
    std::uint32_t& value) {
    const auto entry = registers.find(address);
    if (entry == registers.end()) {
        value = 0;
        return false;
    }
    value = entry->second;
    return true;
}

bool has_agc_shader_resource2(
    const std::map<std::uint32_t, std::uint32_t>& registers,
    std::uint32_t user_data_register) {
    return registers.contains(user_data_register - 1);
}

bool has_agc_user_data_range(
    const std::map<std::uint32_t, std::uint32_t>& registers,
    std::uint32_t user_data_register) {
    for (std::uint32_t index = 0; index < 16; ++index) {
        if (registers.contains(user_data_register + index)) {
            return true;
        }
    }
    return false;
}

std::uint32_t select_agc_export_user_data_register(
    const std::map<std::uint32_t, std::uint32_t>& registers) {
    for (const auto base : {
             kAgcGsUserDataRegister,
             kAgcEsUserDataRegister,
             kAgcVsUserDataRegister}) {
        if (has_agc_shader_resource2(registers, base)) {
            return base;
        }
    }
    for (const auto base : {
             kAgcGsUserDataRegister,
             kAgcEsUserDataRegister,
             kAgcVsUserDataRegister}) {
        if (has_agc_user_data_range(registers, base)) {
            return base;
        }
    }
    return kAgcEsUserDataRegister;
}

bool try_get_agc_shader_header(
    std::uint64_t code_address,
    std::uint64_t& header_address) {
    AcquireSRWLockShared(&g_agc_shader_header_lock);
    const auto entry = g_agc_shader_headers.find(code_address);
    header_address =
        entry == g_agc_shader_headers.end() ? 0 : entry->second;
    ReleaseSRWLockShared(&g_agc_shader_header_lock);
    return header_address != 0;
}

float agc_float(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

struct AgcDrawInfo {
    std::uint32_t vertex_count = 0;
    bool count_known = false;
    bool indexed = false;
    // Where the indices are, and how many the packet says fit there. Zero
    // when the packet does not carry a base of its own - DRAW_INDEX_OFFSET_2
    // works from a base set by an earlier packet, which is not read yet.
    std::uint64_t index_address = 0;
    std::uint32_t index_max = 0;
    std::uint32_t index_bytes = 2;
};

AgcDrawInfo decode_agc_draw_info(
    std::uint64_t packet_address,
    std::uint32_t packet_length,
    std::uint32_t op,
    std::uint32_t packet_register,
    std::uint64_t index_base,
    std::uint32_t index_bytes) {
    AgcDrawInfo info;
    std::uint64_t count_address = 0;
    if (op == kAgcItDrawIndexAuto && packet_length >= 3) {
        count_address = packet_address + 4;
    } else if (op == kAgcItDrawIndex2 && packet_length >= 6) {
        count_address = packet_address + 16;
        info.indexed = true;
        std::uint32_t base_lo = 0;
        std::uint32_t base_hi = 0;
        std::uint32_t max_size = 0;
        if (try_read_u32(packet_address + 8, base_lo) &&
            try_read_u32(packet_address + 12, base_hi)) {
            info.index_address =
                (static_cast<std::uint64_t>(base_hi) << 32) | base_lo;
        }
        if (try_read_u32(packet_address + 4, max_size)) {
            info.index_max = max_size;
        }
    } else if (op == kAgcItDrawIndexOffset2 && packet_length >= 5) {
        count_address = packet_address + 12;
        info.indexed = true;
        // Dword 1 is the largest index the buffer holds, dword 2 the index
        // this draw starts at - in indices, so the width matters.
        std::uint32_t max_size = 0;
        std::uint32_t index_offset = 0;
        if (try_read_u32(packet_address + 4, max_size)) {
            info.index_max = max_size;
        }
        if (index_base != 0 &&
            try_read_u32(packet_address + 8, index_offset)) {
            info.index_address =
                index_base +
                static_cast<std::uint64_t>(index_offset) * index_bytes;
        }
        info.index_bytes = index_bytes;
    } else if (op == kAgcItDrawIndexMultiAuto && packet_length >= 4) {
        std::uint32_t control = 0;
        if (try_read_u32(packet_address + 12, control)) {
            info.vertex_count = (control >> 21) & 0x7FFu;
            info.count_known = true;
        }
    } else if (op == kAgcItDrawIndexIndirect) {
        info.indexed = true;
    } else if (op == kAgcItNop &&
               packet_register == kAgcRDrawIndexAuto &&
               packet_length >= 2) {
        count_address = packet_address + 4;
    }
    if (count_address != 0) {
        info.count_known =
            try_read_u32(count_address, info.vertex_count);
    }
    return info;
}

struct AgcRenderTargetMilestone {
    std::uint32_t count = 0;
    std::uint32_t slot = 0;
    std::uint64_t address = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    std::uint32_t number_type = 0;
    std::uint32_t tile_mode = 0;
    std::uint32_t target_mask = 0;
    std::uint32_t shader_mask = 0;
    bool masks_known = false;
    bool masks_intersect = true;
    // CB_BLENDn_CONTROL for the target drawn to. Zero is blending off.
    std::uint32_t blend_control = 0;
};

constexpr std::uint32_t kAgcCbBlend0Control = 0x1E0;

// Every context register the stream has actually set, once. The coverage
// report lists what was seeded and never overwritten, which cannot
// distinguish a register the guest never touches from one it writes under
// a key we file wrongly. This lists the keys themselves.
void report_agc_context_keys(
    const std::map<std::uint32_t, std::uint32_t>& registers) {
    static volatile LONG done = 0;
    if (InterlockedExchange(&done, 1) != 0) {
        return;
    }
    std::string text;
    std::uint32_t shown = 0;
    for (const auto& entry : registers) {
        if (text.size() > 3600) {
            break;
        }
        char item[16] = {};
        std::snprintf(item, sizeof(item), " %X", entry.first);
        text += item;
        ++shown;
    }
    agc_trace(
        "agc.context_keys count=%zu shown=%u%s\n",
        registers.size(),
        shown,
        text.c_str());
}

AgcRenderTargetMilestone decode_agc_render_targets(
    const std::map<std::uint32_t, std::uint32_t>& registers) {
    report_agc_context_keys(registers);
    AgcRenderTargetMilestone result;
    std::uint32_t target_mask = 0;
    const bool has_target_mask =
        try_get_agc_register(registers, kAgcCbTargetMask, target_mask);
    std::uint32_t shader_mask = 0;
    const bool has_shader_mask =
        try_get_agc_register(registers, kAgcCbShaderMask, shader_mask);
    result.target_mask = target_mask;
    result.shader_mask = shader_mask;
    result.masks_known = has_target_mask && has_shader_mask;
    result.masks_intersect =
        !result.masks_known || (target_mask & shader_mask) != 0;
    for (std::uint32_t slot = 0; slot < kAgcColorTargetCount; ++slot) {
        const auto base_register =
            kAgcCbColor0Base + slot * kAgcCbColorRegisterStride;
        std::uint32_t base_low = 0;
        std::uint32_t base_high = 0;
        std::uint32_t attrib2 = 0;
        std::uint32_t attrib3 = 0;
        std::uint32_t info = 0;
        if (!try_get_agc_register(registers, base_register, base_low) ||
            !try_get_agc_register(
                registers, kAgcCbColor0BaseExt + slot, base_high) ||
            !try_get_agc_register(
                registers, kAgcCbColor0Attrib2 + slot, attrib2) ||
            !try_get_agc_register(
                registers, kAgcCbColor0Attrib3 + slot, attrib3) ||
            !try_get_agc_register(
                registers,
                kAgcCbColor0Info +
                    slot * kAgcCbColorRegisterStride,
                info)) {
            continue;
        }
        const auto address =
            (static_cast<std::uint64_t>(base_high & 0xFFu) << 40) |
            (static_cast<std::uint64_t>(base_low) << 8);
        const auto write_mask = (target_mask >> (slot * 4)) & 0xFu;
        if (address == 0 ||
            (has_target_mask && write_mask == 0)) {
            continue;
        }
        ++result.count;
        if (result.address != 0) {
            continue;
        }
        result.slot = slot;
        result.address = address;
        result.width = ((attrib2 >> 14) & 0x3FFFu) + 1;
        result.height = (attrib2 & 0x3FFFu) + 1;
        result.format = (info >> 2) & 0x1Fu;
        result.number_type = (info >> 8) & 0x7u;
        result.tile_mode = (attrib3 >> 14) & 0x1Fu;
    }
    std::uint32_t blend = 0;
    if (try_get_agc_register(
            registers, kAgcCbBlend0Control + result.slot, blend)) {
        result.blend_control = blend;
    }
    return result;
}

struct AgcScissorMilestone {
    bool known = false;
    bool full = false;
    int x = 0;
    int y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

void intersect_agc_scissor(
    const std::map<std::uint32_t, std::uint32_t>& registers,
    std::uint32_t tl_register,
    std::uint32_t br_register,
    int offset_x,
    int offset_y,
    int& left,
    int& top,
    int& right,
    int& bottom) {
    std::uint32_t tl = 0;
    std::uint32_t br = 0;
    if (!try_get_agc_register(registers, tl_register, tl) ||
        !try_get_agc_register(registers, br_register, br) ||
        (tl == 0 && br == 0)) {
        return;
    }
    left = std::max(left, static_cast<int>(tl & 0x7FFFu) + offset_x);
    top = std::max(top, static_cast<int>((tl >> 16) & 0x7FFFu) + offset_y);
    right = std::min(right, static_cast<int>(br & 0x7FFFu) + offset_x);
    bottom = std::min(
        bottom,
        static_cast<int>((br >> 16) & 0x7FFFu) + offset_y);
}

AgcScissorMilestone decode_agc_scissor(
    const std::map<std::uint32_t, std::uint32_t>& registers,
    std::uint32_t target_width,
    std::uint32_t target_height) {
    AgcScissorMilestone result;
    if (target_width == 0 || target_height == 0) {
        return result;
    }
    int left = 0;
    int top = 0;
    int right = static_cast<int>(target_width);
    int bottom = static_cast<int>(target_height);
    int window_offset_x = 0;
    int window_offset_y = 0;
    std::uint32_t window_offset = 0;
    if (try_get_agc_register(
            registers, kAgcPaScWindowOffset, window_offset)) {
        window_offset_x =
            static_cast<std::int16_t>(window_offset & 0xFFFFu);
        window_offset_y =
            static_cast<std::int16_t>(window_offset >> 16);
    }
    intersect_agc_scissor(
        registers,
        kAgcPaScScreenScissorTl,
        kAgcPaScScreenScissorBr,
        0,
        0,
        left,
        top,
        right,
        bottom);

    std::uint32_t window_tl = 0;
    const bool window_uses_offset =
        try_get_agc_register(
            registers, kAgcPaScWindowScissorTl, window_tl) &&
        (window_tl & 0x80000000u) == 0;
    intersect_agc_scissor(
        registers,
        kAgcPaScWindowScissorTl,
        kAgcPaScWindowScissorBr,
        window_uses_offset ? window_offset_x : 0,
        window_uses_offset ? window_offset_y : 0,
        left,
        top,
        right,
        bottom);

    std::uint32_t generic_tl = 0;
    const bool generic_uses_offset =
        try_get_agc_register(
            registers, kAgcPaScGenericScissorTl, generic_tl) &&
        (generic_tl & 0x80000000u) == 0;
    intersect_agc_scissor(
        registers,
        kAgcPaScGenericScissorTl,
        kAgcPaScGenericScissorBr,
        generic_uses_offset ? window_offset_x : 0,
        generic_uses_offset ? window_offset_y : 0,
        left,
        top,
        right,
        bottom);

    std::uint32_t mode_control = 0;
    const bool vport_scissor_enabled =
        !try_get_agc_register(
            registers, kAgcPaScModeCntl0, mode_control) ||
        ((mode_control >> 1) & 1u) != 0;
    if (vport_scissor_enabled) {
        intersect_agc_scissor(
            registers,
            kAgcPaScVportScissor0Tl,
            kAgcPaScVportScissor0Br,
            0,
            0,
            left,
            top,
            right,
            bottom);
    }
    left = std::clamp(left, 0, static_cast<int>(target_width));
    top = std::clamp(top, 0, static_cast<int>(target_height));
    right = std::clamp(right, left, static_cast<int>(target_width));
    bottom = std::clamp(bottom, top, static_cast<int>(target_height));
    if (left == static_cast<int>(target_width) &&
        top == static_cast<int>(target_height) &&
        right == left &&
        bottom == top) {
        left = 0;
        top = 0;
        right = static_cast<int>(target_width);
        bottom = static_cast<int>(target_height);
    }
    result.known = true;
    result.full =
        left == 0 && top == 0 &&
        right == static_cast<int>(target_width) &&
        bottom == static_cast<int>(target_height);
    result.x = left;
    result.y = top;
    result.width = static_cast<std::uint32_t>(right - left);
    result.height = static_cast<std::uint32_t>(bottom - top);
    return result;
}

struct AgcViewportMilestone {
    bool known = false;
    // Set when the register-derived rectangle was discarded as stale and the
    // target extent used instead; worth seeing in the draw trace.
    bool derived_from_target = false;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float min_depth = 0.0f;
    float max_depth = 1.0f;
};

AgcViewportMilestone decode_agc_viewport(
    const std::map<std::uint32_t, std::uint32_t>& registers,
    const AgcRenderTargetMilestone& target,
    const AgcScissorMilestone& scissor) {
    AgcViewportMilestone result;
    if (target.width == 0 || target.height == 0) {
        return result;
    }
    std::uint32_t z_min_bits = 0;
    std::uint32_t z_max_bits = 0;
    if (try_get_agc_register(
            registers, kAgcPaScVportZMin0, z_min_bits) &&
        try_get_agc_register(
            registers, kAgcPaScVportZMax0, z_max_bits)) {
        const auto z_min = agc_float(z_min_bits);
        const auto z_max = agc_float(z_max_bits);
        if (std::isfinite(z_min) &&
            std::isfinite(z_max) &&
            z_max > z_min) {
            result.min_depth = z_min;
            result.max_depth = z_max;
        }
    }

    std::uint32_t x_scale_bits = 0;
    std::uint32_t x_offset_bits = 0;
    std::uint32_t y_scale_bits = 0;
    std::uint32_t y_offset_bits = 0;
    if (try_get_agc_register(
            registers, kAgcPaClVportXScale, x_scale_bits) &&
        try_get_agc_register(
            registers, kAgcPaClVportXOffset, x_offset_bits) &&
        try_get_agc_register(
            registers, kAgcPaClVportYScale, y_scale_bits) &&
        try_get_agc_register(
            registers, kAgcPaClVportYOffset, y_offset_bits)) {
        const auto x_scale = agc_float(x_scale_bits);
        const auto x_offset = agc_float(x_offset_bits);
        const auto y_scale = agc_float(y_scale_bits);
        const auto y_offset = agc_float(y_offset_bits);
        if (std::isfinite(x_scale) &&
            std::isfinite(x_offset) &&
            std::isfinite(y_scale) &&
            std::isfinite(y_offset) &&
            x_scale > 0.0f &&
            y_scale != 0.0f) {
            result.known = true;
            result.x = x_offset - x_scale;
            result.y = y_offset - y_scale;
            result.width = x_scale * 2.0f;
            result.height = y_scale * 2.0f;
            const auto left =
                std::min(result.x, result.x + result.width);
            const auto right =
                std::max(result.x, result.x + result.width);
            const auto top =
                std::min(result.y, result.y + result.height);
            const auto bottom =
                std::max(result.y, result.y + result.height);
            // The viewport registers are only this draw's if the rectangle
            // fits the target. The indirect register blocks carry a viewport
            // for some targets and not for others, so when one is missing the
            // previous draw's viewport survives in the register map - which is
            // how a 120x67 bloom target ended up rasterising through a
            // 1920x1080 viewport while its scissor correctly said 120x67.
            //
            // A full-target scissor with a rectangle that does not cover the
            // target is that stale case: take the target extent instead,
            // keeping the orientation the registers asked for. This subsumes
            // the rule that used to live here, which doubled the rectangle and
            // so only ever corrected targets that happened to be exactly twice
            // the stale viewport.
            const auto covers_target =
                std::fabs(left) <= 0.5f &&
                std::fabs(top) <= 0.5f &&
                std::fabs(
                    right -
                    static_cast<float>(target.width)) <= 0.5f &&
                std::fabs(
                    bottom -
                    static_cast<float>(target.height)) <= 0.5f;
            if (scissor.full && !covers_target) {
                const auto flipped = result.height < 0.0f;
                const auto width = static_cast<float>(target.width);
                const auto height = static_cast<float>(target.height);
                result.x = 0.0f;
                result.width = width;
                result.y = flipped ? height : 0.0f;
                result.height = flipped ? -height : height;
                result.derived_from_target = true;
            }
            return result;
        }
    }
    result.known = true;
    if (scissor.known && !scissor.full) {
        result.x = static_cast<float>(scissor.x);
        result.y = static_cast<float>(scissor.y);
        result.width = static_cast<float>(scissor.width);
        result.height = static_cast<float>(scissor.height);
    } else {
        result.width = static_cast<float>(target.width);
        result.height = static_cast<float>(target.height);
    }
    return result;
}

// Defined below, with the registry it consults.
void free_shader_allocation(void* allocation);

void free_agc_shader_result(Ps5GpuShaderResult& result) {
    free_shader_allocation(result.spirv);
    free_shader_allocation(result.resource_manifest);
    result = {};
}

// Allocations the native producer owns. A shader result is freed by
// whoever received it, with the bridge's allocator, and a pointer alone
// does not say which producer made it - so the ones made here are kept and
// recognised on the way back. Getting this wrong is not a failure that
// reports itself: it is the bridge's allocator handed a vector's buffer.
std::mutex g_native_allocations_mutex;
std::map<const void*, std::vector<std::uint8_t>> g_native_allocations;

std::uint8_t* keep_native_allocation(std::vector<std::uint8_t> bytes) {
    if (bytes.empty()) {
        return nullptr;
    }
    std::lock_guard guard(g_native_allocations_mutex);
    const auto* key = bytes.data();
    auto& stored = g_native_allocations[key];
    stored = std::move(bytes);
    // Whether results are handed back: a leak here grows with every
    // translation, and the scene after the intro makes hundreds a frame.
    static std::uint64_t kept = 0;
    if (++kept % 20000 == 0) {
        std::uint64_t total = 0;
        for (const auto& [pointer, allocation] : g_native_allocations) {
            (void)pointer;
            total += allocation.size();
        }
        agc_trace("agc.native_allocations kept=%llu live=%zu MB=%llu\n",
                  static_cast<unsigned long long>(kept),
                  g_native_allocations.size(),
                  static_cast<unsigned long long>(total >> 20));
    }
    return stored.data();
}

// A module kept as the words translation produced, handed out as bytes.
// Copying two hundred kilobytes of SPIR-V into a byte vector for every stage
// of every state was a copy and an allocation too many, a few hundred times
// a frame.
std::map<const void*, std::vector<std::uint32_t>> g_native_word_allocations;

std::uint8_t* keep_native_words(std::vector<std::uint32_t> words) {
    if (words.empty()) {
        return nullptr;
    }
    std::lock_guard guard(g_native_allocations_mutex);
    const auto* key = words.data();
    auto& stored = g_native_word_allocations[key];
    stored = std::move(words);
    return reinterpret_cast<std::uint8_t*>(stored.data());
}

void free_shader_allocation(void* allocation) {
    if (allocation == nullptr) {
        return;
    }
    {
        std::lock_guard guard(g_native_allocations_mutex);
        if (g_native_allocations.erase(allocation) != 0 ||
            g_native_word_allocations.erase(allocation) != 0) {
            return;
        }
    }
    ps5rt_gpu_free(allocation);
}

// Translates one graphics stage, filling the result the way the bridge
// would. The user data is read from the registers the draw carried, from
// the base the stage uses - the two stages keep theirs in different
// places, and a shader given the other one resolves nothing.
// What the last graphics translation found, so the trace can say whether
// a stage that declared nothing had nothing to declare or failed to
// resolve it.
thread_local std::uint32_t last_graphics_user_data_count = 0;
thread_local std::uint32_t last_graphics_reuse = 0;
// PS5RT_DECLINE_SHADERS=0xADDR,0xADDR: graphics shaders refused outright,
// so draws using them are dropped. For finding which one hangs the GPU.
__attribute__((noinline)) bool graphics_shader_declined(std::uint64_t address) {
    static const std::vector<std::uint64_t> declined = [] {
        std::vector<std::uint64_t> list;
        const auto* value = std::getenv("PS5RT_DECLINE_SHADERS");
        while (value != nullptr && *value != '\0') {
            char* end = nullptr;
            const auto parsed = std::strtoull(value, &end, 0);
            if (end == value) {
                break;
            }
            list.push_back(parsed);
            value = *end == ',' ? end + 1 : end;
        }
        return list;
    }();
    return std::find(declined.begin(), declined.end(), address) !=
        declined.end();
}

// Shaders the native translator declined and the managed one then failed
// as well, by address and the word decoding stopped at. Both fail the same
// way every time, and the managed attempt cost 6-8ms: a pixel shader with a
// v_fma_mix_f32 in it was tried a thousand times over, the main thread's
// second largest cost in the scene after the intro.
std::mutex g_graphics_hopeless_lock;
std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>>
    g_graphics_hopeless;

__attribute__((noinline)) bool graphics_known_hopeless(
    std::uint64_t address, std::uint32_t pc, std::uint32_t word) {
    std::lock_guard<std::mutex> guard(g_graphics_hopeless_lock);
    return g_graphics_hopeless.count({address, pc, word}) != 0;
}

__attribute__((noinline)) void remember_graphics_hopeless(
    std::uint64_t address, std::uint32_t pc, std::uint32_t word) {
    std::lock_guard<std::mutex> guard(g_graphics_hopeless_lock);
    g_graphics_hopeless.insert({address, pc, word});
}

// Why the last graphics translation was declined: 1 undecoded, 2 no
// complete plan, 4 a duplicated block, 8 no words.
thread_local std::uint32_t last_graphics_declined = 0;
thread_local std::uint32_t last_graphics_failed_pc = 0;
thread_local std::uint32_t last_graphics_failed_word = 0;
// How often a translation's module came from an earlier one, and each key
// whose modules turned out to differ - which is the key missing something
// emission reads.
void note_emission_reuse(const ps5gen5::TranslationResult& translated,
                         std::uint64_t address, const char* kind) {
    static std::atomic<std::uint32_t> counts[5] = {};
    static std::atomic<std::uint32_t> total{0};
    const auto status = translated.emission_reuse < 5
        ? translated.emission_reuse : 0u;
    counts[status].fetch_add(1, std::memory_order_relaxed);
    if (status == 4) {
        agc_trace("agc.emission_mismatch kind=%s address=0x%016llX\n", kind,
                  static_cast<unsigned long long>(address));
    }
    const auto seen = total.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen % 500 == 0) {
        agc_trace(
            "agc.emission_reuse translations=%u unkeyed=%u stored=%u "
            "reused=%u verified=%u mismatched=%u\n",
            seen, counts[0].load(), counts[1].load(), counts[2].load(),
            counts[3].load(), counts[4].load());
    }
}
thread_local std::uint32_t last_graphics_paths = 0;
thread_local std::uint64_t last_graphics_scalar_ns = 0;
thread_local std::uint64_t last_graphics_cfg_ns = 0;
thread_local bool last_graphics_exhausted = false;
thread_local std::uint32_t last_graphics_loads = 0;
thread_local std::uint32_t last_graphics_loads_resolved = 0;
thread_local std::uint32_t last_graphics_images = 0;
thread_local std::uint32_t last_graphics_images_resolved = 0;
thread_local std::uint32_t last_graphics_translated = 0;
thread_local std::uint32_t last_graphics_skipped = 0;
thread_local std::uint32_t last_graphics_descriptors_unknown = 0;
thread_local std::uint32_t last_graphics_descriptors_rejected = 0;
thread_local std::uint32_t last_graphics_reads = 0;
thread_local std::uint32_t last_graphics_reads_failed = 0;
thread_local std::uint32_t last_graphics_subroutine_calls = 0;

// Marks a context register passed among a stage's shader registers.
constexpr std::uint32_t kPixelInputControlTag = 0x80000000u;

// The guest words a graphics state's translation baked in - descriptors
// and pointers it read from memory - collected over both stages of the
// state being compiled.
thread_local std::vector<std::pair<std::uint64_t, std::uint32_t>>
    g_graphics_dependency_reads;

// A state is cached against its registers, and its translation carries
// the buffer addresses it found by reading memory through them. The intro
// video's plane reads its constants through a pointer in a per-frame ring;
// when a frame laid the ring out differently, the same registers came
// back, the state kept the old address, and the plane drew from another
// draw's matrix - a black frame, a third of them once the title ran fast.
// So the words behind a state's registers are part of what it is: their
// hash rides along as a register of its own, and a different layout is a
// different state with its own translation.
constexpr std::uint32_t kDependencyHashTag = 0x40000000u;
// Layouts one set of registers may have before the rest share the first
// one, so a state whose words change every frame does not compile a new
// shader every frame.
constexpr std::size_t kMaximumDependencyLayouts = 16;
struct GraphicsDependencies {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> reads;
    std::uint64_t untagged_hash = 0;
    bool known = false;
    std::set<std::uint64_t> layouts;
};
std::mutex g_graphics_dependencies_mutex;
std::unordered_map<std::uint64_t, GraphicsDependencies>
    g_graphics_dependencies;

std::uint64_t hash_dependency_words(
    const std::vector<std::pair<std::uint64_t, std::uint32_t>>& reads) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& [address, count] : reads) {
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint32_t word = 0;
            if (!try_read_u32(address + index * 4ull, word)) {
                word = 0xDEADBEEFu;
            }
            hash = (hash ^ word) * 1099511628211ULL;
        }
    }
    return hash;
}

bool graphics_dependencies_enabled() {
    static const auto enabled =
        std::getenv("PS5RT_NO_GRAPHICS_DEPENDENCIES") == nullptr;
    return enabled;
}

// A shader's code, read from guest memory a block at a time and kept for
// the translation. The translator reads every word several times - the
// control-flow graph, the scalar walk (once per path, and a big shader has
// hundreds), the emission - and each read was a guest range check and a
// four-byte copy: a fifth of each translation, and after the intro the
// title translates for seconds a frame. Code does not change under a
// translation. A block that cannot be read whole is read a word at a time,
// and a word that cannot be read is zero, as before.
class GuestCodeReader {
public:
    explicit GuestCodeReader(std::uint64_t base) : base_(base) {}

    std::uint32_t operator()(std::uint32_t index) const {
        if (index >= words_.size() && !exhausted_) {
            fill(index);
        }
        if (index < words_.size() && index >= read_extent_) {
            read_extent_ = index + 1;
        }
        return index < words_.size() ? words_[index] : 0;
    }

    // The words read so far, from the first to the highest asked for.
    std::vector<std::uint32_t> read_words() const {
        return std::vector<std::uint32_t>(
            words_.begin(), words_.begin() + read_extent_);
    }

private:
    static constexpr std::size_t kBlockWords = 1024;
    static constexpr std::size_t kMaximumWords = std::size_t{1} << 20;

    void fill(std::uint32_t index) const {
        while (words_.size() <= index && !exhausted_) {
            const auto start = words_.size();
            if (start >= kMaximumWords) {
                exhausted_ = true;
                return;
            }
            words_.resize(start + kBlockWords);
            if (try_read_process_bytes(
                    base_ + start * sizeof(std::uint32_t),
                    words_.data() + start,
                    kBlockWords * sizeof(std::uint32_t))) {
                continue;
            }
            for (std::size_t offset = 0; offset < kBlockWords; ++offset) {
                std::uint32_t word = 0;
                if (!try_read_u32(
                        base_ + (start + offset) * sizeof(std::uint32_t),
                        word)) {
                    words_.resize(start + offset);
                    exhausted_ = true;
                    return;
                }
                words_[start + offset] = word;
            }
        }
    }

    std::uint64_t base_;
    mutable std::vector<std::uint32_t> words_;
    mutable bool exhausted_ = false;
    mutable std::size_t read_extent_ = 0;
};

// What a shader's words alone decide - its graph, structure and emission
// order - kept by the address the words are at and checked against them.
// The scene after the intro translates the same few dozen shaders three
// hundred times a frame, and building these again each time was a third of
// every translation. PS5RT_CODE_FRONT_CACHE=0 turns it off.
struct CachedCodeFront {
    std::vector<std::uint32_t> code;
    ps5gen5::TranslationFrontEnd front;
    // Scalar walks of this code, most recently useful first, each good for
    // any draw whose user data and memory replay_scalar accepts. By how
    // many words of user data and where they land.
    std::mutex walks_mutex;
    std::map<std::uint64_t,
             std::vector<std::shared_ptr<const ps5gen5::ScalarEvaluation>>>
        walks;
};
constexpr std::size_t kMaximumKeptWalks = 16;
std::atomic<std::uint64_t> g_scalar_replay_hits{0};
std::atomic<std::uint64_t> g_scalar_replay_misses{0};
std::atomic<std::uint64_t> g_scalar_replay_verified{0};
std::atomic<std::uint64_t> g_scalar_replay_mismatched{0};

// PS5RT_SCALAR_REPLAY=0 walks every time; PS5RT_SCALAR_REPLAY_VERIFY=N
// walks one replay in N as well and compares (64 unless set, 0 for never).
bool scalar_replay_enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("PS5RT_SCALAR_REPLAY");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

std::uint64_t scalar_replay_verify_every() {
    static const std::uint64_t every = [] {
        const auto* value = std::getenv("PS5RT_SCALAR_REPLAY_VERIFY");
        return value == nullptr ? std::uint64_t{64}
                                : std::strtoull(value, nullptr, 0);
    }();
    return every;
}

// Whether two walks found the same things: what phase two of a translation
// reads from one.
bool same_scalar_findings(
    const ps5gen5::ScalarEvaluation& left,
    const ps5gen5::ScalarEvaluation& right) {
    if (left.loads.size() != right.loads.size() ||
        left.images.size() != right.images.size() ||
        left.buffers.size() != right.buffers.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.loads.size(); ++index) {
        const auto& a = left.loads[index];
        const auto& b = right.loads[index];
        if (a.pc != b.pc || a.address_known != b.address_known ||
            a.base_known != b.base_known || a.dword_count != b.dword_count ||
            a.destination != b.destination ||
            a.offset_register != b.offset_register ||
            a.byte_offset != b.byte_offset ||
            (a.base_known &&
             (a.base_address != b.base_address ||
              a.region_size != b.region_size)) ||
            (a.address_known && a.address != b.address)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.images.size(); ++index) {
        const auto& a = left.images[index];
        const auto& b = right.images[index];
        if (a.pc != b.pc || a.descriptor_known != b.descriptor_known ||
            a.sampler_known != b.sampler_known ||
            a.missing_mask != b.missing_mask ||
            a.descriptor != b.descriptor || a.sampler != b.sampler) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.buffers.size(); ++index) {
        const auto& a = left.buffers[index];
        const auto& b = right.buffers[index];
        if (a.pc != b.pc || a.descriptor_known != b.descriptor_known ||
            a.descriptor != b.descriptor) {
            return false;
        }
    }
    return true;
}
std::mutex g_code_fronts_mutex;
std::unordered_map<std::uint64_t, std::shared_ptr<CachedCodeFront>>
    g_code_fronts;

std::shared_ptr<CachedCodeFront> find_code_front(std::uint64_t address) {
    static const bool enabled = [] {
        const auto* value = std::getenv("PS5RT_CODE_FRONT_CACHE");
        return value == nullptr || value[0] != '0';
    }();
    if (!enabled) {
        return nullptr;
    }
    std::shared_ptr<CachedCodeFront> cached;
    {
        std::lock_guard guard(g_code_fronts_mutex);
        const auto found = g_code_fronts.find(address);
        if (found != g_code_fronts.end()) {
            cached = found->second;
        }
    }
    if (cached == nullptr || cached->code.empty()) {
        return nullptr;
    }
    // The same address can hold other words later.
    static thread_local std::vector<std::uint32_t> current;
    current.resize(cached->code.size());
    const auto bytes = current.size() * sizeof(std::uint32_t);
    if (!try_read_process_bytes(address, current.data(), bytes) ||
        std::memcmp(current.data(), cached->code.data(), bytes) != 0) {
        return nullptr;
    }
    return cached;
}

bool translate_graphics_stage_natively(
    const Ps5GpuShaderRequest& request,
    Ps5GpuShaderResult& result) {
    const auto register_value =
        [&](std::uint32_t address, std::uint32_t& value) {
            if (request.registers == nullptr) {
                return false;
            }
            for (std::uint32_t entry = 0; entry < request.register_count;
                 ++entry) {
                if (request.registers[entry].register_address == address) {
                    value = request.registers[entry].value;
                    return true;
                }
            }
            return false;
        };

    // How many user data registers this stage has, from the same field
    // compute reads it from - the stage's resource descriptor, which sits
    // one register below its user data. Taking sixteen regardless reads
    // registers the stage never set.
    // As many as the draw actually set, up to the sixteen the encoding
    // allows. The stage's resource descriptor declares a count, and using
    // it made resolution worse rather than better - 26 percent of loads
    // against 31 - so the declaration is not what the shader goes by here.
    // What the registers hold is.
    // Every unresolved load in a graphics stage is a buffer load, and the
    // descriptors they want sit at s16, s20, s24 and s28 - above the
    // sixteen user data registers this used to read, and written by
    // nothing the walk can see. So they are user data too, and there are
    // more of them than sixteen.
    const auto user_data_count = std::uint32_t{32};

    std::vector<std::uint32_t> user_data;
    for (std::uint32_t index = 0; index < user_data_count; ++index) {
        std::uint32_t value = 0;
        if (!register_value(
                request.user_data_base_register + index, value)) {
            // Stop rather than skip. A missing register used to be left
            // out of the vector, which moves every later one down a slot -
            // so a shader reading its fourth user register got its fifth,
            // and every address computed from it was wrong in a way
            // nothing downstream could notice.
            break;
        }
        user_data.push_back(value);
    }
    std::uint32_t pixel_input_control[32] = {};
    bool pixel_input_control_known = false;
    for (std::uint32_t index = 0; index < 32; ++index) {
        std::uint32_t value = 0;
        if (register_value(
                kPixelInputControlTag | (kAgcSpiPsInputCntl0 + index),
                value)) {
            pixel_input_control[index] = value;
            pixel_input_control_known = true;
        } else {
            pixel_input_control[index] = index;
        }
    }
    const auto stage = request.stage == PS5GPU_STAGE_PIXEL
        ? ps5gen5::StageKind::Pixel
        : ps5gen5::StageKind::Vertex;
    const GuestCodeReader code_reader(request.shader_address);
    auto code_front = find_code_front(request.shader_address);
    const auto code_front_cached = code_front != nullptr;
    if (!code_front_cached) {
        code_front = std::make_shared<CachedCodeFront>();
    }
    const auto read_guest_word =
        [&](std::uint64_t address, std::uint32_t& word) {
            return try_read_u32(address, word);
        };
    // A walk of this code that still stands for this draw, if one is kept.
    ps5gen5::ScalarEvaluation scalar_walk;
    bool scalar_replayed = false;
    const auto walks_key =
        (static_cast<std::uint64_t>(request.user_data_scalar_register_base)
         << 32) | user_data.size();
    if (code_front_cached && scalar_replay_enabled()) {
        std::vector<std::shared_ptr<const ps5gen5::ScalarEvaluation>> kept;
        {
            std::lock_guard guard(code_front->walks_mutex);
            const auto found = code_front->walks.find(walks_key);
            if (found != code_front->walks.end()) {
                kept = found->second;
            }
        }
        for (std::size_t index = 0; index < kept.size(); ++index) {
            ps5gen5::ScalarReplayRefusal refusal;
            if (!ps5gen5::replay_scalar(
                    *kept[index], user_data, read_guest_word, scalar_walk,
                    &refusal)) {
                static std::atomic<std::uint32_t> shown{0};
                const auto refused =
                    shown.fetch_add(1, std::memory_order_relaxed);
                if (refused % 97 == 0 && refused < 97 * 800) {
                    agc_trace(
                        "agc.scalar_replay_refused stage=%d "
                        "shader=0x%016llX kept=%zu/%zu reason=%u pc=0x%X "
                        "index=%u was=0x%08X now=0x%08X\n",
                        static_cast<int>(request.stage),
                        static_cast<unsigned long long>(
                            request.shader_address),
                        index, kept.size(), refusal.reason, refusal.pc,
                        refusal.index, refusal.was, refusal.now);
                }
                continue;
            }
            scalar_replayed = true;
            if (index != 0) {
                // To the front, for the next draw like this one.
                std::lock_guard guard(code_front->walks_mutex);
                auto& walks = code_front->walks[walks_key];
                const auto at =
                    std::find(walks.begin(), walks.end(), kept[index]);
                if (at != walks.end()) {
                    std::rotate(walks.begin(), at, at + 1);
                }
            }
            break;
        }
        if (scalar_replayed) {
            const auto hits =
                g_scalar_replay_hits.fetch_add(1, std::memory_order_relaxed) +
                1;
            const auto every = scalar_replay_verify_every();
            if (every != 0 && hits % every == 0) {
                g_scalar_replay_verified.fetch_add(
                    1, std::memory_order_relaxed);
                auto walked = ps5gen5::evaluate_scalar(
                    0, user_data, request.user_data_scalar_register_base,
                    code_reader, read_guest_word, request.shader_address);
                if (!same_scalar_findings(walked, scalar_walk)) {
                    const auto count = g_scalar_replay_mismatched.fetch_add(
                        1, std::memory_order_relaxed);
                    if (count < 32) {
                        agc_trace(
                            "agc.scalar_replay_mismatch stage=%d "
                            "shader=0x%016llX loads=%zu/%zu images=%zu/%zu "
                            "buffers=%zu/%zu\n",
                            static_cast<int>(request.stage),
                            static_cast<unsigned long long>(
                                request.shader_address),
                            walked.loads.size(), scalar_walk.loads.size(),
                            walked.images.size(), scalar_walk.images.size(),
                            walked.buffers.size(),
                            scalar_walk.buffers.size());
                    }
                    scalar_walk = std::move(walked);
                }
            }
        } else {
            g_scalar_replay_misses.fetch_add(1, std::memory_order_relaxed);
        }
    }
    auto translated = ps5gen5::translate_shader(
        0,
        code_reader,
        user_data,
        // Where the user data lands in the scalar registers, which is not
        // register zero for a vertex shader - the request says so and the
        // walk has to agree, or every address it computes starts from a
        // register the hardware never wrote.
        request.user_data_scalar_register_base,
        read_guest_word,
        request.shader_address,
        // The same diagnostics compute honours: stores, images or buffers
        // left out of the module, to find which part of a stage breaks it.
        native_shader_producer_omits_stores(),
        native_shader_producer_omits_images(),
        native_shader_producer_omits_buffers(),
        stage,
        // Where this stage's resources start in the set it shares with
        // the other one - the caller probes both stages first and then
        // places the pixel stage's after the vertex stage's.
        static_cast<std::uint32_t>(
            std::max<std::int32_t>(request.global_buffer_base, 0)),
        request.total_global_buffer_count,
        static_cast<std::uint32_t>(
            std::max<std::int32_t>(request.image_binding_base, 0)),
        request.pixel_input_enable,
        request.pixel_input_address,
        pixel_input_control_known ? pixel_input_control : nullptr,
        64,
        1,
        1,
        -1,
        -1,
        -1,
        &code_front->front,
        &scalar_walk,
        scalar_replayed);
    if (!code_front_cached && code_front->front.has_code) {
        code_front->code = code_reader.read_words();
        std::lock_guard guard(g_code_fronts_mutex);
        g_code_fronts[request.shader_address] = code_front;
    }
    if (!scalar_replayed && scalar_replay_enabled() &&
        code_front->front.has_code && scalar_walk.replayable &&
        !scalar_walk.exhausted) {
        auto kept = std::make_shared<const ps5gen5::ScalarEvaluation>(
            std::move(scalar_walk));
        std::lock_guard guard(code_front->walks_mutex);
        auto& walks = code_front->walks[walks_key];
        walks.insert(walks.begin(), std::move(kept));
        if (walks.size() > kMaximumKeptWalks) {
            walks.pop_back();
        }
    }
    {
        const auto total =
            g_scalar_replay_hits.load(std::memory_order_relaxed) +
            g_scalar_replay_misses.load(std::memory_order_relaxed);
        static std::atomic<std::uint64_t> reported{0};
        auto last = reported.load(std::memory_order_relaxed);
        if (total >= last + 8192 &&
            reported.compare_exchange_strong(last, total)) {
            agc_trace(
                "agc.scalar_replay hits=%llu misses=%llu verified=%llu "
                "mismatched=%llu\n",
                static_cast<unsigned long long>(
                    g_scalar_replay_hits.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_scalar_replay_misses.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_scalar_replay_verified.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_scalar_replay_mismatched.load(
                        std::memory_order_relaxed)));
        }
    }
    last_graphics_paths = translated.scalar_paths;
    last_graphics_reuse = translated.emission_reuse;
    note_emission_reuse(translated, request.shader_address, "graphics");
    last_graphics_scalar_ns = translated.scalar_ns;
    last_graphics_cfg_ns = translated.cfg_ns;
    {
        // Where a graphics translation's time goes, in total: the graph,
        // the scalar walk, and everything after them.
        static std::atomic<std::uint64_t> total_scalar_ns{0};
        static std::atomic<std::uint64_t> total_cfg_ns{0};
        static std::atomic<std::uint64_t> total_count{0};
        total_scalar_ns.fetch_add(
            translated.scalar_ns, std::memory_order_relaxed);
        total_cfg_ns.fetch_add(translated.cfg_ns, std::memory_order_relaxed);
        const auto count =
            total_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count % 4096 == 0) {
            agc_trace(
                "agc.graphics_translate_parts count=%llu cfg_ms=%llu "
                "scalar_ms=%llu\n",
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(
                    total_cfg_ns.load(std::memory_order_relaxed) / 1000000),
                static_cast<unsigned long long>(
                    total_scalar_ns.load(std::memory_order_relaxed) /
                    1000000));
        }
    }
    last_graphics_exhausted = translated.scalar_exhausted;
    g_graphics_dependency_reads.insert(
        g_graphics_dependency_reads.end(),
        translated.dependency_reads.begin(),
        translated.dependency_reads.end());
    for (const auto& line : translated.load_trace) {
        agc_trace("%s\n", line.c_str());
    }
    // Which instructions cost this stage a register it knew, and which
    // it could not translate. The same two counts that found the compute
    // gaps, asked of graphics.
    {
        static std::atomic<std::uint32_t> shown{0};
        if (shown.fetch_add(1, std::memory_order_relaxed) < 80) {
            for (const auto& [name, count] : translated.unknown_by_name) {
                agc_trace(
                    "agc.graphics_unknown stage=%d name=%s count=%u\n",
                    static_cast<int>(request.stage),
                    name.c_str(),
                    count);
            }
            for (const auto& [name, count] : translated.skipped_by_name) {
                agc_trace(
                    "agc.graphics_skipped stage=%d name=%s count=%u\n",
                    static_cast<int>(request.stage),
                    name.c_str(),
                    count);
            }
        }
    }
    {
        static std::atomic<std::uint32_t> shown{0};
        if (shown.fetch_add(1, std::memory_order_relaxed) < 60 ||
            std::getenv("PS5RT_NATIVE_SHADER_DUMP") != nullptr) {
            if (translated.subroutine_calls != 0) {
                agc_trace(
                    "agc.graphics_subroutine stage=%d calls=%u\n",
                    static_cast<int>(request.stage),
                    translated.subroutine_calls);
            }
            for (const auto& [target, count] :
                 translated.exports_by_target) {
                agc_trace(
                    "agc.graphics_export stage=%d target=%u count=%u\n",
                    static_cast<int>(request.stage),
                    target,
                    count);
            }
            for (const auto& [reg, count] :
                 translated.buffer_descriptor_unknown_at) {
                agc_trace(
                    "agc.graphics_buffer_missing stage=%d reg=s%u "
                    "count=%u\n",
                    static_cast<int>(request.stage),
                    reg,
                    count);
            }
            // The guest code and user data behind a stage whose V# did
            // not resolve, to be read offline with the same decoder.
            const auto* dump_directory =
                std::getenv("PS5RT_NATIVE_SHADER_DUMP");
            if (dump_directory != nullptr) {
                // Not stopped at the first s_endpgm: compilers put tails
                // after it that branches reach, and a dump cut there does
                // not decode.
                std::vector<std::uint32_t> code;
                for (std::uint32_t index = 0; index < 8192; ++index) {
                    std::uint32_t word = 0;
                    if (!try_read_u32(
                            request.shader_address +
                                static_cast<std::uint64_t>(index) * 4,
                            word)) {
                        break;
                    }
                    code.push_back(word);
                }
                char name[512] = {};
                std::snprintf(
                    name, sizeof(name), "%s/stage%d_%016llX.code",
                    dump_directory, static_cast<int>(request.stage),
                    static_cast<unsigned long long>(request.shader_address));
                if (auto* file = std::fopen(name, "wb")) {
                    std::fwrite(code.data(), 4, code.size(), file);
                    std::fclose(file);
                }
                std::snprintf(
                    name, sizeof(name), "%s/stage%d_%016llX.userdata",
                    dump_directory, static_cast<int>(request.stage),
                    static_cast<unsigned long long>(request.shader_address));
                if (auto* file = std::fopen(name, "wb")) {
                    std::fwrite(user_data.data(), 4, user_data.size(), file);
                    std::fclose(file);
                }
            }
            for (const auto& detail : translated.buffer_missing_detail) {
                agc_trace(
                    "agc.graphics_buffer_detail stage=%d shader=0x%016llX "
                    "user_data=%zu %s\n",
                    static_cast<int>(request.stage),
                    static_cast<unsigned long long>(request.shader_address),
                    user_data.size(),
                    detail.c_str());
            }
            for (const auto& [packed, count] :
                 translated.typed_formats_unhandled) {
                agc_trace(
                    "agc.graphics_typed_format stage=%d data=%u number=%u "
                    "count=%u\n",
                    static_cast<int>(request.stage),
                    packed >> 4,
                    packed & 0xFu,
                    count);
            }
            for (const auto& [name, count] :
                 translated.unresolved_loads_by_name) {
                agc_trace(
                    "agc.graphics_load_unresolved stage=%d name=%s "
                    "count=%u\n",
                    static_cast<int>(request.stage),
                    name.c_str(),
                    count);
            }
            for (const auto& [reg, count] :
                 translated.descriptor_unknown_at) {
                agc_trace(
                    "agc.graphics_descriptor_missing stage=%d reg=s%u "
                    "count=%u\n",
                    static_cast<int>(request.stage),
                    reg,
                    count);
            }
        }
    }
    last_graphics_user_data_count =
        static_cast<std::uint32_t>(user_data.size());
    last_graphics_loads = translated.scalar_loads;
    last_graphics_loads_resolved = translated.scalar_loads_resolved;
    last_graphics_images = translated.images_seen;
    last_graphics_images_resolved = translated.images_resolved;
    last_graphics_translated = translated.instructions_translated;
    last_graphics_skipped = translated.instructions_skipped;
    last_graphics_descriptors_unknown = translated.descriptors_unknown;
    last_graphics_descriptors_rejected = translated.descriptors_rejected;
    last_graphics_reads = translated.memory_reads;
    last_graphics_reads_failed = translated.memory_reads_failed;
    last_graphics_subroutine_calls = translated.subroutine_calls;
    last_graphics_declined = (translated.cfg_decoded ? 0u : 1u) |
        (translated.plan_complete ? 0u : 2u) |
        (translated.plan_duplicated_block ? 4u : 0u) |
        (translated.words.empty() ? 8u : 0u);
    last_graphics_failed_pc = translated.cfg_failed_pc;
    last_graphics_failed_word = translated.cfg_failed_word;
    if (last_graphics_declined != 0) {
        return false;
    }
    auto manifest_bytes = ps5gen5::build_manifest(
        request.stage,
        translated.manifest_buffers,
        translated.manifest_images);

    result.spirv_size = static_cast<std::uint32_t>(
        translated.words.size() * sizeof(std::uint32_t));
    result.spirv = keep_native_words(std::move(translated.words));
    result.resource_manifest_size =
        static_cast<std::uint32_t>(manifest_bytes.size());
    result.resource_manifest =
        keep_native_allocation(std::move(manifest_bytes));
    result.global_memory_binding_count =
        static_cast<std::uint32_t>(translated.manifest_buffers.size());
    result.image_binding_count =
        static_cast<std::uint32_t>(translated.manifest_images.size());
    result.status = PS5GPU_OK;
    return result.spirv != nullptr && result.resource_manifest != nullptr;
}

// What a manifest binds, as a set that can be compared. Order is not
// part of the answer: what matters is whether the same guest memory is
// reached, at the same binding.
struct BoundResources {
    std::map<std::uint32_t, std::uint64_t> buffers;
    std::map<std::uint32_t, std::uint64_t> images;
    bool decoded = false;
};

BoundResources bound_resources_of(
    const std::uint8_t* bytes, std::uint32_t size) {
    BoundResources bound;
    Ps5GpuResourceManifestHeader header = {};
    if (bytes == nullptr || size < sizeof(header)) {
        return bound;
    }
    std::memcpy(&header, bytes, sizeof(header));
    if (header.magic != PS5GPU_RESOURCE_MANIFEST_MAGIC ||
        header.total_size > size) {
        return bound;
    }
    for (std::uint32_t index = 0; index < header.global_count; ++index) {
        const auto at =
            header.global_offset + index * sizeof(Ps5GpuResourceGlobal);
        if (at + sizeof(Ps5GpuResourceGlobal) > size) {
            return bound;
        }
        Ps5GpuResourceGlobal record = {};
        std::memcpy(&record, bytes + at, sizeof(record));
        bound.buffers[record.descriptor_index] = record.base_address;
    }
    for (std::uint32_t index = 0; index < header.image_count; ++index) {
        const auto at =
            header.image_offset + index * sizeof(Ps5GpuResourceImage);
        if (at + sizeof(Ps5GpuResourceImage) > size) {
            return bound;
        }
        Ps5GpuResourceImage record = {};
        std::memcpy(&record, bytes + at, sizeof(record));
        bound.images[record.binding] = record.base_address;
    }
    bound.decoded = true;
    return bound;
}

// Reports where the two producers agree and where they do not. Counting
// only the totals would hide the case that matters most: the same number
// of bindings pointing at different memory.
void compare_shader_bindings(
    std::uint64_t shader_address,
    const Ps5GpuShaderResult& theirs,
    const std::vector<std::uint8_t>& ours_bytes,
    const char* trace_scope) {
    const auto ours = bound_resources_of(
        ours_bytes.data(),
        static_cast<std::uint32_t>(ours_bytes.size()));
    const auto them = bound_resources_of(
        theirs.resource_manifest, theirs.resource_manifest_size);
    if (!ours.decoded || !them.decoded) {
        return;
    }
    // By address, not by binding number. The two producers number their
    // bindings independently and each module is consistent with its own
    // manifest, so an index in common says nothing - what says something
    // is whether the same guest memory is reached at all.
    std::uint32_t same = 0;
    std::uint32_t differing = 0;
    std::uint32_t only_ours = 0;
    std::uint32_t only_theirs = 0;
    const auto compare =
        [&](const std::map<std::uint32_t, std::uint64_t>& mine,
            const std::map<std::uint32_t, std::uint64_t>& other) {
            std::set<std::uint64_t> ours_addresses;
            std::set<std::uint64_t> their_addresses;
            for (const auto& [binding, address] : mine) {
                (void)binding;
                ours_addresses.insert(address);
            }
            for (const auto& [binding, address] : other) {
                (void)binding;
                their_addresses.insert(address);
            }
            for (const auto address : ours_addresses) {
                if (their_addresses.count(address) != 0) {
                    ++same;
                } else {
                    ++only_ours;
                }
            }
            for (const auto address : their_addresses) {
                if (ours_addresses.count(address) == 0) {
                    ++only_theirs;
                    // Named, not just counted. An address the other
                    // producer binds and this one does not is memory the
                    // shader reads and this module cannot reach, and
                    // which memory it is says why.
                    agc_trace(
                        "%s.binding_only_theirs cs=0x%016llX "
                        "address=0x%016llX\n",
                        trace_scope,
                        static_cast<unsigned long long>(shader_address),
                        static_cast<unsigned long long>(address));
                }
            }
        };
    compare(ours.buffers, them.buffers);
    compare(ours.images, them.images);
    (void)differing;
    agc_trace(
        "%s.shader_bindings_compared cs=0x%016llX same=%u differing=%u "
        "only_ours=%u only_theirs=%u\n",
        trace_scope,
        static_cast<unsigned long long>(shader_address),
        same,
        differing,
        only_ours,
        only_theirs);
}

bool compile_agc_graphics_stage(
    const Ps5GpuNativeShaderState& native_state,
    const std::vector<Ps5GpuRegisterValue>& registers,
    Ps5GpuShaderStage stage,
    std::int32_t global_buffer_base,
    std::int32_t total_global_buffer_count,
    std::int32_t image_binding_base,
    std::int32_t required_vertex_output_count,
    Ps5GpuShaderResult& result,
    char* error,
    std::uint32_t error_size) {
    Ps5GpuShaderRequest request = {};
    request.struct_size = sizeof(request);
    request.abi_version = PS5GPU_ABI_VERSION;
    request.stage = stage;
    request.shader_address = stage == PS5GPU_STAGE_PIXEL
        ? native_state.ps_address
        : native_state.es_address;
    request.shader_header_address = stage == PS5GPU_STAGE_PIXEL
        ? native_state.ps_header_address
        : native_state.es_header_address;
    request.registers = registers.data();
    request.register_count =
        static_cast<std::uint32_t>(registers.size());
    request.user_data_base_register = stage == PS5GPU_STAGE_PIXEL
        ? native_state.pixel_user_data_base_register
        : native_state.export_user_data_base_register;
    request.user_data_scalar_register_base =
        stage == PS5GPU_STAGE_VERTEX ? 8u : 0u;
    request.wave_lane_count = 32;
    request.storage_buffer_offset_alignment = 1;
    request.pixel_input_enable = native_state.pixel_input_enable;
    request.pixel_input_address = native_state.pixel_input_address;
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

    result = {};
    result.struct_size = sizeof(result);
    if (error != nullptr && error_size != 0) {
        error[0] = '\0';
    }
    const ShaderStageScope stage_scope(ShaderStage::TranslateGraphics);
    if (graphics_shader_declined(request.shader_address)) {
        return false;
    }
    if (native_shader_producer_enabled() &&
        !native_shader_producer_traces_only() &&
        native_graphics_producer_enabled()) {
        const auto translate_started = wait_performance_counter();
        const auto translated_ok =
            translate_graphics_stage_natively(request, result);
        // Which translations are slow, and whether the scalar walk is why:
        // a new scene put whole seconds of them into single frames.
        const auto translate_ticks =
            wait_performance_counter() - translate_started;
        const auto translate_ms =
            translate_ticks * 1000 / wait_performance_frequency();
        const auto translate_us =
            translate_ticks * 1000000 / wait_performance_frequency();
        // PS5RT_SLOW_TRANSLATE_MS: the threshold, 10 unless set.
        static const long long slow_translate_ms = [] {
            const auto* value = std::getenv("PS5RT_SLOW_TRANSLATE_MS");
            return value == nullptr ? 10LL : std::atoll(value);
        }();
        if (translate_ms >= slow_translate_ms) {
            std::uint64_t spirv_hash = 1469598103934665603ULL;
            for (std::uint32_t index = 0;
                 result.spirv != nullptr && index < result.spirv_size;
                 ++index) {
                spirv_hash =
                    (spirv_hash ^ result.spirv[index]) * 1099511628211ULL;
            }
            std::uint64_t manifest_hash = 1469598103934665603ULL;
            for (std::uint32_t index = 0;
                 result.resource_manifest != nullptr &&
                 index < result.resource_manifest_size;
                 ++index) {
                manifest_hash = (manifest_hash ^
                                 result.resource_manifest[index]) *
                    1099511628211ULL;
            }
            char user_data[200] = {};
            int used = 0;
            for (std::uint32_t index = 0; index < 12; ++index) {
                std::uint32_t value = 0;
                for (std::uint32_t entry = 0;
                     entry < request.register_count; ++entry) {
                    if (request.registers[entry].register_address ==
                        request.user_data_base_register + index) {
                        value = request.registers[entry].value;
                        break;
                    }
                }
                used += std::snprintf(user_data + used,
                                      sizeof(user_data) - used, " %08X",
                                      value);
            }
            agc_trace(
                "agc.slow_translate stage=%d address=0x%016llX ms=%lld us=%lld "
                "paths=%u scalar_us=%llu cfg_us=%llu exhausted=%u reuse=%u "
                "bytes=%u "
                "spirv=%016llX "
                "manifest=%016llX ud=%s\n",
                static_cast<int>(stage),
                static_cast<unsigned long long>(request.shader_address),
                static_cast<long long>(translate_ms),
                static_cast<long long>(translate_us),
                last_graphics_paths,
                static_cast<unsigned long long>(last_graphics_scalar_ns / 1000),
                static_cast<unsigned long long>(last_graphics_cfg_ns / 1000),
                last_graphics_exhausted ? 1u : 0u,
                last_graphics_reuse,
                result.spirv_size,
                static_cast<unsigned long long>(spirv_hash),
                static_cast<unsigned long long>(manifest_hash),
                user_data);
        }
        if (translated_ok) {
            agc_trace(
                "agc.graphics_native stage=%d address=0x%016llX "
                "bytes=%u buffers=%u images=%u userdata=%u base=%u sloads=%u/%u simages=%u/%u translated=%u skipped=%u unknown=%u rejected=%u reads=%u/%u\n",
                static_cast<int>(stage),
                static_cast<unsigned long long>(request.shader_address),
                result.spirv_size,
                result.global_memory_binding_count,
                result.image_binding_count,
                last_graphics_user_data_count,
                request.user_data_scalar_register_base,
                last_graphics_loads_resolved,
                last_graphics_loads,
                last_graphics_images_resolved,
                last_graphics_images,
                last_graphics_translated,
                last_graphics_skipped,
                last_graphics_descriptors_unknown,
                last_graphics_descriptors_rejected,
                last_graphics_reads - last_graphics_reads_failed,
                last_graphics_reads);
            return true;
        }
    }
    // The managed compiler, for what the native one declines. It is slow
    // enough to show in a frame, so each shader that lands here is named.
    const auto declined_natively = last_graphics_declined != 0;
    if (declined_natively &&
        graphics_known_hopeless(
            request.shader_address,
            last_graphics_failed_pc,
            last_graphics_failed_word)) {
        return false;
    }
    const auto fallback_started = wait_performance_counter();
    const auto fallback_status =
        ps5rt_gpu_compile_spirv(&request, &result, error, error_size);
    if (declined_natively && fallback_status != PS5GPU_OK) {
        remember_graphics_hopeless(
            request.shader_address,
            last_graphics_failed_pc,
            last_graphics_failed_word);
    }
    {
        static std::atomic<std::uint32_t> fallbacks{0};
        const auto seen = fallbacks.fetch_add(1) + 1;
        if (seen <= 64 || seen % 1000 == 0) {
            agc_trace(
                "agc.graphics_fallback n=%u stage=%d address=0x%016llX "
                "declined=%u failed_pc=%u failed_word=0x%08X us=%lld "
                "status=%d\n",
                seen, static_cast<int>(stage),
                static_cast<unsigned long long>(request.shader_address),
                last_graphics_declined, last_graphics_failed_pc,
                last_graphics_failed_word,
                static_cast<long long>(
                    (wait_performance_counter() - fallback_started) *
                    1000000 / wait_performance_frequency()),
                static_cast<int>(fallback_status));
        }
    }
    return fallback_status == PS5GPU_OK &&
        result.spirv != nullptr &&
        result.spirv_size >= 5 * sizeof(std::uint32_t) &&
        result.resource_manifest != nullptr &&
        result.resource_manifest_size >=
            sizeof(Ps5GpuResourceManifestHeader);
}

std::uint32_t live_graphics_minimum_state() {
    static const auto minimum = []() {
        const auto* value =
            std::getenv("PS5GPU_NATIVE_LIVE_GRAPHICS_MIN_STATE");
        if (value == nullptr || value[0] == '\0') {
            return 35u;
        }
        char* end = nullptr;
        const auto parsed = std::strtoul(value, &end, 0);
        if (end == value ||
            (end != nullptr && end[0] != '\0') ||
            parsed == 0 ||
            parsed > 4096) {
            return 35u;
        }
        return static_cast<std::uint32_t>(parsed);
    }();
    return minimum;
}

bool live_graphics_compilation_enabled() {
    static const auto enabled = []() {
        const auto* value =
            std::getenv("PS5GPU_NATIVE_LIVE_GRAPHICS");
        return value != nullptr &&
            (std::strcmp(value, "1") == 0 ||
             std::strcmp(value, "true") == 0 ||
             std::strcmp(value, "yes") == 0 ||
             std::strcmp(value, "on") == 0);
    }();
    return enabled;
}

void try_compile_graphics_state(
    AgcSubmittedState& state,
    std::uint32_t shader_state_id,
    Ps5GpuNativeShaderState& native_state,
    const char* trace_scope) {
    if (!live_graphics_compilation_enabled() ||
        shader_state_id < live_graphics_minimum_state() ||
        native_state.es_address == 0 ||
        native_state.ps_address == 0 ||
        state.graphics_attempts[shader_state_id] != 0) {
        return;
    }
    ++state.graphics_attempts[shader_state_id];

    std::vector<Ps5GpuRegisterValue> registers;
    registers.reserve(state.sh_registers.size() + kAgcSpiPsInputCntlCount);
    for (const auto& [address, value] : state.sh_registers) {
        registers.push_back({address, value});
    }
    // The pixel stage's input controls ride along, tagged so they cannot
    // be mistaken for the shader registers of the same number: which
    // export each attribute reads is a context register, and a pixel
    // stage translated without it reads attribute N from export N.
    for (std::uint32_t index = 0; index < kAgcSpiPsInputCntlCount; ++index) {
        const auto found =
            state.cx_registers.find(kAgcSpiPsInputCntl0 + index);
        if (found != state.cx_registers.end()) {
            registers.push_back(
                {kPixelInputControlTag | (kAgcSpiPsInputCntl0 + index),
                 found->second});
        }
    }

    // Two translations a state, not four. The vertex stage goes first
    // and says how many buffers and images it binds; the pixel stage is
    // then translated once with its bindings after those, and the buffer
    // array it declares comes out exactly as long as both stages' together.
    // The vertex stage keeps an array only as long as its own, which a
    // layout with more descriptors than a shader declares allows. Probing
    // both stages and translating both again was half of the main thread's
    // translation time once the scene after the intro registered a few
    // hundred states a frame.
    Ps5GpuShaderResult es_result = {};
    Ps5GpuShaderResult ps_result = {};
    char es_error[2048] = {};
    char ps_error[2048] = {};
    const auto es_ok = compile_agc_graphics_stage(
        native_state,
        registers,
        PS5GPU_STAGE_VERTEX,
        0,
        -1,
        0,
        1,
        es_result,
        es_error,
        sizeof(es_error));
    const auto es_global_count = es_result.global_memory_binding_count;
    const auto es_image_count = es_result.image_binding_count;
    if (!es_ok ||
        es_global_count > static_cast<std::uint32_t>(INT32_MAX) ||
        es_image_count > static_cast<std::uint32_t>(INT32_MAX)) {
        agc_trace(
            "%s.graphics_spirv_probe_error state=%u "
            "es=0x%016llX ps=0x%016llX es_error=%s ps_error=-\n",
            trace_scope,
            shader_state_id,
            static_cast<unsigned long long>(native_state.es_address),
            static_cast<unsigned long long>(native_state.ps_address),
            es_error[0] == '\0' ? "?" : es_error);
        free_agc_shader_result(es_result);
        return;
    }
    const auto ps_ok = compile_agc_graphics_stage(
        native_state,
        registers,
        PS5GPU_STAGE_PIXEL,
        static_cast<std::int32_t>(es_global_count),
        -1,
        static_cast<std::int32_t>(es_image_count),
        -1,
        ps_result,
        ps_error,
        sizeof(ps_error));
    if (!ps_ok) {
        agc_trace(
            "%s.graphics_spirv_error state=%u "
            "es=0x%016llX ps=0x%016llX es_error=%s ps_error=%s\n",
            trace_scope,
            shader_state_id,
            static_cast<unsigned long long>(native_state.es_address),
            static_cast<unsigned long long>(native_state.ps_address),
            es_error[0] == '\0' ? "?" : es_error,
            ps_error[0] == '\0' ? "?" : ps_error);
        free_agc_shader_result(es_result);
        free_agc_shader_result(ps_result);
        return;
    }

    native_state.es_spirv = es_result.spirv;
    native_state.es_spirv_size = es_result.spirv_size;
    native_state.es_resource_manifest =
        es_result.resource_manifest;
    native_state.es_resource_manifest_size =
        es_result.resource_manifest_size;
    native_state.ps_spirv = ps_result.spirv;
    native_state.ps_spirv_size = ps_result.spirv_size;
    native_state.ps_resource_manifest =
        ps_result.resource_manifest;
    native_state.ps_resource_manifest_size =
        ps_result.resource_manifest_size;
    std::uint32_t registered_state_id = 0;
    bool registered = false;
    {
        const ShaderStageScope stage_scope(ShaderStage::RegisterGraphics);
        registered = ps5rt_native_gpu_register_shader_state(
            &native_state,
            &registered_state_id);
    }
    agc_trace(
        "%s.graphics_native_state state=%u registered=%u "
        "result=%u es_bytes=%u ps_bytes=%u "
        "es_globals=%u ps_globals=%u es_images=%u ps_images=%u\n",
        trace_scope,
        shader_state_id,
        registered_state_id,
        registered ? 1u : 0u,
        es_result.spirv_size,
        ps_result.spirv_size,
        es_result.global_memory_binding_count,
        ps_result.global_memory_binding_count,
        es_result.image_binding_count,
        ps_result.image_binding_count);
    free_agc_shader_result(es_result);
    free_agc_shader_result(ps_result);
}

// Which of the seeded defaults the command stream never wrote for itself.
// Those are exactly the registers the decoder used to read as zero, and
// the only ones the seeding can be credited for; an empty list would mean
// the seeding is dead weight.
//
// Sampled at several draw milestones rather than once. The first draw is
// the harshest case, but a register the stream sets later was never a hole
// the seeding had to fill, and only a later sample tells the two apart.
// A whole 400 second run of this title issues about 70 draws, so the
// milestones have to be small to be reached at all.
void report_agc_seed_coverage() {
    static std::uint64_t draws = 0;
    ++draws;
    if (draws != 1 && draws != 10 && draws != 50 &&
        draws != 200 && draws != 1000) {
        return;
    }

    constexpr auto kSeeded =
        sizeof(agc_defaults::kContext) /
        sizeof(agc_defaults::kContext[0]);
    std::string untouched;
    std::uint32_t untouched_count = 0;
    for (const auto& entry : agc_defaults::kContext) {
        if (entry.offset < kAgcContextRegisterSlots &&
            g_stream_written_cx.test(entry.offset)) {
            continue;
        }
        ++untouched_count;
        if (untouched.size() < 850) {
            char text[32] = {};
            std::snprintf(
                text,
                sizeof(text),
                " %03X=%08X",
                entry.offset,
                entry.value);
            untouched += text;
        }
    }
    agc_trace(
        "agc.seed_coverage draw=%llu selector_writes=%llu ps_input_selector=%llu seeded=%u stream_wrote_cx=%u "
        "never_written=%u%s\n",
        static_cast<unsigned long long>(draws),
        static_cast<unsigned long long>(
            g_register_selector_seen.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_register_selector_ps_input.load(std::memory_order_relaxed)),
        static_cast<unsigned>(kSeeded),
        static_cast<unsigned>(g_stream_written_cx.count()),
        untouched_count,
        untouched.c_str());
}

// Defined with the compute dispatch handling, below.
bool agc_buffer_fill_at(std::uint64_t address, std::uint32_t& value);

void trace_agc_draw_milestone(
    AgcSubmittedState& state,
    std::uint32_t submission_draw,
    std::uint32_t packet_offset,
    std::uint32_t op,
    std::uint32_t packet_register,
    const AgcDrawInfo& draw,
    const char* trace_scope) {
    report_agc_seed_coverage();
    std::uint64_t es_address = 0;
    std::uint64_t ps_address = 0;
    try_get_shader_address(
        state.sh_registers,
        kAgcSpiShaderPgmLoEs,
        kAgcSpiShaderPgmHiEs,
        es_address);
    try_get_shader_address(
        state.sh_registers,
        kAgcSpiShaderPgmLoPs,
        kAgcSpiShaderPgmHiPs,
        ps_address);
    std::uint32_t primitive_type = 0;
    try_get_agc_register(
        state.uc_registers,
        kAgcVgtPrimitiveType,
        primitive_type);
    // GE_INDX_OFFSET is where a non-indexed draw starts in the vertex buffer.
    // Dropping it made every draw of a state read the same vertices: 29 draws
    // of Astro Bot's UI state all rendered the identical quad, and the pass
    // that samples the frame did so at the wrong place.
    std::uint32_t first_vertex = 0;
    try_get_agc_register(
        state.uc_registers,
        kAgcGeIndexOffset,
        first_vertex);
    const auto target = decode_agc_render_targets(state.cx_registers);
    const auto scissor =
        decode_agc_scissor(
            state.cx_registers,
            target.width,
            target.height);
    const auto viewport =
        decode_agc_viewport(state.cx_registers, target, scissor);
    // Every draw, formatted with six doubles and written to the log, was
    // nine percent of the main thread in the scene after the intro, where
    // a frame has hundreds of them. The first 256 and one in 1024 after
    // that; PS5RT_TRACE_DRAWS=1 traces them all.
    static const bool trace_every_draw = [] {
        const auto* value = std::getenv("PS5RT_TRACE_DRAWS");
        return value != nullptr && value[0] == '1';
    }();
    static std::atomic<std::uint64_t> traced_draws{0};
    const auto draw_number =
        traced_draws.fetch_add(1, std::memory_order_relaxed);
    if (trace_every_draw || draw_number < 256 ||
        draw_number % 1024 == 0)
    agc_trace(
        "%s.draw submission=%llu draw=%u total=%llu packet_dw=%u "
        "op=0x%02X reg=0x%02X vertices=%u count_known=%d indexed=%d "
        "index=0x%016llX index_max=%u index_bytes=%u "
        "prim=0x%X es=0x%016llX ps=0x%016llX "
        "rt_count=%u rt_slot=%u rt=0x%016llX size=%ux%u "
        "fmt=%u num=%u tile=%u cb_target=0x%08X cb_shader=0x%08X "
        "masks_known=%d masks_intersect=%d "
        "viewport_known=%d viewport_derived=%d "
        "viewport=%.3f,%.3f,%.3fx%.3f,%.3f..%.3f "
        "scissor_known=%d scissor=%d,%d,%ux%u full=%d "
        "cb_color_control=0x%08X\n",
        trace_scope,
        static_cast<unsigned long long>(state.submission_count),
        submission_draw,
        static_cast<unsigned long long>(state.draw_count),
        packet_offset,
        op,
        packet_register,
        draw.vertex_count,
        draw.count_known ? 1 : 0,
        draw.indexed ? 1 : 0,
        static_cast<unsigned long long>(draw.index_address),
        draw.index_max,
        draw.index_bytes,
        primitive_type,
        static_cast<unsigned long long>(es_address),
        static_cast<unsigned long long>(ps_address),
        target.count,
        target.slot,
        static_cast<unsigned long long>(target.address),
        target.width,
        target.height,
        target.format,
        target.number_type,
        target.tile_mode,
        target.target_mask,
        target.shader_mask,
        target.masks_known ? 1 : 0,
        target.masks_intersect ? 1 : 0,
        viewport.known ? 1 : 0,
        viewport.derived_from_target ? 1 : 0,
        static_cast<double>(viewport.x),
        static_cast<double>(viewport.y),
        static_cast<double>(viewport.width),
        static_cast<double>(viewport.height),
        static_cast<double>(viewport.min_depth),
        static_cast<double>(viewport.max_depth),
        scissor.known ? 1 : 0,
        scissor.x,
        scissor.y,
        scissor.width,
        scissor.height,
        scissor.full ? 1 : 0,
        [&] {
            const auto found = state.cx_registers.find(0x202u);
            return found == state.cx_registers.end() ? 0xFFFFFFFFu
                                                     : found->second;
        }());

    // Under managed authority the submitted DCBs are mirrored through the
    // shadow parser; under PS5RECOMP_AGC_NATIVE_SUBMIT they arrive here
    // directly as "agc". Either way this is the one path that feeds the
    // native runtime, and only one of the two scopes is ever live, so
    // accepting both cannot double-feed it. Gating on the shadow alone
    // left the authoritative path issuing draws that registered no
    // shader state and reached the GPU as nothing.
    //
    // Not every draw is a picture. After rendering into a target and before
    // sampling it, AGC runs a rectangle through a two-instruction pixel
    // shader of its own with CB_COLOR_CONTROL in a metadata mode - fast
    // clear elimination (2), DCC decompression (6). The colour block does
    // not write what that shader exports; it rewrites the target's
    // compression state, which this renderer does not keep. Drawn as a
    // draw, it put zeros into the red and green of the UI plane the intro
    // video had just been drawn onto, and the frame went out black.
    const auto metadata_pass = [&] {
        const auto control = state.cx_registers.find(0x202u);
        if (control == state.cx_registers.end()) {
            return false;
        }
        const auto mode = (control->second >> 4) & 0x7u;
        if (mode != 2 && mode != 6) {
            return false;
        }
        // The shader: v_mov_b32 v0, 0; exp mrt0 (red, green); s_endpgm.
        std::uint32_t words[4] = {};
        for (std::uint32_t index = 0; index < 4; ++index) {
            if (!try_read_u32(ps_address + index * 4ull, words[index])) {
                return false;
            }
        }
        return words[0] == 0x7E000280u && words[1] == 0xF8001803u &&
            words[2] == 0x00000000u && words[3] == 0xBF810000u;
    }();
    // A fast clear is not a draw: it marks the target's compression state
    // as holding the clear value and writes no pixels, and the pass above,
    // at the end of the frame, fills in the pixels nothing drew since.
    // This renderer keeps no compression state, so the target kept the
    // last frame's pixels instead - for the "Sony Interactive Entertainment"
    // screen, a UI plane left see-through by the loading screen before it,
    // where the title had cleared it opaque black. The pass says which
    // targets are fast cleared and to what; the next frame's first draw
    // into one fills it first, where the clear would have been.
    static std::map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>>
        pending_fast_clears;
    //
    // Only for the DCC decompression pass (mode 6). The fast clear
    // elimination pass (mode 2) runs on targets whose "clear value" changes
    // every frame - packed data, the history of the temporal filter among
    // them - and filling those with it made the intro video flicker.
    if (metadata_pass && target.address != 0) {
        const auto word = [&](std::uint32_t reg) {
            const auto found = state.cx_registers.find(reg);
            return found == state.cx_registers.end() ? 0u : found->second;
        };
        const auto control = state.cx_registers.find(0x202u);
        const auto mode = control == state.cx_registers.end()
            ? 0u
            : (control->second >> 4) & 0x7u;
        static const bool fast_clears_off = [] {
            const auto* value = std::getenv("PS5RT_NO_FAST_CLEAR");
            return value != nullptr && value[0] == '1';
        }();
        if (mode == 6 && !fast_clears_off) {
            std::pair<std::uint32_t, std::uint32_t> clear = {
                word(0x323u), word(0x324u)};
            // The DCC code the clear left: 0x00 0000, 0x40 0001,
            // 0x80 1110, 0xC0 1111 (r, g, b, a); 0x20 means the registers.
            const auto slot = target.slot;
            const auto dcc_base =
                ((static_cast<std::uint64_t>(word(0x325u + slot * 0xFu)) |
                  (static_cast<std::uint64_t>(word(0x3A8u + slot) & 0xFFu)
                   << 32))
                 << 8);
            std::uint32_t fill = 0;
            if (dcc_base != 0 && agc_buffer_fill_at(dcc_base, fill)) {
                const auto code = fill & 0xFFu;
                const auto info = word(0x31Cu + slot * 0xFu);
                const auto format = (info >> 2) & 0x1Fu;
                const auto number_type = (info >> 8) & 0x7u;
                const bool known_code = code == 0x00u || code == 0x40u ||
                    code == 0x80u || code == 0xC0u;
                const bool rgb_one = code == 0x80u || code == 0xC0u;
                const bool alpha_one = code == 0x40u || code == 0xC0u;
                if (known_code && format == 0x0Cu && number_type == 7u) {
                    // 16_16_16_16 float: one is 0x3C00.
                    const std::uint32_t one = 0x3C00u;
                    const auto rgb = rgb_one ? one : 0u;
                    clear = {rgb | (rgb << 16),
                             rgb | ((alpha_one ? one : 0u) << 16)};
                } else if (known_code && format == 0x0Au) {
                    // 8_8_8_8 unorm.
                    const auto rgb = rgb_one ? 0xFFu : 0u;
                    clear = {rgb | (rgb << 8) | (rgb << 16) |
                                 ((alpha_one ? 0xFFu : 0u) << 24),
                             0u};
                }
                static std::atomic<std::uint32_t> shown{0};
                if (shown.fetch_add(1, std::memory_order_relaxed) < 64) {
                    agc_trace(
                        "agc.dcc_clear_code target=0x%016llX dcc=0x%016llX "
                        "code=0x%02X format=%u number=%u "
                        "words=0x%08X_%08X\n",
                        static_cast<unsigned long long>(target.address),
                        static_cast<unsigned long long>(dcc_base),
                        code,
                        format,
                        number_type,
                        clear.second,
                        clear.first);
                }
            }
            pending_fast_clears[target.address] = clear;
        }
    }
    if (metadata_pass) {
        static std::atomic<std::uint64_t> skipped{0};
        const auto count = skipped.fetch_add(1) + 1;
        if (count <= 4096 || (count & (count - 1)) == 0) {
            // What a fast clear left the target as: CB_COLOR0_CLEAR_WORD0
            // and 1, which is what this pass writes into every pixel the
            // clear marked and nothing has drawn since.
            const auto word = [&](std::uint32_t reg) {
                const auto found = state.cx_registers.find(reg);
                return found == state.cx_registers.end() ? 0xFFFFFFFFu
                                                         : found->second;
            };
            agc_trace(
                "%s.metadata_pass_skipped count=%llu rt=0x%016llX "
                "mode=%u clear=0x%08X_%08X\n",
                trace_scope,
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(target.address),
                (word(0x202u) >> 4) & 0x7u,
                word(0x324u),
                word(0x323u));
        }
    }
    if (!metadata_pass &&
        (std::strcmp(trace_scope, "native_shadow.agc") == 0 ||
         std::strcmp(trace_scope, "agc") == 0)) {
        std::vector<Ps5GpuNativeRegisterValue> sh_registers;
        std::vector<Ps5GpuNativeRegisterValue> cx_registers;
        sh_registers.reserve(state.sh_registers.size());
        cx_registers.reserve(state.cx_registers.size());
        for (const auto& [address, value] : state.sh_registers) {
            sh_registers.push_back({address, value});
        }
        for (const auto& [address, value] : state.cx_registers) {
            cx_registers.push_back({address, value});
        }

        Ps5GpuNativeShaderState native_state = {};
        native_state.struct_size = sizeof(native_state);
        native_state.abi_version = PS5GPU_NATIVE_ABI_VERSION;
        native_state.es_address = es_address;
        native_state.ps_address = ps_address;
        if (try_get_agc_shader_header(
                es_address, native_state.es_header_address)) {
            native_state.flags |=
                PS5GPU_NATIVE_SHADER_STATE_HAS_EXPORT_HEADER;
        }
        if (try_get_agc_shader_header(
                ps_address, native_state.ps_header_address)) {
            native_state.flags |=
                PS5GPU_NATIVE_SHADER_STATE_HAS_PIXEL_HEADER;
        }
        native_state.export_user_data_base_register =
            select_agc_export_user_data_register(state.sh_registers);
        native_state.pixel_user_data_base_register =
            kAgcPsUserDataRegister;
        try_get_agc_register(
            state.cx_registers,
            kAgcSpiPsInputEna,
            native_state.pixel_input_enable);
        try_get_agc_register(
            state.cx_registers,
            kAgcSpiPsInputAddr,
            native_state.pixel_input_address);
        native_state.sh_registers = sh_registers.data();
        native_state.sh_register_count =
            static_cast<std::uint32_t>(sh_registers.size());
        native_state.cx_registers = cx_registers.data();
        native_state.cx_register_count =
            static_cast<std::uint32_t>(cx_registers.size());

        Ps5GpuNativeDraw native_draw = {};
        native_draw.struct_size = sizeof(native_draw);
        native_draw.abi_version = PS5GPU_NATIVE_ABI_VERSION;
        native_draw.submission_id = state.submission_count;
        native_draw.draw_id = submission_draw;
        native_draw.total_draw_id = state.draw_count;
        native_draw.packet_offset_dwords = packet_offset;
        native_draw.packet_opcode = op;
        native_draw.packet_register = packet_register;
        native_draw.flags =
            (draw.count_known ? PS5GPU_NATIVE_DRAW_COUNT_KNOWN : 0u) |
            (draw.indexed ? PS5GPU_NATIVE_DRAW_INDEXED : 0u) |
            (target.address != 0
                ? PS5GPU_NATIVE_DRAW_TARGET_KNOWN
                : 0u) |
            (viewport.known
                ? PS5GPU_NATIVE_DRAW_VIEWPORT_KNOWN
                : 0u) |
            (scissor.known
                ? PS5GPU_NATIVE_DRAW_SCISSOR_KNOWN
                : 0u) |
            (scissor.full ? PS5GPU_NATIVE_DRAW_SCISSOR_FULL : 0u) |
            (target.masks_known
                ? PS5GPU_NATIVE_DRAW_MASKS_KNOWN
                : 0u);
        native_draw.vertex_count = draw.vertex_count;
        if (const auto pending = pending_fast_clears.find(target.address);
            pending != pending_fast_clears.end()) {
            native_draw.flags |= PS5GPU_NATIVE_DRAW_CLEAR_FIRST;
            native_draw.clear_word0 = pending->second.first;
            native_draw.clear_word1 = pending->second.second;
            pending_fast_clears.erase(pending);
        }
        native_draw.first_vertex = first_vertex;
        native_draw.index_address = draw.index_address;
        native_draw.index_bytes = draw.index_bytes;
        native_draw.index_max = draw.index_max;
        // Both masks are all ones when the stream never set them, so a
        // build that cannot read them still draws every channel.
        native_draw.target_mask =
            target.masks_known ? target.target_mask : 0xFFFFFFFFu;
        native_draw.shader_mask =
            target.masks_known ? target.shader_mask : 0xFFFFFFFFu;
        // reserved1 carries CB_BLENDn_CONTROL: the struct keeps its size.
        native_draw.reserved1 = target.blend_control;
        native_draw.primitive_type = primitive_type;
        native_draw.render_target_count = target.count;
        native_draw.render_target_slot = target.slot;
        native_draw.es_address = es_address;
        native_draw.ps_address = ps_address;
        native_draw.render_target_address = target.address;
        native_draw.render_target_width = target.width;
        native_draw.render_target_height = target.height;
        native_draw.render_target_format = target.format;
        native_draw.render_target_number_type = target.number_type;
        native_draw.render_target_tile_mode = target.tile_mode;
        native_draw.viewport_x = viewport.x;
        native_draw.viewport_y = viewport.y;
        native_draw.viewport_width = viewport.width;
        native_draw.viewport_height = viewport.height;
        native_draw.viewport_min_depth = viewport.min_depth;
        native_draw.viewport_max_depth = viewport.max_depth;
        native_draw.scissor_x = scissor.x;
        native_draw.scissor_y = scissor.y;
        native_draw.scissor_width = scissor.width;
        native_draw.scissor_height = scissor.height;
        // Which registers these are, before any layout tag joins them.
        std::uint64_t dependency_key = 1469598103934665603ULL;
        const auto mix_key = [&](std::uint64_t value) {
            dependency_key = (dependency_key ^ value) * 1099511628211ULL;
        };
        mix_key(es_address);
        mix_key(ps_address);
        for (const auto& value : sh_registers) {
            mix_key((static_cast<std::uint64_t>(value.address) << 32) |
                    value.value);
        }
        for (const auto& value : cx_registers) {
            mix_key((static_cast<std::uint64_t>(value.address) << 32) |
                    value.value);
        }
        if (graphics_dependencies_enabled()) {
            std::lock_guard guard(g_graphics_dependencies_mutex);
            const auto found = g_graphics_dependencies.find(dependency_key);
            if (found != g_graphics_dependencies.end() &&
                found->second.known) {
                auto& known = found->second;
                const auto layout = hash_dependency_words(known.reads);
                if (layout != known.untagged_hash &&
                    (known.layouts.count(layout) != 0 ||
                     known.layouts.size() < kMaximumDependencyLayouts)) {
                    if (known.layouts.insert(layout).second) {
                        agc_trace(
                            "agc.graphics_layout es=0x%016llX "
                            "ps=0x%016llX layouts=%zu words=%zu\n",
                            static_cast<unsigned long long>(es_address),
                            static_cast<unsigned long long>(ps_address),
                            known.layouts.size(),
                            known.reads.size());
                    }
                    sh_registers.push_back(
                        {kDependencyHashTag,
                         static_cast<std::uint32_t>(layout)});
                    sh_registers.push_back(
                        {kDependencyHashTag + 1,
                         static_cast<std::uint32_t>(layout >> 32)});
                    native_state.sh_registers = sh_registers.data();
                    native_state.sh_register_count =
                        static_cast<std::uint32_t>(sh_registers.size());
                }
            }
        }
        std::uint32_t shader_state_id = 0;
        bool shader_state_known = false;
        {
            const ShaderStageScope stage_scope(
                ShaderStage::RegisterGraphics);
            shader_state_known = ps5rt_native_gpu_register_shader_state(
                &native_state,
                &shader_state_id);
        }
        if (shader_state_known) {
            native_draw.flags |=
                PS5GPU_NATIVE_DRAW_SHADER_STATE_KNOWN;
            native_draw.reserved0 = shader_state_id;
            g_graphics_dependency_reads.clear();
            try_compile_graphics_state(
                state,
                shader_state_id,
                native_state,
                trace_scope);
            if (graphics_dependencies_enabled() &&
                !g_graphics_dependency_reads.empty()) {
                std::lock_guard guard(g_graphics_dependencies_mutex);
                auto& known = g_graphics_dependencies[dependency_key];
                if (!known.known) {
                    std::set<std::pair<std::uint64_t, std::uint32_t>>
                        unique(
                            g_graphics_dependency_reads.begin(),
                            g_graphics_dependency_reads.end());
                    known.reads.assign(unique.begin(), unique.end());
                    known.untagged_hash = hash_dependency_words(known.reads);
                    known.known = true;
                }
                g_graphics_dependency_reads.clear();
            }
        } else {
            agc_trace(
                "native_gpu.register_shader_state_failed "
                "submission=%llu draw=%u\n",
                static_cast<unsigned long long>(state.submission_count),
                submission_draw);
        }
        if (!ps5rt_native_gpu_submit_draw(&native_draw)) {
            agc_trace(
                "native_gpu.submit_draw_failed submission=%llu draw=%u\n",
                static_cast<unsigned long long>(state.submission_count),
                submission_draw);
        }
    }
}

bool trace_register_writes() {
    static const auto enabled = [] {
        const auto* value = std::getenv("PS5RT_TRACE_REGISTER_WRITES");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// CB_TARGET_MASK (0x08E) has never appeared in the register map, while its
// neighbour CB_SHADER_MASK (0x08F) is written on every frame. Two readings
// fit: the game never sets it, or it sets it and we file the write under
// another key. Both write paths report their neighbourhood here, with the
// path that carried them, so the trace says which.
void note_mask_neighbourhood(
    const char* path,
    const char* space,
    std::uint32_t offset,
    std::uint32_t value) {
    if (offset < 0x08Cu || offset > 0x092u) {
        return;
    }
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) >= 48) {
        return;
    }
    agc_trace(
        "agc.mask_write path=%s space=%s offset=0x%03X value=0x%08X\n",
        path,
        space,
        offset,
        value);
}

void apply_direct_registers(
    std::uint64_t packet_address,
    std::uint32_t packet_length,
    std::map<std::uint32_t, std::uint32_t>& destination,
    const char* space = "?") {
    if (packet_length < 3) {
        return;
    }

    std::uint32_t start_register = 0;
    if (!try_read_u32(packet_address + 4, start_register)) {
        return;
    }
    if ((start_register & kAgcRegisterSelectorMask) != 0) {
        g_register_selector_seen.fetch_add(1, std::memory_order_relaxed);
    }
    start_register = normalize_agc_register_offset(start_register);

    for (std::uint32_t index = 0; index < packet_length - 2; ++index) {
        std::uint32_t value = 0;
        if (!try_read_u32(
                packet_address + 8 +
                    (static_cast<std::uint64_t>(index) * sizeof(value)),
                value)) {
            return;
        }
        destination[start_register + index] = value;
        if (space != nullptr && space[0] == 'c') {
            mark_stream_written_cx(start_register + index);
            note_mask_neighbourhood(
                "direct", space, start_register + index, value);
        }
    }
    if (trace_register_writes()) {
        std::string text;
        for (std::uint32_t index = 0;
             index + 2 < packet_length && text.size() < 400;
             ++index) {
            std::uint32_t value = 0;
            if (!try_read_u32(
                    packet_address + 8 +
                        (static_cast<std::uint64_t>(index) *
                         sizeof(value)),
                    value)) {
                break;
            }
            char entry[32] = {};
            std::snprintf(
                entry,
                sizeof(entry),
                " %03X=%08X",
                start_register + index,
                value);
            text += entry;
        }
        agc_trace(
            "agc.register_direct space=%s start=0x%03X count=%u%s\n",
            space,
            start_register,
            packet_length - 2,
            text.c_str());
    }
}

struct AgcIndirectRegisterEntry {
    std::uint32_t offset = 0;
    std::uint32_t value = 0;
};

bool plausible_agc_sentinel_target(
    const std::vector<AgcIndirectRegisterEntry>& entries,
    std::size_t start,
    std::uint64_t& address,
    std::uint32_t& width,
    std::uint32_t& height) {
    const auto base_low = entries[start].value;
    const auto info = entries[start + 2].value;
    const auto base_high = entries[start + 10].value;
    const auto attrib2 = entries[start + 14].value;
    address =
        (static_cast<std::uint64_t>(base_high & 0xFFu) << 40) |
        (static_cast<std::uint64_t>(base_low) << 8);
    width = ((attrib2 >> 14) & 0x3FFFu) + 1;
    height = (attrib2 & 0x3FFFu) + 1;
    const auto format = (info >> 2) & 0x1Fu;
    std::uint32_t probe = 0;
    return address >= 0x10000 &&
        width >= 2 && width <= 16384 &&
        height >= 2 && height <= 16384 &&
        format != 0 &&
        try_read_u32(address, probe);
}

bool agc_sentinel_viewport_shape(
    const std::vector<AgcIndirectRegisterEntry>& entries,
    std::size_t start) {
    if (start + 10 > entries.size()) {
        return false;
    }
    const auto x_scale = agc_float(entries[start].value);
    const auto y_scale = agc_float(entries[start + 1].value);
    const auto z_scale = agc_float(entries[start + 2].value);
    const auto x_offset = agc_float(entries[start + 3].value);
    const auto y_offset = agc_float(entries[start + 4].value);
    const auto z_offset = agc_float(entries[start + 5].value);
    const auto z_min = agc_float(entries[start + 8].value);
    const auto z_max = agc_float(entries[start + 9].value);
    if (!std::isfinite(x_scale) || !std::isfinite(y_scale) ||
        !std::isfinite(z_scale) || !std::isfinite(x_offset) ||
        !std::isfinite(y_offset) || !std::isfinite(z_offset) ||
        !std::isfinite(z_min) || !std::isfinite(z_max) ||
        std::fabs(x_scale) < 0.5f || std::fabs(x_scale) > 16384.0f ||
        std::fabs(y_scale) < 0.5f || std::fabs(y_scale) > 16384.0f ||
        std::fabs(z_scale) > 16.0f || std::fabs(z_offset) > 16.0f ||
        std::fabs(x_offset) > 32768.0f || std::fabs(y_offset) > 32768.0f ||
        z_min < -0.01f || z_max > 1.01f || z_max < z_min) {
        return false;
    }
    const auto tl = entries[start + 6].value;
    const auto br = entries[start + 7].value;
    const auto left = tl & 0x7FFFu;
    const auto top = (tl >> 16) & 0x7FFFu;
    const auto right = br & 0x7FFFu;
    const auto bottom = (br >> 16) & 0x7FFFu;
    if (right <= left || bottom <= top ||
        right > 16384 || bottom > 16384) {
        return false;
    }
    // The self-consistency that makes this safe without a target: the scissor
    // is exactly the rectangle the scale factors describe.
    const auto width = static_cast<float>(right - left);
    const auto height = static_cast<float>(bottom - top);
    return std::fabs(std::fabs(x_scale) * 2.0f - width) <= 1.0f &&
        std::fabs(std::fabs(y_scale) * 2.0f - height) <= 1.0f;
}

bool plausible_agc_sentinel_viewport(
    const std::vector<AgcIndirectRegisterEntry>& entries,
    std::size_t start,
    std::uint32_t target_width,
    std::uint32_t target_height) {
    const auto x_scale = agc_float(entries[start].value);
    const auto y_scale = agc_float(entries[start + 1].value);
    const auto z_scale = agc_float(entries[start + 2].value);
    const auto x_offset = agc_float(entries[start + 3].value);
    const auto y_offset = agc_float(entries[start + 4].value);
    const auto z_offset = agc_float(entries[start + 5].value);
    const auto z_min = agc_float(entries[start + 8].value);
    const auto z_max = agc_float(entries[start + 9].value);
    if (!std::isfinite(x_scale) ||
        !std::isfinite(y_scale) ||
        !std::isfinite(z_scale) ||
        !std::isfinite(x_offset) ||
        !std::isfinite(y_offset) ||
        !std::isfinite(z_offset) ||
        !std::isfinite(z_min) ||
        !std::isfinite(z_max) ||
        std::fabs(x_scale) < 0.5f ||
        std::fabs(x_scale) > 16384.0f ||
        std::fabs(y_scale) < 0.5f ||
        std::fabs(y_scale) > 16384.0f ||
        std::fabs(z_scale) > 16.0f ||
        std::fabs(z_offset) > 16.0f ||
        std::fabs(x_offset) > 32768.0f ||
        std::fabs(y_offset) > 32768.0f ||
        z_max < z_min) {
        return false;
    }
    const auto tl = entries[start + 6].value;
    const auto br = entries[start + 7].value;
    const auto left = tl & 0x7FFFu;
    const auto top = (tl >> 16) & 0x7FFFu;
    const auto right = br & 0x7FFFu;
    const auto bottom = (br >> 16) & 0x7FFFu;
    return right > left &&
        bottom > top &&
        right <= 16384 &&
        bottom <= 16384 &&
        left < target_width &&
        top < target_height;
}

// The viewport block does not always sit twenty entries past the colour block.
// When it does not, nothing was recovered and the draw silently kept whatever
// viewport the previous draw left behind - which is how a 120x67 bloom target
// ended up rendering through a 1920x1080 viewport. Look for the block instead
// of assuming where it is, preferring one whose scale matches the target
// extent, since a full-target viewport is by far the common case.
constexpr std::size_t kAgcViewportSearchWindow = 96;

bool find_agc_sentinel_viewport(
    const std::vector<AgcIndirectRegisterEntry>& entries,
    std::size_t search_start,
    std::size_t run_end,
    std::uint32_t target_width,
    std::uint32_t target_height,
    std::size_t& viewport_start) {
    const auto limit = std::min(
        run_end,
        search_start + kAgcViewportSearchWindow);
    bool have_candidate = false;
    std::size_t first_candidate = 0;
    for (auto offset = search_start;
         offset + kAgcViewportSentinelOffsets.size() <= limit;
         ++offset) {
        if (!plausible_agc_sentinel_viewport(
                entries,
                offset,
                target_width,
                target_height)) {
            continue;
        }
        if (!have_candidate) {
            first_candidate = offset;
            have_candidate = true;
        }
        const auto x_scale =
            std::fabs(agc_float(entries[offset].value));
        const auto y_scale =
            std::fabs(agc_float(entries[offset + 1].value));
        if (std::fabs(
                x_scale * 2.0f -
                static_cast<float>(target_width)) <= 1.0f &&
            std::fabs(
                y_scale * 2.0f -
                static_cast<float>(target_height)) <= 1.0f) {
            viewport_start = offset;
            return true;
        }
    }
    viewport_start = first_candidate;
    return have_candidate;
}

void apply_agc_sentinel_values(
    const std::vector<AgcIndirectRegisterEntry>& entries,
    std::size_t start,
    const std::uint32_t* offsets,
    std::size_t offset_count,
    std::map<std::uint32_t, std::uint32_t>& destination) {
    for (std::size_t index = 0; index < offset_count; ++index) {
        destination[offsets[index]] = entries[start + index].value;
    }
}


void recover_agc_context_sentinels(
    const std::vector<AgcIndirectRegisterEntry>& entries,
    std::map<std::uint32_t, std::uint32_t>& destination) {
    std::size_t run_start = 0;
    while (run_start < entries.size()) {
        if (entries[run_start].offset !=
            std::numeric_limits<std::uint32_t>::max()) {
            ++run_start;
            continue;
        }
        auto run_end = run_start + 1;
        while (run_end < entries.size() &&
               entries[run_end].offset ==
                   std::numeric_limits<std::uint32_t>::max()) {
            ++run_end;
        }
        for (auto candidate = run_start;
             candidate + kAgcCbColor0SentinelLength <= run_end;) {
            std::uint64_t target_address = 0;
            std::uint32_t target_width = 0;
            std::uint32_t target_height = 0;
            if (!plausible_agc_sentinel_target(
                    entries,
                    candidate,
                    target_address,
                    target_width,
                    target_height)) {
                ++candidate;
                continue;
            }
            apply_agc_sentinel_values(
                entries,
                candidate,
                kAgcCbColor0SentinelOffsets.data(),
                kAgcCbColor0SentinelOffsets.size(),
                destination);
            // Try the historical position first so every case that already
            // recovered keeps recovering from exactly the same entries.
            auto viewport_start = candidate + 20;
            auto recovered_viewport =
                viewport_start + kAgcViewportSentinelOffsets.size() <= run_end &&
                plausible_agc_sentinel_viewport(
                    entries,
                    viewport_start,
                    target_width,
                    target_height);
            if (!recovered_viewport) {
                recovered_viewport = find_agc_sentinel_viewport(
                    entries,
                    candidate + kAgcCbColor0SentinelLength,
                    run_end,
                    target_width,
                    target_height,
                    viewport_start);
            }
            if (recovered_viewport) {
                apply_agc_sentinel_values(
                    entries,
                    viewport_start,
                    kAgcViewportSentinelOffsets.data(),
                    kAgcViewportSentinelOffsets.size(),
                    destination);
            }
            agc_trace(
                "agc.indirect_sentinel target=0x%016llX "
                "size=%ux%u viewport=%d start=%zu offset=%zd\n",
                static_cast<unsigned long long>(target_address),
                target_width,
                target_height,
                recovered_viewport ? 1 : 0,
                candidate,
                recovered_viewport
                    ? static_cast<std::ptrdiff_t>(viewport_start) -
                        static_cast<std::ptrdiff_t>(candidate)
                    : std::ptrdiff_t{-1});
            candidate += kAgcCbColor0SentinelLength;
        }
        // Sweep the same run for viewports that stand on their own. The
        // anchored search above only ever looks a fixed distance past a colour
        // target, so a block that carries nothing but viewports - Astro Bot
        // emits pairs of them for the bloom pyramid - was discarded whole, and
        // the viewport left over from an earlier pass was used instead. Left to
        // right, so the last one written wins, as it does on hardware.
        for (auto candidate = run_start; candidate + 10 <= run_end;) {
            if (!agc_sentinel_viewport_shape(entries, candidate)) {
                ++candidate;
                continue;
            }
            apply_agc_sentinel_values(
                entries,
                candidate,
                kAgcViewportSentinelOffsets.data(),
                kAgcViewportSentinelOffsets.size(),
                destination);
            agc_trace(
                "agc.indirect_viewport start=%zu x_scale=%.1f y_scale=%.1f\n",
                candidate,
                static_cast<double>(agc_float(entries[candidate].value)),
                static_cast<double>(
                    agc_float(entries[candidate + 1].value)));
            candidate += kAgcViewportSentinelOffsets.size();
        }
        run_start = run_end;
    }
}

void apply_indirect_registers(
    std::uint64_t packet_address,
    std::uint32_t packet_length,
    bool native_packet,
    bool context_registers,
    std::map<std::uint32_t, std::uint32_t>& destination,
    const char* space = "?") {
    std::uint32_t register_count = 0;
    std::uint64_t registers_address = 0;
    if (native_packet) {
        std::uint32_t register_count_word = 0;
        if (packet_length < 5 ||
            !try_read_u64(packet_address + 4, registers_address) ||
            !try_read_u32(
                packet_address + 16,
                register_count_word)) {
            return;
        }
        registers_address &= ~3ULL;
        register_count = register_count_word & 0x3FFFu;
    } else if (packet_length < 4 ||
               !try_read_u32(packet_address + 4, register_count) ||
               !try_read_u64(packet_address + 8, registers_address)) {
        return;
    }
    if (register_count > 4096) {
        return;
    }
    // AMD's own struct calls this field num_dwords - "number of DWords that the
    // CP will fetch". We read it as a count of eight byte (offset, value)
    // pairs, and so does KytyPS5, which would mean reading twice the data that
    // is there and treating the neighbouring allocation as register writes.
    // shadPS4's PM4 headers are where the field name comes from; whether AGC's
    // NOP-aliased form follows it is what this switch measures.
    static const auto halve_count = [] {
        const auto* value = std::getenv("PS5RT_AGC_COUNT_IS_DWORDS");
        return value != nullptr && value[0] == '1';
    }();
    if (halve_count) {
        register_count /= 2;
    }

    std::vector<AgcIndirectRegisterEntry> entries(register_count);
    for (std::uint32_t index = 0; index < register_count; ++index) {
        const auto entry_address =
            registers_address +
            (static_cast<std::uint64_t>(index) * 8);
        if (!try_read_u32(entry_address, entries[index].offset) ||
            !try_read_u32(entry_address + 4, entries[index].value)) {
            return;
        }
        if (entries[index].offset !=
            std::numeric_limits<std::uint32_t>::max()) {
            const auto raw_offset = entries[index].offset;
            if ((raw_offset & kAgcRegisterSelectorMask) != 0) {
                g_register_selector_seen.fetch_add(
                    1, std::memory_order_relaxed);
                if ((raw_offset & kAgcRegisterSelectorMask) ==
                    kAgcCxPsShaderUsageBase) {
                    g_register_selector_ps_input.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            entries[index].offset =
                normalize_agc_register_offset(raw_offset);
            destination[entries[index].offset] = entries[index].value;
            if (context_registers) {
                mark_stream_written_cx(entries[index].offset);
                note_mask_neighbourhood(
                    native_packet ? "indirect" : "indirect-aliased",
                    space,
                    entries[index].offset,
                    entries[index].value);
            }
        }
    }
    // Dump a few whole blocks verbatim, past the end, with no pairing applied.
    // Every reading of this format so far has been an interpretation; this is
    // the only way to see the structure itself.
    if (trace_register_writes() && context_registers) {
        static std::atomic<int> dumped {0};
        if (dumped.fetch_add(1) < 400) {
            // agc_trace writes into a 1152 byte buffer, so a block of any size
            // has to go out in slices or it is silently cut short - which is
            // how the interesting render target records kept missing from
            // these dumps while the small filler blocks came through whole.
            constexpr std::uint32_t kSliceDwords = 96;
            const auto total = std::min(register_count * 2, 4096u);
            for (std::uint32_t start = 0; start < total;
                 start += kSliceDwords) {
                std::string raw;
                const auto end =
                    std::min(start + kSliceDwords, total);
                bool short_read = false;
                for (std::uint32_t index = start; index < end; ++index) {
                    std::uint32_t word = 0;
                    if (!try_read_u32(
                            registers_address +
                                (static_cast<std::uint64_t>(index) * 4),
                            word)) {
                        short_read = true;
                        break;
                    }
                    char entry[16] = {};
                    std::snprintf(entry, sizeof(entry), " %08X", word);
                    raw += entry;
                }
                agc_trace(
                    "agc.register_block_raw count=%u block=0x%016llX "
                    "slice=%u dwords=%s\n",
                    register_count,
                    static_cast<unsigned long long>(registers_address),
                    start,
                    raw.c_str());
                if (short_read) {
                    break;
                }
            }
        }
    }
    if (trace_register_writes()) {
        std::string text;
        for (std::uint32_t index = 0;
             index < register_count && text.size() < 400;
             ++index) {
            char entry[32] = {};
            std::snprintf(
                entry,
                sizeof(entry),
                " %03X=%08X",
                entries[index].offset,
                entries[index].value);
            text += entry;
        }
        // The base register these values belong to is not in the entries, so
        // print the packet verbatim: whatever names the range has to be here.
        std::string header;
        for (std::uint32_t index = 0;
             index < packet_length && index < 8;
             ++index) {
            std::uint32_t word = 0;
            if (!try_read_u32(
                    packet_address +
                        (static_cast<std::uint64_t>(index) * 4),
                    word)) {
                break;
            }
            char entry[16] = {};
            std::snprintf(entry, sizeof(entry), " %08X", word);
            header += entry;
        }
        agc_trace(
            "agc.register_indirect space=%s native=%d len=%u count=%u "
            "block=0x%016llX packet=%s entries=%s\n",
            space,
            native_packet ? 1 : 0,
            packet_length,
            register_count,
            static_cast<unsigned long long>(registers_address),
            header.c_str(),
            text.c_str());
    }
    if (context_registers) {
        recover_agc_context_sentinels(entries, destination);
    }
}

bool is_draw_packet(
    std::uint32_t op,
    std::uint32_t packet_register) {
    return op == kAgcItDrawIndexAuto ||
        op == kAgcItDrawIndirect ||
        op == kAgcItDrawIndex2 ||
        op == kAgcItDrawIndexOffset2 ||
        op == kAgcItDrawIndexIndirect ||
        op == kAgcItDrawIndexMultiAuto ||
        (op == kAgcItNop && packet_register == kAgcRDrawIndexAuto);
}

struct AgcComputeDispatchInfo {
    std::uint64_t shader_address = 0;
    std::uint64_t dimensions_address = 0;
    std::uint32_t initiator = 0;
    std::uint32_t raw_x = 0;
    std::uint32_t raw_y = 0;
    std::uint32_t raw_z = 0;
    std::uint32_t base_x = 0;
    std::uint32_t base_y = 0;
    std::uint32_t base_z = 0;
    std::uint32_t group_count_x = 0;
    std::uint32_t group_count_y = 0;
    std::uint32_t group_count_z = 0;
    std::uint32_t local_size_x = 1;
    std::uint32_t local_size_y = 1;
    std::uint32_t local_size_z = 1;
    std::uint32_t thread_count_x =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t thread_count_y =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t thread_count_z =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t wave_lane_count = 64;
    bool indirect = false;
    bool valid_dimensions = false;
};

std::uint32_t get_agc_compute_local_size(
    const std::map<std::uint32_t, std::uint32_t>& registers,
    std::uint32_t register_address) {
    std::uint32_t value = 0;
    return try_get_agc_register(registers, register_address, value)
        ? std::max(value & 0xFFFFu, 1u)
        : 1u;
}

std::uint64_t hash_agc_compute_state(const AgcSubmittedState& state) {
    auto hash = 1469598103934665603ULL;
    const auto add_u32 = [&hash](std::uint32_t value) {
        for (std::uint32_t shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<std::uint8_t>(value >> shift);
            hash *= 1099511628211ULL;
        }
    };
    for (const auto& [address, value] : state.sh_registers) {
        add_u32(address);
        add_u32(value);
    }
    return hash;
}

bool decode_agc_compute_dispatch(
    const AgcSubmittedState& state,
    std::uint64_t packet_address,
    std::uint32_t packet_length,
    std::uint32_t op,
    AgcComputeDispatchInfo& dispatch) {
    dispatch = {};
    dispatch.indirect = op == kAgcItDispatchIndirect;
    if (op == kAgcItDispatchDirect) {
        if (packet_length < 5 ||
            !try_read_u32(packet_address + 16, dispatch.initiator)) {
            return false;
        }
        dispatch.dimensions_address = packet_address + 4;
    } else if (packet_length >= 4) {
        if (!try_read_u64(
                packet_address + 4,
                dispatch.dimensions_address) ||
            !try_read_u32(packet_address + 12, dispatch.initiator)) {
            return false;
        }
    } else {
        std::uint32_t data_offset = 0;
        if (packet_length < 3 ||
            state.indirect_args_address == 0 ||
            !try_read_u32(packet_address + 4, data_offset) ||
            !try_read_u32(packet_address + 8, dispatch.initiator)) {
            return false;
        }
        dispatch.dimensions_address =
            state.indirect_args_address + data_offset;
    }

    if (!try_read_u32(
            dispatch.dimensions_address,
            dispatch.raw_x) ||
        !try_read_u32(
            dispatch.dimensions_address + 4,
            dispatch.raw_y) ||
        !try_read_u32(
            dispatch.dimensions_address + 8,
            dispatch.raw_z)) {
        return false;
    }

    try_get_shader_address(
        state.sh_registers,
        kAgcComputePgmLo,
        kAgcComputePgmHi,
        dispatch.shader_address);
    dispatch.local_size_x = get_agc_compute_local_size(
        state.sh_registers, kAgcComputeNumThreadX);
    dispatch.local_size_y = get_agc_compute_local_size(
        state.sh_registers, kAgcComputeNumThreadY);
    dispatch.local_size_z = get_agc_compute_local_size(
        state.sh_registers, kAgcComputeNumThreadZ);
    dispatch.wave_lane_count =
        (dispatch.initiator & (1u << 15)) != 0 ? 32u : 64u;

    constexpr std::uint32_t kForceStartAtZero = 1u << 2;
    if ((dispatch.initiator & kForceStartAtZero) == 0) {
        try_get_agc_register(
            state.sh_registers,
            kAgcComputeStartX,
            dispatch.base_x);
        try_get_agc_register(
            state.sh_registers,
            kAgcComputeStartY,
            dispatch.base_y);
        try_get_agc_register(
            state.sh_registers,
            kAgcComputeStartZ,
            dispatch.base_z);
    }

    if (dispatch.raw_x == 0 ||
        dispatch.raw_y == 0 ||
        dispatch.raw_z == 0) {
        return true;
    }

    constexpr std::uint32_t kUseThreadDimensions = 1u << 5;
    if ((dispatch.initiator & kUseThreadDimensions) != 0) {
        const auto start_x =
            static_cast<std::uint64_t>(dispatch.base_x) *
            dispatch.local_size_x;
        const auto start_y =
            static_cast<std::uint64_t>(dispatch.base_y) *
            dispatch.local_size_y;
        const auto start_z =
            static_cast<std::uint64_t>(dispatch.base_z) *
            dispatch.local_size_z;
        if (dispatch.raw_x <= start_x ||
            dispatch.raw_y <= start_y ||
            dispatch.raw_z <= start_z) {
            return true;
        }
        dispatch.group_count_x = static_cast<std::uint32_t>(
            (dispatch.raw_x - start_x + dispatch.local_size_x - 1) /
            dispatch.local_size_x);
        dispatch.group_count_y = static_cast<std::uint32_t>(
            (dispatch.raw_y - start_y + dispatch.local_size_y - 1) /
            dispatch.local_size_y);
        dispatch.group_count_z = static_cast<std::uint32_t>(
            (dispatch.raw_z - start_z + dispatch.local_size_z - 1) /
            dispatch.local_size_z);
        dispatch.thread_count_x = dispatch.raw_x;
        dispatch.thread_count_y = dispatch.raw_y;
        dispatch.thread_count_z = dispatch.raw_z;
    } else {
        if (dispatch.raw_x <= dispatch.base_x ||
            dispatch.raw_y <= dispatch.base_y ||
            dispatch.raw_z <= dispatch.base_z) {
            return true;
        }
        dispatch.group_count_x = dispatch.raw_x - dispatch.base_x;
        dispatch.group_count_y = dispatch.raw_y - dispatch.base_y;
        dispatch.group_count_z = dispatch.raw_z - dispatch.base_z;
    }
    dispatch.valid_dimensions =
        dispatch.group_count_x != 0 &&
        dispatch.group_count_y != 0 &&
        dispatch.group_count_z != 0;
    return true;
}

void trace_agc_compute_manifest(
    const AgcComputeDispatchInfo& dispatch,
    const Ps5GpuShaderResult& result,
    const char* trace_scope) {
    if (result.resource_manifest == nullptr ||
        result.resource_manifest_size <
            sizeof(Ps5GpuResourceManifestHeader)) {
        return;
    }

    Ps5GpuResourceManifestHeader header = {};
    std::memcpy(
        &header,
        result.resource_manifest,
        sizeof(header));
    if (header.magic != PS5GPU_RESOURCE_MANIFEST_MAGIC ||
        header.version != PS5GPU_RESOURCE_MANIFEST_VERSION ||
        header.total_size > result.resource_manifest_size) {
        agc_trace(
            "%s.compute_manifest_invalid cs=0x%016llX bytes=%u\n",
            trace_scope,
            static_cast<unsigned long long>(dispatch.shader_address),
            result.resource_manifest_size);
        return;
    }

    const auto range_valid = [&header](
                                 std::uint32_t offset,
                                 std::uint32_t count,
                                 std::size_t record_size) {
        return offset <= header.total_size &&
            count <=
                (header.total_size - offset) / record_size;
    };
    if (!range_valid(
            header.global_offset,
            header.global_count,
            sizeof(Ps5GpuResourceGlobal)) ||
        !range_valid(
            header.image_offset,
            header.image_count,
            sizeof(Ps5GpuResourceImage))) {
        agc_trace(
            "%s.compute_manifest_ranges_invalid cs=0x%016llX\n",
            trace_scope,
            static_cast<unsigned long long>(dispatch.shader_address));
        return;
    }

    for (std::uint32_t index = 0;
         index < header.global_count;
         ++index) {
        Ps5GpuResourceGlobal resource = {};
        std::memcpy(
            &resource,
            result.resource_manifest +
                header.global_offset +
                (index * sizeof(resource)),
            sizeof(resource));
        if ((resource.flags & PS5GPU_RESOURCE_BUFFER_WRITABLE) == 0) {
            continue;
        }
        agc_trace(
            "%s.compute_buffer_writer cs=0x%016llX index=%u "
            "binding=%u scalar=%u addr=0x%016llX size=%u "
            "writeback=%u\n",
            trace_scope,
            static_cast<unsigned long long>(dispatch.shader_address),
            index,
            resource.descriptor_index,
            resource.scalar_address,
            static_cast<unsigned long long>(resource.base_address),
            resource.data_size,
            (resource.flags &
             PS5GPU_RESOURCE_BUFFER_WRITE_BACK) != 0
                ? 1u
                : 0u);
    }

    for (std::uint32_t index = 0;
         index < header.image_count;
         ++index) {
        Ps5GpuResourceImage resource = {};
        std::memcpy(
            &resource,
            result.resource_manifest +
                header.image_offset +
                (index * sizeof(resource)),
            sizeof(resource));
        if ((resource.flags & PS5GPU_RESOURCE_IMAGE_STORAGE) == 0) {
            continue;
        }
        agc_trace(
            "%s.compute_image_writer cs=0x%016llX index=%u "
            "binding=%u pc=0x%X addr=0x%016llX size=%ux%u mip=%u\n",
            trace_scope,
            static_cast<unsigned long long>(dispatch.shader_address),
            index,
            resource.binding,
            resource.pc,
            static_cast<unsigned long long>(resource.base_address),
            resource.width,
            resource.height,
            resource.mip_level);
    }
}

// What AGC's buffer fill writes, by the buffer it fills. A DCC fast clear
// is a fill of the target's DCC metadata with a code that says what colour
// the clear was - 0001 for opaque black - and the fill runs on the GPU, so
// guest memory never sees the code. The pass that decompresses DCC then
// fills the pixels with that colour; reading it from CB_COLORn_CLEAR_WORD
// instead gave transparent black for the title screen's UI plane, whose
// alpha the output pass multiplies the scene by, and the screen went black
// once the title appeared.
struct AgcBufferFill {
    std::uint64_t bytes = 0;
    std::uint32_t value = 0;
};
std::mutex g_agc_buffer_fills_mutex;
std::map<std::uint64_t, AgcBufferFill> g_agc_buffer_fills;

// The fill shader: v4 = s8 << 6 + v0; v0..v3 = s4..s7;
// buffer_store_format_xyzw v[0:3], v4, s[0:3] idxen; s_endpgm.
bool is_agc_buffer_fill_shader(std::uint64_t address) {
    static constexpr std::uint32_t kWords[] = {
        0xD7460004u, 0x04010C08u, 0x7E000204u, 0x7E020205u, 0x7E040206u,
        0x7E060207u, 0xE01C2000u, 0x80000004u, 0xBF810000u};
    for (std::uint32_t index = 0; index < 9; ++index) {
        std::uint32_t word = 0;
        if (!try_read_u32(address + index * 4ull, word) ||
            word != kWords[index]) {
            return false;
        }
    }
    return true;
}

void note_agc_buffer_fill(
    const AgcSubmittedState& state, std::uint64_t shader_address) {
    static std::map<std::uint64_t, bool> known_shaders;
    static std::mutex known_mutex;
    {
        std::lock_guard guard(known_mutex);
        auto found = known_shaders.find(shader_address);
        if (found == known_shaders.end()) {
            found = known_shaders
                        .emplace(shader_address,
                                 is_agc_buffer_fill_shader(shader_address))
                        .first;
        }
        if (!found->second) {
            return;
        }
    }
    const auto user = [&](std::uint32_t index) {
        const auto found =
            state.sh_registers.find(kAgcComputeUserDataRegister + index);
        return found == state.sh_registers.end() ? 0u : found->second;
    };
    const auto base = static_cast<std::uint64_t>(user(0)) |
        (static_cast<std::uint64_t>(user(1) & 0xFFFFu) << 32);
    const auto stride = (user(1) >> 16) & 0x3FFFu;
    const auto records = static_cast<std::uint64_t>(user(2));
    const auto bytes = stride == 0 ? records : records * stride;
    if (base == 0 || bytes == 0) {
        return;
    }
    std::lock_guard guard(g_agc_buffer_fills_mutex);
    g_agc_buffer_fills[base] = {bytes, user(4)};
    if (g_agc_buffer_fills.size() > 4096) {
        g_agc_buffer_fills.erase(g_agc_buffer_fills.begin());
    }
}

// The fill that covers an address, if one did.
bool agc_buffer_fill_at(std::uint64_t address, std::uint32_t& value) {
    std::lock_guard guard(g_agc_buffer_fills_mutex);
    auto found = g_agc_buffer_fills.upper_bound(address);
    if (found == g_agc_buffer_fills.begin()) {
        return false;
    }
    --found;
    if (address >= found->first + found->second.bytes) {
        return false;
    }
    value = found->second.value;
    return true;
}

void try_compile_compute_shader(
    AgcSubmittedState& state,
    const AgcComputeDispatchInfo& dispatch,
    std::uint32_t owner_handle,
    std::uint32_t packet_offset,
    std::uint32_t packet_opcode,
    const char* trace_scope) {
    if (dispatch.shader_address == 0 ||
        !dispatch.valid_dimensions) {
        return;
    }

    const auto state_hash = hash_agc_compute_state(state);
    auto state_id = state.compute_state_ids[state_hash];
    if (state_id == 0) {
        auto& attempt_count = state.compute_attempts[state_hash];
        if (attempt_count != 0) {
            return;
        }
        ++attempt_count;

        std::vector<Ps5GpuRegisterValue> registers;
        registers.reserve(state.sh_registers.size());
        for (const auto& [register_address, value] :
             state.sh_registers) {
            registers.push_back({register_address, value});
        }

        std::uint64_t shader_header = 0;
        try_get_agc_shader_header(
            dispatch.shader_address,
            shader_header);

        std::int32_t work_group_x_register = -1;
        std::int32_t work_group_y_register = -1;
        std::int32_t work_group_z_register = -1;
        std::int32_t thread_group_size_register = -1;
        std::uint32_t rsrc2 = 0;
        try_get_agc_register(
            state.sh_registers,
            kAgcComputePgmRsrc2,
            rsrc2);
        auto next_system_register =
            static_cast<std::int32_t>((rsrc2 >> 1) & 0x1Fu);
        if ((rsrc2 & (1u << 7)) != 0) {
            work_group_x_register = next_system_register++;
        }
        if ((rsrc2 & (1u << 8)) != 0) {
            work_group_y_register = next_system_register++;
        }
        if ((rsrc2 & (1u << 9)) != 0) {
            work_group_z_register = next_system_register++;
        }
        if ((rsrc2 & (1u << 10)) != 0) {
            thread_group_size_register = next_system_register++;
        }

        Ps5GpuShaderRequest request = {};
        request.struct_size = sizeof(request);
        request.abi_version = PS5GPU_ABI_VERSION;
        request.stage = PS5GPU_STAGE_COMPUTE;
        request.shader_address = dispatch.shader_address;
        request.shader_header_address = shader_header;
        request.registers = registers.data();
        request.register_count =
            static_cast<std::uint32_t>(registers.size());
        request.user_data_base_register = kAgcComputeUserDataRegister;
        request.local_size_x = dispatch.local_size_x;
        request.local_size_y = dispatch.local_size_y;
        request.local_size_z = dispatch.local_size_z;
        request.wave_lane_count = dispatch.wave_lane_count;
        request.storage_buffer_offset_alignment = 1;
        request.total_global_buffer_count = -1;
        // Descriptor 0 carries the dispatch's initial scalar registers and
        // the guest bindings start after it. Baking them into the module
        // instead makes one shader produce up to 73 different programs: a
        // 25 second run compiles 524 of them against 123 when they are
        // read, and the host ends up with 232 distinct programs against 21.
        //
        // This hung for as long as the compute translator had no base to
        // shift by. TryCompileComputeShader was written when guest bindings
        // always started at descriptor 0 and it passed a hard 0 through,
        // while the manifest was already placing them at base + index, so
        // every buffer access landed one descriptor low: binding 0 read the
        // register block and the last binding read past the end. The guest
        // wrote what came back into its own command buffer, which is how
        // dispatches with 0xFFFFFFFF group counts appeared - 510 of them in
        // a run. With the base wired through, six runs give 26 to 28 flips
        // and 1100 dispatches with none of those, against one to five flips
        // baking, which cannot keep up with 41 to 128 pipelines to build.
        //
        // PS5RT_COMPUTE_SCALAR_BLOCK=0 goes back to baking.
        static const auto scalar_block_enabled = [] {
            const auto* value =
                std::getenv("PS5RT_COMPUTE_SCALAR_BLOCK");
            return value == nullptr || value[0] != '0';
        }();
        if (scalar_block_enabled) {
            request.global_buffer_base = 1;
            request.initial_scalar_buffer_index = 0;
        } else {
            request.initial_scalar_buffer_index = -1;
        }
        request.compute_work_group_x_register =
            work_group_x_register;
        request.compute_work_group_y_register =
            work_group_y_register;
        request.compute_work_group_z_register =
            work_group_z_register;
        request.compute_thread_group_size_register =
            thread_group_size_register;

        // Whether this shader's control flow can be emitted as structured
        // SPIR-V, which decides how much of a translator the native path
        // needs. The C# one sidesteps the question with a program counter
        // dispatcher, and pays for it with every register living in memory.
        // Shaders come from a compiler working on structured source, so
        // most graphs should reduce - but "should" is not a number, and
        // this is the number.
        static std::set<std::uint64_t> analysed_shaders;
        static SRWLOCK analysed_lock = SRWLOCK_INIT;
        auto analyse_this_shader = false;
        if (analyse_shader_cfg_enabled()) {
            AcquireSRWLockExclusive(&analysed_lock);
            analyse_this_shader =
                analysed_shaders.insert(dispatch.shader_address).second;
            ReleaseSRWLockExclusive(&analysed_lock);
        }
        if (analyse_this_shader) {
            const auto summary = ps5gen5::build_cfg(
                0,
                [&](std::uint32_t pc) {
                    std::uint32_t word = 0;
                    try_read_u32(
                        dispatch.shader_address +
                            static_cast<std::uint64_t>(pc) * 4,
                        word);
                    return word;
                });
            // Now that the graph is known to be the shape structured
            // emission needs, run the native translator over the same
            // shader and say what it produced. The instructions it cannot
            // yet translate are counted rather than hidden, and the memory
            // traffic is reported beside what a program counter dispatcher
            // would have paid for the same instructions - which is the
            // whole reason the translation is being moved.
            if (summary.decoded && summary.reducible) {
                // The user data a dispatch hands its shader, read out of
                // the scalar registers the command buffer set. Every
                // address the shader computes starts from these, so an
                // interpreter given none of them resolves nothing.
                std::vector<std::uint32_t> user_data_values;
                std::uint32_t rsrc2 = 0;
                if (try_get_agc_register(
                        state.sh_registers, kAgcComputePgmRsrc2, rsrc2)) {
                    const auto user_sgpr_count = (rsrc2 >> 1) & 0x1Fu;
                    for (std::uint32_t index = 0; index < user_sgpr_count;
                         ++index) {
                        std::uint32_t value = 0;
                        if (!try_get_agc_register(
                                state.sh_registers,
                                kAgcComputeUserDataRegister + index,
                                value)) {
                            break;
                        }
                        user_data_values.push_back(value);
                    }
                }
                const std::uint32_t user_data_base = 0;
                {
                    // The user data itself, before anything is concluded
                    // from it. Every address the walk computes starts here,
                    // so if they are wrong this is where to look first.
                    static std::atomic<std::uint32_t> shown{0};
                    if (shown.fetch_add(1, std::memory_order_relaxed) < 3) {
                        std::string words;
                        for (std::size_t index = 0;
                             index < user_data_values.size(); ++index) {
                            char buffer[24] = {};
                            std::snprintf(
                                buffer, sizeof(buffer), "s%zu=0x%08X ",
                                index, user_data_values[index]);
                            words += buffer;
                        }
                        agc_trace(
                            "%s.shader_userdata rsrc2=0x%08X "
                            "count=%zu %s\n",
                            trace_scope,
                            rsrc2,
                            user_data_values.size(),
                            words.c_str());
                    }
                }
                const GuestCodeReader code_reader(dispatch.shader_address);
                const auto translated = ps5gen5::translate_shader(
                    0,
                    code_reader,
                    // The user data the dispatch hands the shader, which is
                    // where every address it computes begins.
                    user_data_values,
                    user_data_base,
                    [&](std::uint64_t address, std::uint32_t& word) {
                        const auto ok = try_read_u32(address, word);
                        // Every read failing says the addresses are wrong
                        // rather than the memory unreadable, so the first
                        // few are named.
                        static std::atomic<std::uint32_t> shown{0};
                        if (!ok &&
                            shown.fetch_add(1, std::memory_order_relaxed) <
                                8) {
                            agc_trace(
                                "%s.shader_read_failed "
                                "address=0x%016llX\n",
                                trace_scope,
                                static_cast<unsigned long long>(address));
                        }
                        return ok;
                    },
                    // Where the words came from, which is the address
                    // SGetpcB64 gives the shader.
                    dispatch.shader_address);
                note_emission_reuse(
                    translated, dispatch.shader_address, "compute");
                agc_trace(
                    "%s.shader_native cs=0x%016llX ok=%d words=%zu "
                    "blocks=%u decoded=%d planned=%d duplicated=%d "
                    "sloads=%u/%u images=%u/%u "
                    "paths=%u exhausted=%d reads=%u/%u "
                    "translated=%u skipped=%u "
                    "loads=%u stores=%u cached_reads=%u "
                    "dispatcher_loads=%u dispatcher_stores=%u\n",
                    trace_scope,
                    static_cast<unsigned long long>(
                        dispatch.shader_address),
                    translated.ok ? 1 : 0,
                    translated.words.size(),
                    translated.blocks,
                    translated.cfg_decoded ? 1 : 0,
                    translated.plan_complete ? 1 : 0,
                    translated.plan_duplicated_block ? 1 : 0,
                    translated.scalar_loads_resolved,
                    translated.scalar_loads,
                    translated.images_resolved,
                    translated.images_seen,
                    translated.scalar_paths,
                    translated.scalar_exhausted ? 1 : 0,
                    translated.memory_reads -
                        translated.memory_reads_failed,
                    translated.memory_reads,
                    translated.instructions_translated,
                    translated.instructions_skipped,
                    translated.register_stats.loads_emitted,
                    translated.register_stats.stores_emitted,
                    translated.register_stats.reads_served_from_cache,
                    translated.dispatcher_loads,
                    translated.dispatcher_stores);
                // Where the first few load addresses came from. An
                // address that is not a guest pointer is a register that
                // held something else, and the register says which.
                for (const auto& load : translated.first_loads) {
                    agc_trace(
                        "%s.shader_load pc=%u smem=%d word=0x%08X "
                        "base=s%u low=0x%08X "
                        "high=0x%08X address=0x%016llX known=%d\n",
                        trace_scope,
                        load.pc,
                        load.from_smem ? 1 : 0,
                        load.word0,
                        load.base_register,
                        load.base_low,
                        load.base_high,
                        static_cast<unsigned long long>(load.address),
                        load.address_known ? 1 : 0);
                }
                // An image that resolved nothing, with the registers it
                // wanted and the registers anything was actually put in.
                // The two not overlapping says the descriptor is addressed
                // some way this walk does not follow rather than that a
                // value was lost.
                for (const auto& image :
                     translated.first_unresolved_images) {
                    std::string written;
                    for (const auto destination :
                         translated.load_destinations) {
                        char buffer[12] = {};
                        std::snprintf(
                            buffer, sizeof(buffer), "%u,", destination);
                        written += buffer;
                    }
                    agc_trace(
                        "%s.shader_image_unresolved pc=%u srsrc=s%u "
                        "ssamp=s%u sample=%d missing=0x%02X wrote=%s\n",
                        trace_scope,
                        image.pc,
                        image.resource_register,
                        image.sampler_register,
                        image.is_sample ? 1 : 0,
                        image.missing_mask,
                        written.c_str());
                }

                // Which instructions cost the walk a register it knew.
                // An unknown that has to be unknown is honest; one that is
                // unknown because the walk cannot execute what wrote it is
                // work waiting, and this names it.
                for (const auto& [name, count] :
                     translated.unknown_by_name) {
                    agc_trace(
                        "%s.shader_unknown name=%s count=%u\n",
                        trace_scope,
                        name.c_str(),
                        count);
                }
                // The five commonest things it cannot do yet, which is how
                // the next piece of work gets chosen.
                std::vector<std::pair<std::uint32_t, std::string>> ranked;
                ranked.reserve(translated.skipped_by_name.size());
                for (const auto& [name, count] :
                     translated.skipped_by_name) {
                    ranked.push_back({count, name});
                }
                std::sort(ranked.begin(), ranked.end(),
                          [](const auto& left, const auto& right) {
                              return left.first > right.first;
                          });
                for (std::size_t index = 0;
                     index < ranked.size() && index < 5;
                     ++index) {
                    agc_trace(
                        "%s.shader_skipped cs=0x%016llX name=%s count=%u\n",
                        trace_scope,
                        static_cast<unsigned long long>(
                            dispatch.shader_address),
                        ranked[index].second.c_str(),
                        ranked[index].first);
                }
            }
            agc_trace(
                "%s.shader_cfg cs=0x%016llX decoded=%d reducible=%d "
                "instructions=%u blocks=%u edges=%u back_edges=%u "
                "words=%u failed_pc=%u failed_word=0x%08X\n",
                trace_scope,
                static_cast<unsigned long long>(dispatch.shader_address),
                summary.decoded ? 1 : 0,
                summary.reducible ? 1 : 0,
                summary.instructions,
                summary.blocks,
                summary.edges,
                summary.back_edges,
                summary.words,
                summary.failed_pc,
                summary.failed_word);
        }

        Ps5GpuShaderResult result = {};
        result.struct_size = sizeof(result);
        char error[1024] = {};
        Ps5GpuResult compile_result = PS5GPU_OK;

        // The native translator, when it is the producer. It owns its own
        // memory rather than the bridge's, so the frees below have to know
        // which of the two ran - freeing a vector's buffer with the
        // bridge's allocator is not a failure that reports itself.
        std::vector<std::uint32_t> native_words;
        std::vector<std::uint8_t> native_manifest;
        auto produced_natively = false;
        if (native_shader_producer_enabled()) {
            const ShaderStageScope stage_scope(
                ShaderStage::TranslateCompute);
            // The scalar registers the command buffer set, which is
            // where every address the shader computes begins. Read the
            // same way the analysis path reads them, from the same
            // registers - a producer given different user data than the
            // walk that resolved the descriptors would declare bindings
            // for memory the module never reaches.
            std::vector<std::uint32_t> user_data_values;
            std::uint32_t producer_rsrc2 = 0;
            // Where the workgroup's position lands: the SGPRs after the
            // user data, one for each axis COMPUTE_PGM_RSRC2 enables.
            std::int32_t producer_group_registers[3] = {-1, -1, -1};
            // PS5RT_COMPUTE_OLD_IDS=1: the old shape - 64 by 1 by 1 and no
            // thread or group numbers - to tell its effects apart.
            static const auto old_ids = [] {
                const auto* value = std::getenv("PS5RT_COMPUTE_OLD_IDS");
                return value != nullptr && value[0] == '1';
            }();
            if (!old_ids && try_get_agc_register(
                    state.sh_registers, kAgcComputePgmRsrc2,
                    producer_rsrc2)) {
                auto next_register =
                    static_cast<std::int32_t>((producer_rsrc2 >> 1) & 0x1Fu);
                for (std::uint32_t axis = 0; axis < 3; ++axis) {
                    if ((producer_rsrc2 & (1u << (7 + axis))) != 0) {
                        producer_group_registers[axis] = next_register++;
                    }
                }
            }
            if (try_get_agc_register(
                    state.sh_registers, kAgcComputePgmRsrc2,
                    producer_rsrc2)) {
                const auto user_sgpr_count = (producer_rsrc2 >> 1) & 0x1Fu;
                for (std::uint32_t index = 0; index < user_sgpr_count;
                     ++index) {
                    std::uint32_t value = 0;
                    if (!try_get_agc_register(
                            state.sh_registers,
                            kAgcComputeUserDataRegister + index,
                            value)) {
                        break;
                    }
                    user_data_values.push_back(value);
                }
            }
            const GuestCodeReader code_reader(dispatch.shader_address);
            const auto translated = ps5gen5::translate_shader(
                0,
                code_reader,
                user_data_values,
                0,
                [&](std::uint64_t address, std::uint32_t& word) {
                    return try_read_u32(address, word);
                },
                dispatch.shader_address,
                native_shader_producer_omits_stores(),
                native_shader_producer_omits_images(),
                native_shader_producer_omits_buffers(),
                ps5gen5::StageKind::Compute,
                0,
                -1,
                0,
                0,
                0,
                nullptr,
                old_ids ? 64u : dispatch.local_size_x,
                old_ids ? 1u : dispatch.local_size_y,
                old_ids ? 1u : dispatch.local_size_z,
                producer_group_registers[0],
                producer_group_registers[1],
                producer_group_registers[2]);
            note_emission_reuse(
                translated, dispatch.shader_address, "compute");
            // The gate that once stood here let only shaders writing
            // nothing be used, on the theory that a wrong binding was
            // corrupting the title. It was not: the fault was a module
            // claiming one block as the merge of several constructs, and
            // the driver died compiling it whether the module wrote
            // anything or not. Stores are back, and "readonly" remains as
            // a way to take them out again.
            if (translated.cfg_decoded && translated.plan_complete &&
                !translated.plan_duplicated_block &&
                !translated.words.empty() &&
                (native_shader_block_limit() == 0 ||
                 translated.blocks <= native_shader_block_limit()) &&
                !native_shader_producer_traces_only()) {
                native_words = translated.words;
                native_manifest = ps5gen5::build_manifest(
                    PS5GPU_STAGE_COMPUTE,
                    translated.manifest_buffers,
                    translated.manifest_images);
                produced_natively = true;
                result.spirv = reinterpret_cast<std::uint8_t*>(
                    native_words.data());
                result.spirv_size = static_cast<std::uint32_t>(
                    native_words.size() * sizeof(std::uint32_t));
                result.resource_manifest = native_manifest.data();
                result.resource_manifest_size =
                    static_cast<std::uint32_t>(native_manifest.size());
                result.global_memory_binding_count =
                    static_cast<std::uint32_t>(
                        translated.manifest_buffers.size());
                result.image_binding_count = static_cast<std::uint32_t>(
                    translated.manifest_images.size());
            }
            // The module on disk, so that what the driver was given can be
            // looked at without the title running. Named by the shader's
            // address, which is what every other trace keys on.
            const auto* dump_directory =
                std::getenv("PS5RT_NATIVE_SHADER_DUMP");
            if (dump_directory != nullptr && produced_natively) {
                char name[512] = {};
                std::snprintf(
                    name, sizeof(name), "%s/shader_%016llX.spv",
                    dump_directory,
                    static_cast<unsigned long long>(
                        dispatch.shader_address));
                if (auto* file = std::fopen(name, "wb")) {
                    std::fwrite(
                        native_words.data(), sizeof(std::uint32_t),
                        native_words.size(), file);
                    std::fclose(file);
                }
                // The guest code too, for tools/gen5-disasm.
                std::snprintf(
                    name, sizeof(name), "%s/code_%016llX.code",
                    dump_directory,
                    static_cast<unsigned long long>(
                        dispatch.shader_address));
                if (auto* file = std::fopen(name, "wb")) {
                    for (std::uint32_t index = 0; index < 16384; ++index) {
                        std::uint32_t word = 0;
                        if (!try_read_u32(
                                dispatch.shader_address +
                                    static_cast<std::uint64_t>(index) * 4,
                                word)) {
                            break;
                        }
                        std::fwrite(&word, 4, 1, file);
                        if (word == 0xBF810000u) {
                            break;
                        }
                    }
                    std::fclose(file);
                }
                std::snprintf(
                    name, sizeof(name), "%s/manifest_%016llX.bin",
                    dump_directory,
                    static_cast<unsigned long long>(
                        dispatch.shader_address));
                if (auto* file = std::fopen(name, "wb")) {
                    std::fwrite(
                        native_manifest.data(), 1, native_manifest.size(),
                        file);
                    std::fclose(file);
                }
            }
            agc_trace(
                "%s.compute_native_produced cs=0x%016llX used=%d "
                "words=%zu manifest=%zu buffers=%zu images=%zu "
                "translated=%u skipped=%u writes=%d "
                "baked_desc=%u baked_data=%u emitted=%u unbound=%u\n",
                trace_scope,
                static_cast<unsigned long long>(dispatch.shader_address),
                produced_natively ? 1 : 0,
                native_words.size(),
                native_manifest.size(),
                translated.manifest_buffers.size(),
                translated.manifest_images.size(),
                translated.instructions_translated,
                translated.instructions_skipped,
                translated.writes_memory ? 1 : 0,
                translated.loads_baked_descriptor,
                translated.loads_baked_data,
                translated.loads_emitted,
                translated.loads_unbound);
        }

        if (produced_natively && native_shader_producer_also_calls_bridge()) {
            // Both producers, on the same shader, in the same run. Two
            // runs of a title cannot be compared - the game state differs
            // by timing - but two translations of one dispatch can, and
            // what they should agree about is which memory they bind. A
            // module that binds different memory than the producer the
            // title has been drawing with is wrong however well it runs.
            Ps5GpuShaderResult theirs = {};
            theirs.struct_size = sizeof(theirs);
            char ignored[1024] = {};
            const ShaderStageScope stage_scope(
                ShaderStage::TranslateCompute);
            if (ps5rt_gpu_compile_spirv(
                    &request, &theirs, ignored, sizeof(ignored)) ==
                PS5GPU_OK) {
                compare_shader_bindings(
                    dispatch.shader_address,
                    theirs,
                    native_manifest,
                    trace_scope);
                ps5rt_gpu_free(theirs.spirv);
                ps5rt_gpu_free(theirs.resource_manifest);
            }
        }
        if (!produced_natively) {
            const ShaderStageScope stage_scope(
                ShaderStage::TranslateCompute);
            compile_result = ps5rt_gpu_compile_spirv(
                &request,
                &result,
                error,
                sizeof(error));
        }
        if (compile_result != PS5GPU_OK) {
            agc_trace(
                "%s.compute_spirv_error cs=0x%016llX "
                "hash=0x%016llX result=%d error=%s\n",
                trace_scope,
                static_cast<unsigned long long>(
                    dispatch.shader_address),
                static_cast<unsigned long long>(state_hash),
                static_cast<int>(compile_result),
                error[0] == '\0' ? "?" : error);
            return;
        }

        agc_trace(
            "%s.compute_spirv cs=0x%016llX hash=0x%016llX "
            "bytes=%u globals=%u images=%u manifest=%u\n",
            trace_scope,
            static_cast<unsigned long long>(dispatch.shader_address),
            static_cast<unsigned long long>(state_hash),
            result.spirv_size,
            result.global_memory_binding_count,
            result.image_binding_count,
            result.resource_manifest_size);
        trace_agc_compute_manifest(dispatch, result, trace_scope);

        Ps5GpuNativeComputeState native_state = {};
        native_state.struct_size = sizeof(native_state);
        native_state.abi_version = PS5GPU_NATIVE_ABI_VERSION;
        native_state.state_hash = state_hash;
        native_state.shader_address = dispatch.shader_address;
        native_state.shader_header_address = shader_header;
        native_state.spirv = result.spirv;
        native_state.spirv_size = result.spirv_size;
        native_state.resource_manifest = result.resource_manifest;
        native_state.resource_manifest_size =
            result.resource_manifest_size;
        bool registered = false;
        {
            const ShaderStageScope stage_scope(
                ShaderStage::RegisterCompute);
            registered = ps5rt_native_gpu_register_compute_state(
                &native_state,
                &state_id);
        }
        // Only the bridge's allocations go back to the bridge. The native
        // producer's are vectors on this stack.
        if (!produced_natively) {
            ps5rt_gpu_free(result.spirv);
            ps5rt_gpu_free(result.resource_manifest);
        }
        if (!registered || state_id == 0) {
            agc_trace(
                "%s.compute_native_register_failed "
                "cs=0x%016llX hash=0x%016llX\n",
                trace_scope,
                static_cast<unsigned long long>(
                    dispatch.shader_address),
                static_cast<unsigned long long>(state_hash));
            return;
        }
        state.compute_state_ids[state_hash] = state_id;
        agc_trace(
            "%s.compute_native_state id=%u cs=0x%016llX "
            "hash=0x%016llX\n",
            trace_scope,
            state_id,
            static_cast<unsigned long long>(dispatch.shader_address),
            static_cast<unsigned long long>(state_hash));
    }

    Ps5GpuNativeComputeDispatch native_dispatch = {};
    native_dispatch.struct_size = sizeof(native_dispatch);
    native_dispatch.abi_version = PS5GPU_NATIVE_ABI_VERSION;
    native_dispatch.submission_id = state.submission_count;
    native_dispatch.dispatch_id = state.dispatch_count;
    native_dispatch.owner_handle = owner_handle;
    native_dispatch.packet_offset_dwords = packet_offset;
    native_dispatch.packet_opcode = packet_opcode;
    native_dispatch.compute_state_id = state_id;
    native_dispatch.base_group_x = dispatch.base_x;
    native_dispatch.base_group_y = dispatch.base_y;
    native_dispatch.base_group_z = dispatch.base_z;
    native_dispatch.group_count_x = dispatch.group_count_x;
    native_dispatch.group_count_y = dispatch.group_count_y;
    native_dispatch.group_count_z = dispatch.group_count_z;
    native_dispatch.local_size_x = dispatch.local_size_x;
    native_dispatch.local_size_y = dispatch.local_size_y;
    native_dispatch.local_size_z = dispatch.local_size_z;
    native_dispatch.thread_count_x = dispatch.thread_count_x;
    native_dispatch.thread_count_y = dispatch.thread_count_y;
    native_dispatch.thread_count_z = dispatch.thread_count_z;
    // Either side of the submission, because the fault happens somewhere
    // after a state is registered and before a pipeline is built, and
    // this is the only call between the two.
    note_agc_buffer_fill(state, dispatch.shader_address);
    agc_trace(
        "%s.compute_submit_begin state=%u cs=0x%016llX\n",
        trace_scope,
        state_id,
        static_cast<unsigned long long>(dispatch.shader_address));
    const auto submitted = ps5rt_native_gpu_submit_compute(&native_dispatch);
    agc_trace(
        "%s.compute_submit_end state=%u ok=%d\n",
        trace_scope,
        state_id,
        submitted ? 1 : 0);
    if (!submitted) {
        agc_trace(
            "%s.compute_native_submit_failed id=%u "
            "submission=%llu dispatch=%llu\n",
            trace_scope,
            state_id,
            static_cast<unsigned long long>(state.submission_count),
            static_cast<unsigned long long>(state.dispatch_count));
    }
}

void trace_agc_compute_dispatch(
    AgcSubmittedState& state,
    const AgcComputeDispatchInfo& dispatch,
    std::uint32_t packet_offset,
    std::uint32_t op,
    const char* trace_scope) {
    std::string user_data;
    for (std::uint32_t index = 0; index < 16; ++index) {
        std::uint32_t value = 0;
        if (!try_get_agc_register(
                state.sh_registers,
                kAgcComputeUserDataRegister + index,
                value)) {
            continue;
        }
        char entry[40] = {};
        std::snprintf(
            entry,
            sizeof(entry),
            "%su%u=%08X",
            user_data.empty() ? "" : ",",
            index,
            value);
        user_data += entry;
    }
    if (user_data.empty()) {
        user_data = "-";
    }

    if (agc_packet_trace_enabled())
    agc_trace(
        "%s.compute_dispatch submission=%llu dispatch=%llu "
        "offset=%u op=0x%02X cs=0x%016llX dims=0x%016llX "
        "raw=%u/%u/%u base=%u/%u/%u groups=%u/%u/%u "
        "local=%u/%u/%u wave=%u initiator=0x%08X valid=%u "
        "user=[%s]\n",
        trace_scope,
        static_cast<unsigned long long>(state.submission_count),
        static_cast<unsigned long long>(state.dispatch_count),
        packet_offset,
        op,
        static_cast<unsigned long long>(dispatch.shader_address),
        static_cast<unsigned long long>(dispatch.dimensions_address),
        dispatch.raw_x,
        dispatch.raw_y,
        dispatch.raw_z,
        dispatch.base_x,
        dispatch.base_y,
        dispatch.base_z,
        dispatch.group_count_x,
        dispatch.group_count_y,
        dispatch.group_count_z,
        dispatch.local_size_x,
        dispatch.local_size_y,
        dispatch.local_size_z,
        dispatch.wave_lane_count,
        dispatch.initiator,
        dispatch.valid_dimensions ? 1u : 0u,
        user_data.c_str());
}

void try_compile_pixel_shader(AgcSubmittedState& state) {
    std::uint64_t shader_address = 0;
    if (!try_get_shader_address(
            state.sh_registers,
            kAgcSpiShaderPgmLoPs,
            kAgcSpiShaderPgmHiPs,
            shader_address)) {
        return;
    }

    auto& attempt_count = state.shader_attempts[shader_address];
    if (attempt_count >= 3) {
        return;
    }
    ++attempt_count;

    std::vector<Ps5GpuRegisterValue> registers;
    registers.reserve(state.sh_registers.size());
    for (const auto& [register_address, value] : state.sh_registers) {
        registers.push_back({register_address, value});
    }

    Ps5GpuShaderRequest request{};
    request.struct_size = sizeof(request);
    request.abi_version = PS5GPU_ABI_VERSION;
    request.stage = PS5GPU_STAGE_PIXEL;
    request.shader_address = shader_address;
    request.registers = registers.data();
    request.register_count =
        static_cast<std::uint32_t>(registers.size());
    request.user_data_base_register = kAgcPsUserDataRegister;
    request.wave_lane_count = 32;
    request.storage_buffer_offset_alignment = 1;
    request.total_global_buffer_count = -1;
    request.initial_scalar_buffer_index = -1;
    request.compute_work_group_x_register = -1;
    request.compute_work_group_y_register = -1;
    request.compute_work_group_z_register = -1;
    request.compute_thread_group_size_register = -1;

    const auto input_ena = state.cx_registers.find(kAgcSpiPsInputEna);
    const auto input_addr = state.cx_registers.find(kAgcSpiPsInputAddr);
    request.pixel_input_enable =
        input_ena == state.cx_registers.end() ? 0 : input_ena->second;
    request.pixel_input_address =
        input_addr == state.cx_registers.end() ? 0 : input_addr->second;

    Ps5GpuShaderResult result{};
    result.struct_size = sizeof(result);
    char error[768] = {};
    const auto compile_result = ps5rt_gpu_compile_spirv(
        &request,
        &result,
        error,
        sizeof(error));
    if (compile_result == PS5GPU_OK) {
        agc_trace(
            "agc.shader_spirv stage=ps addr=0x%016llX "
            "bytes=%u attrs=%u globals=%u images=%u inputs=%u "
            "manifest=%u\n",
            static_cast<unsigned long long>(shader_address),
            result.spirv_size,
            result.attribute_count,
            result.global_memory_binding_count,
            result.image_binding_count,
            result.vertex_input_count,
            result.resource_manifest_size);
        free_shader_allocation(result.spirv);
        free_shader_allocation(result.resource_manifest);
        return;
    }

    agc_trace(
        "agc.shader_spirv_error stage=ps addr=0x%016llX "
        "attempt=%u result=%d error=%s\n",
        static_cast<unsigned long long>(shader_address),
        attempt_count,
        static_cast<int>(compile_result),
        error[0] == '\0' ? "?" : error);
}

// The labels a submitted command buffer asks the GPU to write. Nothing
// here used to write them, so a guest polling a fence never saw it move
// and the native submit produced draws but never a frame.
//
// Packet layouts are from KytyPS5's PM4 parser (GPL-2.0, portable per
// CLAUDE.md): RELEASE_MEM carries event control, a destination address and
// a 64-bit value, and WRITE_DATA carries a control word, an address and a
// run of data dwords.
//
// This decoder has no pipeline to be at the end of, so the write lands as
// soon as the packet is parsed. That is a simplification - the value is
// right for any buffer, only its timing is early - not a guess about one.
void apply_agc_writeback(
    std::uint64_t packet_address,
    std::uint32_t packet_length,
    std::uint32_t op,
    std::uint32_t packet_register,
    const char* trace_scope) {
    const bool aliased = op == kAgcItNop;
    const bool release_mem = aliased
        ? packet_register == kAgcRReleaseMem
        : op == kAgcItReleaseMem;

    auto word = [&](std::uint32_t index, std::uint32_t& value) {
        return index + 1 < packet_length &&
            try_read_u32(
                packet_address +
                    (static_cast<std::uint64_t>(index) + 1) * 4,
                value);
    };

    if (release_mem) {
        std::uint32_t control = 0;
        std::uint32_t selector = 0;
        std::uint32_t address_lo = 0;
        std::uint32_t address_hi = 0;
        std::uint32_t data_lo = 0;
        std::uint32_t data_hi = 0;
        if (!word(0, control) || !word(1, selector) ||
            !word(2, address_lo) || !word(3, address_hi) ||
            !word(4, data_lo) || !word(5, data_hi)) {
            return;
        }

        // The aliased form keeps the data selector in a different field,
        // and numbers it differently: 2 is a 64-bit value and 3 the clock.
        std::uint32_t data_select = aliased
            ? (selector >> 16) & 0xFFu
            : (selector >> 29) & 0x7u;
        if (aliased) {
            data_select = data_select == 3 ? 4u : data_select;
        }

        const auto address =
            static_cast<std::uint64_t>(address_lo) |
            (static_cast<std::uint64_t>(address_hi) << 32);
        const auto value =
            static_cast<std::uint64_t>(data_lo) |
            (static_cast<std::uint64_t>(data_hi) << 32);
        if (address == 0) {
            return;
        }

        bool wrote = false;
        if (data_select == 1) {
            wrote = try_write_u32(address, data_lo);
        } else if (data_select == 2) {
            wrote = try_write_u64(address, value);
        } else if (data_select == 4) {
            LARGE_INTEGER counter = {};
            QueryPerformanceCounter(&counter);
            wrote = try_write_u64(
                address,
                static_cast<std::uint64_t>(counter.QuadPart));
        }

        if (agc_packet_trace_enabled())
        agc_trace(
            "%s.release_mem event=%u index=%u select=%u "
            "address=0x%016llX value=0x%016llX wrote=%d\n",
            trace_scope,
            control & 0x3Fu,
            (control >> 8) & 0x7u,
            data_select,
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(value),
            wrote ? 1 : 0);
        return;
    }

    // WRITE_DATA: control, then a 64-bit address, then the payload.
    std::uint32_t control = 0;
    std::uint32_t address_lo = 0;
    std::uint32_t address_hi = 0;
    if (!word(0, control) || !word(1, address_lo) ||
        !word(2, address_hi) || packet_length < 5) {
        return;
    }

    // The destination selector is split across two fields, which is how
    // AGC itself assembles the control word: bit 30 carries the low bit
    // and bits 8..11 the rest. Reading it as one nibble at bit 8 named a
    // different destination for every packet whose selector exceeded 1.
    // Selector 0 is a register rather than memory. The aliased form does
    // not carry the field at all and is always a memory write.
    const auto destination_select =
        ((control >> 30) & 0x1u) | ((control >> 7) & 0x1Eu);
    if (!aliased && destination_select == 0) {
        return;
    }

    // Bit 16 disables the address increment: every payload dword is
    // written to the same address rather than to consecutive ones. A
    // fence written this way ends up holding its last value, so treating
    // the packet as a copy left the label holding the first.
    const bool write_one_address = !aliased && ((control >> 16) & 0x1u) != 0;

    const auto address =
        static_cast<std::uint64_t>(address_lo) |
        (static_cast<std::uint64_t>(address_hi) << 32);
    if (address == 0) {
        return;
    }

    std::uint32_t written = 0;
    for (std::uint32_t index = 3; index + 1 < packet_length; ++index) {
        std::uint32_t value = 0;
        const auto destination = write_one_address
            ? address
            : address + (static_cast<std::uint64_t>(index) - 3) * 4;
        if (!word(index, value) || !try_write_u32(destination, value)) {
            break;
        }
        ++written;
    }

    agc_trace(
        "%s.write_data control=0x%08X select=%u one_address=%d "
        "address=0x%016llX dwords=%u\n",
        trace_scope,
        control,
        destination_select,
        write_one_address ? 1 : 0,
        static_cast<unsigned long long>(address),
        written);
}

// What the buffer asks the GPU to wait for. The parse never blocks - this
// decoder runs the whole buffer through - but the addresses and reference
// values are the other half of the writeback question: a label the guest
// polls that we fill with something else is invisible until the two lists
// are put side by side.
//
// Standard WAIT_REG_MEM keeps a 64-bit address in words 1-2; the aliased
// forms put the low half in word 0, which is what the patch helper for
// these packets already assumes.
void trace_agc_wait(
    std::uint64_t packet_address,
    std::uint32_t packet_length,
    std::uint32_t op,
    std::uint32_t packet_register,
    const char* trace_scope) {
    auto word = [&](std::uint32_t index, std::uint32_t& value) {
        return index + 1 < packet_length &&
            try_read_u32(
                packet_address +
                    (static_cast<std::uint64_t>(index) + 1) * 4,
                value);
    };

    const bool aliased = op == kAgcItNop;
    const bool wide = op == kAgcItWaitRegMem64 ||
        (aliased && packet_register == kAgcRWaitMem64);
    std::uint32_t control = 0;
    std::uint32_t address_lo = 0;
    std::uint32_t address_hi = 0;
    std::uint32_t reference = 0;
    std::uint32_t reference_hi = 0;
    std::uint32_t mask = 0;
    std::uint32_t mask_hi = 0;
    bool ok = false;
    if (aliased) {
        // Read from a live packet, not from a guess: the words arrive as
        // address, then mask, then reference, then control, then the poll
        // interval. Taking the third word as the reference reported every
        // fence as waiting for 0xFFFFFFFF - which is the mask - and left
        // the compare function reading as zero because control was never
        // read at all.
        ok = word(0, address_lo) && word(1, address_hi) &&
            word(2, mask) && word(3, reference);
        if (ok && wide) {
            ok = word(2, mask) && word(3, mask_hi) &&
                word(4, reference) && word(5, reference_hi) &&
                word(6, control);
        } else if (ok) {
            ok = word(4, control);
        }
    } else if (wide) {
        // Read from AGC's own builder: control, a split address whose
        // high half is eighteen bits, then a 64-bit reference and a
        // 64-bit mask in that order. Reading the mask first - which is
        // where the older layout put it - reported every fence as
        // waiting on 0xFFFFFFFF with an empty mask.
        ok = word(0, control) && word(1, address_lo) &&
            word(2, address_hi) && word(3, reference) &&
            word(4, reference_hi) && word(5, mask) && word(6, mask_hi);
    } else {
        ok = word(0, control) && word(1, address_lo) &&
            word(2, address_hi) && word(3, reference) && word(4, mask);
    }
    if (!ok) {
        return;
    }

    const auto address =
        static_cast<std::uint64_t>(address_lo) |
        (static_cast<std::uint64_t>(address_hi & 0x3FFFFu) << 32);
    std::uint32_t present = 0;
    const bool readable = try_read_u32(address, present);
    if (agc_packet_trace_enabled())
    agc_trace(
        "%s.wait_mem width=%s address=0x%016llX reference=0x%08X%08X "
        "mask=0x%08X%08X control=0x%08X function=%u holds=0x%08X "
        "readable=%d\n",
        trace_scope,
        wide ? "64" : "32",
        static_cast<unsigned long long>(address),
        reference_hi,
        reference,
        mask_hi,
        mask,
        control,
        control & 0x7u,
        present,
        readable ? 1 : 0);
}

// A DMA_DATA packet, executed: a copy between two ranges of guest memory,
// or a fill with one value. The packets were built and traced and never
// carried out, so every copy a title asked the GPU for simply did not
// happen - the intro video's frames among them, which the title copies
// from the player's buffer into its own texture before drawing it.
//
// The two layouts are the ones the builders above write: the compute form
// is seven dwords with the selectors last, the graphics form eight with
// the selectors first. A selector of 0 or 3 is memory, 1 the GDS (not
// modelled), 2 on the destination nowhere and on the source an immediate
// value to fill with.
constexpr std::uint32_t kAgcRDmaDataPacket = 0x19;
extern "C" void ps5rt_native_gpu_guest_written(
    std::uint64_t address, std::uint64_t size);
std::atomic<std::uint64_t> g_dma_data_executed{0};
std::atomic<std::uint64_t> g_dma_data_executed_bytes{0};

void apply_agc_dma_data(
    std::uint64_t packet_address,
    std::uint32_t packet_length,
    const char* trace_scope) {
    std::uint64_t destination = 0;
    std::uint64_t source = 0;
    std::uint32_t byte_count = 0;
    std::uint32_t destination_select = 0;
    std::uint32_t source_select = 0;
    if (packet_length == 7) {
        std::uint32_t selectors = 0;
        if (!try_read_u64(packet_address + 4, destination) ||
            !try_read_u64(packet_address + 12, source) ||
            !try_read_u32(packet_address + 20, byte_count) ||
            !try_read_u32(packet_address + 24, selectors)) {
            return;
        }
        destination_select = selectors & 0xFFu;
        source_select = (selectors >> 8) & 0xFFu;
    } else if (packet_length >= 8) {
        std::uint32_t control0 = 0;
        if (!try_read_u32(packet_address + 4, control0) ||
            !try_read_u32(packet_address + 12, byte_count) ||
            !try_read_u64(packet_address + 16, destination) ||
            !try_read_u64(packet_address + 24, source)) {
            return;
        }
        destination_select = control0 & 0xFFu;
        source_select = (control0 >> 16) & 0xFFu;
    } else {
        return;
    }
    const auto memory = [](std::uint32_t select) {
        return select == 0 || select == 3;
    };
    if (byte_count == 0 || byte_count > 256u * 1024u * 1024u ||
        destination < 0x10000 || !memory(destination_select)) {
        return;
    }
    // The GPU runtime watches textures for writes by catching the faults
    // they raise, and this write raises none. Tell it first, or the texture
    // is never uploaded again - the intro video stayed on its first, black,
    // frame.
    ps5rt_native_gpu_guest_written(destination, byte_count);
    auto done = false;
    if (source_select == 2) {
        const auto value = static_cast<std::uint32_t>(source);
        std::uint32_t chunk[4096];
        for (auto& word : chunk) {
            word = value;
        }
        done = true;
        for (std::uint64_t offset = 0; done && offset < byte_count;
             offset += sizeof(chunk)) {
            const auto length = static_cast<std::size_t>(
                std::min<std::uint64_t>(sizeof(chunk), byte_count - offset));
            done = try_write_process_bytes(destination + offset, chunk, length);
        }
    } else if (memory(source_select) && source >= 0x10000) {
        // The source is this process's own memory, so the write reads it
        // directly - one call for twelve megabytes of video frame.
        done = try_write_process_bytes(
            destination,
            reinterpret_cast<const void*>(source),
            byte_count);
    }
    if (done) {
        g_dma_data_executed.fetch_add(1, std::memory_order_relaxed);
        g_dma_data_executed_bytes.fetch_add(
            byte_count, std::memory_order_relaxed);
    }
    static std::atomic<std::uint32_t> shown{0};
    static std::atomic<std::uint32_t> shown_large{0};
    // The small ones are labels; the large ones are frames of video, and
    // whether those arrive is the question more often asked.
    if (shown.fetch_add(1, std::memory_order_relaxed) < 16 ||
        (byte_count >= (1u << 20) &&
         shown_large.fetch_add(1, std::memory_order_relaxed) < 64)) {
        agc_trace(
            "%s.dma_data_executed dst=0x%016llX src=0x%016llX bytes=%u "
            "dst_sel=%u src_sel=%u done=%d\n",
            trace_scope,
            static_cast<unsigned long long>(destination),
            static_cast<unsigned long long>(source),
            byte_count,
            destination_select,
            source_select,
            done ? 1 : 0);
    }
}

void parse_agc_dcb(
    AgcSubmittedState& state,
    std::uint64_t command_address,
    std::uint32_t dword_count,
    const char* trace_scope,
    bool compile_pixel_shaders,
    bool compile_compute_shaders,
    const char* submission_kind,
    std::uint32_t owner_handle) {
    std::uint32_t offset = 0;
    std::uint32_t packet_count = 0;
    std::uint32_t draw_count = 0;
    // Twenty-nine consecutive draws of one shader state rendered identically,
    // which means whatever distinguishes them is a packet we either do not
    // decode or do not act on. PS5RT_TRACE_DRAW_PACKETS lists what the guest
    // emits between one draw and the next.
    static const auto trace_draw_packets = [] {
        const auto* value = std::getenv("PS5RT_TRACE_DRAW_PACKETS");
        return value != nullptr && value[0] == '1';
    }();
    std::string packets_since_draw;
    while (offset < dword_count) {
        const auto packet_address =
            command_address +
            (static_cast<std::uint64_t>(offset) * sizeof(std::uint32_t));
        std::uint32_t header = 0;
        if (!try_read_u32(packet_address, header)) {
            break;
        }

        const auto packet_type = header >> 30;
        if (packet_type == 2) {
            ++offset;
            continue;
        }
        if (packet_type != 3) {
            break;
        }

        const auto packet_length = agc_pm4_length(header);
        if (packet_length == 0 ||
            packet_length > dword_count - offset) {
            break;
        }

        const auto op = (header >> 8) & 0xFFu;
        const auto packet_register = (header >> 2) & 0x3Fu;
        ++packet_count;

        // The defaults were only ever applied where the stream asked for a
        // reset, and the graphics stream never asks: its submissions
        // carried a hundred context registers where the compute stream's
        // carried a hundred and forty-seven. So every draw in the run was
        // decoded against whatever the command buffer had set and nothing
        // else - no CB_COLOR_CONTROL, no scissor rectangles, none of the
        // state a real context starts in. Seeding on first use costs one
        // flag and covers the stream that never resets.
        if (!state.seeded) {
            seed_agc_default_state(state);
        }

        if (op == kAgcItNop &&
            (packet_register == kAgcRDrawReset ||
             packet_register == kAgcRAcbReset)) {
            state.sh_registers.clear();
            state.cx_registers.clear();
            state.uc_registers.clear();
            state.indirect_args_address = 0;
            seed_agc_default_state(state);
        } else if (op == kAgcItSetShReg) {
            apply_direct_registers(
                packet_address,
                packet_length,
                state.sh_registers,
                "sh");
        } else if (op == kAgcItSetContextReg) {
            apply_direct_registers(
                packet_address,
                packet_length,
                state.cx_registers,
                "cx");
        } else if (op == kAgcItSetUconfigReg) {
            apply_direct_registers(
                packet_address,
                packet_length,
                state.uc_registers,
                "uc");
        } else if (op == kAgcItSetShRegIndirect) {
            apply_indirect_registers(
                packet_address,
                packet_length,
                true,
                false,
                state.sh_registers,
                "sh");
        } else if (op == kAgcItSetContextRegIndirect) {
            apply_indirect_registers(
                packet_address,
                packet_length,
                true,
                true,
                state.cx_registers,
                "cx");
        } else if (op == kAgcItSetUconfigRegIndirect) {
            apply_indirect_registers(
                packet_address,
                packet_length,
                true,
                false,
                state.uc_registers,
                "uc");
        } else if (op == kAgcItNop &&
                   packet_register == kAgcRShRegsIndirect) {
            apply_indirect_registers(
                packet_address,
                packet_length,
                false,
                false,
                state.sh_registers,
                "sh");
        } else if (op == kAgcItNop &&
                   packet_register == kAgcRCxRegsIndirect) {
            apply_indirect_registers(
                packet_address,
                packet_length,
                false,
                true,
                state.cx_registers,
                "cx");
        } else if (op == kAgcItNop &&
                   packet_register == kAgcRUcRegsIndirect) {
            apply_indirect_registers(
                packet_address,
                packet_length,
                false,
                false,
                state.uc_registers,
                "uc");
        } else if (op == kAgcItNop && packet_register == kAgcRDmaDataPacket) {
            apply_agc_dma_data(packet_address, packet_length, trace_scope);
        } else if (
            op == kAgcItReleaseMem ||
            op == kAgcItWriteData ||
            (op == kAgcItNop &&
             (packet_register == kAgcRReleaseMem ||
              packet_register == kAgcRWriteData))) {
            apply_agc_writeback(
                packet_address,
                packet_length,
                op,
                packet_register,
                trace_scope);
        } else if (
            op == kAgcItWaitRegMem ||
            op == kAgcItWaitRegMem64 ||
            (op == kAgcItNop &&
             (packet_register == kAgcRWaitMem32 ||
              packet_register == kAgcRWaitMem64))) {
            trace_agc_wait(
                packet_address,
                packet_length,
                op,
                packet_register,
                trace_scope);
        } else if (op == kAgcItIndexBaseOp && packet_length >= 3) {
            std::uint32_t base_lo = 0;
            std::uint32_t base_hi = 0;
            if (try_read_u32(packet_address + 4, base_lo) &&
                try_read_u32(packet_address + 8, base_hi)) {
                state.index_base =
                    (static_cast<std::uint64_t>(base_hi) << 32) | base_lo;
            }
        } else if (op == kAgcItIndexTypeOp && packet_length >= 2) {
            std::uint32_t index_type = 0;
            if (try_read_u32(packet_address + 4, index_type)) {
                state.index_bytes = (index_type & 0x3u) == 0 ? 2u : 4u;
            }
        } else if (op == kAgcItEventWriteEop) {
            // GFX10 replaced this with RELEASE_MEM and it has not been
            // seen on this title. Guessing a layout for a packet that
            // never arrives would be worse than recording that it did.
            agc_trace(
                "%s.event_write_eop_unhandled len=%u\n",
                trace_scope,
                packet_length);
        }

        if (op == kAgcItSetBase &&
            packet_length >= 4) {
            std::uint32_t base_selector = 0;
            std::uint64_t indirect_args_address = 0;
            if (try_read_u32(
                    packet_address + 4,
                    base_selector) &&
                base_selector == 1 &&
                try_read_u64(
                    packet_address + 8,
                    indirect_args_address)) {
                state.indirect_args_address =
                    indirect_args_address;
            }
        }

        if (op == kAgcItDispatchDirect ||
            op == kAgcItDispatchIndirect) {
            AgcComputeDispatchInfo dispatch = {};
            ++state.dispatch_count;
            if (decode_agc_compute_dispatch(
                    state,
                    packet_address,
                    packet_length,
                    op,
                    dispatch)) {
                trace_agc_compute_dispatch(
                    state,
                    dispatch,
                    offset,
                    op,
                    trace_scope);
                if (compile_compute_shaders) {
                    try_compile_compute_shader(
                        state,
                        dispatch,
                        owner_handle,
                        offset,
                        op,
                        trace_scope);
                }
            } else {
                agc_trace(
                    "%s.compute_dispatch_invalid submission=%llu "
                    "dispatch=%llu offset=%u op=0x%02X len=%u "
                    "indirect_base=0x%016llX\n",
                    trace_scope,
                    static_cast<unsigned long long>(
                        state.submission_count),
                    static_cast<unsigned long long>(
                        state.dispatch_count),
                    offset,
                    op,
                    packet_length,
                    static_cast<unsigned long long>(
                        state.indirect_args_address));
            }
        }

        if (op == kAgcItNop &&
            packet_register == kAgcRFlip &&
            packet_length >= 6) {
            std::uint32_t handle = 0;
            std::uint32_t buffer_index = 0;
            std::uint32_t flip_mode = 0;
            std::uint32_t flip_argument_low = 0;
            std::uint32_t flip_argument_high = 0;
            if (try_read_u32(packet_address + 4, handle) &&
                try_read_u32(packet_address + 8, buffer_index) &&
                try_read_u32(packet_address + 12, flip_mode) &&
                try_read_u32(
                    packet_address + 16,
                    flip_argument_low) &&
                try_read_u32(
                    packet_address + 20,
                    flip_argument_high) &&
                // Both the shadow parse and the authoritative native
                // one must act on this packet. Only the shadow used to,
                // so with PS5RECOMP_AGC_NATIVE_SUBMIT on nothing ever
                // asked videoout to present: buffers registered, no
                // flip. The scopes cannot both be live at once - "agc"
                // exists only when the native path has authority.
                (std::strcmp(trace_scope, "native_shadow.agc") == 0 ||
                 std::strcmp(trace_scope, "agc") == 0)) {
                observe_videoout_shadow_flip(
                    handle,
                    static_cast<std::int32_t>(buffer_index),
                    flip_mode,
                    (static_cast<std::uint64_t>(
                         flip_argument_high) << 32) |
                        flip_argument_low,
                    state.submission_count);
            }
        }

        if (trace_draw_packets && !is_draw_packet(op, packet_register)) {
            char entry[32] = {};
            std::snprintf(
                entry,
                sizeof(entry),
                " %02X/%02X:%u",
                op,
                packet_register,
                packet_length);
            if (packets_since_draw.size() < 900) {
                packets_since_draw += entry;
            }
        }
        if (is_draw_packet(op, packet_register) && trace_register_writes()) {
            // We decode one word of this packet - the vertex count. It is
            // seven dwords long, and whatever distinguishes consecutive draws
            // of one shader state has to be in the rest of it.
            std::string words;
            for (std::uint32_t index = 0;
                 index < packet_length && index < 12;
                 ++index) {
                std::uint32_t word = 0;
                if (!try_read_u32(
                        packet_address +
                            (static_cast<std::uint64_t>(index) * 4),
                        word)) {
                    break;
                }
                char entry[16] = {};
                std::snprintf(entry, sizeof(entry), " %08X", word);
                words += entry;
            }
            agc_trace(
                "agc.draw_packet_raw submission=%llu draw=%u len=%u words=%s\n",
                static_cast<unsigned long long>(state.submission_count),
                draw_count + 1,
                packet_length,
                words.c_str());
        }
        if (is_draw_packet(op, packet_register)) {
            ++draw_count;
            ++state.draw_count;
            trace_agc_draw_milestone(
                state,
                draw_count,
                offset,
                op,
                packet_register,
                decode_agc_draw_info(
                    packet_address,
                    packet_length,
                    op,
                    packet_register,
                    state.index_base,
                    state.index_bytes),
                trace_scope);
            if (compile_pixel_shaders) {
                try_compile_pixel_shader(state);
            }
            if (trace_draw_packets) {
                agc_trace(
                    "%s.draw_packets submission=%llu draw=%u offset=%u "
                    "sh=%zu cx=%zu uc=%zu between=%s\n",
                    trace_scope,
                    static_cast<unsigned long long>(
                        state.submission_count),
                    draw_count,
                    offset,
                    state.sh_registers.size(),
                    state.cx_registers.size(),
                    state.uc_registers.size(),
                    packets_since_draw.empty()
                        ? "-"
                        : packets_since_draw.c_str());
                packets_since_draw.clear();
            }
        }

        offset += packet_length;
    }

    agc_trace(
        "%s.%s submission=%llu addr=0x%016llX "
        "dwords=%u packets=%u draws=%u total_draws=%llu "
        "dispatches=%llu "
        "sh=%zu cx=%zu uc=%zu\n",
        trace_scope,
        submission_kind,
        static_cast<unsigned long long>(state.submission_count),
        static_cast<unsigned long long>(command_address),
        dword_count,
        packet_count,
        draw_count,
        static_cast<unsigned long long>(state.draw_count),
        static_cast<unsigned long long>(state.dispatch_count),
        state.sh_registers.size(),
        state.cx_registers.size(),
        state.uc_registers.size());
}

} // namespace

// Called from the runner's periodic bridge-call heartbeat, because the
// audio loop keeps that ticking at about 190 iterations a second even
// while everything else is stuck.
extern "C" void ps5rt_report_outstanding_waits(std::uint64_t minimum_ms) {
    const auto now = GetTickCount64();
    std::string text;
    AcquireSRWLockShared(&g_wait_slots_lock);
    for (const auto* slot : g_wait_slots) {
        const auto kind = slot->active_kind.load(std::memory_order_acquire);
        if (kind < 0 || text.size() > 600) {
            continue;
        }
        const auto since =
            slot->active_since_ms.load(std::memory_order_relaxed);
        const auto age = now > since ? now - since : 0;
        if (age < minimum_ms) {
            continue;
        }
        char line[112] = {};
        std::snprintf(
            line,
            sizeof(line),
            " tid=%u:%s(0x%llX):%llums",
            slot->thread_id,
            wait_kind_name(static_cast<WaitKind>(kind)),
            static_cast<unsigned long long>(
                slot->active_detail.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(age));
        text += line;
    }
    ReleaseSRWLockShared(&g_wait_slots_lock);
    if (!text.empty()) {
        videoout_shadow_trace("hle.outstanding_waits%s\n", text.c_str());
    }
}

std::uint64_t ps5rt_agc_init_real(
    std::uint64_t state_address,
    std::uint64_t version) {
    PS5_HLE_GUARD();
    const auto result =
        state_address != 0 && is_supported_agc_defaults_version(version)
            ? 0ULL
            : 0x80020003ULL;
    trace_stderr(
        "hle=sceAgcInit state=0x%016llX version=%llu "
        "result=0x%08llX\n",
        static_cast<unsigned long long>(state_address),
        static_cast<unsigned long long>(version),
        static_cast<unsigned long long>(result));
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_agc_get_register_defaults_real(std::uint64_t version) {
    PS5_HLE_GUARD();
    static const auto built = [] {
        build_agc_register_defaults();
        return true;
    }();
    (void)built;
    const auto result = is_supported_agc_defaults_version(version)
        ? reinterpret_cast<std::uint64_t>(&g_agc_register_defaults)
        : 0;
    static volatile LONG trace_count = 0;
    const auto count = InterlockedIncrement(&trace_count);
    if (count <= 8) {
        agc_trace(
            "agc.get_register_defaults count=%ld version=%llu "
            "result=0x%016llX cx_table=0x%016llX groups=%u\n",
            static_cast<long>(count),
            static_cast<unsigned long long>(version),
            static_cast<unsigned long long>(result),
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(
                    g_agc_register_defaults.space_slots[0])),
            g_agc_register_defaults.group_count);
    }
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_agc_get_register_defaults_internal_real(
    std::uint64_t version) {
    PS5_HLE_GUARD();
    static const auto built = [] {
        build_agc_register_defaults();
        return true;
    }();
    (void)built;
    const auto result = is_supported_agc_defaults_version(version)
        ? reinterpret_cast<std::uint64_t>(&g_agc_register_defaults_internal)
        : 0;
    static volatile LONG trace_count = 0;
    const auto count = InterlockedIncrement(&trace_count);
    if (count <= 8) {
        agc_trace(
            "agc.get_register_defaults_internal count=%ld version=%llu "
            "result=0x%016llX\n",
            static_cast<long>(count),
            static_cast<unsigned long long>(version),
            static_cast<unsigned long long>(result));
    }
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_agc_get_data_packet_payload_address_real(
    std::uint64_t output_address,
    std::uint64_t command_address,
    std::uint64_t type) {
    PS5_HLE_GUARD();
    constexpr std::uint64_t kInvalidArgument = 0x80020016ULL;
    constexpr std::uint64_t kMemoryFault = 0x8002000EULL;
    if (output_address == 0 || command_address == 0) {
        restore_guest_fs();
        return kInvalidArgument;
    }

    auto payload_address = command_address + 8;
    if (type == 0) {
        std::uint32_t header = 0;
        if (!try_read_u32(command_address, header)) {
            restore_guest_fs();
            return kMemoryFault;
        }
        payload_address = (header & 0x3FFF0000U) == 0x3FFF0000U
            ? 0
            : command_address + 4;
    }
    if (!try_write_u64(output_address, payload_address)) {
        restore_guest_fs();
        return kMemoryFault;
    }

    static volatile LONG trace_count = 0;
    const auto count = InterlockedIncrement(&trace_count);
    if (count <= 8) {
        agc_trace(
            "agc.get_packet_payload count=%ld out=0x%016llX "
            "cmd=0x%016llX type=%llu payload=0x%016llX\n",
            static_cast<long>(count),
            static_cast<unsigned long long>(output_address),
            static_cast<unsigned long long>(command_address),
            static_cast<unsigned long long>(type),
            static_cast<unsigned long long>(payload_address));
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_agc_write_data_patch_address_real(
    std::uint64_t first,
    std::uint64_t second) {
    PS5_HLE_GUARD();
    constexpr std::uint64_t kInvalidArgument = 0x80020016ULL;
    constexpr std::uint64_t kMemoryFault = 0x8002000EULL;
    constexpr std::uint64_t kGuestAddressLimit = 0x0000700000000000ULL;
    constexpr std::uint32_t kAgcRWriteData = 0x15;

    // AGC's own patch helper accepts one thing: a packet whose opcode
    // byte is WRITE_DATA, and it writes the full 64-bit address at
    // offset 8. Recognising only the NOP-aliased spelling meant a
    // standard packet fell through to the branch below, which treats the
    // second argument as a bare address to write - so a caller passing a
    // packet got its header overwritten instead of its address field.
    const auto is_write_data_packet = [](std::uint64_t address) {
        std::uint32_t header = 0;
        if (address == 0 || !try_read_u32(address, header)) {
            return false;
        }
        const auto op = (header >> 8) & 0xFFU;
        return op == kAgcItWriteData ||
            (op == kAgcItNop &&
             ((header >> 2) & 0x3FU) == kAgcRWriteData);
    };

    std::uint64_t field_address = 0;
    std::uint64_t value = 0;
    if (is_write_data_packet(first)) {
        field_address = first + 8;
        value = second;
    } else if (is_write_data_packet(second)) {
        field_address = second + 8;
        value = first;
    } else {
        field_address = second;
        value = first;
    }

    if (field_address == 0 || field_address >= kGuestAddressLimit ||
        !is_writable_process_range(field_address, sizeof(std::uint64_t))) {
        static volatile LONG reject_trace_count = 0;
        const auto count = InterlockedIncrement(&reject_trace_count);
        if (count <= 8) {
            agc_trace(
                "agc.patch_write_data_rejected count=%ld "
                "field=0x%016llX value=0x%016llX\n",
                static_cast<long>(count),
                static_cast<unsigned long long>(field_address),
                static_cast<unsigned long long>(value));
        }
        restore_guest_fs();
        return kInvalidArgument;
    }
    if (!try_write_u64(field_address, value)) {
        restore_guest_fs();
        return kMemoryFault;
    }

    static volatile LONG trace_count = 0;
    const auto count = InterlockedIncrement(&trace_count);
    if (count <= 8) {
        agc_trace(
            "agc.patch_write_data_address count=%ld "
            "field=0x%016llX value=0x%016llX\n",
            static_cast<long>(count),
            static_cast<unsigned long long>(field_address),
            static_cast<unsigned long long>(value));
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_agc_wait_reg_mem_patch_address_real(
    std::uint64_t command_address,
    std::uint64_t address) {
    PS5_HLE_GUARD();
    constexpr std::uint64_t kInvalidArgument = 0x80020016ULL;
    constexpr std::uint64_t kMemoryFault = 0x8002000EULL;

    std::uint32_t header = 0;
    if (!try_read_u32(command_address, header)) {
        restore_guest_fs();
        return kInvalidArgument;
    }

    const auto op = (header >> 8) & 0xFFU;
    const auto packet_register = (header >> 2) & 0x3FU;

    // What the packet actually looks like, in full. Three encodings are
    // in play - a standard WAIT_REG_MEM, a NOP-aliased one, and the pair
    // AGC's own helper expects, where a four-dword SET_UCONFIG_REG packet
    // carries the address and the wait packet follows it - and which one
    // this title emits decides every other layout question. Recording the
    // words costs one trace and settles it.
    {
        static volatile LONG shape_trace_count = 0;
        const auto shape_count = InterlockedIncrement(&shape_trace_count);
        if (shape_count <= 4) {
            std::string words;
            for (std::uint32_t index = 0; index < 14; ++index) {
                std::uint32_t value = 0;
                if (!try_read_u32(
                        command_address +
                            static_cast<std::uint64_t>(index) * 4,
                        value)) {
                    break;
                }
                char entry[16] = {};
                std::snprintf(entry, sizeof(entry), " %08X", value);
                words += entry;
            }
            agc_trace(
                "agc.patch_wait_shape count=%ld cmd=0x%016llX len=%u"
                "%s\n",
                static_cast<long>(shape_count),
                static_cast<unsigned long long>(command_address),
                ((header >> 16) & 0x3FFFU) + 2,
                words.c_str());
        }
    }

    bool wrote = false;
    if (op == kAgcItWaitRegMem || op == kAgcItWaitRegMem64) {
        // Verified against AGC's own patch helper: the low half keeps the
        // alignment bits already in the word, and the high half is 18 bits
        // wide with the rest of the word preserved.
        const std::uint32_t alignment_mask =
            op == kAgcItWaitRegMem64 ? 0x7U : 0x3U;
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        wrote =
            try_read_u32(command_address + 8, low) &&
            try_read_u32(command_address + 12, high) &&
            try_write_u32(
                command_address + 8,
                (low & alignment_mask) |
                    (static_cast<std::uint32_t>(address) & ~alignment_mask)) &&
            try_write_u32(
                command_address + 12,
                (high & 0xFFFC0000U) |
                    (static_cast<std::uint32_t>(address >> 32) & 0x3FFFFU));
    } else if (op == kAgcItNop &&
               (packet_register == kAgcRWaitMem32 ||
                packet_register == kAgcRWaitMem64)) {
        const auto alignment_mask =
            packet_register == kAgcRWaitMem32 ? ~0x3U : ~0x7U;
        wrote =
            try_write_u32(
                command_address + 4,
                static_cast<std::uint32_t>(address) & alignment_mask) &&
            try_write_u32(
                command_address + 8,
                static_cast<std::uint32_t>(address >> 32) & 0x3FFFFU);
    } else {
        restore_guest_fs();
        return kInvalidArgument;
    }

    static volatile LONG trace_count = 0;
    const auto count = InterlockedIncrement(&trace_count);
    if (count <= 8) {
        agc_trace(
            "agc.patch_wait_address count=%ld cmd=0x%016llX "
            "address=0x%016llX op=0x%02X reg=0x%02X result=%s\n",
            static_cast<long>(count),
            static_cast<unsigned long long>(command_address),
            static_cast<unsigned long long>(address),
            op,
            packet_register,
            wrote ? "ok" : "fault");
    }
    restore_guest_fs();
    return wrote ? 0 : kMemoryFault;
}

std::uint64_t ps5rt_agc_create_shader_real(
    std::uint64_t destination_address,
    std::uint64_t header_address,
    std::uint64_t code_address) {
    PS5_HLE_GUARD();
    constexpr std::uint64_t kInvalidArgument = 0x80020016ULL;
    constexpr std::uint64_t kMemoryFault = 0x8002000EULL;

    if (header_address == 0 || code_address == 0) {
        restore_guest_fs();
        return kInvalidArgument;
    }

    std::uint32_t file_header = 0;
    std::uint32_t version = 0;
    if (!try_read_u32(header_address, file_header) ||
        !try_read_u32(header_address + 4, version)) {
        restore_guest_fs();
        return kMemoryFault;
    }
    if (file_header != kAgcShaderFileHeader ||
        version != kAgcShaderVersion) {
        agc_trace(
            "agc.create_shader invalid_header dst=0x%016llX "
            "header=0x%016llX code=0x%016llX file=0x%08X version=0x%08X\n",
            static_cast<unsigned long long>(destination_address),
            static_cast<unsigned long long>(header_address),
            static_cast<unsigned long long>(code_address),
            file_header,
            version);
        restore_guest_fs();
        return kInvalidArgument;
    }

    if (!relocate_agc_pointer_field(
            header_address + kAgcShaderCxRegistersOffset) ||
        !relocate_agc_pointer_field(
            header_address + kAgcShaderShRegistersOffset) ||
        !relocate_agc_pointer_field(
            header_address + kAgcShaderUserDataOffset) ||
        !relocate_agc_pointer_field(
            header_address + kAgcShaderSpecialsOffset) ||
        !relocate_agc_pointer_field(
            header_address + kAgcShaderInputSemanticsOffset) ||
        !relocate_agc_pointer_field(
            header_address + kAgcShaderOutputSemanticsOffset) ||
        !try_write_u64(
            header_address + kAgcShaderCodeOffset,
            code_address)) {
        restore_guest_fs();
        return kMemoryFault;
    }

    std::uint64_t user_data_address = 0;
    if (!try_read_u64(
            header_address + kAgcShaderUserDataOffset,
            user_data_address)) {
        restore_guest_fs();
        return kMemoryFault;
    }
    if (user_data_address != 0) {
        for (std::uint64_t offset = 0; offset <= 0x20; offset += 8) {
            if (!relocate_agc_pointer_field(
                    user_data_address + offset)) {
                restore_guest_fs();
                return kMemoryFault;
            }
        }
    }

    if (!patch_agc_shader_program_registers(
            header_address,
            code_address)) {
        agc_trace(
            "agc.create_shader invalid_registers header=0x%016llX "
            "code=0x%016llX\n",
            static_cast<unsigned long long>(header_address),
            static_cast<unsigned long long>(code_address));
        restore_guest_fs();
        return kInvalidArgument;
    }

    if (destination_address != 0 &&
        !try_write_u64(destination_address, header_address)) {
        restore_guest_fs();
        return kMemoryFault;
    }

    AcquireSRWLockExclusive(&g_agc_shader_header_lock);
    g_agc_shader_headers[code_address] = header_address;
    ReleaseSRWLockExclusive(&g_agc_shader_header_lock);

    static volatile LONG create_shader_count = 0;
    const auto count = InterlockedIncrement(&create_shader_count);
    if (count <= 8) {
        agc_trace(
            "agc.create_shader count=%ld dst=0x%016llX "
            "header=0x%016llX code=0x%016llX ok\n",
            static_cast<long>(count),
            static_cast<unsigned long long>(destination_address),
            static_cast<unsigned long long>(header_address),
            static_cast<unsigned long long>(code_address));
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_agc_get_fused_shader_size_real(
    std::uint64_t size_align_address,
    std::uint64_t first_shader_address,
    std::uint64_t second_shader_address) {
    PS5_HLE_GUARD();
    if (size_align_address == 0 ||
        first_shader_address == 0 ||
        second_shader_address == 0) {
        restore_guest_fs();
        return 0x8A6C000A;
    }

    const std::array<std::uint8_t, 9> empty_size_align = {};
    const auto result = try_write_process_bytes(
        size_align_address,
        empty_size_align.data(),
        empty_size_align.size())
        ? 0ULL
        : 0x80020101ULL;
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_agc_fuse_shader_halves_real(
    std::uint64_t destination_address,
    std::uint64_t first_shader_address,
    std::uint64_t second_shader_address,
    std::uint64_t) {
    PS5_HLE_GUARD();
    if (destination_address == 0 ||
        first_shader_address == 0 ||
        second_shader_address == 0) {
        restore_guest_fs();
        return 0x8A6C000A;
    }

    const std::array<std::uint8_t, 0x60> empty_shader = {};
    if (!try_write_process_bytes(
            destination_address,
            empty_shader.data(),
            empty_shader.size())) {
        restore_guest_fs();
        return 0x80020101;
    }

    restore_guest_fs();
    return 0x8A6C0008;
}

std::uint64_t ps5rt_agc_driver_submit_dcb_real(
    std::uint64_t packet_address, std::uint64_t, std::uint64_t) {
    PS5_HLE_GUARD();
    std::uint64_t command_address = 0;
    std::uint32_t dword_count = 0;
    if (packet_address == 0 ||
        !try_read_u64(packet_address, command_address) ||
        !try_read_u32(packet_address + 8, dword_count) ||
        command_address == 0 ||
        dword_count == 0 ||
        dword_count > kAgcMaximumDwords) {
        agc_trace(
            "agc.submit_dcb_invalid packet=0x%016llX\n",
            static_cast<unsigned long long>(packet_address));
        restore_guest_fs();
        return 0x80020003;
    }

    AcquireSRWLockExclusive(&g_agc_submit_lock);
    ++g_agc_submitted_state.submission_count;
    parse_agc_dcb(
        g_agc_submitted_state,
        command_address,
        dword_count,
        "agc",
        true,
        true,
        "submit_dcb",
        0);
    ReleaseSRWLockExclusive(&g_agc_submit_lock);
    restore_guest_fs();
    return 0;
}

void ps5rt_agc_shadow_submit_dcb(std::uint64_t packet_address) {
    PS5_HLE_GUARD();
    std::uint64_t command_address = 0;
    std::uint32_t dword_count = 0;
    if (packet_address == 0 ||
        !try_read_u64(packet_address, command_address) ||
        !try_read_u32(packet_address + 8, dword_count) ||
        command_address == 0 ||
        dword_count == 0 ||
        dword_count > kAgcMaximumDwords) {
        agc_trace(
            "native_shadow.agc.submit_dcb_invalid "
            "packet=0x%016llX\n",
            static_cast<unsigned long long>(packet_address));
        restore_guest_fs();
        return;
    }

    AcquireSRWLockExclusive(&g_agc_shadow_lock);
    ++g_agc_shadow_state.submission_count;
    parse_agc_dcb(
        g_agc_shadow_state,
        command_address,
        dword_count,
        "native_shadow.agc",
        false,
        true,
        "submit_dcb",
        0);
    ReleaseSRWLockExclusive(&g_agc_shadow_lock);
    restore_guest_fs();
}

void ps5rt_agc_shadow_submit_acb(
    std::uint64_t owner_handle,
    std::uint64_t packet_address) {
    PS5_HLE_GUARD();
    std::uint64_t command_address = 0;
    std::uint32_t dword_count = 0;
    if (packet_address == 0 ||
        !try_read_u64(packet_address, command_address) ||
        !try_read_u32(packet_address + 8, dword_count) ||
        command_address == 0 ||
        dword_count == 0 ||
        dword_count > kAgcMaximumDwords) {
        agc_trace(
            "native_shadow.agc.submit_acb_invalid owner=%u "
            "packet=0x%016llX\n",
            static_cast<std::uint32_t>(owner_handle),
            static_cast<unsigned long long>(packet_address));
        restore_guest_fs();
        return;
    }

    AcquireSRWLockExclusive(&g_agc_compute_lock);
    auto& state = g_agc_shadow_compute_states[
        static_cast<std::uint32_t>(owner_handle)];
    ++state.submission_count;
    char trace_scope[80] = {};
    std::snprintf(
        trace_scope,
        sizeof(trace_scope),
        "native_shadow.agc.compute[%u]",
        static_cast<std::uint32_t>(owner_handle));
    parse_agc_dcb(
        state,
        command_address,
        dword_count,
        trace_scope,
        false,
        true,
        "submit_acb",
        static_cast<std::uint32_t>(owner_handle));
    ReleaseSRWLockExclusive(&g_agc_compute_lock);
    restore_guest_fs();
}

std::uint64_t ps5rt_agc_driver_submit_acb_real(
    std::uint64_t owner_handle,
    std::uint64_t packet_address,
    std::uint64_t) {
    PS5_HLE_GUARD();
    std::uint64_t command_address = 0;
    std::uint32_t dword_count = 0;
    if (packet_address == 0 ||
        !try_read_u64(packet_address, command_address) ||
        !try_read_u32(packet_address + 8, dword_count) ||
        command_address == 0 ||
        dword_count == 0 ||
        dword_count > kAgcMaximumDwords) {
        agc_trace(
            "agc.submit_acb_invalid owner=%u packet=0x%016llX\n",
            static_cast<std::uint32_t>(owner_handle),
            static_cast<unsigned long long>(packet_address));
        restore_guest_fs();
        return 0x80020003;
    }

    AcquireSRWLockExclusive(&g_agc_compute_lock);
    auto& state =
        g_agc_compute_states[static_cast<std::uint32_t>(owner_handle)];
    ++state.submission_count;
    char trace_scope[64] = {};
    std::snprintf(
        trace_scope,
        sizeof(trace_scope),
        "agc.compute[%u]",
        static_cast<std::uint32_t>(owner_handle));
    parse_agc_dcb(
        state,
        command_address,
        dword_count,
        trace_scope,
        false,
        true,
        "submit_acb",
        static_cast<std::uint32_t>(owner_handle));
    ReleaseSRWLockExclusive(&g_agc_compute_lock);
    restore_guest_fs();
    return 0;
}


std::uint64_t ps5rt_agc_dcb_draw_index_real(
    std::uint64_t, std::uint64_t) {
    PS5_HLE_GUARD();
    trace_stderr("hle=sceAgcDcbDrawIndex\n");
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_agc_dcb_set_sh_registers_real(
    std::uint64_t, std::uint64_t, std::uint64_t) {
    PS5_HLE_GUARD();
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_agc_dcb_nop_real(std::uint64_t) {
    PS5_HLE_GUARD();
    restore_guest_fs();
    return 0;
}




// --- AGC command buffer writers ---
//
// These build PM4 packets in the guest's own command buffer: read a cursor,
// bump it, write a handful of dwords. Served from the managed bridge they
// averaged between four and forty milliseconds a call - not because the work
// is large, but because every call crossed into the bridge and back, and the
// title makes thousands of them per frame. Here the work is what it looks
// like. The packet layouts follow the managed implementation exactly; the
// decoder on the other side reads what this writes.

namespace {

constexpr std::uint32_t kAgcRZero = 0x00;
constexpr std::uint32_t kAgcRPushMarker = 0x0B;
constexpr std::uint32_t kAgcRPopMarker = 0x0C;
constexpr std::uint32_t kAgcRAcquireMem = 0x14;
constexpr std::uint32_t kAgcCbSetShRegisterRangeMarker = 0x6875000D;

constexpr std::uint64_t kAgcCbCursorUpOffset = 0x10;
constexpr std::uint64_t kAgcCbCursorDownOffset = 0x18;
constexpr std::uint64_t kAgcCbCallbackOffset = 0x20;
constexpr std::uint64_t kAgcCbUserDataOffset = 0x28;
constexpr std::uint64_t kAgcCbReservedDwOffset = 0x30;

std::uint32_t agc_pm4(
    std::uint32_t length_dwords, std::uint32_t op, std::uint32_t reg) {
    const auto length =
        (static_cast<std::uint32_t>(
             static_cast<std::uint16_t>(length_dwords)) -
         2U) &
        0x3FFFU;
    return 0xC0000000U | (length << 16) | ((op & 0xFFU) << 8) |
        ((reg & 0x3FU) << 2);
}

std::uint32_t agc_remaining_dwords(
    std::uint64_t cursor_up,
    std::uint64_t cursor_down,
    std::uint32_t reserved_dwords) {
    const std::uint64_t available = cursor_down >= cursor_up
        ? (cursor_down - cursor_up) / sizeof(std::uint32_t)
        : 0;
    const std::uint64_t capped =
        available > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : available;
    return capped > reserved_dwords
        ? static_cast<std::uint32_t>(capped) - reserved_dwords
        : 0;
}

using AgcFullCallback = PS5RT_GUEST_ABI std::uint64_t (*)(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t);

// Claims dwords at the buffer's up-cursor and advances it. When the buffer is
// full the guest supplies its own callback to make room; the managed
// implementation calls it through the scheduler, and calling it directly is
// the same thing without the detour.
bool agc_allocate_command_dwords(
    std::uint64_t command_buffer_address,
    std::uint32_t size_dwords,
    std::uint64_t& command_address) {
    command_address = 0;
    if (size_dwords == 0 || command_buffer_address == 0) {
        return false;
    }

    std::uint64_t cursor_up = 0;
    std::uint64_t cursor_down = 0;
    std::uint64_t callback = 0;
    std::uint64_t user_data = 0;
    std::uint32_t reserved_dwords = 0;
    if (!try_read_u64(
            command_buffer_address + kAgcCbCursorUpOffset, cursor_up) ||
        !try_read_u64(
            command_buffer_address + kAgcCbCursorDownOffset, cursor_down) ||
        !try_read_u64(
            command_buffer_address + kAgcCbCallbackOffset, callback) ||
        !try_read_u64(
            command_buffer_address + kAgcCbUserDataOffset, user_data) ||
        !try_read_u32(
            command_buffer_address + kAgcCbReservedDwOffset,
            reserved_dwords)) {
        return false;
    }

    if (size_dwords >
        agc_remaining_dwords(cursor_up, cursor_down, reserved_dwords)) {
        agc_trace(
            "agc.cmd_alloc_full buf=0x%016llX need=%u callback=0x%016llX\n",
            static_cast<unsigned long long>(command_buffer_address),
            size_dwords,
            static_cast<unsigned long long>(callback));
        if (callback == 0) {
            return false;
        }

        reinterpret_cast<AgcFullCallback>(callback)(
            command_buffer_address,
            static_cast<std::uint64_t>(size_dwords) + reserved_dwords,
            user_data,
            0,
            0);

        if (!try_read_u64(
                command_buffer_address + kAgcCbCursorUpOffset, cursor_up) ||
            !try_read_u64(
                command_buffer_address + kAgcCbCursorDownOffset,
                cursor_down) ||
            !try_read_u32(
                command_buffer_address + kAgcCbReservedDwOffset,
                reserved_dwords) ||
            size_dwords >
                agc_remaining_dwords(
                    cursor_up, cursor_down, reserved_dwords)) {
            agc_trace(
                "agc.cmd_alloc_callback_no_space buf=0x%016llX need=%u\n",
                static_cast<unsigned long long>(command_buffer_address),
                size_dwords);
            return false;
        }
    }

    const auto next_cursor = cursor_up +
        (static_cast<std::uint64_t>(size_dwords) * sizeof(std::uint32_t));
    if (!try_write_u64(
            command_buffer_address + kAgcCbCursorUpOffset, next_cursor)) {
        return false;
    }

    command_address = cursor_up;
    return true;
}

// The managed versions trace every call. These run thousands of times a
// frame, and a WriteFile each would put back a good part of what moving them
// here takes away, so the first few of each are enough to show the shape.
bool agc_trace_first(volatile LONG& counter) {
    return InterlockedIncrement(&counter) <= 4;
}

}  // namespace

std::uint64_t ps5rt_agc_cb_release_mem_real(
    std::uint64_t command_buffer_address,
    std::uint64_t action_raw,
    std::uint64_t gcr_control_raw,
    std::uint64_t destination_raw,
    std::uint64_t cache_policy_raw,
    std::uint64_t destination_address,
    std::uint64_t data_selection_raw,
    std::uint64_t data,
    std::uint64_t gds_offset_raw,
    std::uint64_t gds_size_raw,
    std::uint64_t interrupt_raw,
    std::uint64_t interrupt_context_id_raw) {
    PS5_HLE_GUARD();
    const auto action = static_cast<std::uint32_t>(action_raw & 0xFF);
    const auto gcr_control =
        static_cast<std::uint32_t>(gcr_control_raw & 0xFFFF);
    const auto destination =
        static_cast<std::uint32_t>(destination_raw & 0xFF);
    const auto cache_policy =
        static_cast<std::uint32_t>(cache_policy_raw & 0xFF);
    const auto data_selection =
        static_cast<std::uint32_t>(data_selection_raw & 0xFF);
    const auto gds_offset =
        static_cast<std::uint32_t>(gds_offset_raw & 0xFFFF);
    const auto gds_size = static_cast<std::uint32_t>(gds_size_raw & 0xFFFF);
    const auto interrupt = static_cast<std::uint32_t>(interrupt_raw & 0xFF);
    const auto interrupt_context_id =
        static_cast<std::uint32_t>(interrupt_context_id_raw);

    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 || destination > 1 ||
        data_selection > 3 || gds_offset != 0 || gds_size > 2 ||
        interrupt > 3 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 8, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(8, kAgcItNop, kAgcRReleaseMem)) ||
        !try_write_u32(command_address + 4, action | (cache_policy << 8)) ||
        !try_write_u32(
            command_address + 8,
            gcr_control | (data_selection << 16) | (interrupt << 24)) ||
        !try_write_u32(
            command_address + 12,
            static_cast<std::uint32_t>(destination_address)) ||
        !try_write_u32(
            command_address + 16,
            static_cast<std::uint32_t>(destination_address >> 32)) ||
        !try_write_u32(
            command_address + 20, static_cast<std::uint32_t>(data)) ||
        !try_write_u32(
            command_address + 24,
            static_cast<std::uint32_t>(data >> 32)) ||
        !try_write_u32(command_address + 28, interrupt_context_id)) {
        restore_guest_fs();
        return 0;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.cb_release_mem buf=0x%016llX cmd=0x%016llX action=0x%02X "
            "gcr=0x%04X dst=0x%016llX data_sel=%u data=0x%016llX\n",
            static_cast<unsigned long long>(command_buffer_address),
            static_cast<unsigned long long>(command_address),
            action,
            gcr_control,
            static_cast<unsigned long long>(destination_address),
            data_selection,
            static_cast<unsigned long long>(data));
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_cb_set_sh_register_range_direct_real(
    std::uint64_t command_buffer_address,
    std::uint64_t offset_raw,
    std::uint64_t values_address,
    std::uint64_t value_count_raw) {
    PS5_HLE_GUARD();
    const auto offset = static_cast<std::uint32_t>(offset_raw);
    const auto value_count = static_cast<std::uint32_t>(value_count_raw);
    if (command_buffer_address == 0 || offset == 0 || offset > 0x3FF ||
        value_count == 0) {
        restore_guest_fs();
        return 0;
    }

    std::uint64_t marker_address = 0;
    std::uint64_t command_address = 0;
    if (!agc_allocate_command_dwords(
            command_buffer_address, 2, marker_address) ||
        !try_write_u32(marker_address, agc_pm4(2, kAgcItNop, kAgcRZero)) ||
        !try_write_u32(marker_address + 4, kAgcCbSetShRegisterRangeMarker) ||
        !agc_allocate_command_dwords(
            command_buffer_address, value_count + 2, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(value_count + 2, kAgcItSetShReg, 0)) ||
        !try_write_u32(command_address + 4, offset)) {
        restore_guest_fs();
        return 0;
    }

    for (std::uint32_t index = 0; index < value_count; ++index) {
        std::uint32_t value = 0;
        if (values_address != 0 &&
            !try_read_u32(
                values_address +
                    (static_cast<std::uint64_t>(index) *
                     sizeof(std::uint32_t)),
                value)) {
            restore_guest_fs();
            return 0;
        }
        if (!try_write_u32(
                command_address + 8 +
                    (static_cast<std::uint64_t>(index) *
                     sizeof(std::uint32_t)),
                value)) {
            restore_guest_fs();
            return 0;
        }
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.cb_set_sh_range buf=0x%016llX cmd=0x%016llX offset=0x%08X "
            "count=%u\n",
            static_cast<unsigned long long>(command_buffer_address),
            static_cast<unsigned long long>(command_address),
            offset,
            value_count);
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_cb_set_sh_registers_direct_real(
    std::uint64_t command_buffer_address,
    std::uint64_t registers_address,
    std::uint64_t register_count_raw) {
    PS5_HLE_GUARD();
    const auto register_count =
        static_cast<std::uint32_t>(register_count_raw);
    if (register_count == 0) {
        restore_guest_fs();
        return 0;
    }
    if (command_buffer_address == 0 || registers_address == 0 ||
        register_count > 4096) {
        restore_guest_fs();
        return 0;
    }

    // Offset and value pairs, sorted by offset so that consecutive offsets
    // share one SET_SH_REG packet - the same grouping the managed version
    // does, and what the decoder on the other side expects to see.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> registers;
    registers.reserve(register_count);
    for (std::uint32_t index = 0; index < register_count; ++index) {
        const auto entry_address =
            registers_address + (static_cast<std::uint64_t>(index) * 8);
        std::uint32_t offset = 0;
        std::uint32_t value = 0;
        if (!try_read_u32(entry_address, offset) ||
            !try_read_u32(entry_address + sizeof(std::uint32_t), value)) {
            restore_guest_fs();
            return 0;
        }
        registers.emplace_back(offset, value);
    }

    std::stable_sort(
        registers.begin(),
        registers.end(),
        [](const auto& left, const auto& right) {
            return left.first < right.first;
        });

    std::uint64_t first_command_address = 0;
    std::size_t start_index = 0;
    while (start_index < registers.size()) {
        auto end_index = start_index + 1;
        while (end_index < registers.size() &&
               registers[end_index].first ==
                   registers[end_index - 1].first + 1) {
            ++end_index;
        }

        const auto value_count =
            static_cast<std::uint32_t>(end_index - start_index);
        const auto packet_dwords = value_count + 2;
        std::uint64_t command_address = 0;
        if (!agc_allocate_command_dwords(
                command_buffer_address, packet_dwords, command_address) ||
            !try_write_u32(
                command_address,
                agc_pm4(packet_dwords, kAgcItSetShReg, 0)) ||
            !try_write_u32(
                command_address + 4,
                registers[start_index].first & 0xFFFFU)) {
            restore_guest_fs();
            return 0;
        }

        if (first_command_address == 0) {
            first_command_address = command_address;
        }
        for (auto index = start_index; index < end_index; ++index) {
            if (!try_write_u32(
                    command_address + 8 +
                        (static_cast<std::uint64_t>(index - start_index) *
                         sizeof(std::uint32_t)),
                    registers[index].second)) {
                restore_guest_fs();
                return 0;
            }
        }

        start_index = end_index;
    }

    restore_guest_fs();
    return first_command_address;
}

std::uint64_t ps5rt_agc_dcb_acquire_mem_real(
    std::uint64_t command_buffer_address,
    std::uint64_t engine_raw,
    std::uint64_t cb_db_op_raw,
    std::uint64_t gcr_control_raw,
    std::uint64_t base_address,
    std::uint64_t size_bytes,
    std::uint64_t poll_cycles_raw) {
    PS5_HLE_GUARD();
    const auto engine = static_cast<std::uint32_t>(engine_raw & 0xFF);
    const auto cb_db_op = static_cast<std::uint32_t>(cb_db_op_raw);
    const auto gcr_control = static_cast<std::uint32_t>(gcr_control_raw);
    const auto poll_cycles = static_cast<std::uint32_t>(poll_cycles_raw);
    const bool no_size = size_bytes == UINT64_MAX;

    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 || engine > 1 ||
        (!no_size && (size_bytes & 0xFF) != 0) ||
        (!no_size && (size_bytes >> 40) != 0) ||
        (base_address & 0xFF) != 0 || (base_address >> 40) != 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 8, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(8, kAgcItNop, kAgcRAcquireMem)) ||
        !try_write_u32(command_address + 4, (engine << 31) | cb_db_op) ||
        !try_write_u32(
            command_address + 8,
            no_size ? 0U : static_cast<std::uint32_t>(size_bytes >> 8)) ||
        !try_write_u32(command_address + 12, 0) ||
        !try_write_u32(
            command_address + 16,
            static_cast<std::uint32_t>(base_address >> 8)) ||
        !try_write_u32(command_address + 20, 0) ||
        !try_write_u32(command_address + 24, poll_cycles / 40) ||
        !try_write_u32(command_address + 28, gcr_control)) {
        restore_guest_fs();
        return 0;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.dcb_acquire_mem buf=0x%016llX cmd=0x%016llX engine=%u "
            "cbdb=0x%08X gcr=0x%08X base=0x%016llX size=0x%016llX\n",
            static_cast<unsigned long long>(command_buffer_address),
            static_cast<unsigned long long>(command_address),
            engine,
            cb_db_op,
            gcr_control,
            static_cast<unsigned long long>(base_address),
            static_cast<unsigned long long>(size_bytes));
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_dcb_push_marker_real(
    std::uint64_t command_buffer_address,
    std::uint64_t marker_address) {
    PS5_HLE_GUARD();
    if (command_buffer_address == 0) {
        restore_guest_fs();
        return 0;
    }

    char marker[4096] = {};
    std::uint32_t length = 0;
    for (; length < 4095; ++length) {
        std::uint8_t byte = 0;
        if (!try_read_u8(marker_address + length, byte)) {
            restore_guest_fs();
            return 0;
        }
        if (byte == 0) {
            break;
        }
        marker[length] = static_cast<char>(byte);
    }

    const std::uint32_t payload_dwords =
        (length + 4) / 4 > 1 ? (length + 4) / 4 : 1;
    const std::uint32_t packet_dwords = payload_dwords + 1;
    std::uint64_t command_address = 0;
    if (!agc_allocate_command_dwords(
            command_buffer_address, packet_dwords, command_address) ||
        !try_write_u32(
            command_address,
            agc_pm4(packet_dwords, kAgcItNop, kAgcRPushMarker))) {
        restore_guest_fs();
        return 0;
    }

    for (std::uint32_t index = 0; index < payload_dwords; ++index) {
        std::uint32_t value = 0;
        for (std::uint32_t byte_index = 0; byte_index < 4; ++byte_index) {
            const auto marker_index = (index * 4) + byte_index;
            if (marker_index < length) {
                value |= static_cast<std::uint32_t>(
                             static_cast<std::uint8_t>(
                                 marker[marker_index]))
                    << (byte_index * 8);
            }
        }
        if (!try_write_u32(
                command_address + 4 +
                    (static_cast<std::uint64_t>(index) *
                     sizeof(std::uint32_t)),
                value)) {
            restore_guest_fs();
            return 0;
        }
    }

    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_dcb_pop_marker_real(
    std::uint64_t command_buffer_address) {
    PS5_HLE_GUARD();
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 2, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(2, kAgcItNop, kAgcRPopMarker)) ||
        !try_write_u32(command_address + 4, 0)) {
        restore_guest_fs();
        return 0;
    }

    restore_guest_fs();
    return command_address;
}


// --- AGC indirect register patching ---
//
// Two families, both tiny and both enormous in the profile. The patch-add
// exports read one dword, add to a 14-bit count, and write it back; the
// managed bridge charged 4.5 ms a call for that, across nearly thirteen
// thousand calls in 135 seconds. The set-registers-indirect exports write
// four dwords. Neither does anything that needs a runtime.

namespace {

enum class AgcRegisterSpace { Context, Shader, UserConfig };

std::uint32_t agc_indirect_native_opcode(AgcRegisterSpace space) {
    switch (space) {
        case AgcRegisterSpace::Context: return kAgcItSetContextRegIndirect;
        case AgcRegisterSpace::Shader: return kAgcItSetShRegIndirect;
        default: return kAgcItSetUconfigRegIndirect;
    }
}

std::uint32_t agc_indirect_legacy_register(AgcRegisterSpace space) {
    switch (space) {
        case AgcRegisterSpace::Context: return kAgcRCxRegsIndirect;
        case AgcRegisterSpace::Shader: return kAgcRShRegsIndirect;
        default: return kAgcRUcRegsIndirect;
    }
}

const char* agc_register_space_name(AgcRegisterSpace space) {
    switch (space) {
        case AgcRegisterSpace::Context: return "cx";
        case AgcRegisterSpace::Shader: return "sh";
        default: return "uc";
    }
}

// The packet says which of the two encodings it is. AGC emits the same
// command either as its own opcode or NOP-aliased with the register field
// naming the space, and the count sits at a different offset in each.
std::uint64_t agc_add_indirect_patch_registers(
    std::uint64_t command_address,
    std::uint64_t register_count_raw,
    AgcRegisterSpace space) {
    const auto register_count =
        static_cast<std::uint32_t>(register_count_raw);
    std::uint32_t header = 0;
    if (command_address == 0 || !try_read_u32(command_address, header)) {
        restore_guest_fs();
        return 0x80020003;
    }

    const auto op = (header >> 8) & 0xFFu;
    const auto packet_register = (header >> 2) & 0x3Fu;
    const bool is_native = op == agc_indirect_native_opcode(space);
    const bool is_legacy = op == kAgcItNop &&
        packet_register == agc_indirect_legacy_register(space);
    const auto count_address =
        command_address + (is_native ? 16 : 4);
    std::uint32_t count_word = 0;
    if ((!is_native && !is_legacy) ||
        !try_read_u32(count_address, count_word)) {
        restore_guest_fs();
        return 0x80020003;
    }

    const auto new_count =
        ((count_word & 0x3FFFu) + register_count) & 0x3FFFu;
    if (!try_write_u32(count_address, (count_word & ~0x3FFFu) | new_count)) {
        restore_guest_fs();
        return 0x80020101;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.patch_%s_add cmd=0x%016llX add=%u total=%u format=%s\n",
            agc_register_space_name(space),
            static_cast<unsigned long long>(command_address),
            register_count,
            new_count,
            is_native ? "native" : "legacy");
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t agc_dcb_set_registers_indirect(
    std::uint64_t command_buffer_address,
    std::uint64_t registers_address,
    std::uint64_t register_count_raw,
    AgcRegisterSpace space) {
    const auto register_count =
        static_cast<std::uint32_t>(register_count_raw);
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 4, command_address) ||
        !try_write_u32(
            command_address,
            agc_pm4(4, kAgcItNop, agc_indirect_legacy_register(space))) ||
        !try_write_u32(command_address + 4, register_count) ||
        !try_write_u32(
            command_address + 8,
            static_cast<std::uint32_t>(registers_address)) ||
        !try_write_u32(
            command_address + 12,
            static_cast<std::uint32_t>(registers_address >> 32))) {
        restore_guest_fs();
        return 0;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.dcb_set_%s_indirect buf=0x%016llX cmd=0x%016llX "
            "regs=0x%016llX count=%u format=legacy\n",
            agc_register_space_name(space),
            static_cast<unsigned long long>(command_buffer_address),
            static_cast<unsigned long long>(command_address),
            static_cast<unsigned long long>(registers_address),
            register_count);
    }
    restore_guest_fs();
    return command_address;
}

}  // namespace

std::uint64_t ps5rt_agc_set_cx_reg_indirect_patch_add_registers_real(
    std::uint64_t command_address, std::uint64_t register_count) {
    PS5_HLE_GUARD();
    return agc_add_indirect_patch_registers(
        command_address, register_count, AgcRegisterSpace::Context);
}

std::uint64_t ps5rt_agc_set_sh_reg_indirect_patch_add_registers_real(
    std::uint64_t command_address, std::uint64_t register_count) {
    PS5_HLE_GUARD();
    return agc_add_indirect_patch_registers(
        command_address, register_count, AgcRegisterSpace::Shader);
}

std::uint64_t ps5rt_agc_set_uc_reg_indirect_patch_add_registers_real(
    std::uint64_t command_address, std::uint64_t register_count) {
    PS5_HLE_GUARD();
    return agc_add_indirect_patch_registers(
        command_address, register_count, AgcRegisterSpace::UserConfig);
}

std::uint64_t ps5rt_agc_dcb_set_cx_registers_indirect_real(
    std::uint64_t command_buffer_address,
    std::uint64_t registers_address,
    std::uint64_t register_count) {
    PS5_HLE_GUARD();
    return agc_dcb_set_registers_indirect(
        command_buffer_address,
        registers_address,
        register_count,
        AgcRegisterSpace::Context);
}

std::uint64_t ps5rt_agc_dcb_set_sh_registers_indirect_real(
    std::uint64_t command_buffer_address,
    std::uint64_t registers_address,
    std::uint64_t register_count) {
    PS5_HLE_GUARD();
    return agc_dcb_set_registers_indirect(
        command_buffer_address,
        registers_address,
        register_count,
        AgcRegisterSpace::Shader);
}

std::uint64_t ps5rt_agc_dcb_set_uc_registers_indirect_real(
    std::uint64_t command_buffer_address,
    std::uint64_t registers_address,
    std::uint64_t register_count) {
    PS5_HLE_GUARD();
    return agc_dcb_set_registers_indirect(
        command_buffer_address,
        registers_address,
        register_count,
        AgcRegisterSpace::UserConfig);
}


// --- AGC packet emitters and patchers, second tier ---
//
// The profile after the first two rounds still had a cluster of exports
// sitting between 4.2 and 5.5 ms per call whatever they did: event write,
// wait-reg-mem, num-instances, index-count, and the three patch-set-address
// entry points. That flatness is the crossing, and these are what is left of
// it - together another 54 seconds of a 135-second run.

namespace {

constexpr std::uint32_t kAgcItNumInstances = 0x2F;
// Not kAgcItEventWriteEop (0x47): the plain event write is its own opcode.
constexpr std::uint32_t kAgcItEventWrite = 0x46;
constexpr std::uint32_t kAgcRIndexCount = 0x1C;

std::uint32_t agc_encode_wait_reg_mem_poll(std::uint32_t poll_cycles) {
    const auto scaled = poll_cycles >> 4;
    return scaled < 0xFFFFu ? scaled : 0xFFFFu;
}

// The two widths pack the operation field differently: the 32-bit form
// splits it 2+2, the 64-bit form 1+2 a bit further along.
std::uint32_t agc_encode_wait_reg_mem32_control(
    std::uint32_t compare_function,
    std::uint32_t operation,
    std::uint32_t cache_policy) {
    return 0x10u | (compare_function & 0x7u) | ((operation & 0x3u) << 8) |
        ((operation & 0xCu) << 4) | ((cache_policy & 0x3u) << 25);
}

std::uint32_t agc_encode_wait_reg_mem64_control(
    std::uint32_t compare_function,
    std::uint32_t operation,
    std::uint32_t cache_policy) {
    return 0x10u | (compare_function & 0x7u) | ((operation & 0x1u) << 8) |
        ((operation & 0x6u) << 5) | ((cache_policy & 0x3u) << 25);
}

std::uint64_t agc_set_indirect_patch_address(
    std::uint64_t command_address,
    std::uint64_t registers_address,
    AgcRegisterSpace space) {
    std::uint32_t header = 0;
    if (command_address == 0 || registers_address == 0 ||
        !try_read_u32(command_address, header)) {
        restore_guest_fs();
        return 0x80020003;
    }

    const auto op = (header >> 8) & 0xFFu;
    const auto packet_register = (header >> 2) & 0x3Fu;
    const bool is_native = op == agc_indirect_native_opcode(space);
    const bool is_legacy = op == kAgcItNop &&
        packet_register == agc_indirect_legacy_register(space);
    if (!is_native && !is_legacy) {
        restore_guest_fs();
        return 0x80020003;
    }

    bool wrote = false;
    if (is_native) {
        // The low two bits of the first dword are not part of the address.
        std::uint32_t current_lo = 0;
        wrote = try_read_u32(command_address + 4, current_lo) &&
            try_write_u32(
                command_address + 4,
                (current_lo & 0x3u) |
                    (static_cast<std::uint32_t>(registers_address) &
                     0xFFFFFFFCu)) &&
            try_write_u32(
                command_address + 8,
                static_cast<std::uint32_t>(registers_address >> 32));
    } else {
        wrote = try_write_u32(
                    command_address + 8,
                    static_cast<std::uint32_t>(registers_address)) &&
            try_write_u32(
                command_address + 12,
                static_cast<std::uint32_t>(registers_address >> 32));
    }

    restore_guest_fs();
    return wrote ? 0 : 0x80020101;
}

}  // namespace

std::uint64_t ps5rt_agc_dcb_event_write_real(
    std::uint64_t command_buffer_address,
    std::uint64_t event_type_raw,
    std::uint64_t event_address) {
    PS5_HLE_GUARD();
    const auto event_type = static_cast<std::uint32_t>(event_type_raw & 0xFF);
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 || event_type > 0x3F ||
        event_address != 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 2, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(2, kAgcItEventWrite, 0)) ||
        !try_write_u32(command_address + 4, event_type)) {
        restore_guest_fs();
        return 0;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.dcb_event_write buf=0x%016llX cmd=0x%016llX type=%u\n",
            static_cast<unsigned long long>(command_buffer_address),
            static_cast<unsigned long long>(command_address),
            event_type);
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_dcb_set_num_instances_real(
    std::uint64_t command_buffer_address, std::uint64_t instance_count_raw) {
    PS5_HLE_GUARD();
    const auto instance_count =
        static_cast<std::uint32_t>(instance_count_raw);
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 2, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(2, kAgcItNumInstances, 0)) ||
        !try_write_u32(command_address + 4, instance_count)) {
        restore_guest_fs();
        return 0;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.dcb_set_num_instances buf=0x%016llX cmd=0x%016llX "
            "count=%u\n",
            static_cast<unsigned long long>(command_buffer_address),
            static_cast<unsigned long long>(command_address),
            instance_count);
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_dcb_set_index_count_real(
    std::uint64_t command_buffer_address, std::uint64_t index_count_raw) {
    PS5_HLE_GUARD();
    const auto index_count = static_cast<std::uint32_t>(index_count_raw);
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 2, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(2, kAgcItNop, kAgcRIndexCount)) ||
        !try_write_u32(command_address + 4, index_count)) {
        restore_guest_fs();
        return 0;
    }
    restore_guest_fs();
    return command_address;
}

// An indexed draw from a range of the bound index buffer: the largest
// index count, where in the buffer the draw starts, the count again, and
// the flags the packet keeps. After the intro the title issues one of these
// per object, thousands a frame, and through the bridge each was the main
// thread's single largest cost.
std::uint64_t ps5rt_agc_dcb_draw_index_offset_real(
    std::uint64_t command_buffer_address,
    std::uint64_t index_offset_raw,
    std::uint64_t index_count_raw,
    std::uint64_t flags_raw) {
    PS5_HLE_GUARD();
    const auto index_offset = static_cast<std::uint32_t>(index_offset_raw);
    const auto index_count = static_cast<std::uint32_t>(index_count_raw);
    const auto flags = static_cast<std::uint32_t>(flags_raw) & 0xE0000001u;
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 5, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(5, kAgcItDrawIndexOffset2, 0)) ||
        !try_write_u32(command_address + 4, index_count) ||
        !try_write_u32(command_address + 8, index_offset) ||
        !try_write_u32(command_address + 12, index_count) ||
        !try_write_u32(command_address + 16, flags)) {
        restore_guest_fs();
        return 0;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.dcb_draw_index_offset buf=0x%016llX cmd=0x%016llX "
            "offset=%u count=%u flags=0x%08X\n",
            static_cast<unsigned long long>(command_buffer_address),
            static_cast<unsigned long long>(command_address),
            index_offset,
            index_count,
            flags);
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_dcb_wait_reg_mem_real(
    std::uint64_t command_buffer_address,
    std::uint64_t size_raw,
    std::uint64_t compare_function_raw,
    std::uint64_t operation_raw,
    std::uint64_t cache_policy_raw,
    std::uint64_t address,
    std::uint64_t reference,
    std::uint64_t mask,
    std::uint64_t poll_cycles_raw) {
    PS5_HLE_GUARD();
    const auto size = static_cast<std::uint32_t>(size_raw & 0xFF);
    const auto compare_function =
        static_cast<std::uint32_t>(compare_function_raw & 0xFF);
    const auto operation = static_cast<std::uint32_t>(operation_raw & 0xFF);
    const auto cache_policy =
        static_cast<std::uint32_t>(cache_policy_raw & 0xFF);
    const auto poll_cycles = static_cast<std::uint32_t>(poll_cycles_raw);
    if (command_buffer_address == 0 || size > 1 || compare_function > 7 ||
        operation > 4 || cache_policy > 3) {
        restore_guest_fs();
        return 0;
    }

    const std::uint32_t packet_dwords = size == 0 ? 7u : 9u;
    const auto packet_register = size == 0 ? kAgcRWaitMem32 : kAgcRWaitMem64;
    std::uint64_t command_address = 0;
    if (!agc_allocate_command_dwords(
            command_buffer_address, packet_dwords, command_address) ||
        !try_write_u32(
            command_address,
            agc_pm4(packet_dwords, kAgcItNop, packet_register)) ||
        !try_write_u32(
            command_address + 4,
            static_cast<std::uint32_t>(address) &
                (size == 0 ? ~0x3u : ~0x7u)) ||
        !try_write_u32(
            command_address + 8,
            static_cast<std::uint32_t>(address >> 32) & 0x3FFFFu) ||
        !try_write_u32(
            command_address + 12, static_cast<std::uint32_t>(mask))) {
        restore_guest_fs();
        return 0;
    }

    const bool wrote = size == 0
        ? try_write_u32(
              command_address + 16, static_cast<std::uint32_t>(reference)) &&
            try_write_u32(
                command_address + 20,
                agc_encode_wait_reg_mem32_control(
                    compare_function, operation, cache_policy)) &&
            try_write_u32(
                command_address + 24,
                agc_encode_wait_reg_mem_poll(poll_cycles))
        : try_write_u32(
              command_address + 16,
              static_cast<std::uint32_t>(mask >> 32)) &&
            try_write_u32(
                command_address + 20,
                static_cast<std::uint32_t>(reference)) &&
            try_write_u32(
                command_address + 24,
                static_cast<std::uint32_t>(reference >> 32)) &&
            try_write_u32(
                command_address + 28,
                agc_encode_wait_reg_mem64_control(
                    compare_function, operation, cache_policy)) &&
            try_write_u32(
                command_address + 32,
                agc_encode_wait_reg_mem_poll(poll_cycles));
    if (!wrote) {
        restore_guest_fs();
        return 0;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.dcb_wait_reg_mem buf=0x%016llX cmd=0x%016llX size=%u "
            "compare=%u op=%u cache=%u addr=0x%016llX ref=0x%016llX "
            "mask=0x%016llX poll=%u\n",
            static_cast<unsigned long long>(command_buffer_address),
            static_cast<unsigned long long>(command_address),
            size,
            compare_function,
            operation,
            cache_policy,
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(reference),
            static_cast<unsigned long long>(mask),
            poll_cycles);
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_queue_end_of_pipe_action_patch_address_real(
    std::uint64_t command_address, std::uint64_t address) {
    PS5_HLE_GUARD();
    std::uint32_t header = 0;
    if (command_address == 0 || !try_read_u32(command_address, header) ||
        ((header >> 8) & 0xFFu) != kAgcItNop ||
        ((header >> 2) & 0x3Fu) != kAgcRReleaseMem) {
        restore_guest_fs();
        return 0x80020003;
    }

    const bool wrote =
        try_write_u32(
            command_address + 12, static_cast<std::uint32_t>(address)) &&
        try_write_u32(
            command_address + 16,
            static_cast<std::uint32_t>(address >> 32));
    restore_guest_fs();
    return wrote ? 0 : 0x80020101;
}

std::uint64_t ps5rt_agc_set_cx_reg_indirect_patch_set_address_real(
    std::uint64_t command_address, std::uint64_t registers_address) {
    PS5_HLE_GUARD();
    return agc_set_indirect_patch_address(
        command_address, registers_address, AgcRegisterSpace::Context);
}

std::uint64_t ps5rt_agc_set_sh_reg_indirect_patch_set_address_real(
    std::uint64_t command_address, std::uint64_t registers_address) {
    PS5_HLE_GUARD();
    return agc_set_indirect_patch_address(
        command_address, registers_address, AgcRegisterSpace::Shader);
}

std::uint64_t ps5rt_agc_set_uc_reg_indirect_patch_set_address_real(
    std::uint64_t command_address, std::uint64_t registers_address) {
    PS5_HLE_GUARD();
    return agc_set_indirect_patch_address(
        command_address, registers_address, AgcRegisterSpace::UserConfig);
}


// --- AGC draw, dispatch, and prim state ---
//
// What the profile has left. These are not flat in the way the earlier
// tiers were - draw-index-auto averaged 16.9 ms and then 20.1 ms as the
// frame rate rose, which is the shape of a call that waits rather than one
// that computes. The packets themselves are seven dwords and five dwords.
// Whatever the waiting is, it is not made cheaper by doing it in C#.

namespace {

constexpr std::uint32_t kAgcItIndexBufferSize = 0x13;
constexpr std::uint32_t kAgcItIndexBase = 0x26;

constexpr std::uint64_t kAgcShaderSpecialGeCntlOffset = 0x00;
constexpr std::uint64_t kAgcShaderSpecialVgtShaderStagesEnOffset = 0x08;
constexpr std::uint64_t kAgcShaderSpecialVgtGsOutPrimTypeOffset = 0x20;
constexpr std::uint64_t kAgcShaderSpecialGeUserVgprEnOffset = 0x28;

// AGC's direct dispatch API takes workgroup counts. The caller's
// USE_THREAD_DIMENSIONS bit is preserved when set but never forced.
std::uint32_t agc_direct_dispatch_initiator(std::uint32_t modifier) {
    return (modifier & 0xA038u) | 0x41u;
}

// A shader register is a {u32 offset, u32 value} pair, copied as a pair.
bool agc_copy_shader_register(
    std::uint64_t source_address, std::uint64_t destination_address) {
    std::uint32_t offset = 0;
    std::uint32_t value = 0;
    return try_read_u32(source_address, offset) &&
        try_read_u32(source_address + sizeof(std::uint32_t), value) &&
        try_write_u32(destination_address, offset) &&
        try_write_u32(destination_address + sizeof(std::uint32_t), value);
}

}  // namespace

std::uint64_t ps5rt_agc_dcb_draw_index_auto_real(
    std::uint64_t command_buffer_address,
    std::uint64_t index_count_raw,
    std::uint64_t modifier) {
    PS5_HLE_GUARD();
    const auto index_count = static_cast<std::uint32_t>(index_count_raw);
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 || modifier != 0x40000000ULL ||
        !agc_allocate_command_dwords(
            command_buffer_address, 7, command_address) ||
        !try_write_u32(
            command_address,
            agc_pm4(7, kAgcItNop, kAgcRDrawIndexAuto)) ||
        !try_write_u32(command_address + 4, index_count) ||
        !try_write_u32(command_address + 8, 0) ||
        !try_write_u32(command_address + 12, 0) ||
        !try_write_u32(command_address + 16, 0) ||
        !try_write_u32(command_address + 20, 0) ||
        !try_write_u32(command_address + 24, 0)) {
        restore_guest_fs();
        return 0;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.dcb_draw_index_auto buf=0x%016llX cmd=0x%016llX count=%u\n",
            static_cast<unsigned long long>(command_buffer_address),
            static_cast<unsigned long long>(command_address),
            index_count);
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_dcb_set_index_buffer_real(
    std::uint64_t command_buffer_address,
    std::uint64_t index_buffer_address,
    std::uint64_t index_count_raw) {
    PS5_HLE_GUARD();
    const auto index_count = static_cast<std::uint32_t>(index_count_raw);
    std::uint64_t command_address = 0;
    // Five dwords hold two packets: a three-dword base and a two-dword size.
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 5, command_address) ||
        !try_write_u32(command_address, agc_pm4(3, kAgcItIndexBase, 0)) ||
        !try_write_u32(
            command_address + 4,
            static_cast<std::uint32_t>(index_buffer_address)) ||
        !try_write_u32(
            command_address + 8,
            static_cast<std::uint32_t>(index_buffer_address >> 32)) ||
        !try_write_u32(
            command_address + 12,
            agc_pm4(2, kAgcItIndexBufferSize, 0)) ||
        !try_write_u32(command_address + 16, index_count)) {
        restore_guest_fs();
        return 0;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.dcb_set_index_buffer buf=0x%016llX cmd=0x%016llX "
            "addr=0x%016llX count=%u\n",
            static_cast<unsigned long long>(command_buffer_address),
            static_cast<unsigned long long>(command_address),
            static_cast<unsigned long long>(index_buffer_address),
            index_count);
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_cb_dispatch_real(
    std::uint64_t command_buffer_address,
    std::uint64_t group_count_x_raw,
    std::uint64_t group_count_y_raw,
    std::uint64_t group_count_z_raw,
    std::uint64_t modifier_raw) {
    PS5_HLE_GUARD();
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 5, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(5, kAgcItDispatchDirect, 0)) ||
        !try_write_u32(
            command_address + 4,
            static_cast<std::uint32_t>(group_count_x_raw)) ||
        !try_write_u32(
            command_address + 8,
            static_cast<std::uint32_t>(group_count_y_raw)) ||
        !try_write_u32(
            command_address + 12,
            static_cast<std::uint32_t>(group_count_z_raw)) ||
        !try_write_u32(
            command_address + 16,
            agc_direct_dispatch_initiator(
                static_cast<std::uint32_t>(modifier_raw)))) {
        restore_guest_fs();
        return 0;
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_create_prim_state_real(
    std::uint64_t cx_registers_address,
    std::uint64_t uc_registers_address,
    std::uint64_t hull_shader_address,
    std::uint64_t geometry_shader_address,
    std::uint64_t primitive_type_raw) {
    PS5_HLE_GUARD();
    const auto primitive_type =
        static_cast<std::uint32_t>(primitive_type_raw);
    if (cx_registers_address == 0 || uc_registers_address == 0 ||
        hull_shader_address != 0 || geometry_shader_address == 0) {
        restore_guest_fs();
        return 0x80020003;
    }

    std::uint8_t shader_type = 0;
    std::uint64_t specials_address = 0;
    if (!try_read_process_bytes(
            geometry_shader_address + kAgcShaderTypeOffset,
            &shader_type,
            sizeof(shader_type)) ||
        (shader_type != 2 && shader_type != 6) ||
        !try_read_u64(
            geometry_shader_address + kAgcShaderSpecialsOffset,
            specials_address) ||
        specials_address == 0) {
        restore_guest_fs();
        return 0x80020003;
    }

    if (!agc_copy_shader_register(
            specials_address + kAgcShaderSpecialVgtShaderStagesEnOffset,
            cx_registers_address) ||
        !agc_copy_shader_register(
            specials_address + kAgcShaderSpecialVgtGsOutPrimTypeOffset,
            cx_registers_address + 8) ||
        !agc_copy_shader_register(
            specials_address + kAgcShaderSpecialGeCntlOffset,
            uc_registers_address) ||
        !agc_copy_shader_register(
            specials_address + kAgcShaderSpecialGeUserVgprEnOffset,
            uc_registers_address + 8) ||
        !try_write_u32(uc_registers_address + 16, kAgcVgtPrimitiveType) ||
        !try_write_u32(uc_registers_address + 20, primitive_type)) {
        restore_guest_fs();
        return 0x80020101;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.create_prim_state cx=0x%016llX uc=0x%016llX gs=0x%016llX "
            "type=%u prim=0x%08X\n",
            static_cast<unsigned long long>(cx_registers_address),
            static_cast<unsigned long long>(uc_registers_address),
            static_cast<unsigned long long>(geometry_shader_address),
            static_cast<unsigned>(shader_type),
            primitive_type);
    }
    restore_guest_fs();
    return 0;
}

// --- AGC packet writers, third tier: the async ring and the rest ---
//
// What the HLE profile has left, and it is almost all one shape. Over 135
// seconds the bridge spent 44 of them inside these eighteen exports, and
// every one of them is a handful of dwords written into a command buffer.
// The Acb entry points are the async-compute mirrors of Dcb writers that
// are already native here; the packets differ, the cost does not.
//
// Two of them are not packet writers at all. CreateInterpolantMapping
// fills thirty-two SPI_PS_INPUT_CNTL register pairs from the geometry
// shader output semantics, and cost 4.85 ms a call to do it. The
// uncatalogued qj7QZpgr9Uw writes a single dword.

namespace {

constexpr std::uint32_t kAgcItGetLodStats = 0x8E;
constexpr std::uint32_t kAgcRWaitFlipDone = 0x06;
constexpr std::uint32_t kAgcRDmaData = 0x19;
constexpr std::uint64_t kAgcShaderNumInputSemanticsOffset = 0x50;
constexpr std::uint64_t kAgcShaderNumOutputSemanticsOffset = 0x56;

// An indirect dispatch or draw modifier keeps the caller cache and order
// bits and forces the initiator bits AGC always sets. Same expression the
// direct dispatch path uses, spelled once.
std::uint32_t agc_indirect_dispatch_initiator(std::uint32_t modifier) {
    return (modifier & 0xA038u) | 0x41u;
}

}  // namespace

std::uint64_t ps5rt_agc_cb_nop_real(
    std::uint64_t command_buffer_address, std::uint64_t dword_count_raw) {
    PS5_HLE_GUARD();
    const auto dword_count = static_cast<std::uint32_t>(dword_count_raw);
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 || dword_count < 2 ||
        dword_count > 0x4001 ||
        !agc_allocate_command_dwords(
            command_buffer_address, dword_count, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(dword_count, kAgcItNop, kAgcRZero))) {
        restore_guest_fs();
        return 0;
    }

    for (std::uint32_t index = 1; index < dword_count; ++index) {
        if (!try_write_u32(
                command_address + index * sizeof(std::uint32_t), 0)) {
            restore_guest_fs();
            return 0;
        }
    }
    restore_guest_fs();
    return command_address;
}

// Serves both sceAgcDcbWriteData and sceAgcAcbWriteData; the managed
// implementation aliases the second to the first.
std::uint64_t ps5rt_agc_dcb_write_data_real(
    std::uint64_t command_buffer_address,
    std::uint64_t destination_raw,
    std::uint64_t cache_policy_raw,
    std::uint64_t destination_address,
    std::uint64_t data_address,
    std::uint64_t dword_count_raw,
    std::uint64_t increment_raw,
    std::uint64_t write_confirm_raw) {
    PS5_HLE_GUARD();
    const auto destination =
        static_cast<std::uint32_t>(destination_raw & 0xFF);
    const auto cache_policy =
        static_cast<std::uint32_t>(cache_policy_raw & 0xFF);
    const auto dword_count = static_cast<std::uint32_t>(dword_count_raw);
    const auto increment = static_cast<std::uint32_t>(increment_raw & 0xFF);
    const auto write_confirm =
        static_cast<std::uint32_t>(write_confirm_raw & 0xFF);
    if (command_buffer_address == 0 || destination_address == 0 ||
        data_address == 0 || dword_count > 0x3FFD) {
        restore_guest_fs();
        return 0;
    }

    const auto packet_dwords = dword_count + 4;
    std::uint64_t command_address = 0;
    if (!agc_allocate_command_dwords(
            command_buffer_address, packet_dwords, command_address) ||
        !try_write_u32(
            command_address,
            agc_pm4(packet_dwords, kAgcItNop, kAgcRWriteData)) ||
        !try_write_u32(
            command_address + 4,
            destination | (cache_policy << 8) | (increment << 16) |
                (write_confirm << 24)) ||
        !try_write_u32(
            command_address + 8,
            static_cast<std::uint32_t>(destination_address)) ||
        !try_write_u32(
            command_address + 12,
            static_cast<std::uint32_t>(destination_address >> 32))) {
        restore_guest_fs();
        return 0;
    }

    for (std::uint32_t index = 0; index < dword_count; ++index) {
        std::uint32_t value = 0;
        if (!try_read_u32(
                data_address + index * sizeof(std::uint32_t), value) ||
            !try_write_u32(
                command_address + 16 + index * sizeof(std::uint32_t),
                value)) {
            restore_guest_fs();
            return 0;
        }
    }
    restore_guest_fs();
    return command_address;
}

// Direct execution submits synchronously, so there is no independent
// command processor to stall. A well-formed NOP keeps the cursor and the
// packet addresses coherent, which is all the caller can observe.
std::uint64_t ps5rt_agc_dcb_stall_command_buffer_parser_real(
    std::uint64_t command_buffer_address,
    std::uint64_t size_raw,
    std::uint64_t address,
    std::uint64_t reference) {
    PS5_HLE_GUARD();
    (void)address;
    (void)reference;
    const auto size = static_cast<std::uint32_t>(size_raw & 0xFF);
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 || size > 1 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 2, command_address) ||
        !try_write_u32(command_address, agc_pm4(2, kAgcItNop, kAgcRZero)) ||
        !try_write_u32(command_address + 4, 0)) {
        restore_guest_fs();
        return 0;
    }
    restore_guest_fs();
    return command_address;
}

std::atomic<std::uint64_t> g_dma_data_calls{0};
std::atomic<std::uint64_t> g_dma_data_bytes{0};
std::atomic<std::uint64_t> g_dma_data_max_bytes{0};

bool dma_data_trace_enabled() {
    static const auto enabled = [] {
        const auto* value = std::getenv("PS5RT_TRACE_DMA_DATA");
        return value != nullptr && value[0] != '0';
    }();
    return enabled;
}

void note_dma_data(
    const char* scope,
    std::uint64_t destination_address,
    std::uint64_t source_address,
    std::uint32_t byte_count,
    std::uint32_t control0) {
    g_dma_data_calls.fetch_add(1, std::memory_order_relaxed);
    g_dma_data_bytes.fetch_add(byte_count, std::memory_order_relaxed);
    auto largest = g_dma_data_max_bytes.load(std::memory_order_relaxed);
    while (byte_count > largest &&
           !g_dma_data_max_bytes.compare_exchange_weak(
               largest,
               byte_count,
               std::memory_order_relaxed)) {
    }
    if (!dma_data_trace_enabled()) {
        return;
    }
    agc_trace(
        "%s dst=0x%016llX src=0x%016llX bytes=%u control0=0x%08X "
        "calls=%llu total=%llu max=%llu\n",
        scope,
        static_cast<unsigned long long>(destination_address),
        static_cast<unsigned long long>(source_address),
        byte_count,
        control0,
        static_cast<unsigned long long>(
            g_dma_data_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_dma_data_bytes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_dma_data_max_bytes.load(std::memory_order_relaxed)));
}

std::uint64_t ps5rt_agc_dcb_dma_data_real(
    std::uint64_t command_buffer_address,
    std::uint64_t destination_raw,
    std::uint64_t destination_cache_raw,
    std::uint64_t source_raw,
    std::uint64_t destination_address,
    std::uint64_t source_cache_raw,
    std::uint64_t control4_raw,
    std::uint64_t source_address,
    std::uint64_t byte_count_raw,
    std::uint64_t control7_raw,
    std::uint64_t control8_raw,
    std::uint64_t control9_raw) {
    PS5_HLE_GUARD();
    const auto byte_count = static_cast<std::uint32_t>(byte_count_raw);
    if (command_buffer_address == 0 || byte_count == 0 ||
        (byte_count & 3) != 0) {
        restore_guest_fs();
        return 0;
    }

    const auto control0 =
        static_cast<std::uint32_t>(destination_raw & 0xFF) |
        (static_cast<std::uint32_t>(destination_cache_raw & 0xFF) << 8) |
        (static_cast<std::uint32_t>(source_raw & 0xFF) << 16) |
        (static_cast<std::uint32_t>(source_cache_raw & 0xFF) << 24);
    const auto control1 =
        static_cast<std::uint32_t>(control4_raw & 0xFF) |
        (static_cast<std::uint32_t>(control7_raw & 0xFF) << 8) |
        (static_cast<std::uint32_t>(control8_raw & 0xFF) << 16) |
        (static_cast<std::uint32_t>(control9_raw & 0xFF) << 24);
    std::uint64_t command_address = 0;
    if (!agc_allocate_command_dwords(
            command_buffer_address, 8, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(8, kAgcItNop, kAgcRDmaData)) ||
        !try_write_u32(command_address + 4, control0) ||
        !try_write_u32(command_address + 8, control1) ||
        !try_write_u32(command_address + 12, byte_count) ||
        !try_write_u32(
            command_address + 16,
            static_cast<std::uint32_t>(destination_address)) ||
        !try_write_u32(
            command_address + 20,
            static_cast<std::uint32_t>(destination_address >> 32)) ||
        !try_write_u32(
            command_address + 24,
            static_cast<std::uint32_t>(source_address)) ||
        !try_write_u32(
            command_address + 28,
            static_cast<std::uint32_t>(source_address >> 32))) {
        restore_guest_fs();
        return 0;
    }

    note_dma_data(
        "agc.dcb_dma_data",
        destination_address,
        source_address,
        byte_count,
        control0);
    restore_guest_fs();
    return command_address;
}

// The async form takes fewer register arguments than it has parameters:
// the managed implementation reads the source and the length off the guest
// stack, which puts them at positions seven and eight, and never looks at
// five or six. Kept exactly that way here.
std::uint64_t ps5rt_agc_acb_dma_data_real(
    std::uint64_t command_buffer_address,
    std::uint64_t destination_selector_raw,
    std::uint64_t source_selector_raw,
    std::uint64_t destination_address,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t source_or_immediate,
    std::uint64_t byte_count_raw) {
    PS5_HLE_GUARD();
    const auto byte_count = static_cast<std::uint32_t>(byte_count_raw);
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 || byte_count == 0 ||
        byte_count > 256u * 1024u * 1024u ||
        !agc_allocate_command_dwords(
            command_buffer_address, 7, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(7, kAgcItNop, kAgcRDmaData)) ||
        !try_write_u32(
            command_address + 4,
            static_cast<std::uint32_t>(destination_address)) ||
        !try_write_u32(
            command_address + 8,
            static_cast<std::uint32_t>(destination_address >> 32)) ||
        !try_write_u32(
            command_address + 12,
            static_cast<std::uint32_t>(source_or_immediate)) ||
        !try_write_u32(
            command_address + 16,
            static_cast<std::uint32_t>(source_or_immediate >> 32)) ||
        !try_write_u32(command_address + 20, byte_count) ||
        !try_write_u32(
            command_address + 24,
            static_cast<std::uint32_t>(destination_selector_raw & 0xFF) |
                (static_cast<std::uint32_t>(source_selector_raw & 0xFF)
                 << 8))) {
        restore_guest_fs();
        return 0;
    }
    note_dma_data(
        "agc.acb_dma_data",
        destination_address,
        source_or_immediate,
        byte_count,
        static_cast<std::uint32_t>(destination_selector_raw & 0xFF) |
            (static_cast<std::uint32_t>(source_selector_raw & 0xFF) << 8));
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_dcb_set_base_indirect_args_real(
    std::uint64_t command_buffer_address,
    std::uint64_t base_index_raw,
    std::uint64_t address) {
    PS5_HLE_GUARD();
    const auto base_index = static_cast<std::uint32_t>(base_index_raw);
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 4, command_address) ||
        !try_write_u32(
            command_address,
            agc_pm4(4, kAgcItSetBase, 0) | (base_index << 1)) ||
        !try_write_u32(command_address + 4, 1) ||
        !try_write_u32(
            command_address + 8,
            static_cast<std::uint32_t>(address) & ~7u) ||
        !try_write_u32(
            command_address + 12,
            static_cast<std::uint32_t>(address >> 32))) {
        restore_guest_fs();
        return 0;
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_dcb_dispatch_indirect_real(
    std::uint64_t command_buffer_address,
    std::uint64_t data_offset_raw,
    std::uint64_t modifier_raw) {
    PS5_HLE_GUARD();
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 3, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(3, kAgcItDispatchIndirect, 0)) ||
        !try_write_u32(
            command_address + 4,
            static_cast<std::uint32_t>(data_offset_raw)) ||
        !try_write_u32(
            command_address + 8,
            agc_indirect_dispatch_initiator(
                static_cast<std::uint32_t>(modifier_raw)))) {
        restore_guest_fs();
        return 0;
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_acb_dispatch_indirect_real(
    std::uint64_t command_buffer_address,
    std::uint64_t arguments_address,
    std::uint64_t modifier_raw) {
    PS5_HLE_GUARD();
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 4, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(4, kAgcItDispatchIndirect, 0)) ||
        !try_write_u32(
            command_address + 4,
            static_cast<std::uint32_t>(arguments_address)) ||
        !try_write_u32(
            command_address + 8,
            static_cast<std::uint32_t>(arguments_address >> 32)) ||
        !try_write_u32(
            command_address + 12,
            agc_indirect_dispatch_initiator(
                static_cast<std::uint32_t>(modifier_raw)))) {
        restore_guest_fs();
        return 0;
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_dcb_draw_index_indirect_real(
    std::uint64_t command_buffer_address,
    std::uint64_t data_offset_raw,
    std::uint64_t modifier_raw) {
    PS5_HLE_GUARD();
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 5, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(5, kAgcItDrawIndexIndirect, 0)) ||
        !try_write_u32(
            command_address + 4,
            static_cast<std::uint32_t>(data_offset_raw)) ||
        !try_write_u32(command_address + 8, 0) ||
        !try_write_u32(command_address + 12, 0) ||
        !try_write_u32(
            command_address + 16,
            static_cast<std::uint32_t>(modifier_raw))) {
        restore_guest_fs();
        return 0;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.dcb_draw_index_indirect buf=0x%016llX cmd=0x%016llX "
            "offset=0x%08X modifier=0x%08X\n",
            static_cast<unsigned long long>(command_buffer_address),
            static_cast<unsigned long long>(command_address),
            static_cast<std::uint32_t>(data_offset_raw),
            static_cast<std::uint32_t>(modifier_raw));
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_dcb_get_lod_stats_real(
    std::uint64_t command_buffer_address,
    std::uint64_t cache_policy_raw,
    std::uint64_t destination_address,
    std::uint64_t control_raw,
    std::uint64_t counter_mask_raw,
    std::uint64_t reset_counters_raw,
    std::uint64_t enable_raw,
    std::uint64_t counter_select_raw) {
    PS5_HLE_GUARD();
    if (command_buffer_address == 0) {
        restore_guest_fs();
        return 0;
    }

    const auto packet_control =
        ((static_cast<std::uint32_t>(cache_policy_raw) & 0x3u) << 28) |
        ((static_cast<std::uint32_t>(enable_raw) & 0x1u) << 19) |
        ((static_cast<std::uint32_t>(reset_counters_raw) & 0x1u) << 18) |
        ((static_cast<std::uint32_t>(counter_mask_raw) & 0xFFu) << 10) |
        ((static_cast<std::uint32_t>(counter_select_raw) & 0xFFu) << 2);
    std::uint64_t command_address = 0;
    if (!agc_allocate_command_dwords(
            command_buffer_address, 5, command_address) ||
        !try_write_u32(command_address, agc_pm4(5, kAgcItGetLodStats, 0)) ||
        !try_write_u32(
            command_address + 4,
            static_cast<std::uint32_t>(control_raw)) ||
        !try_write_u32(
            command_address + 8,
            static_cast<std::uint32_t>(destination_address) & ~0x3Fu) ||
        !try_write_u32(
            command_address + 12,
            static_cast<std::uint32_t>(destination_address >> 32)) ||
        !try_write_u32(command_address + 16, packet_control)) {
        restore_guest_fs();
        return 0;
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_dcb_wait_until_safe_for_rendering_real(
    std::uint64_t command_buffer_address,
    std::uint64_t video_out_handle_raw,
    std::uint64_t display_buffer_index_raw) {
    PS5_HLE_GUARD();
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 7, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(7, kAgcItNop, kAgcRWaitFlipDone)) ||
        !try_write_u32(
            command_address + 4,
            static_cast<std::uint32_t>(video_out_handle_raw)) ||
        !try_write_u32(
            command_address + 8,
            static_cast<std::uint32_t>(display_buffer_index_raw)) ||
        !try_write_u32(command_address + 12, 0) ||
        !try_write_u32(command_address + 16, 0) ||
        !try_write_u32(command_address + 20, 0) ||
        !try_write_u32(command_address + 24, 0)) {
        restore_guest_fs();
        return 0;
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_dcb_set_flip_real(
    std::uint64_t command_buffer_address,
    std::uint64_t video_out_handle_raw,
    std::uint64_t display_buffer_index_raw,
    std::uint64_t flip_mode_raw,
    std::uint64_t flip_argument) {
    PS5_HLE_GUARD();
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 6, command_address) ||
        !try_write_u32(command_address, agc_pm4(6, kAgcItNop, kAgcRFlip)) ||
        !try_write_u32(
            command_address + 4,
            static_cast<std::uint32_t>(video_out_handle_raw)) ||
        !try_write_u32(
            command_address + 8,
            static_cast<std::uint32_t>(display_buffer_index_raw)) ||
        !try_write_u32(
            command_address + 12,
            static_cast<std::uint32_t>(flip_mode_raw)) ||
        !try_write_u32(
            command_address + 16,
            static_cast<std::uint32_t>(flip_argument)) ||
        !try_write_u32(
            command_address + 20,
            static_cast<std::uint32_t>(flip_argument >> 32))) {
        restore_guest_fs();
        return 0;
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.dcb_set_flip buf=0x%016llX cmd=0x%016llX handle=%u "
            "index=%d mode=%u arg=0x%016llX\n",
            static_cast<unsigned long long>(command_buffer_address),
            static_cast<unsigned long long>(command_address),
            static_cast<std::uint32_t>(video_out_handle_raw),
            static_cast<int>(
                static_cast<std::int32_t>(display_buffer_index_raw)),
            static_cast<std::uint32_t>(flip_mode_raw),
            static_cast<unsigned long long>(flip_argument));
    }
    restore_guest_fs();
    return command_address;
}

// The async event write carries an address only for the two event types
// that name one; everything else is two dwords.
std::uint64_t ps5rt_agc_acb_event_write_real(
    std::uint64_t command_buffer_address,
    std::uint64_t event_type_raw,
    std::uint64_t event_address) {
    PS5_HLE_GUARD();
    const auto event_type = static_cast<std::uint32_t>(event_type_raw & 0xFF);
    if (command_buffer_address == 0 || event_type >= 0x40) {
        restore_guest_fs();
        return 0;
    }

    const bool has_address = (event_type & ~1u) == 0x38;
    const std::uint32_t packet_dwords = has_address ? 4u : 2u;
    std::uint64_t command_address = 0;
    if (!agc_allocate_command_dwords(
            command_buffer_address, packet_dwords, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(packet_dwords, kAgcItEventWrite, 0)) ||
        !try_write_u32(
            command_address + 4,
            has_address ? (event_type | 0x100u) : (event_type & 0x3Fu))) {
        restore_guest_fs();
        return 0;
    }

    if (has_address &&
        (!try_write_u32(
             command_address + 8,
             static_cast<std::uint32_t>(event_address) & ~7u) ||
         !try_write_u32(
             command_address + 12,
             static_cast<std::uint32_t>(event_address >> 32)))) {
        restore_guest_fs();
        return 0;
    }
    restore_guest_fs();
    return command_address;
}

std::uint64_t ps5rt_agc_acb_acquire_mem_real(
    std::uint64_t command_buffer_address,
    std::uint64_t gcr_control_raw,
    std::uint64_t base_address,
    std::uint64_t size_bytes,
    std::uint64_t poll_cycles_raw) {
    PS5_HLE_GUARD();
    const bool no_size = size_bytes == UINT64_MAX;
    if (command_buffer_address == 0 ||
        (!no_size && (size_bytes & 0xFF) != 0) ||
        (!no_size && (size_bytes >> 40) != 0) ||
        (base_address & 0xFF) != 0 || (base_address >> 40) != 0) {
        restore_guest_fs();
        return 0;
    }

    std::uint64_t command_address = 0;
    if (!agc_allocate_command_dwords(
            command_buffer_address, 8, command_address) ||
        !try_write_u32(
            command_address, agc_pm4(8, kAgcItNop, kAgcRAcquireMem)) ||
        !try_write_u32(command_address + 4, 0x80000000u) ||
        !try_write_u32(
            command_address + 8,
            no_size ? 0u : static_cast<std::uint32_t>(size_bytes >> 8)) ||
        !try_write_u32(command_address + 12, 0) ||
        !try_write_u32(
            command_address + 16,
            static_cast<std::uint32_t>(base_address >> 8)) ||
        !try_write_u32(command_address + 20, 0) ||
        !try_write_u32(
            command_address + 24,
            static_cast<std::uint32_t>(poll_cycles_raw) / 40) ||
        !try_write_u32(
            command_address + 28,
            static_cast<std::uint32_t>(gcr_control_raw))) {
        restore_guest_fs();
        return 0;
    }
    restore_guest_fs();
    return command_address;
}

// Unlike the direct form this one has no operation parameter: the async
// ring always encodes operation zero.
std::uint64_t ps5rt_agc_acb_wait_reg_mem_real(
    std::uint64_t command_buffer_address,
    std::uint64_t size_raw,
    std::uint64_t compare_function_raw,
    std::uint64_t cache_policy_raw,
    std::uint64_t address,
    std::uint64_t reference,
    std::uint64_t mask,
    std::uint64_t poll_cycles_raw) {
    PS5_HLE_GUARD();
    const auto size = static_cast<std::uint32_t>(size_raw & 0xFF);
    const auto compare_function =
        static_cast<std::uint32_t>(compare_function_raw & 0xFF);
    const auto cache_policy =
        static_cast<std::uint32_t>(cache_policy_raw & 0xFF);
    const auto poll_cycles = static_cast<std::uint32_t>(poll_cycles_raw);
    if (command_buffer_address == 0 || size > 1 || compare_function > 7 ||
        cache_policy > 3) {
        restore_guest_fs();
        return 0;
    }

    const std::uint32_t packet_dwords = size == 0 ? 7u : 9u;
    const auto packet_register = size == 0 ? kAgcRWaitMem32 : kAgcRWaitMem64;
    std::uint64_t command_address = 0;
    if (!agc_allocate_command_dwords(
            command_buffer_address, packet_dwords, command_address) ||
        !try_write_u32(
            command_address,
            agc_pm4(packet_dwords, kAgcItNop, packet_register)) ||
        !try_write_u32(
            command_address + 4,
            static_cast<std::uint32_t>(address) &
                (size == 0 ? ~0x3u : ~0x7u)) ||
        !try_write_u32(
            command_address + 8,
            static_cast<std::uint32_t>(address >> 32) & 0x3FFFFu) ||
        !try_write_u32(
            command_address + 12, static_cast<std::uint32_t>(mask))) {
        restore_guest_fs();
        return 0;
    }

    const bool wrote = size == 0
        ? try_write_u32(
              command_address + 16, static_cast<std::uint32_t>(reference)) &&
            try_write_u32(
                command_address + 20,
                agc_encode_wait_reg_mem32_control(
                    compare_function, 0, cache_policy)) &&
            try_write_u32(
                command_address + 24,
                agc_encode_wait_reg_mem_poll(poll_cycles))
        : try_write_u32(
              command_address + 16,
              static_cast<std::uint32_t>(mask >> 32)) &&
            try_write_u32(
                command_address + 20,
                static_cast<std::uint32_t>(reference)) &&
            try_write_u32(
                command_address + 24,
                static_cast<std::uint32_t>(reference >> 32)) &&
            try_write_u32(
                command_address + 28,
                agc_encode_wait_reg_mem64_control(
                    compare_function, 0, cache_policy)) &&
            try_write_u32(
                command_address + 32,
                agc_encode_wait_reg_mem_poll(poll_cycles));
    if (!wrote) {
        restore_guest_fs();
        return 0;
    }
    restore_guest_fs();
    return command_address;
}

// Thirty-two SPI_PS_INPUT_CNTL pairs, one per pixel shader input. Each
// input names a semantic; the register points it at the parameter the
// geometry shader writes that semantic to. The slot index is the pixel
// shader's attribute number, the OFFSET field the geometry shader's export
// number, and the two only coincide when both shaders list the same
// semantics in the same order.
//
// This used to write slot i -> parameter i. The UI plane the intro video is
// drawn onto exports five parameters and reads four, the fourth of them the
// fifth export: its pixel shader unpacked a transformed coordinate as its
// packed colour and alpha, and drew nothing. The output count is also a
// sixteen-bit field; read as 32 bits it took the next field along and
// claimed three million outputs.
//
// A semantic word: the semantic in bits 0-7, the export it lives in
// (hardware mapping) in 8-15, flat shading in bit 22, and the default value
// for an input nothing writes in bits 28-29.
std::uint64_t ps5rt_agc_create_interpolant_mapping_real(
    std::uint64_t registers_address,
    std::uint64_t geometry_shader_address,
    std::uint64_t pixel_shader_address) {
    PS5_HLE_GUARD();
    if (registers_address == 0 || geometry_shader_address == 0) {
        restore_guest_fs();
        return 0x80020003;
    }

    std::uint64_t output_semantics_address = 0;
    std::uint32_t output_count_word = 0;
    if (!try_read_u64(
            geometry_shader_address + kAgcShaderOutputSemanticsOffset,
            output_semantics_address) ||
        !try_read_u32(
            geometry_shader_address + kAgcShaderNumOutputSemanticsOffset,
            output_count_word)) {
        restore_guest_fs();
        return 0x80020101;
    }
    const auto output_semantics_count =
        std::min<std::uint32_t>(output_count_word & 0xFFFFu, 32u);

    std::uint64_t input_semantics_address = 0;
    std::uint32_t input_semantics_count = 0;
    if (pixel_shader_address != 0) {
        if (!try_read_u64(
                pixel_shader_address + kAgcShaderInputSemanticsOffset,
                input_semantics_address) ||
            !try_read_u32(
                pixel_shader_address + kAgcShaderNumInputSemanticsOffset,
                input_semantics_count)) {
            restore_guest_fs();
            return 0x80020101;
        }
        input_semantics_count =
            std::min<std::uint32_t>(input_semantics_count, 32u);
    }

    // What the geometry shader exports, by semantic.
    std::uint32_t outputs[32] = {};
    for (std::uint32_t index = 0; index < output_semantics_count; ++index) {
        if (output_semantics_address == 0 ||
            !try_read_u32(
                output_semantics_address + index * sizeof(std::uint32_t),
                outputs[index])) {
            outputs[index] = 0;
        }
    }
    constexpr std::uint32_t kUseDefault = 0x20u;
    constexpr std::uint32_t kFlatShade = 0x400u;
    const auto export_of = [&](std::uint32_t semantic_word) {
        for (std::uint32_t index = 0; index < output_semantics_count;
             ++index) {
            if ((outputs[index] & 0xFFu) == (semantic_word & 0xFFu)) {
                return static_cast<std::int32_t>(
                    (outputs[index] >> 8) & 0xFFu);
            }
        }
        return -1;
    };

    std::uint32_t remapped = 0;
    for (std::uint32_t index = 0; index < 32; ++index) {
        std::uint32_t value = 0;
        if (pixel_shader_address == 0 || input_semantics_address == 0) {
            // No pixel shader to match against: the exports in order.
            if (index < output_semantics_count) {
                value = (outputs[index] >> 8) & 0x1Fu;
            }
        } else if (index < input_semantics_count) {
            std::uint32_t input = 0;
            if (try_read_u32(
                    input_semantics_address + index * sizeof(std::uint32_t),
                    input)) {
                const auto exported = export_of(input);
                if (exported >= 0) {
                    value = static_cast<std::uint32_t>(exported) & 0x1Fu;
                } else {
                    value = kUseDefault | (((input >> 28) & 0x3u) << 8);
                }
                if (((input >> 22) & 0x1u) != 0) {
                    value |= kFlatShade;
                }
                if ((value & 0x3Fu) != index) {
                    ++remapped;
                }
            }
        }

        const auto destination = registers_address + index * 8;
        if (!try_write_u32(destination, kAgcSpiPsInputCntl0 + index) ||
            !try_write_u32(
                destination + sizeof(std::uint32_t), value)) {
            restore_guest_fs();
            return 0x80020101;
        }
    }

    static volatile LONG trace_count = 0;
    if (agc_trace_first(trace_count)) {
        agc_trace(
            "agc.create_interpolant_mapping regs=0x%016llX gs=0x%016llX "
            "ps=0x%016llX outputs=%u inputs=%u remapped=%u\n",
            static_cast<unsigned long long>(registers_address),
            static_cast<unsigned long long>(geometry_shader_address),
            static_cast<unsigned long long>(pixel_shader_address),
            output_semantics_count,
            input_semantics_count,
            remapped);
    }
    restore_guest_fs();
    return 0;
}

// Uncatalogued; the NID is the name. One dword, and the value is the one
// the managed implementation writes.
std::uint64_t ps5rt_agc_unknown_qj7_real(
    std::uint64_t command_buffer_address,
    std::uint64_t,
    std::uint64_t) {
    PS5_HLE_GUARD();
    std::uint64_t command_address = 0;
    if (command_buffer_address == 0 ||
        !agc_allocate_command_dwords(
            command_buffer_address, 1, command_address) ||
        !try_write_u32(command_address, 0x80000000u)) {
        restore_guest_fs();
        return 0;
    }
    restore_guest_fs();
    return command_address;
}

// --- The online imports the title reaches once it gets far enough ---
//
// Twenty-six NIDs that had no handler and fell through to the managed
// bridge, which logs one auto_stub line per NID and puts zero in RAX.
// They are not a cost - none of them appears in the top twenty by call
// count over 135 seconds - but they are the whole of what the title asks
// for and does not get, so they are worth naming.
//
// Two belong to libSceRudp, two to libSceNet, and the remaining
// twenty-two are one transaction:
//
//   ParameterToGetPublicProfiles ctor / initialize / terminate / dtor
//   Transaction<IntrusivePtr<GetPublicProfilesResponse>, ...> ctor / dtor
//   TransactionBase<...>::start / setResponseInformationOption / finish
//   BasicProfileApi::getPublicProfiles
//   Transaction::getResponse
//   GetPublicProfilesResponse::getProfiles
//   IntrusivePtr<...> ctor / dtor / operator= / operator-> / get
//
// The title is asking PSN for public player profiles. Offline that can
// never succeed, and returning a negative SCE error is not obviously
// safer than returning zero: SharpEmu already found that a negative
// return from CppWebApi::Common::initialize aborts PS5-component startup
// outright. So these keep the value the bridge was returning, and what
// they add is a name, a first-call trace, and no crossing.
//
// IntrusivePtr is implemented for real, because one probe settled the
// question that blocked it. The mangled name does not say how
// getProfiles returns, and guessing wrong turns a reliable null into a
// garbage pointer, so the first version traced instead of guessing:
//
//   np.response_ptr_arrow        this=0x1A956E60058 slot0=0x0
//   np.response_get_profiles     rdi=0x402E3FEFB0 rsi=0x0
//   np.profile_vector_ptr_assign source=0x402E3FEFB0 source_slot0=0x0
//
// RSI is the null `this` that operator-> had just returned, and RDI is a
// stack address that operator= then reads as its source. That is a
// hidden return pointer, so getProfiles returns by value and RDI is the
// slot. It also means the chain worked only by luck: nothing wrote that
// slot, and it read as zero because the stack happened to hold zero.
//
// So IntrusivePtr is treated as what it is everywhere else - one pointer
// at offset zero. The observable result is the same null it already was,
// arrived at deliberately rather than by accident.

namespace {

constexpr std::uint32_t kNpTraceLimit = 4;

bool np_trace_first(volatile LONG& counter) {
    return InterlockedIncrement(&counter) <= kNpTraceLimit;
}

// agc_trace is the generic stderr tracer in this file - trace_stderr is
// compiled out - so it carries these lines too despite the name.
#define PS5RT_NP_TRACE_1(name, a) \
    do { \
        static volatile LONG np_trace_count = 0; \
        if (np_trace_first(np_trace_count)) { \
            agc_trace( \
                "np.%s this=0x%016llX\n", \
                (name), \
                static_cast<unsigned long long>(a)); \
        } \
    } while (0)

#define PS5RT_NP_TRACE_2(name, a, b) \
    do { \
        static volatile LONG np_trace_count = 0; \
        if (np_trace_first(np_trace_count)) { \
            agc_trace( \
                "np.%s this=0x%016llX arg1=0x%016llX\n", \
                (name), \
                static_cast<unsigned long long>(a), \
                static_cast<unsigned long long>(b)); \
        } \
    } while (0)

#define PS5RT_NP_TRACE_3(name, a, b, c) \
    do { \
        static volatile LONG np_trace_count = 0; \
        if (np_trace_first(np_trace_count)) { \
            agc_trace( \
                "np.%s this=0x%016llX arg1=0x%016llX arg2=0x%016llX\n", \
                (name), \
                static_cast<unsigned long long>(a), \
                static_cast<unsigned long long>(b), \
                static_cast<unsigned long long>(c)); \
        } \
    } while (0)

// For the pointer-shaped members: report what the object holds at offset
// zero as well as where it is. If IntrusivePtr is the single pointer it
// is everywhere else, this is the value get() would be returning, and
// whether it is null tells us whether the objects are being filled in.
void np_trace_pointer_member(
    const char* name, volatile LONG& counter, std::uint64_t object) {
    if (!np_trace_first(counter)) {
        return;
    }
    std::uint64_t slot0 = 0;
    const bool readable = try_read_u64(object, slot0);
    agc_trace(
        "np.%s this=0x%016llX slot0=%s0x%016llX\n",
        name,
        static_cast<unsigned long long>(object),
        readable ? "" : "unreadable:",
        static_cast<unsigned long long>(slot0));
}

}  // namespace

std::uint64_t ps5rt_rudp_set_event_handler_real(
    std::uint64_t handler, std::uint64_t user_argument) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_2("rudp_set_event_handler", handler, user_argument);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_rudp_enable_internal_io_thread_real(
    std::uint64_t argument) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_1("rudp_enable_internal_io_thread", argument);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_net_getsockname_real(
    std::uint64_t socket,
    std::uint64_t address,
    std::uint64_t address_length) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_3("net_getsockname", socket, address, address_length);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_net_sendto_real(
    std::uint64_t socket,
    std::uint64_t buffer,
    std::uint64_t length,
    std::uint64_t flags,
    std::uint64_t destination,
    std::uint64_t destination_length) {
    PS5_HLE_GUARD();
    (void)flags;
    (void)destination;
    (void)destination_length;
    PS5RT_NP_TRACE_3("net_sendto", socket, buffer, length);
    restore_guest_fs();
    return 0;
}

// LibContext and InitParams: constructed once during CppWebApi setup.
// Sizes unknown, so nothing is written into them.
std::uint64_t ps5rt_np_cppwebapi_lib_context_ctor_real(
    std::uint64_t object) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_1("lib_context_ctor", object);
    restore_guest_fs();
    return object;
}

std::uint64_t ps5rt_np_cppwebapi_init_params_ctor_real(
    std::uint64_t object) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_1("init_params_ctor", object);
    restore_guest_fs();
    return object;
}

std::uint64_t ps5rt_np_cppwebapi_init_params_dtor_real(
    std::uint64_t object) {
    PS5_HLE_GUARD();
    (void)object;
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_np_response_ptr_ctor_real(std::uint64_t object) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_1("response_ptr_ctor", object);
    (void)try_write_u64(object, 0);
    restore_guest_fs();
    return object;
}

std::uint64_t ps5rt_np_response_ptr_dtor_real(std::uint64_t object) {
    PS5_HLE_GUARD();
    (void)object;
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_np_response_ptr_arrow_real(std::uint64_t object) {
    PS5_HLE_GUARD();
    static volatile LONG trace_count = 0;
    np_trace_pointer_member("response_ptr_arrow", trace_count, object);
    std::uint64_t pointer = 0;
    if (!try_read_u64(object, pointer)) {
        pointer = 0;
    }
    restore_guest_fs();
    return pointer;
}

std::uint64_t ps5rt_np_profile_vector_ptr_ctor_real(std::uint64_t object) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_1("profile_vector_ptr_ctor", object);
    (void)try_write_u64(object, 0);
    restore_guest_fs();
    return object;
}

std::uint64_t ps5rt_np_profile_vector_ptr_dtor_real(std::uint64_t object) {
    PS5_HLE_GUARD();
    (void)object;
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_np_profile_vector_ptr_assign_real(
    std::uint64_t object, std::uint64_t source) {
    PS5_HLE_GUARD();
    static volatile LONG trace_count = 0;
    if (np_trace_first(trace_count)) {
        std::uint64_t source_slot0 = 0;
        const bool readable = try_read_u64(source, source_slot0);
        agc_trace(
            "np.profile_vector_ptr_assign this=0x%016llX source=0x%016llX "
            "source_slot0=%s0x%016llX\n",
            static_cast<unsigned long long>(object),
            static_cast<unsigned long long>(source),
            readable ? "" : "unreadable:",
            static_cast<unsigned long long>(source_slot0));
    }
    std::uint64_t pointer = 0;
    if (!try_read_u64(source, pointer)) {
        pointer = 0;
    }
    (void)try_write_u64(object, pointer);
    restore_guest_fs();
    return object;
}

std::uint64_t ps5rt_np_profile_vector_ptr_get_real(std::uint64_t object) {
    PS5_HLE_GUARD();
    static volatile LONG trace_count = 0;
    np_trace_pointer_member("profile_vector_ptr_get", trace_count, object);
    std::uint64_t pointer = 0;
    if (!try_read_u64(object, pointer)) {
        pointer = 0;
    }
    restore_guest_fs();
    return pointer;
}

std::uint64_t ps5rt_np_transaction_ctor_real(std::uint64_t object) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_1("transaction_ctor", object);
    restore_guest_fs();
    return object;
}

std::uint64_t ps5rt_np_transaction_dtor_real(std::uint64_t object) {
    PS5_HLE_GUARD();
    (void)object;
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_np_transaction_start_real(
    std::uint64_t object, std::uint64_t lib_context) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_2("transaction_start", object, lib_context);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_np_transaction_finish_real(std::uint64_t object) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_1("transaction_finish", object);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_np_transaction_set_response_option_real(
    std::uint64_t object, std::uint64_t option) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_2("transaction_set_response_option", object, option);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_np_transaction_get_response_real(
    std::uint64_t object, std::uint64_t out_response) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_2("transaction_get_response", object, out_response);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_np_public_profiles_parameter_ctor_real(
    std::uint64_t object) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_1("public_profiles_parameter_ctor", object);
    restore_guest_fs();
    return object;
}

std::uint64_t ps5rt_np_public_profiles_parameter_dtor_real(
    std::uint64_t object) {
    PS5_HLE_GUARD();
    (void)object;
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_np_public_profiles_parameter_initialize_real(
    std::uint64_t object,
    std::uint64_t lib_context,
    std::uint64_t account_id_text) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_3(
        "public_profiles_parameter_initialize",
        object,
        lib_context,
        account_id_text);
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_np_public_profiles_parameter_terminate_real(
    std::uint64_t object) {
    PS5_HLE_GUARD();
    PS5RT_NP_TRACE_1("public_profiles_parameter_terminate", object);
    restore_guest_fs();
    return 0;
}

// Static, not a member: the first trace showed RDI holding 0x3E9 while
// RSI and RDX held the parameter and transaction objects the
// constructors had just reported. So there is no `this`, and the three
// arguments of the mangled signature land in RDI, RSI and RDX.
std::uint64_t ps5rt_np_get_public_profiles_real(
    std::uint64_t user_context,
    std::uint64_t parameter,
    std::uint64_t transaction,
    std::uint64_t) {
    PS5_HLE_GUARD();
    static volatile LONG trace_count = 0;
    if (np_trace_first(trace_count)) {
        agc_trace(
            "np.get_public_profiles user=%d parameter=0x%016llX "
            "transaction=0x%016llX\n",
            static_cast<int>(static_cast<std::int32_t>(user_context)),
            static_cast<unsigned long long>(parameter),
            static_cast<unsigned long long>(transaction));
    }
    restore_guest_fs();
    return 0;
}

// Returns IntrusivePtr by value, so RDI is the caller return slot and
// RSI is `this`. Constructing the returned pointer as null is the whole
// job here: without it the caller copies whatever the stack held.
std::uint64_t ps5rt_np_response_get_profiles_real(
    std::uint64_t return_slot, std::uint64_t object) {
    PS5_HLE_GUARD();
    static volatile LONG trace_count = 0;
    if (np_trace_first(trace_count)) {
        agc_trace(
            "np.response_get_profiles slot=0x%016llX this=0x%016llX\n",
            static_cast<unsigned long long>(return_slot),
            static_cast<unsigned long long>(object));
    }
    (void)try_write_u64(return_slot, 0);
    restore_guest_fs();
    return return_slot;
}


// --- APR / AMPR: the path the title's assets actually arrive on ---
//
// Ported from KytyPS5 src/libs/libAmpr.cpp (GPL-2.0), which is where the
// command buffer layout below comes from. The title agrees with it: its
// sceAmprAprCommandBufferConstructor arrives with reserved_state0 at
// command_buffer+0x18 and reserved_state1 at +0x20, which are exactly
// APR_COMMAND_BUFFER_MAP_OFFSET and APR_COMMAND_BUFFER_SG_OFFSET.
//
// Like Kyty, the guest-visible record bytes carry only an opcode and the
// header counters the title reads back; the parameters of each command
// live in a side table keyed by the record offset. Reproducing the
// hardware's record encoding would buy nothing - nothing but this code
// ever executes these buffers.

namespace {

constexpr std::uint64_t kAprCbTypeOffset = 0x00;
constexpr std::uint64_t kAprCbOffsetOffset = 0x04;
constexpr std::uint64_t kAprCbNumOffset = 0x08;
constexpr std::uint64_t kAprCbSizeOffset = 0x0C;
constexpr std::uint64_t kAprCbDataOffset = 0x10;
constexpr std::uint64_t kAprCbHeaderSize = 0x18;
constexpr std::uint64_t kAprCbMapOffset = 0x18;
constexpr std::uint64_t kAprCbScatterGatherOffset = 0x20;
constexpr std::uint32_t kAprCbSizeMaximum = 64u * 1024u * 1024u;
constexpr std::uint64_t kAprReadFileRecordSize = 0x14;
constexpr std::uint64_t kAprReadFileRecordSizeExtended = 0x18;
constexpr std::uint32_t kAprTypeGatherScatterValid = 0x00010000u;
constexpr std::uint64_t kAprMaximumReadLength = 0x0000000100000000ULL;
constexpr std::uint64_t kAprMaximumFileOffset = 0x0000010000000000ULL;
constexpr std::uint64_t kAprMaximumApplicationAddress = 0x0000F00000000000ULL;
constexpr std::uint64_t kAprHostReadChunkBytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kAprInvalidFileId = 0xFFFFFFFFu;

constexpr std::uint64_t kAprErrorInvalidValue = 0x80020016ULL;
constexpr std::uint64_t kAprErrorNoEntry = 0x80020002ULL;

struct AprReadFileCommand {
    std::uint64_t record_offset = 0;
    std::uint32_t file_id = 0;
    std::uint64_t destination = 0;
    std::uint64_t size = 0;
    std::uint64_t file_offset = 0;
};

struct AprCommandBufferState {
    std::uint64_t buffer = 0;
    std::uint64_t size = 0;
    std::uint64_t write_offset = 0;
    std::vector<AprReadFileCommand> reads;
};

struct AprSubmission {
    std::uint64_t result_address = 0;
    std::int32_t execution_result = 0;
    std::uint32_t error_offset = 0;
};

SRWLOCK g_apr_lock = SRWLOCK_INIT;
std::map<std::uint64_t, AprCommandBufferState> g_apr_command_buffers;
std::map<std::uint32_t, std::string> g_apr_file_paths;
std::map<std::uint32_t, std::uint64_t> g_apr_file_sizes;
std::map<std::uint32_t, AprSubmission> g_apr_submissions;
std::uint32_t g_apr_next_submission_id = 1;

std::atomic<std::uint64_t> g_apr_resolved{0};
std::atomic<std::uint64_t> g_apr_resolve_failed{0};
std::atomic<std::uint64_t> g_apr_reads{0};
std::atomic<std::uint64_t> g_apr_read_bytes{0};
std::atomic<std::uint64_t> g_apr_read_failed{0};
std::atomic<std::uint64_t> g_apr_submissions_run{0};

bool apr_trace_enabled() {
    static const auto enabled = [] {
        const auto* value = std::getenv("PS5RT_TRACE_APR");
        return value != nullptr && value[0] != '0';
    }();
    return enabled;
}

std::uint32_t apr_file_id_for(const std::string& guest_path) {
    std::uint32_t hash = 2166136261u;
    for (const auto character : guest_path) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 16777619u;
    }
    return hash & 0x7FFFFFFFu;
}

std::uint64_t apr_read_file_record_size(std::uint64_t file_offset) {
    return (file_offset >> 32u) != 0
        ? kAprReadFileRecordSizeExtended
        : kAprReadFileRecordSize;
}

bool apr_read_range_valid(std::uint64_t destination, std::uint64_t size) {
    return destination >= 0x10000 &&
        size != 0 &&
        size <= kAprMaximumReadLength &&
        destination <= kAprMaximumApplicationAddress - size;
}

bool apr_path_from_list(
    std::uint64_t path_list,
    std::uint64_t index,
    std::string& out) {
    std::uint64_t pointer = 0;
    if (try_read_u64(path_list + index * sizeof(std::uint64_t), pointer) &&
        pointer >= 0x10000 &&
        read_process_c_string(pointer, 1024, out) &&
        !out.empty()) {
        return true;
    }
    // A single path is also passed directly rather than through a
    // one-element array; only meaningful for the first entry.
    if (index == 0 &&
        read_process_c_string(path_list, 1024, out) &&
        !out.empty()) {
        return true;
    }
    out.clear();
    return false;
}

bool apr_host_file_size(const std::string& host_path, std::uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!GetFileAttributesExA(
            host_path.c_str(), GetFileExInfoStandard, &attributes)) {
        return false;
    }
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        size = 0;
        return true;
    }
    size = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) |
        attributes.nFileSizeLow;
    return true;
}

// The directories under data/prein/ in the dump, read once: characters/ and
// common/ first, which is where the first missing files were found, then
// the rest in the order the file system lists them.
const std::vector<std::string>& apr_prein_directories(
    const std::string& prein_host) {
    static const std::vector<std::string> directories = [&] {
        std::vector<std::string> found = {"characters", "common"};
        WIN32_FIND_DATAA entry = {};
        const auto pattern = prein_host + "*";
        const auto handle = FindFirstFileA(pattern.c_str(), &entry);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                const std::string name = entry.cFileName;
                if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                    name == "." || name == ".." ||
                    std::find(found.begin(), found.end(), name) !=
                        found.end()) {
                    continue;
                }
                found.push_back(name);
            } while (FindNextFileA(handle, &entry));
            FindClose(handle);
        }
        return found;
    }();
    return directories;
}

// Returns 0 on success, a PS5 kernel error otherwise.
std::uint64_t apr_resolve_one(
    const std::string& guest_path,
    std::uint32_t* id_out,
    std::uint64_t* size_out) {
    auto host_path = map_ps5_path(guest_path.c_str());
    std::uint64_t size = 0;
    // A file asked for under data/prein/effects/ that the dump only has
    // under characters/ or common/ is taken from there: a hundred and ten
    // animations the title looks for under effects/anim sit in
    // characters/anim in this dump (the extraction keeps one copy of a file
    // the package has twice, it seems). Without them the scarf animations
    // never loaded, the "Animation" component's template had no data, and
    // the title died instancing it after the intros.
    // PS5RT_APR_SIBLING_DIRS=0 turns it off.
    static const bool sibling_dirs = [] {
        const auto* value = std::getenv("PS5RT_APR_SIBLING_DIRS");
        return value == nullptr || value[0] != '0';
    }();
    //
    // Not only effects/ and not only those two: the Team Asobi logo's
    // skeleton, animations and model are asked for under effects/ and
    // are in ui/. Without them the logo's Animation component had no data
    // and the title died instancing it just after the PS Studios video, in
    // about a third of runs - which is what made it look like a race.
    if (sibling_dirs && !host_path.empty() &&
        !apr_host_file_size(host_path, size)) {
        const std::string prein = "/prein/";
        const auto at = guest_path.find(prein);
        const auto directory_end = at == std::string::npos
            ? std::string::npos
            : guest_path.find('/', at + prein.size());
        if (directory_end != std::string::npos) {
            const auto directory = guest_path.substr(
                at + prein.size(), directory_end - at - prein.size());
            auto prein_host =
                map_ps5_path(guest_path.substr(0, at + prein.size()).c_str());
            if (!prein_host.empty() && prein_host.back() != '/' &&
                prein_host.back() != '\\') {
                prein_host += '/';
            }
            for (const auto& sibling : apr_prein_directories(prein_host)) {
                if (sibling == directory) {
                    continue;
                }
                auto other = guest_path;
                other.replace(at + prein.size(),
                              directory_end - at - prein.size(), sibling);
                const auto other_host = map_ps5_path(other.c_str());
                if (!other_host.empty() &&
                    apr_host_file_size(other_host, size)) {
                    if (apr_trace_enabled()) {
                        agc_trace("apr.resolve_sibling guest=%s host=%s\n",
                                  guest_path.c_str(), other_host.c_str());
                    }
                    host_path = other_host;
                    break;
                }
            }
        }
    }
    if (host_path.empty() || !apr_host_file_size(host_path, size)) {
        g_apr_resolve_failed.fetch_add(1, std::memory_order_relaxed);
        if (apr_trace_enabled()) {
            agc_trace(
                "apr.resolve_failed guest=%s host=%s\n",
                guest_path.c_str(),
                host_path.c_str());
        }
        return kAprErrorNoEntry;
    }

    const auto file_id = apr_file_id_for(guest_path);
    AcquireSRWLockExclusive(&g_apr_lock);
    g_apr_file_paths[file_id] = host_path;
    g_apr_file_sizes[file_id] = size;
    ReleaseSRWLockExclusive(&g_apr_lock);
    g_apr_resolved.fetch_add(1, std::memory_order_relaxed);

    if (id_out != nullptr) {
        *id_out = file_id;
    }
    if (size_out != nullptr) {
        *size_out = size;
    }
    if (apr_trace_enabled()) {
        agc_trace(
            "apr.resolve guest=%s host=%s id=0x%08X bytes=%llu\n",
            guest_path.c_str(),
            host_path.c_str(),
            file_id,
            static_cast<unsigned long long>(size));
    }
    return 0;
}

std::uint64_t apr_resolve_paths(
    std::uint64_t path_list,
    std::uint32_t count,
    std::uint64_t ids_address,
    std::uint64_t sizes_address,
    std::uint64_t error_index_address) {
    if (path_list == 0 || count == 0 || count > 1024 ||
        (ids_address == 0 && sizes_address == 0)) {
        return kAprErrorInvalidValue;
    }

    for (std::uint32_t index = 0; index < count; ++index) {
        std::string guest_path;
        std::uint32_t file_id = kAprInvalidFileId;
        std::uint64_t size = 0;
        auto result = apr_path_from_list(path_list, index, guest_path)
            ? apr_resolve_one(guest_path, &file_id, &size)
            : kAprErrorInvalidValue;
        if (result != 0) {
            if (ids_address != 0) {
                (void)try_write_u32(
                    ids_address + index * sizeof(std::uint32_t),
                    kAprInvalidFileId);
            }
            if (sizes_address != 0) {
                (void)try_write_u64(
                    sizes_address + index * sizeof(std::uint64_t), 0);
            }
            if (error_index_address != 0) {
                (void)try_write_u32(error_index_address, index);
            }
            return result;
        }
        if (ids_address != 0 &&
            !try_write_u32(
                ids_address + index * sizeof(std::uint32_t), file_id)) {
            return kAprErrorInvalidValue;
        }
        if (sizes_address != 0 &&
            !try_write_u64(
                sizes_address + index * sizeof(std::uint64_t), size)) {
            return kAprErrorInvalidValue;
        }
    }
    return 0;
}

// The destination is guest memory the GPU layer may have armed read-only
// for its texture write watch at any moment, and try_write_process_bytes
// caches a range's writability with nothing to invalidate it when the
// other module changes protection - a stale hit there is a memcpy into a
// read-only page, which is an access violation inside ucrtbase with no
// guest frame to explain it. WriteProcessMemory takes the page as it finds
// it and reports failure instead of faulting. It does need the pages to
// exist, hence the commit first.
bool apr_run_read(const AprReadFileCommand& command, std::uint64_t& copied) {
    copied = 0;
    std::string host_path;
    AcquireSRWLockShared(&g_apr_lock);
    const auto entry = g_apr_file_paths.find(command.file_id);
    const auto known = entry != g_apr_file_paths.end();
    if (known) {
        host_path = entry->second;
    }
    ReleaseSRWLockShared(&g_apr_lock);
    if (!known) {
        return false;
    }

    const auto file = CreateFileA(
        host_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER file_size = {};
    if (!GetFileSizeEx(file, &file_size) ||
        command.file_offset >=
            static_cast<std::uint64_t>(file_size.QuadPart)) {
        CloseHandle(file);
        return true;
    }
    LARGE_INTEGER seek = {};
    seek.QuadPart = static_cast<LONGLONG>(command.file_offset);
    if (!SetFilePointerEx(file, seek, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return false;
    }

    const auto readable = std::min<std::uint64_t>(
        command.size,
        static_cast<std::uint64_t>(file_size.QuadPart) - command.file_offset);
    std::vector<std::uint8_t> chunk(
        static_cast<std::size_t>(
            std::min<std::uint64_t>(kAprHostReadChunkBytes, readable)));
    auto succeeded = true;
    while (copied < readable) {
        const auto request = static_cast<DWORD>(
            std::min<std::uint64_t>(chunk.size(), readable - copied));
        DWORD taken = 0;
        if (!ReadFile(file, chunk.data(), request, &taken, nullptr) ||
            taken == 0) {
            succeeded = taken != 0;
            break;
        }
        const auto destination = command.destination + copied;
        SIZE_T written = 0;
        // WriteProcessMemory does not fault either: over a page the GPU
        // runtime made read-only to watch it, it fails with 998 and the
        // asset arrives in part - the "Sony Interactive Entertainment"
        // plate came out as noise and a black box. Lift the watch first,
        // and once more if the worker re-armed it in between.
        auto wrote = false;
        for (int attempt = 0; attempt < 2 && !wrote; ++attempt) {
            ps5rt_native_gpu_guest_written(destination, taken);
            wrote = ps5rt_guest_commit_range(destination, taken) &&
                WriteProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<void*>(destination),
                    chunk.data(),
                    taken,
                    &written) != FALSE &&
                written == taken;
        }
        if (!wrote) {
            succeeded = false;
            break;
        }
        copied += taken;
    }
    CloseHandle(file);
    return succeeded;
}

std::uint64_t apr_execute(
    std::uint64_t command_buffer,
    std::int32_t& execution_result,
    std::uint32_t& error_offset) {
    execution_result = 0;
    error_offset = 0;

    AprCommandBufferState state;
    AcquireSRWLockShared(&g_apr_lock);
    const auto entry = g_apr_command_buffers.find(command_buffer);
    const auto known = entry != g_apr_command_buffers.end();
    if (known) {
        state = entry->second;
    }
    ReleaseSRWLockShared(&g_apr_lock);
    if (!known) {
        return kAprErrorInvalidValue;
    }

    auto commands = state.reads;
    std::sort(
        commands.begin(),
        commands.end(),
        [](const AprReadFileCommand& left, const AprReadFileCommand& right) {
            return left.record_offset < right.record_offset;
        });

    g_apr_submissions_run.fetch_add(1, std::memory_order_relaxed);
    for (const auto& command : commands) {
        if (command.record_offset >= state.write_offset) {
            continue;
        }
        std::uint64_t copied = 0;
        const auto ok = apr_run_read(command, copied);
        g_apr_reads.fetch_add(1, std::memory_order_relaxed);
        g_apr_read_bytes.fetch_add(copied, std::memory_order_relaxed);
        if (apr_trace_enabled()) {
            agc_trace(
                "apr.read id=0x%08X dst=0x%016llX bytes=%llu offset=%llu "
                "copied=%llu ok=%u total=%llu\n",
                command.file_id,
                static_cast<unsigned long long>(command.destination),
                static_cast<unsigned long long>(command.size),
                static_cast<unsigned long long>(command.file_offset),
                static_cast<unsigned long long>(copied),
                ok ? 1u : 0u,
                static_cast<unsigned long long>(
                    g_apr_read_bytes.load(std::memory_order_relaxed)));
        }
        if (!ok) {
            g_apr_read_failed.fetch_add(1, std::memory_order_relaxed);
            execution_result = -1;
            error_offset = static_cast<std::uint32_t>(command.record_offset);
            return 0;
        }
    }
    return 0;
}

bool apr_write_result(
    std::uint64_t result_address,
    std::int32_t execution_result,
    std::uint32_t error_offset) {
    if (result_address == 0) {
        return true;
    }
    return try_write_u32(
               result_address,
               static_cast<std::uint32_t>(execution_result)) &&
        try_write_u32(result_address + 4, error_offset);
}

std::uint64_t apr_submit(
    std::uint64_t command_buffer,
    std::uint64_t result_address,
    std::uint64_t out_submission_id_address,
    bool keep_submission) {
    if (command_buffer == 0) {
        return kAprErrorInvalidValue;
    }

    std::uint32_t submission_id = 0;
    if (keep_submission) {
        AcquireSRWLockExclusive(&g_apr_lock);
        submission_id = g_apr_next_submission_id++;
        if (submission_id == 0) {
            submission_id = g_apr_next_submission_id++;
        }
        g_apr_submissions[submission_id] =
            AprSubmission{result_address, 0, 0};
        ReleaseSRWLockExclusive(&g_apr_lock);
    }

    std::int32_t execution_result = 0;
    std::uint32_t error_offset = 0;
    const auto failure =
        apr_execute(command_buffer, execution_result, error_offset);
    if (failure != 0) {
        return failure;
    }

    if (keep_submission) {
        AcquireSRWLockExclusive(&g_apr_lock);
        auto stored = g_apr_submissions.find(submission_id);
        if (stored != g_apr_submissions.end()) {
            stored->second.execution_result = execution_result;
            stored->second.error_offset = error_offset;
        }
        ReleaseSRWLockExclusive(&g_apr_lock);
        if (out_submission_id_address != 0 &&
            !try_write_u32(out_submission_id_address, submission_id)) {
            return kAprErrorInvalidValue;
        }
    }

    return apr_write_result(result_address, execution_result, error_offset)
        ? 0
        : kAprErrorInvalidValue;
}

std::uint64_t apr_failure(std::uint64_t error) {
    // libkernel's APR entry points answer -1 on failure, not the raw
    // kernel error; the error itself only travels through errno.
    (void)error;
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(-1));
}

}  // namespace

std::uint64_t ps5rt_ampr_command_buffer_constructor_real(
    std::uint64_t command_buffer) {
    PS5_HLE_GUARD();
    if (command_buffer != 0) {
        std::array<std::uint8_t, kAprCbHeaderSize> zero = {};
        (void)try_write_process_bytes(
            command_buffer, zero.data(), zero.size());
        AcquireSRWLockExclusive(&g_apr_lock);
        g_apr_command_buffers.erase(command_buffer);
        ReleaseSRWLockExclusive(&g_apr_lock);
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_ampr_command_buffer_destructor_real(
    std::uint64_t command_buffer) {
    PS5_HLE_GUARD();
    if (command_buffer != 0) {
        AcquireSRWLockExclusive(&g_apr_lock);
        g_apr_command_buffers.erase(command_buffer);
        ReleaseSRWLockExclusive(&g_apr_lock);
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_ampr_apr_command_buffer_constructor_real(
    std::uint64_t command_buffer,
    std::uint64_t reserved_state0,
    std::uint64_t reserved_state1) {
    PS5_HLE_GUARD();
    if (command_buffer == 0) {
        restore_guest_fs();
        return 0;
    }
    if (reserved_state0 == 0) {
        reserved_state0 = command_buffer + kAprCbMapOffset;
    }
    if (reserved_state1 == 0) {
        reserved_state1 = command_buffer + kAprCbScatterGatherOffset;
    }
    (void)try_write_u64(reserved_state0, 0);
    if (reserved_state1 != reserved_state0) {
        (void)try_write_u64(reserved_state1, 0);
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_ampr_apr_command_buffer_destructor_real(std::uint64_t) {
    PS5_HLE_GUARD();
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_ampr_command_buffer_set_buffer_real(
    std::uint64_t command_buffer,
    std::uint64_t buffer,
    std::uint64_t size_raw) {
    PS5_HLE_GUARD();
    const auto size = static_cast<std::uint32_t>(size_raw);
    if (command_buffer == 0 || buffer == 0 || (buffer & 3u) != 0 ||
        size == 0 || size > kAprCbSizeMaximum || (size & 3u) != 0) {
        restore_guest_fs();
        return apr_failure(kAprErrorInvalidValue);
    }

    (void)try_write_u64(command_buffer + kAprCbDataOffset, buffer);
    (void)try_write_u32(command_buffer + kAprCbSizeOffset, size);
    (void)try_write_u32(command_buffer + kAprCbOffsetOffset, 0);
    (void)try_write_u32(command_buffer + kAprCbNumOffset, 0);

    AcquireSRWLockExclusive(&g_apr_lock);
    auto& state = g_apr_command_buffers[command_buffer];
    state.buffer = buffer;
    state.size = size;
    state.write_offset = 0;
    state.reads.clear();
    ReleaseSRWLockExclusive(&g_apr_lock);

    if (apr_trace_enabled()) {
        agc_trace(
            "apr.set_buffer cb=0x%016llX buffer=0x%016llX bytes=%u\n",
            static_cast<unsigned long long>(command_buffer),
            static_cast<unsigned long long>(buffer),
            size);
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_ampr_command_buffer_clear_buffer_real(
    std::uint64_t command_buffer) {
    PS5_HLE_GUARD();
    std::uint64_t buffer = 0;
    if (command_buffer != 0) {
        (void)try_read_u64(command_buffer + kAprCbDataOffset, buffer);
        (void)try_write_u64(command_buffer + kAprCbDataOffset, 0);
        (void)try_write_u32(command_buffer + kAprCbSizeOffset, 0);
        (void)try_write_u32(command_buffer + kAprCbOffsetOffset, 0);
        (void)try_write_u32(command_buffer + kAprCbNumOffset, 0);
        AcquireSRWLockExclusive(&g_apr_lock);
        g_apr_command_buffers.erase(command_buffer);
        ReleaseSRWLockExclusive(&g_apr_lock);
    }
    restore_guest_fs();
    return buffer;
}

std::uint64_t ps5rt_ampr_command_buffer_reset_real(
    std::uint64_t command_buffer) {
    PS5_HLE_GUARD();
    if (command_buffer != 0) {
        (void)try_write_u32(command_buffer + kAprCbOffsetOffset, 0);
        (void)try_write_u32(command_buffer + kAprCbNumOffset, 0);
        AcquireSRWLockExclusive(&g_apr_lock);
        auto state = g_apr_command_buffers.find(command_buffer);
        if (state != g_apr_command_buffers.end()) {
            state->second.write_offset = 0;
            state->second.reads.clear();
        }
        ReleaseSRWLockExclusive(&g_apr_lock);
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_ampr_command_buffer_get_current_offset_real(
    std::uint64_t command_buffer) {
    PS5_HLE_GUARD();
    std::uint32_t offset = 0;
    if (command_buffer != 0) {
        (void)try_read_u32(command_buffer + kAprCbOffsetOffset, offset);
    }
    restore_guest_fs();
    return offset;
}

std::uint64_t ps5rt_ampr_command_buffer_get_num_commands_real(
    std::uint64_t command_buffer) {
    PS5_HLE_GUARD();
    std::uint32_t count = 0;
    if (command_buffer != 0) {
        (void)try_read_u32(command_buffer + kAprCbNumOffset, count);
    }
    restore_guest_fs();
    return count;
}

std::uint64_t ps5rt_ampr_command_buffer_get_size_real(
    std::uint64_t command_buffer) {
    PS5_HLE_GUARD();
    std::uint32_t size = 0;
    if (command_buffer != 0) {
        (void)try_read_u32(command_buffer + kAprCbSizeOffset, size);
    }
    restore_guest_fs();
    return size;
}

std::uint64_t ps5rt_ampr_measure_command_size_read_file_real(
    std::uint64_t,
    std::uint64_t destination,
    std::uint64_t size,
    std::uint64_t file_offset) {
    PS5_HLE_GUARD();
    const auto measured =
        apr_read_range_valid(destination, size) &&
            file_offset < kAprMaximumFileOffset
        ? apr_read_file_record_size(file_offset)
        : 0ULL;
    restore_guest_fs();
    return measured;
}

// Seven parameters under the SysV ABI, so file_offset arrives on the guest
// stack rather than in a register; declaring it is enough for gcc to read
// it from there.
std::uint64_t ps5rt_ampr_apr_command_buffer_read_file_real(
    std::uint64_t command_buffer,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t file_id_raw,
    std::uint64_t destination,
    std::uint64_t size,
    std::uint64_t file_offset) {
    PS5_HLE_GUARD();
    const auto file_id = static_cast<std::uint32_t>(file_id_raw);
    if (command_buffer == 0 ||
        !apr_read_range_valid(destination, size) ||
        file_offset >= kAprMaximumFileOffset) {
        restore_guest_fs();
        return apr_failure(kAprErrorInvalidValue);
    }

    const auto record_size = apr_read_file_record_size(file_offset);
    auto accepted = false;
    std::uint64_t record_offset = 0;
    std::uint64_t record_address = 0;
    std::uint64_t committed_offset = 0;
    std::uint32_t committed_count = 0;

    AcquireSRWLockExclusive(&g_apr_lock);
    auto entry = g_apr_command_buffers.find(command_buffer);
    if (entry != g_apr_command_buffers.end()) {
        auto& state = entry->second;
        if (state.buffer != 0 && record_size <= state.size &&
            state.write_offset <= state.size - record_size) {
            record_offset = state.write_offset;
            record_address = state.buffer + record_offset;
            state.reads.push_back(
                AprReadFileCommand{
                    record_offset, file_id, destination, size, file_offset});
            state.write_offset += record_size;
            committed_offset = state.write_offset;
            committed_count =
                static_cast<std::uint32_t>(state.reads.size());
            accepted = true;
        }
    }
    ReleaseSRWLockExclusive(&g_apr_lock);

    if (!accepted) {
        restore_guest_fs();
        return apr_failure(kAprErrorInvalidValue);
    }

    std::array<std::uint8_t, kAprReadFileRecordSizeExtended> record = {};
    record[0] = 0x17;
    (void)try_write_process_bytes(
        record_address,
        record.data(),
        static_cast<std::size_t>(record_size));
    (void)try_write_u32(
        command_buffer + kAprCbOffsetOffset,
        static_cast<std::uint32_t>(committed_offset));
    (void)try_write_u32(command_buffer + kAprCbNumOffset, committed_count);
    std::uint32_t type = 0;
    if (try_read_u32(command_buffer + kAprCbTypeOffset, type)) {
        (void)try_write_u32(
            command_buffer + kAprCbTypeOffset,
            type | kAprTypeGatherScatterValid);
    }

    if (apr_trace_enabled()) {
        agc_trace(
            "apr.queue_read cb=0x%016llX id=0x%08X dst=0x%016llX bytes=%llu "
            "offset=%llu record=%llu\n",
            static_cast<unsigned long long>(command_buffer),
            file_id,
            static_cast<unsigned long long>(destination),
            static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(file_offset),
            static_cast<unsigned long long>(record_offset));
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_kernel_apr_resolve_filepaths_to_ids_real(
    std::uint64_t path_list,
    std::uint64_t count,
    std::uint64_t ids,
    std::uint64_t error_index) {
    PS5_HLE_GUARD();
    const auto result = apr_resolve_paths(
        path_list, static_cast<std::uint32_t>(count), ids, 0, error_index);
    restore_guest_fs();
    return result == 0 ? 0 : apr_failure(result);
}

std::uint64_t ps5rt_kernel_apr_resolve_filepaths_to_ids_and_sizes_real(
    std::uint64_t path_list,
    std::uint64_t count,
    std::uint64_t ids,
    std::uint64_t sizes,
    std::uint64_t error_index) {
    PS5_HLE_GUARD();
    const auto result = apr_resolve_paths(
        path_list,
        static_cast<std::uint32_t>(count),
        ids,
        sizes,
        error_index);
    restore_guest_fs();
    return result == 0 ? 0 : apr_failure(result);
}

std::uint64_t ps5rt_kernel_apr_get_file_size_real(
    std::uint64_t file_id_raw, std::uint64_t size_address) {
    PS5_HLE_GUARD();
    if (size_address == 0) {
        restore_guest_fs();
        return apr_failure(kAprErrorInvalidValue);
    }
    const auto file_id = static_cast<std::uint32_t>(file_id_raw);
    std::uint64_t size = 0;
    AcquireSRWLockShared(&g_apr_lock);
    const auto entry = g_apr_file_sizes.find(file_id);
    const auto known = entry != g_apr_file_sizes.end();
    if (known) {
        size = entry->second;
    }
    ReleaseSRWLockShared(&g_apr_lock);
    if (!known || !try_write_u64(size_address, size)) {
        restore_guest_fs();
        return apr_failure(kAprErrorNoEntry);
    }
    restore_guest_fs();
    return 0;
}

std::uint64_t ps5rt_kernel_apr_submit_command_buffer_real(
    std::uint64_t command_buffer, std::uint64_t) {
    PS5_HLE_GUARD();
    const auto result = apr_submit(command_buffer, 0, 0, false);
    restore_guest_fs();
    return result == 0 ? 0 : apr_failure(result);
}

std::uint64_t ps5rt_kernel_apr_submit_command_buffer_and_get_id_real(
    std::uint64_t command_buffer,
    std::uint64_t,
    std::uint64_t out_submission_id) {
    PS5_HLE_GUARD();
    if (out_submission_id == 0) {
        restore_guest_fs();
        return apr_failure(kAprErrorInvalidValue);
    }
    const auto result =
        apr_submit(command_buffer, 0, out_submission_id, true);
    restore_guest_fs();
    return result == 0 ? 0 : apr_failure(result);
}

std::uint64_t ps5rt_kernel_apr_submit_command_buffer_and_get_result_real(
    std::uint64_t command_buffer,
    std::uint64_t,
    std::uint64_t result_address,
    std::uint64_t out_submission_id) {
    PS5_HLE_GUARD();
    const auto result = apr_submit(
        command_buffer, result_address, out_submission_id, true);
    restore_guest_fs();
    return result == 0 ? 0 : apr_failure(result);
}

// Submission is synchronous here, so by the time the title waits the work
// is already done and this only reports the result it recorded.
std::uint64_t ps5rt_kernel_apr_wait_command_buffer_real(
    std::uint64_t submission_id_raw) {
    PS5_HLE_GUARD();
    const auto submission_id = static_cast<std::uint32_t>(submission_id_raw);
    AprSubmission submission;
    AcquireSRWLockExclusive(&g_apr_lock);
    const auto entry = g_apr_submissions.find(submission_id);
    const auto known = entry != g_apr_submissions.end();
    if (known) {
        submission = entry->second;
        g_apr_submissions.erase(entry);
    }
    ReleaseSRWLockExclusive(&g_apr_lock);
    if (!known) {
        restore_guest_fs();
        return apr_failure(kAprErrorInvalidValue);
    }
    const auto written = apr_write_result(
        submission.result_address,
        submission.execution_result,
        submission.error_offset);
    restore_guest_fs();
    return written ? 0 : apr_failure(kAprErrorInvalidValue);
}


// --- libc string search ---
//
// Three functions and 425,000 bridge crossings a run: strchr and strrchr
// alone are 192,783 and 192,368 of them, on one thread, and together with
// strstr they are 39 per cent of everything that reaches the managed side.
// Guest addresses are host addresses here, so these are the ordinary
// implementations over the guest's own bytes - the same memory the bridge
// would have walked, without the crossing.
std::uint64_t ps5rt_libc_strchr_real(
    std::uint64_t string_address, std::uint64_t character) {
    PS5_HLE_GUARD();
    std::uint64_t result = 0;
    if (string_address >= 0x10000) {
        const auto wanted = static_cast<char>(character & 0xFF);
        auto* cursor = reinterpret_cast<const char*>(string_address);
        for (;; ++cursor) {
            if (*cursor == wanted) {
                result = reinterpret_cast<std::uint64_t>(cursor);
                break;
            }
            if (*cursor == '\0') {
                break;
            }
        }
    }
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_libc_strrchr_real(
    std::uint64_t string_address, std::uint64_t character) {
    PS5_HLE_GUARD();
    std::uint64_t result = 0;
    if (string_address >= 0x10000) {
        const auto wanted = static_cast<char>(character & 0xFF);
        auto* cursor = reinterpret_cast<const char*>(string_address);
        for (;; ++cursor) {
            if (*cursor == wanted) {
                result = reinterpret_cast<std::uint64_t>(cursor);
            }
            if (*cursor == '\0') {
                break;
            }
        }
    }
    restore_guest_fs();
    return result;
}

std::uint64_t ps5rt_libc_strstr_real(
    std::uint64_t haystack_address, std::uint64_t needle_address) {
    PS5_HLE_GUARD();
    std::uint64_t result = 0;
    if (haystack_address >= 0x10000 && needle_address >= 0x10000) {
        const auto* haystack =
            reinterpret_cast<const char*>(haystack_address);
        const auto* needle =
            reinterpret_cast<const char*>(needle_address);
        if (needle[0] == '\0') {
            result = haystack_address;
        } else {
            const auto* found = std::strstr(haystack, needle);
            result = found == nullptr
                ? 0
                : reinterpret_cast<std::uint64_t>(found);
        }
    }
    restore_guest_fs();
    return result;
}


} // extern "C"

// libSceAvPlayer: the intro video. Last, because it uses the path mapping
// and the guest string reader above.
#include "ps5rt_avplayer.h"
#include "ps5rt_json.h"
