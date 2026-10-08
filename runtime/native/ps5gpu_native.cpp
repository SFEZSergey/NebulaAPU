// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "ps5gpu_bridge_api.h"
#include "ps5gpu_capture.h"
#include "ps5gpu_fixed_spirv.h"
#include "ps5gpu_gen5_preflight.h"
#include "ps5gpu_native_api.h"
#include "ps5gpu_window.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

namespace {

constexpr std::uint32_t kDefaultQueueCapacity = 16384;
constexpr std::uint32_t kMaximumQueueCapacity = 1u << 20;
constexpr std::uint64_t kAstroDisplayAddress = 0x0000000507410000ULL;
constexpr std::uint64_t kAstroCompositionPixelHash =
    0xA8419247FCA5A48BULL;
constexpr std::uint64_t kAstroState29ExportAddress =
    0x00000005003E3C00ULL;
constexpr std::uint64_t kAstroState29PixelAddress =
    0x00000005003E4600ULL;
constexpr std::uint64_t kAstroCompositionSourceAddress =
    0x000000053B9F0000ULL;
constexpr std::uint64_t kAstroCompositionTexture1Address =
    0x0000000532830000ULL;
constexpr std::uint64_t kAstroCompositionTexture2Address =
    0x0000000556760000ULL;
constexpr std::uint64_t kAstroCompositionPixelBufferAddress =
    0x0000000502DF5520ULL;
constexpr std::uint64_t kAstroState28ExportAddress =
    0x000000050071E200ULL;
constexpr std::uint64_t kAstroState28PixelAddress =
    0x000000050071EC00ULL;
constexpr std::uint64_t kAstroState28TargetAddress =
    0x000000053D410000ULL;
constexpr std::array<std::uint64_t, 5> kAstroState28BufferAddresses = {
    0x00000005074079D0ULL,
    0x0000000502DF6AC8ULL,
    0x0000000507408C00ULL,
    0x0000000502DF6A98ULL,
    0x0000000502DF6A70ULL,
};
constexpr std::array<std::uint64_t, 4> kAstroState28ImageAddresses = {
    0x000000053A500000ULL,
    0x000000053B330000ULL,
    0x000000053B430000ULL,
    0x000000053B480000ULL,
};
constexpr std::array<std::uint32_t, 4> kAstroState28ImageWidths = {
    960,
    480,
    240,
    120,
};
constexpr std::array<std::uint32_t, 4> kAstroState28ImageHeights = {
    540,
    270,
    135,
    67,
};
struct AstroPostPassConfig {
    std::uint32_t shader_state_file = 0;
    std::uint64_t es_address = 0;
    std::uint64_t ps_address = 0;
    std::uint64_t target_address = 0;
    std::uint32_t image_count = 0;
};
constexpr std::array<AstroPostPassConfig, 8> kAstroPostPasses = {{
    {20, 0x000000050071B900ULL, 0x000000050071C300ULL,
     0x000000053AA00000ULL, 1},
    {21, 0x00000005003E6000ULL, 0x00000005003E6A00ULL,
     0x000000053A500000ULL, 4},
    {22, 0x000000050071CC00ULL, 0x000000050071D600ULL,
     0x000000053B270000ULL, 17},
    {23, 0x000000050071CC00ULL, 0x000000050071D600ULL,
     0x000000053B330000ULL, 17},
    {24, 0x000000050071CC00ULL, 0x000000050071D600ULL,
     0x000000053B3F0000ULL, 17},
    {25, 0x000000050071CC00ULL, 0x000000050071D600ULL,
     0x000000053B430000ULL, 17},
    {26, 0x000000050071CC00ULL, 0x000000050071D600ULL,
     0x000000053B470000ULL, 17},
    {27, 0x000000050071CC00ULL, 0x000000050071D600ULL,
     0x000000053B480000ULL, 17},
}};
struct AstroCopyPassConfig {
    std::uint32_t shader_state_file = 0;
    std::uint64_t es_address = 0;
    std::uint64_t ps_address = 0;
    std::uint64_t target_address = 0;
};
constexpr std::array<AstroCopyPassConfig, 3> kAstroCopyPasses = {{
    {17, 0x0000000500795200ULL, 0x0000000500795C00ULL,
     0x000000053B9F0000ULL},
    {18, 0x0000000500796700ULL, 0x0000000500797100ULL,
     0x000000053BE70000ULL},
    {19, 0x0000000500798000ULL, 0x0000000500798A00ULL,
     0x000000053AA00000ULL},
}};
constexpr std::size_t kAstroPostStorageBufferCount = 4;
constexpr std::size_t kAstroPostMaximumImageCount = 17;
constexpr std::uint32_t kAstroCompositionSourceWidth = 2432;
constexpr std::uint32_t kAstroCompositionSourceHeight = 1368;
constexpr VkDeviceSize kAstroNullBufferBytes = 4096;
constexpr VkDeviceSize kAstroPixelBufferBytes = 160;
constexpr std::size_t kAstroCompositionStorageBufferCount = 5;
constexpr std::size_t kAstroCompositionPixelBufferIndex = 4;
constexpr std::size_t kAstroCompositionVertexBufferIndex = 3;
constexpr VkDeviceSize kAstroVertexBufferBytes = 72;
constexpr VkDeviceSize kAstroState29AuxBufferBytes = 256;
constexpr VkDeviceSize kAstroState29BufferBytes = 4096;
constexpr std::uint64_t kCapturePageSize = 4096;
// A frame's sampled images alone run to a couple of hundred megabytes: one
// 1920x1080 RGBA16F surface is 16.7 MB and a post chain holds dozens of them.
// The ceiling used to be 16384 pages - 64 MiB - which a real Astro Bot frame
// reached about half way through, after which capture_memory_range_locked
// silently stopped recording. Replay then read those pages back as zeroes, so
// every texture bound after the cut looked empty and the frame looked black
// for reasons that were entirely an artefact of the capture. Keep a ceiling,
// because the pages are written verbatim into the capture file, but make it
// large enough for a real frame and say so out loud when it is hit.
constexpr std::size_t kDefaultCapturedMemoryMegabytes = 512;
constexpr std::uint64_t kMaximumShaderResourceBytes = 256 * 1024;
constexpr std::uint32_t kMaximumSampledSurfaces = 4096;
constexpr std::uint64_t kMaximumGuestImageUploadBytes =
    512ULL * 1024ULL * 1024ULL;

enum class CommandType : std::uint32_t {
    Draw,
    Compute,
    Flip,
};

enum class RealCompositionMode : std::uint32_t {
    Real,
    RealEsGreen,
    FullscreenRealPs,
    FullscreenCopySource,
};

enum class RealEsBufferMode : std::uint32_t {
    ConstantTable,
    ConstantVertex,
    TableVertex,
    VertexVertex,
};

void runtime_trace(const char* format, ...);
void runtime_trace_flush();

// Knowing that a guest texture changed, without reading it to find out.
//
// Every dispatch used to re-read every texture it binds - 57GB in a two
// minute run - because the only record of what had already been uploaded
// lived on a per-state object, and this title registers a state per
// dispatch. Moving that record somewhere durable and comparing hashes was
// tried and measured worse three times, and it would not have helped much
// anyway: computing the hash still reads every byte, and the reading is
// most of the cost.
//
// shadPS4 answers this without reading. VideoCore::PageManager keeps a
// per-page count of write watchers, lowers the page permissions to match,
// and learns that the guest wrote from the access violation that follows.
// This is the same idea at the granularity of a whole texture: the first
// write anywhere in the range marks it and unprotects the whole thing, so
// a texture the guest rewrites costs one fault per upload rather than one
// per page, and a texture it never touches costs nothing at all.
//
// The watches are deliberately never freed. The fault handler reads them
// from whatever thread faulted and has no way to know a surface has gone;
// there is one per distinct texture address, which is bounded by the
// title.
struct GuestWriteWatch {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    DWORD original_protect = 0;
    // A range that is partly the title's own read-only pages: only these
    // read-write pieces are lowered, and only these are put back.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writable_parts;
    std::atomic<bool> armed{false};
    std::atomic<bool> dirty{true};
    // Bumped on every arming. Someone who took the range's bytes while it
    // was armed knows they are still good only while this is unchanged:
    // armed-and-clean alone cannot tell them apart from a disarm and a
    // re-arm by someone else in between.
    std::atomic<std::uint64_t> arm_serial{0};
};

// Puts back what arming a watch lowered.
bool guest_watch_restore(GuestWriteWatch& watch) {
    DWORD previous = 0;
    if (watch.writable_parts.empty()) {
        return VirtualProtect(
                   reinterpret_cast<void*>(watch.begin),
                   static_cast<SIZE_T>(watch.end - watch.begin),
                   watch.original_protect,
                   &previous) != 0;
    }
    auto restored = true;
    for (const auto& [begin, end] : watch.writable_parts) {
        restored = VirtualProtect(
                       reinterpret_cast<void*>(begin),
                       static_cast<SIZE_T>(end - begin),
                       PAGE_READWRITE,
                       &previous) != 0 &&
            restored;
    }
    return restored;
}

// Bumped whenever this file changes a page's protection, and read by the
// guest region cache so it never answers from before the change.
std::atomic<std::uint64_t> g_guest_protect_generation{0};

// Which range each recent change touched, so the region cache below drops
// only the entries a change overlaps (the HLE keeps the same ring for its
// own cache). A change made without a range - or more changes than the
// ring holds - still drops everything.
constexpr std::uint64_t kProtectChangeRing = 256;
struct ProtectChange {
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> begin{0};
    std::atomic<std::uint64_t> end{0};
};
ProtectChange g_protect_changes[kProtectChangeRing];
SRWLOCK g_protect_change_lock = SRWLOCK_INIT;

void note_protect_change(std::uint64_t begin, std::uint64_t end) {
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

bool protect_unchanged_since(
    std::uint64_t start, std::uint64_t end,
    std::uint64_t since, std::uint64_t current) {
    // PS5GPU_NATIVE_REGION_CACHE_STRICT=1: any change drops every entry.
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

// The HLE side keeps its own writability cache and cannot see protection
// changes made here. Tell it, through the export the host executable
// carries; if it is not there the cache keeps its old behaviour rather
// than this module failing to load.
void bump_host_protect_generation(
    std::uint64_t begin = 0, std::uint64_t end = ~std::uint64_t{0}) {
    // Which range changed, when the host can take it: its region cache then
    // drops only the entries that overlap. Told only that something
    // changed, it dropped everything, and the title's main thread spent a
    // fifth of the intro video in VirtualQuery refilling it - the watches
    // here change protection several times a frame.
    using Changed = void (*)(std::uint64_t, std::uint64_t);
    static const auto changed = [] {
        const auto host = GetModuleHandleW(nullptr);
        return host == nullptr
            ? nullptr
            : reinterpret_cast<Changed>(reinterpret_cast<void*>(
                  GetProcAddress(host, "ps5rt_guest_protect_changed")));
    }();
    if (changed != nullptr) {
        changed(begin, end);
        return;
    }
    using Bump = void (*)();
    static const auto bump = [] {
        const auto host = GetModuleHandleW(nullptr);
        return host == nullptr
            ? nullptr
            : reinterpret_cast<Bump>(reinterpret_cast<void*>(
                  GetProcAddress(
                      host, "ps5rt_guest_protect_generation_bump")));
    }();
    static std::atomic<std::uint64_t> announced{0};
    if (announced.fetch_add(1, std::memory_order_relaxed) == 0) {
        runtime_trace(
            "native_gpu.host_protect_generation resolved=%u\n",
            bump != nullptr ? 1u : 0u);
    }
    if (bump != nullptr) {
        bump();
    }
}

SRWLOCK g_guest_watch_lock = SRWLOCK_INIT;
std::map<std::uint64_t, GuestWriteWatch*> g_guest_watches;
std::atomic<std::uint64_t> g_guest_watch_faults{0};
std::atomic<std::uint64_t> g_guest_watch_arms{0};
std::atomic<std::uint64_t> g_guest_watch_refused{0};

constexpr std::uint64_t kGuestWatchPageSize = 4096;

std::atomic<std::uint64_t> g_guest_watch_races{0};
// Bytes committed to make a texture range watchable, against the refusals
// that used to cost a re-upload a frame.
std::atomic<std::uint64_t> g_guest_watch_committed_bytes{0};
// Watches that stood down so an overlapping, larger one could arm.
std::atomic<std::uint64_t> g_guest_watch_yielded{0};
std::atomic<std::uint64_t> g_indexed_draws{0};
std::atomic<std::uint64_t> g_command_ring_uses{0};
std::atomic<std::uint64_t> g_command_ring_waits{0};
std::atomic<std::uint64_t> g_index_buffers_created{0};
std::atomic<std::uint64_t> g_index_buffer_uploads{0};
std::atomic<std::uint64_t> g_index_buffer_bytes{0};
std::atomic<std::uint64_t> g_index_buffer_failures{0};
std::atomic<std::uint64_t> g_index_buffer_read_failures{0};
// How far below a fault a watch may still start and cover it. The widest
// texture seen in a run is a 3840x2160 render target at eight bytes a
// pixel, so 64MB leaves room and keeps the walk short.
constexpr std::uint64_t kGuestWatchSpanLimit = 64ULL * 1024ULL * 1024ULL;

LONG CALLBACK guest_watch_exception(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->NumberParameters < 2 ||
        record->ExceptionInformation[0] != 1) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto fault =
        static_cast<std::uint64_t>(record->ExceptionInformation[1]);
    {
        MEMORY_BASIC_INFORMATION entry_region = {};
        const auto entry_queried =
            VirtualQuery(
                reinterpret_cast<const void*>(fault),
                &entry_region,
                sizeof(entry_region)) == sizeof(entry_region);
        if (entry_queried &&
            entry_region.State == MEM_COMMIT &&
            (entry_region.Protect & 0xFFU) == PAGE_READONLY) {
            static std::atomic<std::uint64_t> entries{0};
            if (entries.fetch_add(1, std::memory_order_relaxed) < 24) {
                runtime_trace(
                    "native_gpu.watch_entry fault=0x%016llX thread=%lu "
                    "base=0x%016llX bytes=%llu\n",
                    static_cast<unsigned long long>(fault),
                    static_cast<unsigned long>(GetCurrentThreadId()),
                    static_cast<unsigned long long>(
                        reinterpret_cast<std::uintptr_t>(
                            entry_region.BaseAddress)),
                    static_cast<unsigned long long>(
                        entry_region.RegionSize));
                runtime_trace_flush();
            }
        }
    }

    // Watches are per texture and overlap, and nothing merges them, so
    // the one covering an address is not always the nearest key below it:
    // a later watch beginning between them wins upper_bound and does not
    // contain the fault. Stepping back one entry then answered "no watch"
    // for a page a watch had protected, the handler declined, and the
    // report called it a fatal write into a read-only page - which is what
    // ended runs at 540 and 280 flips. Walk back over the candidates the
    // way guest_watch_disarm_covering already does.
    //
    // And an armed one first. Two watches covering a page, the nearer one
    // already down and the other still holding the page read-only: taking
    // the nearer one found it disarmed and the page unwritable, declined
    // without a word, and the title's main thread died writing its light
    // constants as the title screen loaded.
    GuestWriteWatch* hit = nullptr;
    AcquireSRWLockShared(&g_guest_watch_lock);
    auto entry = g_guest_watches.upper_bound(fault);
    while (entry != g_guest_watches.begin()) {
        --entry;
        if (fault >= entry->second->begin && fault < entry->second->end) {
            if (hit == nullptr) {
                hit = entry->second;
            }
            if (entry->second->armed.load(std::memory_order_acquire)) {
                hit = entry->second;
                break;
            }
        }
        // Keys below this one can still cover the fault, but only while
        // some watch reaches far enough; the widest seen so far bounds
        // how far back it is worth looking.
        if (entry->second->begin + kGuestWatchSpanLimit < fault) {
            break;
        }
    }
    ReleaseSRWLockShared(&g_guest_watch_lock);
    if (hit == nullptr) {
        // A write fault on a committed read-only page in the guest window
        // that no watch covers should not exist: this module is the only
        // thing that makes those pages read-only. Say so with the nearest
        // watches on either side rather than leave the report to guess.
        MEMORY_BASIC_INFORMATION region = {};
        const auto queried =
            VirtualQuery(
                reinterpret_cast<const void*>(fault),
                &region,
                sizeof(region)) == sizeof(region);
        // A committed page that refuses a write is the anomaly: this module
        // is the only thing that lowers guest pages to read-only, so one no
        // watch covers should not exist. A reserved page is ordinary.
        const auto anomalous =
            queried &&
            region.State == MEM_COMMIT &&
            (region.Protect &
             (PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_NOACCESS)) != 0;
        static std::atomic<std::uint64_t> ordinary_declines{0};
        static std::atomic<std::uint64_t> anomalous_declines{0};
        const auto seen = anomalous
            ? anomalous_declines.fetch_add(1, std::memory_order_relaxed)
            : ordinary_declines.fetch_add(1, std::memory_order_relaxed);
        if (seen < (anomalous ? 16u : 2u)) {
            std::uint64_t below_begin = 0;
            std::uint64_t below_end = 0;
            std::uint64_t above_begin = 0;
            std::uint64_t watches = 0;
            AcquireSRWLockShared(&g_guest_watch_lock);
            watches = g_guest_watches.size();
            auto after = g_guest_watches.upper_bound(fault);
            if (after != g_guest_watches.end()) {
                above_begin = after->second->begin;
            }
            if (after != g_guest_watches.begin()) {
                --after;
                below_begin = after->second->begin;
                below_end = after->second->end;
            }
            ReleaseSRWLockShared(&g_guest_watch_lock);
            runtime_trace(
                "native_gpu.watch_declined anomalous=%d fault=0x%016llX "
                "state=0x%08lX protect=0x%08lX base=0x%016llX "
                "below=0x%016llX..0x%016llX above=0x%016llX "
                "watches=%llu\n",
                anomalous ? 1 : 0,
                static_cast<unsigned long long>(fault),
                static_cast<unsigned long>(queried ? region.State : 0),
                static_cast<unsigned long>(queried ? region.Protect : 0),
                static_cast<unsigned long long>(
                    queried
                        ? reinterpret_cast<std::uint64_t>(region.BaseAddress)
                        : 0),
                static_cast<unsigned long long>(below_begin),
                static_cast<unsigned long long>(below_end),
                static_cast<unsigned long long>(above_begin),
                static_cast<unsigned long long>(watches));
            runtime_trace_flush();
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Two threads writing one armed page is ordinary: the first disarms it
    // and carries on, and the second arrives to find the watch already
    // down. Declining then reports a fatal access violation on a page that
    // is open again by the time anyone looks - which is what ended a 540
    // flip run as a write into a PAGE_READONLY guest page from ucrtbase.
    // Retry instead, a couple of times for one address so a fault that is
    // not this cannot loop.
    if (!hit->armed.load(std::memory_order_acquire)) {
        MEMORY_BASIC_INFORMATION region = {};
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (VirtualQuery(
                reinterpret_cast<const void*>(fault),
                &region,
                sizeof(region)) != sizeof(region) ||
            region.State != MEM_COMMIT ||
            (region.Protect & writable) == 0) {
            static std::atomic<std::uint64_t> declined{0};
            if (declined.fetch_add(1, std::memory_order_relaxed) < 16) {
                runtime_trace(
                    "native_gpu.watch_declined_disarmed fault=0x%016llX "
                    "watch=0x%016llX..0x%016llX state=0x%08lX "
                    "protect=0x%08lX\n",
                    static_cast<unsigned long long>(fault),
                    static_cast<unsigned long long>(hit->begin),
                    static_cast<unsigned long long>(hit->end),
                    static_cast<unsigned long>(region.State),
                    static_cast<unsigned long>(region.Protect));
                runtime_trace_flush();
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
        static thread_local std::uint64_t last_retry = 0;
        static thread_local int retries = 0;
        if (fault == last_retry) {
            ++retries;
        } else {
            last_retry = fault;
            retries = 1;
        }
        if (retries > 2) {
            static std::atomic<std::uint64_t> exhausted{0};
            if (exhausted.fetch_add(1, std::memory_order_relaxed) < 16) {
                runtime_trace(
                    "native_gpu.watch_retries_exhausted fault=0x%016llX "
                    "watch=0x%016llX..0x%016llX protect=0x%08lX\n",
                    static_cast<unsigned long long>(fault),
                    static_cast<unsigned long long>(hit->begin),
                    static_cast<unsigned long long>(hit->end),
                    static_cast<unsigned long>(region.Protect));
                runtime_trace_flush();
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
        g_guest_watch_races.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    const auto restored = guest_watch_restore(*hit);
    const auto restore_error = restored ? DWORD{0} : GetLastError();
    note_protect_change(hit->begin, hit->end);
        bump_host_protect_generation(hit->begin, hit->end);
    hit->dirty.store(true, std::memory_order_release);
    hit->armed.store(false, std::memory_order_release);
    if (!restored) {
        // The guest write still has to fault into whatever handles it
        // rather than repeat forever against a page we failed to reopen.
        static std::atomic<std::uint64_t> failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) < 16) {
            MEMORY_BASIC_INFORMATION region = {};
            const auto queried =
                VirtualQuery(
                    reinterpret_cast<const void*>(fault),
                    &region,
                    sizeof(region)) == sizeof(region);
            runtime_trace(
                "native_gpu.watch_restore_failed fault=0x%016llX "
                "error=%lu watch=0x%016llX..0x%016llX original=0x%08lX "
                "state=0x%08lX protect=0x%08lX\n",
                static_cast<unsigned long long>(fault),
                static_cast<unsigned long>(restore_error),
                static_cast<unsigned long long>(hit->begin),
                static_cast<unsigned long long>(hit->end),
                static_cast<unsigned long>(hit->original_protect),
                static_cast<unsigned long>(queried ? region.State : 0),
                static_cast<unsigned long>(queried ? region.Protect : 0));
            runtime_trace_flush();
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_guest_watch_faults.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_EXECUTION;
}

void ensure_guest_watch_handler() {
    static std::once_flag once;
    std::call_once(once, [] {
        AddVectoredExceptionHandler(1, &guest_watch_exception);
    });
}

GuestWriteWatch& guest_watch_for(std::uint64_t address, std::uint64_t size) {
    const auto begin = address & ~(kGuestWatchPageSize - 1);
    const auto end =
        (address + size + kGuestWatchPageSize - 1) &
        ~(kGuestWatchPageSize - 1);
    AcquireSRWLockExclusive(&g_guest_watch_lock);
    auto entry = g_guest_watches.find(begin);
    if (entry == g_guest_watches.end()) {
        auto* watch = new GuestWriteWatch();
        watch->begin = begin;
        watch->end = end;
        entry = g_guest_watches.emplace(begin, watch).first;
    } else if (entry->second->end < end) {
        // A larger view of the same texture. Widen it and start again
        // rather than leave the tail unwatched.
        entry->second->armed.store(false, std::memory_order_release);
        entry->second->dirty.store(true, std::memory_order_release);
        entry->second->end = end;
    }
    auto& watch = *entry->second;
    ReleaseSRWLockExclusive(&g_guest_watch_lock);
    return watch;
}

// Arms after an upload, so the next dispatch can tell whether anything has
// happened since. Refuses anything that is not one plain committed
// read-write region: a range that spans regions cannot be protected in one
// call, and a range that is not read-write to begin with is not something
// to be making read-only.
bool guest_watch_disarm_covering(std::uint64_t address, std::size_t size);

bool guest_watch_arm(GuestWriteWatch& watch) {
    ensure_guest_watch_handler();
    if (watch.armed.load(std::memory_order_acquire)) {
        return true;
    }
    const auto length = static_cast<SIZE_T>(watch.end - watch.begin);
    // A watch this file armed earlier splits the guest's one big
    // reservation into pieces, so a later, larger texture inside it finds
    // its first region shorter than the range it wants and used to be
    // refused - 21 times a run, which is 21 textures re-read every frame
    // for nothing. Spanning regions is not the problem; what matters is
    // that every page in the range is committed and read-write, because
    // that is what disarming restores it to. Walk them and ask.
    MEMORY_BASIC_INFORMATION region = {};
    auto queried = false;
    auto uniform = true;
    // Pages the title mapped read-only itself - render targets it gave the
    // GPU and never meant to touch from the CPU. Nothing on the CPU writes
    // them, so they are watched as they stand; what can change them - our
    // own write-back, a DMA - already says so. Refused instead, the intro's
    // 64MB HDR target was read and found empty every frame.
    auto native_readonly = false;
    auto read_write = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writable_parts;
    for (auto cursor = watch.begin; cursor < watch.end;) {
        MEMORY_BASIC_INFORMATION part = {};
        if (VirtualQuery(
                reinterpret_cast<const void*>(cursor),
                &part,
                sizeof(part)) != sizeof(part)) {
            uniform = false;
            break;
        }
        if (cursor == watch.begin) {
            region = part;
            queried = true;
        }
        if (part.RegionSize == 0) {
            uniform = false;
            region = part;
            break;
        }
        // A texture range can hold pages the title has never written, which
        // the lazy commit leaves reserved. Refusing the whole watch over
        // them is what this cost: a 27MB texture with 323KB reserved inside
        // it was refused, marked dirty for good, and re-uploaded every
        // frame. Image upload then took 5.9s of a 12.2s frame, the queue
        // filled half a second after the scene appeared, and 3057 frames
        // were discarded with their presents in them - which is why the
        // flips stopped at 21 while the title kept drawing.
        //
        // Commit them instead. The disarm restores one protection across
        // the whole range, so the range has to be uniform; and these pages
        // are inside a texture the GPU is about to read, where an
        // uncommitted page reads as zero either way. The cost is bounded by
        // the texture: 323KB against 27MB in the case that was measured.
        if (part.State == MEM_RESERVE) {
            const auto reserved_bytes = (std::min)(
                static_cast<std::uint64_t>(part.RegionSize),
                watch.end - cursor);
            if (VirtualAlloc(
                    reinterpret_cast<void*>(cursor),
                    static_cast<SIZE_T>(reserved_bytes),
                    MEM_COMMIT,
                    PAGE_READWRITE) == nullptr) {
                uniform = false;
                region = part;
                break;
            }
            g_guest_watch_committed_bytes.fetch_add(
                reserved_bytes, std::memory_order_relaxed);
            read_write = true;
            writable_parts.emplace_back(cursor, cursor + reserved_bytes);
            cursor += reserved_bytes;
            continue;
        }
        // The other reason a range is not read-write is that a watch of
        // ours already lowered it: textures overlap, and a 51MB one whose
        // middle 27MB another watch had armed was refused every frame it
        // was bound. Ask that watch to stand down, the way the write path
        // already does before deciding a page cannot be written, and look
        // again. Its own texture becomes dirty and is re-read once; the
        // alternative was re-uploading the larger one for the whole run.
        if (part.State == MEM_COMMIT &&
            (part.Protect & 0xFFU) == PAGE_READONLY) {
            const auto covered = (std::min)(
                static_cast<std::uint64_t>(part.RegionSize),
                watch.end - cursor);
            const auto yielded = guest_watch_disarm_covering(
                cursor, static_cast<std::size_t>(covered));
            if (!yielded) {
                native_readonly = true;
                cursor += covered;
                continue;
            }
            if (yielded) {
                MEMORY_BASIC_INFORMATION again = {};
                if (VirtualQuery(
                        reinterpret_cast<const void*>(cursor),
                        &again,
                        sizeof(again)) == sizeof(again) &&
                    again.State == MEM_COMMIT &&
                    (again.Protect & 0xFFU) == PAGE_READWRITE &&
                    again.RegionSize != 0) {
                    g_guest_watch_yielded.fetch_add(
                        1, std::memory_order_relaxed);
                    const auto yielded_bytes = (std::min)(
                        static_cast<std::uint64_t>(again.RegionSize),
                        watch.end - cursor);
                    read_write = true;
                    writable_parts.emplace_back(cursor, cursor + yielded_bytes);
                    cursor += yielded_bytes;
                    continue;
                }
            }
        }
        if (part.State != MEM_COMMIT ||
            (part.Protect & 0xFFU) != PAGE_READWRITE) {
            uniform = false;
            region = part;
            break;
        }
        read_write = true;
        writable_parts.emplace_back(
            cursor,
            (std::min)(
                cursor + static_cast<std::uint64_t>(part.RegionSize),
                watch.end));
        cursor += part.RegionSize;
    }
    // Read-only and read-write in one range: lower the read-write pieces
    // alone, and remember them, so a disarm opens only what was open.
    if (queried && uniform && native_readonly && read_write) {
        auto lowered = true;
        for (const auto& [begin, end] : writable_parts) {
            DWORD previous = 0;
            lowered = VirtualProtect(
                          reinterpret_cast<void*>(begin),
                          static_cast<SIZE_T>(end - begin),
                          PAGE_READONLY,
                          &previous) != 0 &&
                lowered;
        }
        watch.writable_parts = std::move(writable_parts);
        note_protect_change(watch.begin, watch.end);
        bump_host_protect_generation(watch.begin, watch.end);
        if (!lowered) {
            guest_watch_restore(watch);
            g_guest_watch_refused.fetch_add(1, std::memory_order_relaxed);
            watch.dirty.store(true, std::memory_order_release);
            return false;
        }
        watch.dirty.store(false, std::memory_order_release);
        watch.arm_serial.fetch_add(1, std::memory_order_acq_rel);
        watch.armed.store(true, std::memory_order_release);
        g_guest_watch_arms.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    watch.writable_parts.clear();
    if (queried && uniform && native_readonly) {
        watch.original_protect = PAGE_READONLY;
        watch.dirty.store(false, std::memory_order_release);
        watch.arm_serial.fetch_add(1, std::memory_order_acq_rel);
        watch.armed.store(true, std::memory_order_release);
        g_guest_watch_arms.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (!queried || !uniform) {
        const auto refusals =
            g_guest_watch_refused.fetch_add(1, std::memory_order_relaxed);
        if (refusals < 8) {
            runtime_trace(
                "native_gpu.guest_watch_refused begin=0x%016llX "
                "wanted=%llu queried=%d base=0x%016llX state=0x%08lX "
                "protect=0x%08lX type=0x%08lX region_bytes=%llu\n",
                static_cast<unsigned long long>(watch.begin),
                static_cast<unsigned long long>(length),
                queried ? 1 : 0,
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uintptr_t>(
                        region.AllocationBase)),
                static_cast<unsigned long>(region.State),
                static_cast<unsigned long>(region.Protect),
                static_cast<unsigned long>(region.Type),
                static_cast<unsigned long long>(region.RegionSize));
        }
        watch.dirty.store(true, std::memory_order_release);
        return false;
    }
    DWORD previous = 0;
    if (VirtualProtect(
            reinterpret_cast<void*>(watch.begin),
            length,
            PAGE_READONLY,
            &previous) == 0) {
        g_guest_watch_refused.fetch_add(1, std::memory_order_relaxed);
        watch.dirty.store(true, std::memory_order_release);
        return false;
    }
    note_protect_change(watch.begin, watch.end);
        bump_host_protect_generation(watch.begin, watch.end);
    watch.original_protect = previous;
    watch.dirty.store(false, std::memory_order_release);
    watch.arm_serial.fetch_add(1, std::memory_order_acq_rel);
    watch.armed.store(true, std::memory_order_release);
    g_guest_watch_arms.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Our own arming can stand in the way of our own write. This title
// generates textures on the GPU: the same 64KB of guest memory is a
// compute pass's write-back destination and, a few dispatches later, the
// texture uploaded from those bytes. The upload arms the range read-only,
// and the next frame's write-back is refused - VirtualProtect leaves the
// pages PAGE_READONLY inside a PAGE_READWRITE allocation, so even
// WriteProcessMemory answers 998, sixteen pages of one buffer, once a
// frame, every frame. The compute pass then feeds a texture nothing.
//
// Disarming is what the fault handler already does when the guest writes
// through an armed page, and a write from here says exactly the same
// thing: the bytes are about to change, so what was uploaded is stale.
bool guest_watch_disarm_covering(std::uint64_t address, std::size_t size);

// Defined below with the other diagnostics.
std::uint64_t traced_image_address();

extern "C" PS5GPU_NATIVE_API void PS5GPU_NATIVE_CALL
ps5gpu_native_guest_memory_written(std::uint64_t address, std::uint64_t size) {
    if (address == 0 || size == 0) {
        return;
    }
    const auto disarmed = guest_watch_disarm_covering(
        address, static_cast<std::size_t>(size));
    const auto traced = traced_image_address();
    if (traced != 0 && traced >= address && traced < address + size) {
        runtime_trace(
            "native_gpu.traced_image_written address=0x%016llX "
            "bytes=%llu disarmed=%d\n",
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(size),
            disarmed ? 1 : 0);
    }
}

bool guest_watch_disarm_covering(std::uint64_t address, std::size_t size) {
    const auto begin = address;
    const auto end = address + size;
    std::vector<GuestWriteWatch*> hits;
    AcquireSRWLockShared(&g_guest_watch_lock);
    auto entry = g_guest_watches.upper_bound(begin);
    if (entry != g_guest_watches.begin()) {
        --entry;
    }
    for (; entry != g_guest_watches.end() && entry->second->begin < end;
         ++entry) {
        if (entry->second->end > begin &&
            entry->second->armed.load(std::memory_order_acquire)) {
            hits.push_back(entry->second);
        }
    }
    ReleaseSRWLockShared(&g_guest_watch_lock);

    auto disarmed = false;
    for (auto* watch : hits) {
        const auto restored = guest_watch_restore(*watch);
        note_protect_change(watch->begin, watch->end);
        bump_host_protect_generation(watch->begin, watch->end);
        watch->dirty.store(true, std::memory_order_release);
        watch->armed.store(false, std::memory_order_release);
        disarmed = disarmed || restored;
    }
    return disarmed;
}

// Fifty milliseconds per compute dispatch on the worker, of which the GPU
// waits account for nine seconds out of ninety-six. Split the rest: getting
// a pipeline for the state, getting its resources up to date, and recording
// and submitting the work. Written only by the worker thread.
std::uint64_t g_compute_pipeline_ticks = 0;
std::uint64_t g_compute_pipeline_count = 0;
std::uint64_t g_pipeline_cached = 0;
std::uint64_t g_pipeline_built = 0;
std::uint64_t g_pipeline_shared = 0;
std::uint64_t g_pipeline_created = 0;
std::uint64_t g_pipeline_decode_ticks = 0;
std::uint64_t g_pipeline_create_ticks = 0;
std::uint64_t g_pipeline_descriptor_ticks = 0;
std::uint64_t g_pipeline_buffer_ticks = 0;
std::uint64_t g_pipeline_buffer_n = 0;
std::set<std::uint64_t> g_pipeline_shapes;
std::uint64_t g_pipeline_image_ticks = 0;
std::uint64_t g_compute_resource_ticks = 0;
std::uint64_t g_compute_record_ticks = 0;
thread_local std::uint64_t g_record_wait_ticks = 0;
// A frame's own work, in three: what comes before its command buffer is
// begun, recording it, and submitting, waiting and presenting after.
thread_local std::uint64_t g_flip_pre_ticks = 0;
thread_local std::uint64_t g_flip_record_ticks = 0;
thread_local std::uint64_t g_flip_post_ticks = 0;
thread_local std::uint64_t g_flip_finish_ticks = 0;
thread_local std::uint64_t g_record_buffer_copy_ticks = 0;
thread_local std::uint64_t g_record_image_ticks = 0;
thread_local std::uint64_t g_record_submit_ticks = 0;
thread_local std::uint64_t g_record_readback_ticks = 0;
thread_local std::uint64_t g_record_end_ticks = 0;
thread_local std::uint64_t g_record_result_ticks = 0;
thread_local std::uint64_t g_record_writeback_ticks = 0;
thread_local std::uint64_t g_writeback_n = 0;
thread_local std::uint64_t g_writeback_bytes = 0;
thread_local std::uint64_t g_writeback_changed_pages = 0;
thread_local std::uint64_t g_writeback_map_ticks = 0;
thread_local std::uint64_t g_writeback_compare_ticks = 0;
thread_local std::uint64_t g_writeback_small_n = 0;
thread_local std::uint64_t g_writeback_small_bytes = 0;
thread_local std::uint64_t g_writeback_large_n = 0;
thread_local std::uint64_t g_writeback_large_bytes = 0;
thread_local std::uint64_t g_writeback_largest = 0;
thread_local std::uint64_t g_writeback_clean_n = 0;
thread_local std::uint64_t g_writeback_unmarked_n = 0;
thread_local std::uint64_t g_writeback_unmarked_bytes = 0;
thread_local std::uint64_t g_writeback_false_clean = 0;
thread_local std::uint64_t g_writeback_scan_ticks = 0;
thread_local std::uint64_t g_writeback_splice_ticks = 0;
thread_local std::uint64_t g_writeback_splice_n = 0;

// Updating a dispatch's resources is now the largest phase. It is two
// loops - one over the buffers it binds, one over the images - and they
// want telling apart.
// update_guest_descriptor_resources runs on the worker for a dispatch and
// on the guest thread for a shader state, so a shared counter here adds two
// threads together and reports a number that belongs to neither. That is
// how an image phase came to read larger than the resource phase containing
// it. Per thread, and the flip reports the worker's.
thread_local std::uint64_t g_resource_buffer_ticks = 0;
thread_local std::uint64_t g_buffer_map_ticks = 0;
thread_local std::uint64_t g_buffer_read_ticks = 0;
thread_local std::uint64_t g_buffer_read_n = 0;
thread_local std::uint64_t g_buffer_read_bytes = 0;
thread_local std::uint64_t g_buffer_fill_bytes = 0;
thread_local std::uint64_t g_buffer_fill_ticks = 0;
thread_local std::uint64_t g_buffer_probe_ticks = 0;
thread_local std::uint64_t g_buffer_n = 0;
thread_local std::uint64_t g_buffer_bytes = 0;
// Per frame: how many buffer reads repeat an address and size read earlier
// in the same frame, and how many of those are read-only to the shader.
//
// A cache over exactly those was built and measured and does not pay. The
// scope has to be one guest submission, not a frame - a submission reaches
// the GPU as a unit, so nothing the CPU does can land between two of its
// dispatches, while across a frame the guest may wait for one dispatch and
// refill its scratch buffer, and we run far enough behind that the cached
// bytes would be the refilled ones. Keyed that way it hits 3036 times a
// run for 2400MB, and the flips do not move: 60, 49, 58 against 46, 54, 66
// without it. Holding the bytes by moving the scratch into the cache
// rather than copying gives 57, 58, 61 and halves the hits, because the
// emptied arena slot has to be reallocated on the next miss.
//
// The reads are not the expense they look like. buf_read is 2.6 to 3.5
// seconds of a 25 second run for eight gigabytes, which is memory
// bandwidth and nothing else; removing a quarter of it is under the noise
// between two runs. The counters stay because the question will be asked
// again.
thread_local std::uint64_t g_buffer_repeat_n = 0;
std::uint64_t g_lazy_buffer_creates = 0;
thread_local std::uint64_t g_buffer_repeat_bytes = 0;
thread_local std::uint64_t g_buffer_repeat_readonly_n = 0;
thread_local std::uint64_t g_buffer_repeat_readonly_bytes = 0;
thread_local std::map<
    std::pair<std::uint64_t, std::uint64_t>,
    std::uint64_t> g_buffer_frame_reads;
// 44 per cent of descriptors read an address already read this frame, and
// that is 11.6GB of the 20GB the buffer phase moves - about 5.5 seconds of
// a 33 second worker budget if none of it had to happen. Whether it has to
// is a question about the bytes, so it was measured rather than assumed:
// hash every repeat and compare it with the first read of that frame.
//
// 2661 repeats of 2704 come back byte-identical, 1721MB of 1731MB. The
// other 43 are real: the guest rewrites those buffers between one dispatch
// and the next. Every one of them is a descriptor the shader cannot write,
// so the writable flag does not separate them - 27 addresses, mostly a
// ring of three 256KB buffers at 0x400284D90, 0x400324D90 and 0x4003C4D90
// that the title rotates through within a frame.
//
// That settles it: a frame-scoped payload cache would be right 98 per cent
// of the time and wrong the rest, which is a wrong answer rather than a
// coarser one. Verifying instead of caching costs a read of the guest
// bytes either way, so it saves the write half of a memcpy and nothing
// more. The bytes can only be shared once something tells us when the
// guest writes them, and the write-watch that would do it is already
// implicated in the writeback failures at 0x5707F0000.
//
// Behind PS5GPU_NATIVE_TRACE_REPEAT_HASH because it hashes every repeat,
// which on its own takes the run from 100 flips to 17.
std::mutex g_buffer_frame_hash_lock;
std::map<
    std::pair<std::uint64_t, std::uint64_t>,
    std::uint64_t> g_buffer_frame_hashes;
// Not thread_local: a dispatch's resources are prepared from the guest
// thread as well as the worker, and per-thread counts reported 43 of the
// 263 differences that actually happened.
std::atomic<std::uint64_t> g_buffer_repeat_same_n{0};
std::atomic<std::uint64_t> g_buffer_repeat_same_bytes{0};
std::atomic<std::uint64_t> g_buffer_repeat_differ_n{0};
std::atomic<std::uint64_t> g_buffer_repeat_differ_bytes{0};
// Split by the flag a frame cache would key on: a descriptor the shader
// cannot write is the only one worth caching, so the question is whether
// any of the differences land there.
std::atomic<std::uint64_t> g_buffer_repeat_differ_ro_n{0};
std::atomic<std::uint64_t> g_buffer_repeat_differ_ro_bytes{0};
thread_local std::uint64_t g_resource_image_ticks = 0;
// Images are 47 seconds of the 50 that updating a dispatch's resources
// costs. Preparing one reads the whole guest image, asks whether it is
// all zeroes, detiles it, converts it, hashes it and uploads it - once
// per dispatch per image - so which of those steps it is wants deciding
// rather than assuming.
thread_local std::uint64_t g_image_read_ticks = 0;
thread_local std::uint64_t g_image_zero_ticks = 0;
// Preparations that ended at the all-zero render target exit, which
// is the one almost all of them take, and the ones a verdict from
// earlier in the frame answered without reading anything.
thread_local std::uint64_t g_image_zero_exits = 0;
std::atomic<std::uint64_t> g_image_zero_epoch{0};
struct ImageZeroVerdict {
    std::uint64_t size = 0;
    std::uint64_t epoch = 0;
    // The arming the bytes were read under. Armed and clean alone could be
    // someone else's arming after a write: the intro's second video frame
    // buffer was read as zeroes once, written, re-armed for its other
    // plane, and then taken as empty for the rest of the video - green on
    // every other frame, in one run of three.
    std::uint64_t serial = 0;
};
// One entry per distinct render target address, which the title bounds.
thread_local std::map<std::uint64_t, ImageZeroVerdict> g_image_zero_seen;
thread_local std::uint64_t g_image_zero_cached = 0;
thread_local std::uint64_t g_image_detile_ticks = 0;
thread_local std::uint64_t g_image_convert_ticks = 0;
thread_local std::uint64_t g_image_upload_ticks = 0;
thread_local std::uint64_t g_image_prepared = 0;
thread_local std::uint64_t g_image_bytes_read = 0;
thread_local std::uint64_t g_image_watch_skipped = 0;
thread_local std::uint64_t g_image_direct_uploads = 0;
// PS5GPU_NATIVE_TRACE_IMAGE_TOP: which images the bytes read go to, and
// why the watch did not spare them - to tell a texture the title really
// rewrites from one we keep reading for nothing.
struct ImageReadTally {
    std::uint64_t reads = 0;
    std::uint64_t bytes = 0;
    std::uint64_t unarmed = 0;
    std::uint64_t dirty = 0;
    std::uint64_t uninitialized = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t unified_format = 0;
    std::uint32_t tile_mode = 0;
};
thread_local std::map<std::uint64_t, ImageReadTally> g_image_read_tally;
// Large read-only buffers kept on the device between dispatches while
// their guest range stays unwritten. PS5GPU_NATIVE_BUFFER_CACHE=0 turns
// it off.
constexpr VkDeviceSize kCachedBufferMinimumBytes = 256 * 1024;
std::uint64_t g_buffer_cache_hits = 0;
std::uint64_t g_buffer_cache_bytes = 0;
std::uint64_t g_shared_buffer_hits = 0;
std::uint64_t g_shared_buffer_bytes = 0;
bool buffer_cache_enabled() {
    static const auto enabled = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_BUFFER_CACHE");
        // A capture takes every buffer as it is read, and the two together
        // stalled the worker at the first flip.
        const auto* capture = std::getenv("PS5GPU_NATIVE_CAPTURE_PATH");
        if (capture != nullptr && capture[0] != '\0') {
            return false;
        }
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// PS5GPU_NATIVE_TRACE_BUFFER_TOP: the same for the buffers dispatches read.
struct BufferReadTally {
    std::uint64_t reads = 0;
    std::uint64_t bytes = 0;
    std::uint32_t flags = 0;
    // Of a large buffer's reads, how many 4KB pages differed from the
    // previous read of the same buffer, and how many were compared.
    std::uint64_t pages_changed = 0;
    std::uint64_t pages_compared = 0;
    std::vector<std::uint8_t> previous;
};
thread_local std::map<std::uint64_t, BufferReadTally> g_buffer_read_tally;

// With PS5GPU_NATIVE_TRACE_BUFFER_TOP: how much of a large buffer changed
// since it was last read - whether reading only what changed would pay.
void tally_buffer_pages(
    std::uint64_t address, const void* bytes, std::size_t size) {
    static const auto enabled = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_TRACE_BUFFER_TOP");
        return value != nullptr && value[0] == '1';
    }();
    if (!enabled || size < (1u << 20) || bytes == nullptr) {
        return;
    }
    auto& tally = g_buffer_read_tally[address];
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    if (tally.previous.size() == size) {
        for (std::size_t offset = 0; offset < size; offset += 4096) {
            const auto length = std::min<std::size_t>(4096, size - offset);
            ++tally.pages_compared;
            if (std::memcmp(
                    tally.previous.data() + offset, data + offset, length) !=
                0) {
                ++tally.pages_changed;
            }
        }
    }
    tally.previous.assign(data, data + size);
}
void tally_buffer_read(std::uint64_t address, std::uint64_t bytes, std::uint32_t flags) {
    static const auto enabled = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_TRACE_BUFFER_TOP");
        return value != nullptr && value[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    auto& tally = g_buffer_read_tally[address];
    ++tally.reads;
    tally.bytes += bytes;
    tally.flags = flags;
}
// Iterations of the loop that prepares uploads. It must come out at
// least as large as img_n, which counts the preparations made from
// inside it; the last attempt at timing this loop reported 36 against
// 355 and was believed for one run too long.
thread_local std::uint64_t g_image_loop_n = 0;
// The title exited on an unhandled C++ exception at 113 seconds, and the
// obvious candidate is that nothing ever releases a registered compute
// state while each one holds a manifest of up to seventeen megabytes.
// These say whether that is what is happening, and whether the snapshot
// those manifests carry is ever the thing that gets uploaded - it is only
// reached when reading the guest's live memory fails.
std::uint64_t g_buffer_guest_read_failed = 0;
std::uint64_t g_buffer_snapshot_used = 0;

inline std::int64_t worker_phase_counter() {
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}
bool read_current_process_memory(
    std::uint64_t address,
    void* destination,
    std::size_t size,
    void* user);
bool guest_range_usable(std::uint64_t address, std::size_t size, bool write);
bool write_current_process_memory(
    std::uint64_t address,
    const void* source,
    std::size_t size);
std::uint64_t hash_bytes(
    const void* data,
    std::size_t size,
    std::uint64_t hash = 1469598103934665603ULL);

// A hash for "did these bytes change", eight bytes a step. hash_bytes goes a
// byte at a time, which for a video frame - twelve megabytes, one a frame -
// was a pass the worker could not afford. Nothing keeps these values past
// the run, so they need not match hash_bytes.
std::uint64_t hash_words(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t lanes[4] = {
        0x9E3779B97F4A7C15ULL, 0xC2B2AE3D27D4EB4FULL,
        0x165667B19E3779F9ULL, 0x27D4EB2F165667C5ULL};
    std::size_t offset = 0;
    for (; offset + 32 <= size; offset += 32) {
        for (int lane = 0; lane < 4; ++lane) {
            std::uint64_t word = 0;
            std::memcpy(&word, bytes + offset + lane * 8, sizeof(word));
            lanes[lane] = (lanes[lane] ^ word) * 0x9FB21C651E98DF25ULL;
            lanes[lane] ^= lanes[lane] >> 29;
        }
    }
    auto hash = lanes[0] ^ (lanes[1] * 3) ^ (lanes[2] * 5) ^ (lanes[3] * 7) ^
        static_cast<std::uint64_t>(size);
    for (; offset < size; ++offset) {
        hash = (hash ^ bytes[offset]) * 1099511628211ULL;
    }
    return hash;
}

// Whether a range holds nothing but zeroes. A word at a time, and it stops
// at the first byte that is not, so the answer for a range that has content
// costs almost nothing to get.
bool bytes_all_zero(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size != 0 &&
           (reinterpret_cast<std::uintptr_t>(bytes) %
            sizeof(std::uint64_t)) != 0) {
        if (*bytes != 0) {
            return false;
        }
        ++bytes;
        --size;
    }
    while (size >= sizeof(std::uint64_t)) {
        std::uint64_t word = 0;
        std::memcpy(&word, bytes, sizeof(word));
        if (word != 0) {
            return false;
        }
        bytes += sizeof(word);
        size -= sizeof(word);
    }
    while (size != 0) {
        if (*bytes != 0) {
            return false;
        }
        ++bytes;
        --size;
    }
    return true;
}

std::size_t capture_flip_count() {
    static const std::size_t flips = [] {
        const auto* value =
            std::getenv("PS5GPU_NATIVE_CAPTURE_FLIPS");
        if (value == nullptr || value[0] == '\0') {
            return std::size_t{1};
        }
        char* end = nullptr;
        const auto parsed = std::strtoull(value, &end, 0);
        if (end == value || end == nullptr || end[0] != '\0' ||
            parsed < 1 || parsed > 64) {
            runtime_trace(
                "native_gpu.capture_flips_unknown value=%s fallback=1\n",
                value);
            return std::size_t{1};
        }
        return static_cast<std::size_t>(parsed);
    }();
    return flips;
}

std::size_t capture_draw_threshold() {
    static const std::size_t draws = [] {
        const auto* value =
            std::getenv("PS5GPU_NATIVE_CAPTURE_DRAW_THRESHOLD");
        if (value == nullptr || value[0] == '\0') {
            return std::size_t{0};
        }
        char* end = nullptr;
        const auto parsed = std::strtoull(value, &end, 0);
        if (end == value ||
            end == nullptr ||
            end[0] != '\0' ||
            parsed > 1000000) {
            runtime_trace(
                "native_gpu.capture_draw_threshold_unknown value=%s "
                "fallback=0\n",
                value);
            return std::size_t{0};
        }
        return static_cast<std::size_t>(parsed);
    }();
    return draws;
}

std::size_t maximum_captured_memory_pages() {
    static const std::size_t pages = [] {
        auto megabytes = kDefaultCapturedMemoryMegabytes;
        const auto* value =
            std::getenv("PS5GPU_NATIVE_CAPTURE_MEMORY_MB");
        if (value != nullptr && value[0] != '\0') {
            char* end = nullptr;
            const auto parsed = std::strtoull(value, &end, 0);
            if (end != value &&
                end != nullptr &&
                end[0] == '\0' &&
                parsed >= 1 &&
                parsed <= 8192) {
                megabytes = static_cast<std::size_t>(parsed);
            } else {
                runtime_trace(
                    "native_gpu.capture_memory_limit_unknown value=%s "
                    "fallback=%zu\n",
                    value,
                    kDefaultCapturedMemoryMegabytes);
            }
        }
        return static_cast<std::size_t>(
            static_cast<std::uint64_t>(megabytes) * 1024ULL * 1024ULL /
            kCapturePageSize);
    }();
    return pages;
}

// The guest's CB_BLEND*_CONTROL never reaches the GPU layer: it rides in the
// sentinel value streams the AGC decode discards, so every draw is an opaque
// overwrite. A state that draws the same quad thirty-three times only makes
// sense if those draws accumulate, which is what blending is. This switch
// forces the commonest mode so the hypothesis costs one probe to test, rather
// than being assumed while the real decode gets built.
void apply_forced_blend(VkPipelineColorBlendAttachmentState& attachment);

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

// CB_BLENDn_CONTROL as Vulkan blend state. Blending was never applied: every
// guest pipeline drew opaque, so a fade - a black quad whose alpha rises
// over the picture - painted the picture out entirely, which is what the
// intro video looked like.
//
// The register's fields: colour source factor in 4:0, operation in 7:5,
// destination factor in 12:8, the same three for alpha at 20:16, 23:21 and
// 28:24, whether alpha has its own at 29, and enable at 30.
VkBlendFactor guest_blend_factor(std::uint32_t value) {
    switch (value) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 2: return VK_BLEND_FACTOR_SRC_COLOR;
        case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6: return VK_BLEND_FACTOR_DST_ALPHA;
        case 7: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_COLOR;
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 13: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 15: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 16: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 17: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 18: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 19: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 20: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp guest_blend_op(std::uint32_t value) {
    switch (value) {
        case 1: return VK_BLEND_OP_SUBTRACT;
        case 2: return VK_BLEND_OP_MIN;
        case 3: return VK_BLEND_OP_MAX;
        case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
    }
}

void apply_guest_blend(
    VkPipelineColorBlendAttachmentState& attachment,
    std::uint32_t control) {
    if ((control & (1u << 30)) == 0) {
        return;
    }
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = guest_blend_factor(control & 0x1Fu);
    attachment.colorBlendOp = guest_blend_op((control >> 5) & 0x7u);
    attachment.dstColorBlendFactor = guest_blend_factor((control >> 8) & 0x1Fu);
    if ((control & (1u << 29)) != 0) {
        attachment.srcAlphaBlendFactor =
            guest_blend_factor((control >> 16) & 0x1Fu);
        attachment.alphaBlendOp = guest_blend_op((control >> 21) & 0x7u);
        attachment.dstAlphaBlendFactor =
            guest_blend_factor((control >> 24) & 0x1Fu);
    } else {
        attachment.srcAlphaBlendFactor = attachment.srcColorBlendFactor;
        attachment.alphaBlendOp = attachment.colorBlendOp;
        attachment.dstAlphaBlendFactor = attachment.dstColorBlendFactor;
    }
}

void apply_forced_blend(
    VkPipelineColorBlendAttachmentState& attachment) {
    // "alpha" is the commonest mode but is a no-op against shaders that emit
    // opaque alpha - which these do, every pixel of the frame reads alpha 1.0 -
    // so it cannot tell us whether blending matters. "add" can: thirty-three
    // additive passes over one quad are impossible to miss.
    static const auto mode = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_FORCE_BLEND");
        if (value == nullptr || value[0] == '\0') {
            return 0;
        }
        if (std::strcmp(value, "add") == 0) {
            return 2;
        }
        if (std::strcmp(value, "alpha") == 0 ||
            std::strcmp(value, "1") == 0) {
            return 1;
        }
        return 0;
    }();
    static auto reported = false;
    if (!reported) {
        reported = true;
        runtime_trace(
            "native_gpu.forced_blend mode=%s\n",
            mode == 2 ? "add" : mode == 1 ? "alpha" : "off");
    }
    if (mode == 0) {
        return;
    }
    attachment.blendEnable = VK_TRUE;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    if (mode == 2) {
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        return;
    }
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
}

RealCompositionMode real_composition_mode_from_environment() {
    const auto* value =
        std::getenv("PS5GPU_NATIVE_REAL_COMPOSITION_MODE");
    if (value == nullptr || value[0] == '\0' ||
        std::strcmp(value, "real") == 0) {
        return RealCompositionMode::Real;
    }
    if (std::strcmp(value, "real-es-green") == 0) {
        return RealCompositionMode::RealEsGreen;
    }
    if (std::strcmp(value, "fullscreen-real-ps") == 0) {
        return RealCompositionMode::FullscreenRealPs;
    }
    if (std::strcmp(value, "fullscreen-copy-source") == 0) {
        return RealCompositionMode::FullscreenCopySource;
    }
    runtime_trace(
        "native_gpu.real_composition_mode_unknown value=%s "
        "fallback=real\n",
        value);
    return RealCompositionMode::Real;
}

RealCompositionMode astro_state29_mode_from_environment() {
    const auto* value =
        std::getenv("PS5GPU_NATIVE_ASTRO_STATE29_MODE");
    if (value == nullptr || value[0] == '\0' ||
        std::strcmp(value, "real") == 0) {
        return RealCompositionMode::Real;
    }
    if (std::strcmp(value, "real-es-green") == 0) {
        return RealCompositionMode::RealEsGreen;
    }
    if (std::strcmp(value, "fullscreen-real-ps") == 0) {
        return RealCompositionMode::FullscreenRealPs;
    }
    if (std::strcmp(value, "fullscreen-copy-source") == 0) {
        return RealCompositionMode::FullscreenCopySource;
    }
    runtime_trace(
        "native_gpu.astro_state29_mode_unknown value=%s "
        "fallback=real\n",
        value);
    return RealCompositionMode::Real;
}

int astro_state28_copy_image_from_environment() {
    const auto* value =
        std::getenv("PS5GPU_NATIVE_ASTRO_STATE28_COPY_IMAGE");
    if (value == nullptr || value[0] == '\0') {
        return -1;
    }
    char* end = nullptr;
    const auto parsed = std::strtol(value, &end, 0);
    if (end == value ||
        (end != nullptr && end[0] != '\0') ||
        parsed < -1 ||
        parsed >= static_cast<long>(
            kAstroState28ImageAddresses.size())) {
        runtime_trace(
            "native_gpu.astro_state28_copy_image_unknown value=%s "
            "fallback=-1\n",
            value);
        return -1;
    }
    return static_cast<int>(parsed);
}

int astro_post_copy_state_from_environment() {
    const auto* value =
        std::getenv("PS5GPU_NATIVE_ASTRO_POST_COPY_STATE");
    if (value == nullptr || value[0] == '\0') {
        return -1;
    }
    char* end = nullptr;
    const auto parsed = std::strtol(value, &end, 0);
    const auto known_state = std::any_of(
        kAstroPostPasses.begin(),
        kAstroPostPasses.end(),
        [parsed](const AstroPostPassConfig& pass) {
            return static_cast<long>(pass.shader_state_file) == parsed;
        });
    if (end == value ||
        (end != nullptr && end[0] != '\0') ||
        (parsed != -1 && !known_state)) {
        runtime_trace(
            "native_gpu.astro_post_copy_state_unknown value=%s "
            "fallback=-1\n",
            value);
        return -1;
    }
    return static_cast<int>(parsed);
}

int sample_copy_state_from_environment() {
    const auto* value =
        std::getenv("PS5GPU_NATIVE_SAMPLE_COPY_STATE");
    if (value == nullptr || value[0] == '\0') {
        return -1;
    }
    char* end = nullptr;
    const auto parsed = std::strtol(value, &end, 0);
    if (end == value ||
        (end != nullptr && end[0] != '\0') ||
        parsed < -1 ||
        parsed > 4096) {
        runtime_trace(
            "native_gpu.sample_copy_state_unknown value=%s "
            "fallback=-1\n",
            value);
        return -1;
    }
    return static_cast<int>(parsed);
}

const char* real_composition_mode_name(RealCompositionMode mode) {
    switch (mode) {
    case RealCompositionMode::Real:
        return "real";
    case RealCompositionMode::RealEsGreen:
        return "real-es-green";
    case RealCompositionMode::FullscreenRealPs:
        return "fullscreen-real-ps";
    case RealCompositionMode::FullscreenCopySource:
        return "fullscreen-copy-source";
    }
    return "real";
}

RealEsBufferMode real_es_buffer_mode_from_environment() {
    const auto* value =
        std::getenv("PS5GPU_NATIVE_REAL_ES_BUFFER_MODE");
    if (value == nullptr || value[0] == '\0' ||
        std::strcmp(value, "constant-table") == 0) {
        return RealEsBufferMode::ConstantTable;
    }
    if (std::strcmp(value, "constant-vertex") == 0) {
        return RealEsBufferMode::ConstantVertex;
    }
    if (std::strcmp(value, "table-vertex") == 0) {
        return RealEsBufferMode::TableVertex;
    }
    if (std::strcmp(value, "vertex-vertex") == 0) {
        return RealEsBufferMode::VertexVertex;
    }
    runtime_trace(
        "native_gpu.real_es_buffer_mode_unknown value=%s "
        "fallback=constant-table\n",
        value);
    return RealEsBufferMode::ConstantTable;
}

const char* real_es_buffer_mode_name(RealEsBufferMode mode) {
    switch (mode) {
    case RealEsBufferMode::ConstantTable:
        return "constant-table";
    case RealEsBufferMode::ConstantVertex:
        return "constant-vertex";
    case RealEsBufferMode::TableVertex:
        return "table-vertex";
    case RealEsBufferMode::VertexVertex:
        return "vertex-vertex";
    }
    return "constant-table";
}

// Translated shaders reach the runtime from two places. Everything the
// offline recompiler produced sits in PS5GPU_NATIVE_SHADER_DIR, while states
// compiled live during an earlier run are written to
// PS5GPU_NATIVE_LIVE_SHADER_CACHE_DIR by cache_live_graphics_state. A state
// that only exists in the live cache used to be invisible here, so its draws
// found no pipeline and were dropped in silence. Search both, dump first.
const std::vector<std::filesystem::path>& shader_search_directories() {
    static const std::vector<std::filesystem::path> directories = [] {
        std::vector<std::filesystem::path> found;
        for (const auto* name : {
                 "PS5GPU_NATIVE_SHADER_DIR",
                 "PS5GPU_NATIVE_LIVE_SHADER_CACHE_DIR"}) {
            const auto* value = std::getenv(name);
            if (value == nullptr || value[0] == '\0') {
                continue;
            }
            std::filesystem::path directory(value);
            if (std::find(found.begin(), found.end(), directory) ==
                found.end()) {
                found.push_back(std::move(directory));
            }
        }
        return found;
    }();
    return directories;
}

std::string shader_search_summary() {
    std::string text;
    for (const auto& directory : shader_search_directories()) {
        if (!text.empty()) {
            text += ";";
        }
        text += directory.string();
    }
    return text.empty() ? std::string("<none>") : text;
}

std::filesystem::path resolve_shader_path(const std::string& name) {
    for (const auto& directory : shader_search_directories()) {
        auto candidate = directory / name;
        std::error_code error;
        if (std::filesystem::exists(candidate, error) && !error) {
            return candidate;
        }
    }
    return {};
}

bool load_spirv_file(
    const std::filesystem::path& path,
    std::vector<std::uint32_t>& words) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return false;
    }
    const auto size = input.tellg();
    if (size <= 0 ||
        static_cast<std::uint64_t>(size) % sizeof(std::uint32_t) != 0) {
        return false;
    }
    words.resize(
        static_cast<std::size_t>(size) / sizeof(std::uint32_t));
    input.seekg(0);
    input.read(
        reinterpret_cast<char*>(words.data()),
        size);
    return static_cast<bool>(input) &&
        words.size() >= 5 &&
        words[0] == spv::MagicNumber;
}

bool spirv_uses_descriptors(
    const std::vector<std::uint32_t>& words) {
    std::vector<std::uint32_t> descriptor_variables;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto instruction = words[offset];
        const auto word_count = instruction >> 16;
        const auto opcode = instruction & 0xFFFFu;
        if (word_count == 0 || offset + word_count > words.size()) {
            return true;
        }
        if (opcode == static_cast<std::uint32_t>(spv::OpDecorate) &&
            word_count >= 3 &&
            words[offset + 2] ==
                static_cast<std::uint32_t>(
                    spv::DecorationDescriptorSet)) {
            descriptor_variables.push_back(words[offset + 1]);
        }
        offset += word_count;
    }
    if (descriptor_variables.empty()) {
        return false;
    }
    const auto is_descriptor_variable =
        [&descriptor_variables](std::uint32_t id) {
            return std::find(
                descriptor_variables.begin(),
                descriptor_variables.end(),
                id) != descriptor_variables.end();
        };
    bool inside_function = false;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto instruction = words[offset];
        const auto word_count = instruction >> 16;
        const auto opcode = instruction & 0xFFFFu;
        if (word_count == 0 || offset + word_count > words.size()) {
            return true;
        }
        if (opcode == static_cast<std::uint32_t>(spv::OpFunction)) {
            inside_function = true;
        } else if (
            opcode == static_cast<std::uint32_t>(spv::OpFunctionEnd)) {
            inside_function = false;
        } else if (inside_function) {
            if (opcode == static_cast<std::uint32_t>(spv::OpLoad) &&
                word_count >= 4 &&
                is_descriptor_variable(words[offset + 3])) {
                return true;
            }
            if ((opcode ==
                     static_cast<std::uint32_t>(spv::OpAccessChain) ||
                 opcode ==
                     static_cast<std::uint32_t>(
                         spv::OpInBoundsAccessChain) ||
                 opcode ==
                     static_cast<std::uint32_t>(spv::OpPtrAccessChain)) &&
                word_count >= 4 &&
                is_descriptor_variable(words[offset + 3])) {
                return true;
            }
        }
        offset += word_count;
    }
    return false;
}

struct LoadedResourceManifest {
    Ps5GpuResourceManifestHeader header = {};
    std::vector<Ps5GpuResourceGlobal> globals;
    std::vector<Ps5GpuResourceImage> images;
    std::vector<Ps5GpuResourceVertexInput> vertex_inputs;
    // The manifest body, owned. Decoding it without a copy - pointing at
    // the registered state's own buffer, which by inspection outlives every
    // decode - measured well and did not hold: two runs, one unhandled C++
    // exception at 113 seconds and one start-up stall with no flips at all,
    // against eight clean runs either side of it. Something in the
    // ownership does not survive contact, and the cheap version of this is
    // not to copy less but to carry less; see the data section below.
    std::vector<std::uint8_t> bytes;
};

template <typename T>
bool copy_manifest_records(
    const std::vector<std::uint8_t>& bytes,
    std::uint32_t offset,
    std::uint32_t count,
    std::vector<T>& records) {
    if (count > 65536 ||
        offset > bytes.size() ||
        static_cast<std::uint64_t>(count) * sizeof(T) >
            bytes.size() - offset) {
        return false;
    }
    records.resize(count);
    if (!records.empty()) {
        std::memcpy(
            records.data(),
            bytes.data() + offset,
            records.size() * sizeof(T));
    }
    return true;
}

// Cuts a manifest down to the records that describe its resources and
// drops the copy of the guest buffers behind them. What the records say
// about that data - where it was and how big it was - is left alone, so
// everything downstream still knows the sizes; only the bytes go. The
// header's total size is rewritten to match, because decoding checks it.
void trim_manifest_data_section(std::vector<std::uint8_t>& manifest) {
    Ps5GpuResourceManifestHeader header = {};
    if (manifest.size() < sizeof(header)) {
        return;
    }
    std::memcpy(&header, manifest.data(), sizeof(header));
    if (header.magic != PS5GPU_RESOURCE_MANIFEST_MAGIC ||
        header.version != PS5GPU_RESOURCE_MANIFEST_VERSION ||
        header.header_size != sizeof(header) ||
        header.total_size != manifest.size() ||
        header.data_offset < sizeof(header) ||
        header.data_offset >= manifest.size()) {
        return;
    }
    header.total_size = header.data_offset;
    manifest.resize(header.data_offset);
    manifest.shrink_to_fit();
    std::memcpy(manifest.data(), &header, sizeof(header));
}

bool decode_resource_manifest(
    const std::uint8_t* bytes,
    std::size_t size,
    LoadedResourceManifest& manifest) {
    if (bytes == nullptr ||
        size < sizeof(Ps5GpuResourceManifestHeader) ||
        size > 256ULL * 1024ULL * 1024ULL) {
        return false;
    }
    manifest = {};
    manifest.bytes.assign(bytes, bytes + size);
    std::memcpy(
        &manifest.header,
        manifest.bytes.data(),
        sizeof(manifest.header));
    if (manifest.header.magic != PS5GPU_RESOURCE_MANIFEST_MAGIC ||
        manifest.header.version != PS5GPU_RESOURCE_MANIFEST_VERSION ||
        manifest.header.header_size != sizeof(manifest.header) ||
        manifest.header.total_size != manifest.bytes.size() ||
        manifest.header.data_offset > manifest.bytes.size()) {
        return false;
    }
    return copy_manifest_records(
               manifest.bytes,
               manifest.header.global_offset,
               manifest.header.global_count,
               manifest.globals) &&
        copy_manifest_records(
               manifest.bytes,
               manifest.header.image_offset,
               manifest.header.image_count,
               manifest.images) &&
        copy_manifest_records(
               manifest.bytes,
               manifest.header.vertex_offset,
               manifest.header.vertex_count,
               manifest.vertex_inputs);
}

bool load_resource_manifest_file(
    const std::filesystem::path& path,
    LoadedResourceManifest& manifest) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return false;
    }
    const auto size = input.tellg();
    if (size <= 0 ||
        size > static_cast<std::streamoff>(
            256ULL * 1024ULL * 1024ULL)) {
        return false;
    }
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        size);
    return static_cast<bool>(input) &&
        decode_resource_manifest(
            bytes.data(),
            bytes.size(),
            manifest);
}

// The modules of every state compiled during this run, kept in memory for
// the draw loop to build a pipeline from. The files the cache writes are
// named by state id, and ids are handed out in the order states are first
// seen - so the same name holds a different shader from one run to the
// next, and a file on disk is only this run's if this run wrote it.
using SharedSpirv = std::shared_ptr<const std::vector<std::uint32_t>>;

struct LiveGraphicsState {
    SharedSpirv es_spirv;
    SharedSpirv ps_spirv;
    LoadedResourceManifest es_manifest;
    LoadedResourceManifest ps_manifest;
};
std::mutex g_live_graphics_states_mutex;
std::map<std::uint32_t, LiveGraphicsState> g_live_graphics_states;

// Per-frame constants - a matrix, a colour conversion's scale - live in a
// ring the title rewrites every frame. The worker used to read them when it
// processed the frame, by which time the title had moved on: the intro
// video's vertex stage found its matrix zeroed and drew nothing. A GPU
// reads them when the draw is submitted, and so do we: the bytes are taken
// on the submitting thread and the worker uses what was taken.
//
// Every buffer the state's manifest names, including one whose descriptor
// came from user data: a state's hash covers its user data, so the address
// is the state's own. Leaving those out left the intro's matrix - which is
// named by user data - to the worker, and by the time the worker reached
// it the title had put three more frames through the same ring. Only small
// ones: vertex data does not live in a ring and copying it every draw is
// not free.
constexpr std::uint64_t kDrawSnapshotMaximumBytes = 64 * 1024;

// PS5GPU_NATIVE_TRACE_IMAGE_ADDRESS: one guest image whose uploads and
// write notifications are traced.
std::uint64_t traced_image_address() {
    static const auto traced = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_TRACE_IMAGE_ADDRESS");
        return value == nullptr ? std::uint64_t{0}
                                : std::strtoull(value, nullptr, 0);
    }();
    return traced;
}

// PS5GPU_NATIVE_TRACE_BUFFER_ADDRESS, for the snapshot's own trace.
std::uint64_t traced_buffer_address() {
    static const auto traced = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_TRACE_BUFFER_ADDRESS");
        return value == nullptr ? std::uint64_t{0}
                                : std::strtoull(value, nullptr, 0);
    }();
    return traced;
}
std::mutex g_draw_snapshot_mutex;
std::map<std::uint32_t, std::vector<std::pair<std::uint64_t, std::uint64_t>>>
    g_state_fixed_buffers;
std::map<std::pair<std::uint32_t, std::uint64_t>, std::vector<std::uint8_t>>
    g_draw_snapshots;
std::deque<std::pair<std::uint32_t, std::uint64_t>> g_draw_snapshot_order;

void remember_state_fixed_buffers(
    std::uint32_t state_id,
    const LoadedResourceManifest& es,
    const LoadedResourceManifest& ps) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> buffers;
    for (const auto* manifest : {&es, &ps}) {
        for (const auto& global : manifest->globals) {
            if (global.base_address < 0x10000 || global.data_size == 0 ||
                global.data_size > kDrawSnapshotMaximumBytes) {
                continue;
            }
            buffers.push_back({global.base_address, global.data_size});
        }
    }
    std::lock_guard guard(g_draw_snapshot_mutex);
    g_state_fixed_buffers[state_id] = std::move(buffers);
}

// Defined with the capture, below: puts what a snapshot took into it.
void capture_snapshot_bytes(
    std::uint64_t address, const std::vector<std::uint8_t>& bytes);

// On the submitting thread: the bytes a draw's constants hold now.
void take_draw_snapshots(std::uint32_t state_id) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> buffers;
    {
        std::lock_guard guard(g_draw_snapshot_mutex);
        const auto found = g_state_fixed_buffers.find(state_id);
        if (found == g_state_fixed_buffers.end()) {
            return;
        }
        buffers = found->second;
    }
    for (const auto& [address, size] : buffers) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!read_current_process_memory(
                address, bytes.data(), bytes.size(), nullptr)) {
            continue;
        }
        if (traced_buffer_address() == address) {
            static std::atomic<int> reported{0};
            if (reported.fetch_add(1) < 32) {
                const auto* floats =
                    reinterpret_cast<const float*>(bytes.data());
                const auto count = std::min<std::size_t>(bytes.size() / 4, 16);
                std::string values;
                char text[32];
                for (std::size_t index = 0; index < count; ++index) {
                    std::snprintf(text, sizeof(text), " %g", floats[index]);
                    values += text;
                }
                runtime_trace(
                    "native_gpu.submit_snapshot state=%u address=0x%016llX "
                    "floats=%s\n",
                    state_id,
                    static_cast<unsigned long long>(address),
                    values.c_str());
            }
        }
        const auto key = std::make_pair(state_id, address);
        std::lock_guard guard(g_draw_snapshot_mutex);
        const auto existing = g_draw_snapshots.find(key);
        if (existing == g_draw_snapshots.end()) {
            g_draw_snapshot_order.push_back(key);
        }
        g_draw_snapshots[key] = std::move(bytes);
        while (g_draw_snapshot_order.size() > 8192) {
            g_draw_snapshots.erase(g_draw_snapshot_order.front());
            g_draw_snapshot_order.pop_front();
        }
    }
}

// On the submitting thread: a draw's indices, which live in the same
// per-frame rings as its constants. Read by the worker frames later they
// were another frame's, and the UI plane the intro video is drawn onto came
// out as scattered cells of the wrong triangles.
void take_index_snapshot(
    std::uint32_t state_id, std::uint64_t address, std::uint64_t size) {
    if (address < 0x10000 || size == 0 || size > kDrawSnapshotMaximumBytes) {
        return;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!read_current_process_memory(
            address, bytes.data(), bytes.size(), nullptr)) {
        return;
    }
    const auto key = std::make_pair(state_id, address);
    std::lock_guard guard(g_draw_snapshot_mutex);
    if (g_draw_snapshots.find(key) == g_draw_snapshots.end()) {
        g_draw_snapshot_order.push_back(key);
    }
    g_draw_snapshots[key] = std::move(bytes);
    while (g_draw_snapshot_order.size() > 8192) {
        g_draw_snapshots.erase(g_draw_snapshot_order.front());
        g_draw_snapshot_order.pop_front();
    }
}

// Defined with the capture, below: records what the worker just read.
void capture_worker_read(std::uint64_t address, std::uint64_t size);

// On the worker: guest bytes for a buffer, from the draw's snapshot when
// there is one and from guest memory otherwise.
// Whether a draw has its own copy of the bytes at `address`, which makes
// them that draw's rather than the address's.
bool draw_snapshot_exists(std::uint32_t state_id, std::uint64_t address) {
    std::lock_guard guard(g_draw_snapshot_mutex);
    return g_draw_snapshots.find(std::make_pair(state_id, address)) !=
        g_draw_snapshots.end();
}

bool read_draw_buffer(
    std::uint32_t state_id,
    std::uint64_t address,
    void* destination,
    std::size_t size) {
    auto from_snapshot = false;
    {
        std::lock_guard guard(g_draw_snapshot_mutex);
        const auto found =
            g_draw_snapshots.find(std::make_pair(state_id, address));
        if (found != g_draw_snapshots.end() && found->second.size() >= size) {
            std::memcpy(destination, found->second.data(), size);
            from_snapshot = true;
        }
    }
    if (traced_buffer_address() == address) {
        runtime_trace(
            "native_gpu.draw_buffer_source state=%u address=0x%016llX "
            "size=%llu snapshot=%u\n",
            state_id,
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(size),
            from_snapshot ? 1u : 0u);
    }
    if (from_snapshot) {
        // Into the capture as the worker used it. Taken on the submitting
        // thread instead, later frames' snapshots kept landing in the
        // capture until the worker - frames behind - got round to writing
        // it, and replay drew the intro with another frame's constants.
        const auto* bytes = static_cast<const std::uint8_t*>(destination);
        capture_snapshot_bytes(
            address, std::vector<std::uint8_t>(bytes, bytes + size));
        return true;
    }
    const auto read =
        read_current_process_memory(address, destination, size, nullptr);
    if (read) {
        capture_worker_read(address, size);
    }
    return read;
}

bool take_live_graphics_state(
    std::uint32_t state_id, LiveGraphicsState& state) {
    std::lock_guard guard(g_live_graphics_states_mutex);
    const auto found = g_live_graphics_states.find(state_id);
    if (found == g_live_graphics_states.end()) {
        return false;
    }
    state = std::move(found->second);
    g_live_graphics_states.erase(found);
    return true;
}

// What a graphics state leaves behind outside the capture: its shaders
// waiting to be built and the constant ranges its draws snapshot. Called
// when the state itself is let go.
std::atomic<std::uint64_t> g_retired_graphics_captures{0};
std::atomic<std::uint64_t> g_dropped_live_graphics_states{0};
void forget_graphics_state(std::uint32_t state_id) {
    {
        std::lock_guard guard(g_live_graphics_states_mutex);
        g_live_graphics_states.erase(state_id);
    }
    std::lock_guard guard(g_draw_snapshot_mutex);
    g_state_fixed_buffers.erase(state_id);
}

// One copy of each distinct module. The title registers a few hundred
// states a frame after the intro, each with a vertex and a pixel module of
// a hundred or two kilobytes, and most are the same words as a state before
// them - the translator's emission cache hands back identical modules.
// Copying each into its own vector, and again into both pipeline variants
// the worker makes of it, was an eighth of the title's render thread and a
// fifth of the worker. Keyed by size and a hash, confirmed by the words.
std::mutex g_interned_spirv_mutex;
std::unordered_multimap<std::uint64_t, std::weak_ptr<const std::vector<std::uint32_t>>>
    g_interned_spirv;

SharedSpirv intern_spirv(const std::uint8_t* bytes, std::uint32_t size) {
    const auto hash = hash_words(bytes, size) ^
        (static_cast<std::uint64_t>(size) * 0x9E3779B97F4A7C15ULL);
    std::lock_guard guard(g_interned_spirv_mutex);
    const auto [first, last] = g_interned_spirv.equal_range(hash);
    for (auto at = first; at != last; ++at) {
        auto kept = at->second.lock();
        if (kept != nullptr &&
            kept->size() * sizeof(std::uint32_t) == size &&
            std::memcmp(kept->data(), bytes, size) == 0) {
            return kept;
        }
    }
    auto words = std::make_shared<std::vector<std::uint32_t>>(
        size / sizeof(std::uint32_t));
    std::memcpy(words->data(), bytes, size);
    SharedSpirv shared = std::move(words);
    // Modules nothing holds any more go when the table has grown: a sweep
    // every doubling keeps it near the live count.
    static std::size_t sweep_at = 1024;
    if (g_interned_spirv.size() >= sweep_at) {
        for (auto at = g_interned_spirv.begin();
             at != g_interned_spirv.end();) {
            at = at->second.expired() ? g_interned_spirv.erase(at)
                                      : std::next(at);
        }
        sweep_at = std::max<std::size_t>(1024, g_interned_spirv.size() * 2);
    }
    g_interned_spirv.emplace(hash, shared);
    return shared;
}

// A graphics state's modules as registered, kept while a capture is armed
// so the capture can carry them. Replay has no translator; without these it
// had no pipeline for any state compiled during the run, which by the
// scene is every state there is.
struct CaptureGraphicsPayload {
    SharedSpirv es_spirv;
    SharedSpirv ps_spirv;
    std::vector<std::uint8_t> es_manifest;
    std::vector<std::uint8_t> ps_manifest;
};
std::mutex g_capture_graphics_payloads_mutex;
std::map<std::uint32_t, CaptureGraphicsPayload> g_capture_graphics_payloads;

bool capture_requested() {
    static const bool requested = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_CAPTURE_PATH");
        return value != nullptr && value[0] != '\0';
    }();
    return requested;
}

bool cache_live_graphics_state(
    std::uint32_t state_id,
    const Ps5GpuNativeShaderState& state) {
    const auto has_payload =
        state.es_spirv != nullptr ||
        state.es_spirv_size != 0 ||
        state.es_resource_manifest != nullptr ||
        state.es_resource_manifest_size != 0 ||
        state.ps_spirv != nullptr ||
        state.ps_spirv_size != 0 ||
        state.ps_resource_manifest != nullptr ||
        state.ps_resource_manifest_size != 0;
    if (!has_payload) {
        return true;
    }

    constexpr std::uint32_t kMaximumSpirvBytes =
        64u * 1024u * 1024u;
    constexpr std::uint32_t kMaximumManifestBytes =
        256u * 1024u * 1024u;
    const auto valid_spirv = [](const std::uint8_t* bytes,
                                std::uint32_t size) {
        if (bytes == nullptr ||
            size < 5 * sizeof(std::uint32_t) ||
            size > kMaximumSpirvBytes ||
            (size % sizeof(std::uint32_t)) != 0) {
            return false;
        }
        std::uint32_t magic = 0;
        std::memcpy(&magic, bytes, sizeof(magic));
        return magic == spv::MagicNumber;
    };
    if (!valid_spirv(state.es_spirv, state.es_spirv_size) ||
        !valid_spirv(state.ps_spirv, state.ps_spirv_size) ||
        state.es_resource_manifest == nullptr ||
        state.es_resource_manifest_size <
            sizeof(Ps5GpuResourceManifestHeader) ||
        state.es_resource_manifest_size > kMaximumManifestBytes ||
        state.ps_resource_manifest == nullptr ||
        state.ps_resource_manifest_size <
            sizeof(Ps5GpuResourceManifestHeader) ||
        state.ps_resource_manifest_size > kMaximumManifestBytes) {
        runtime_trace(
            "native_gpu.graphics_cache_invalid state=%u "
            "es_spirv=%u es_manifest=%u ps_spirv=%u ps_manifest=%u\n",
            state_id,
            state.es_spirv_size,
            state.es_resource_manifest_size,
            state.ps_spirv_size,
            state.ps_resource_manifest_size);
        return false;
    }

    LoadedResourceManifest es_manifest;
    LoadedResourceManifest ps_manifest;
    if (!decode_resource_manifest(
            state.es_resource_manifest,
            state.es_resource_manifest_size,
            es_manifest) ||
        !decode_resource_manifest(
            state.ps_resource_manifest,
            state.ps_resource_manifest_size,
            ps_manifest) ||
        es_manifest.header.stage != PS5GPU_STAGE_VERTEX ||
        ps_manifest.header.stage != PS5GPU_STAGE_PIXEL) {
        runtime_trace(
            "native_gpu.graphics_cache_manifest_invalid state=%u\n",
            state_id);
        return false;
    }
    {
        LiveGraphicsState live;
        live.es_spirv = intern_spirv(state.es_spirv, state.es_spirv_size);
        live.ps_spirv = intern_spirv(state.ps_spirv, state.ps_spirv_size);
        if (capture_requested()) {
            CaptureGraphicsPayload payload;
            payload.es_spirv = live.es_spirv;
            payload.ps_spirv = live.ps_spirv;
            payload.es_manifest.assign(
                state.es_resource_manifest,
                state.es_resource_manifest + state.es_resource_manifest_size);
            payload.ps_manifest.assign(
                state.ps_resource_manifest,
                state.ps_resource_manifest + state.ps_resource_manifest_size);
            std::lock_guard payload_guard(g_capture_graphics_payloads_mutex);
            g_capture_graphics_payloads[state_id] = std::move(payload);
            // The recent ones are what a frame draws with.
            while (g_capture_graphics_payloads.size() > 16384) {
                g_capture_graphics_payloads.erase(
                    g_capture_graphics_payloads.begin());
            }
        }
        live.es_manifest = es_manifest;
        live.ps_manifest = ps_manifest;
        std::lock_guard guard(g_live_graphics_states_mutex);
        g_live_graphics_states[state_id] = std::move(live);
        // Shaders are kept here until the worker builds the state, and a
        // state that is registered again after being built is kept again.
        // After the intro the title registers a few hundred states a
        // frame, most of them drawn once, at a hundred or two kilobytes of
        // SPIR-V each: unbounded, this was gigabytes. The oldest go first;
        // a state that loses its shaders before being built is not drawn,
        // and 4096 is a dozen frames of the worker falling behind.
        // PS5GPU_NATIVE_LIVE_STATE_CAP sets it.
        static const std::size_t live_cap = [] {
            const auto* value = std::getenv("PS5GPU_NATIVE_LIVE_STATE_CAP");
            const auto parsed =
                value == nullptr ? 0ull : std::strtoull(value, nullptr, 0);
            return parsed == 0 ? std::size_t{4096}
                               : static_cast<std::size_t>(parsed);
        }();
        while (g_live_graphics_states.size() > live_cap &&
               g_live_graphics_states.begin()->first != state_id) {
            g_live_graphics_states.erase(g_live_graphics_states.begin());
            g_dropped_live_graphics_states.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    remember_state_fixed_buffers(state_id, es_manifest, ps_manifest);
    // Once per registration, a few hundred a frame in the scene; the flush
    // after it forced the log to disk each time.
    static std::atomic<std::uint32_t> kept_traced{0};
    if (kept_traced.fetch_add(1, std::memory_order_relaxed) < 256) {
        runtime_trace("native_gpu.live_state_kept state=%u\n", state_id);
    }

    // The files are read only at start-up, and the runner empties the
    // directory before that - the state above is what this run builds
    // from. Writing four of them for every registration was six percent of
    // the title's main thread during the intro.
    // PS5GPU_NATIVE_KEEP_LIVE_SHADER_FILES=1 writes them for inspection.
    static const auto keep_files =
        environment_flag_enabled("PS5GPU_NATIVE_KEEP_LIVE_SHADER_FILES");
    if (!keep_files) {
        return true;
    }

    const auto* directory =
        std::getenv("PS5GPU_NATIVE_LIVE_SHADER_CACHE_DIR");
    if (directory == nullptr || directory[0] == '\0') {
        directory = std::getenv("PS5GPU_NATIVE_SHADER_DIR");
    }
    if (directory == nullptr || directory[0] == '\0') {
        runtime_trace(
            "native_gpu.graphics_cache_directory_missing state=%u\n",
            state_id);
        return false;
    }
    const std::filesystem::path shader_directory(directory);
    std::error_code directory_error;
    std::filesystem::create_directories(
        shader_directory,
        directory_error);
    if (directory_error) {
        runtime_trace(
            "native_gpu.graphics_cache_directory_failed state=%u "
            "error=%d\n",
            state_id,
            directory_error.value());
        return false;
    }

    const auto write_blob = [](
        const std::filesystem::path& path,
        const std::uint8_t* bytes,
        std::uint32_t size) {
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(bytes),
            static_cast<std::streamsize>(size));
        return static_cast<bool>(output);
    };
    const auto stem =
        "state-" + std::to_string(state_id);
    const auto es_spirv_path =
        shader_directory / (stem + "-es.spv");
    const auto es_manifest_path =
        shader_directory / (stem + "-es.resources.bin");
    const auto ps_spirv_path =
        shader_directory / (stem + "-ps.spv");
    const auto ps_manifest_path =
        shader_directory / (stem + "-ps.resources.bin");
    if (!write_blob(
            es_spirv_path,
            state.es_spirv,
            state.es_spirv_size) ||
        !write_blob(
            es_manifest_path,
            state.es_resource_manifest,
            state.es_resource_manifest_size) ||
        !write_blob(
            ps_spirv_path,
            state.ps_spirv,
            state.ps_spirv_size) ||
        !write_blob(
            ps_manifest_path,
            state.ps_resource_manifest,
            state.ps_resource_manifest_size)) {
        runtime_trace(
            "native_gpu.graphics_cache_write_failed state=%u dir=%s\n",
            state_id,
            shader_directory.string().c_str());
        return false;
    }

    runtime_trace(
        "native_gpu.graphics_state_cached state=%u "
        "es_spirv=%u es_manifest=%u ps_spirv=%u ps_manifest=%u "
        "es_buffers=%u es_images=%u ps_buffers=%u ps_images=%u\n",
        state_id,
        state.es_spirv_size,
        state.es_resource_manifest_size,
        state.ps_spirv_size,
        state.ps_resource_manifest_size,
        es_manifest.header.global_count,
        es_manifest.header.image_count,
        ps_manifest.header.global_count,
        ps_manifest.header.image_count);
    return true;
}

template <typename T>
T load_module_export(HMODULE module, const char* name) {
    const auto raw = GetProcAddress(module, name);
    T function = nullptr;
    static_assert(sizeof(function) == sizeof(raw));
    std::memcpy(&function, &raw, sizeof(function));
    return function;
}

struct RegisteredComputeState {
    std::uint32_t state_id = 0;
    std::uint32_t flags = 0;
    std::uint64_t state_hash = 0;
    std::uint64_t shader_address = 0;
    std::uint64_t shader_header_address = 0;
    std::vector<std::uint32_t> spirv;
    std::vector<std::uint8_t> resource_manifest;
};

// When each recent flip was submitted, by flip id.
constexpr std::uint64_t kFlipSubmittedSlots = 256;
std::atomic<std::int64_t> g_flip_submitted_at[kFlipSubmittedSlots] = {};

struct QueuedCommand {
    CommandType type = CommandType::Draw;
    Ps5GpuNativeDraw draw = {};
    Ps5GpuNativeComputeDispatch compute = {};
    std::shared_ptr<const RegisteredComputeState> compute_state;
    Ps5GpuNativeFlip flip = {};
};

// Backend-neutral state produced by the AGC command decoder.
struct GpuIrDraw {
    std::uint64_t submission_id = 0;
    std::uint64_t draw_id = 0;
    std::uint64_t render_target_address = 0;
    std::uint64_t es_address = 0;
    std::uint64_t ps_address = 0;
    std::uint64_t ps_hash = 0;
    std::uint32_t shader_state_id = 0;
    std::uint32_t render_target_width = 0;
    std::uint32_t render_target_height = 0;
    std::uint32_t render_target_format = 0;
    std::uint32_t render_target_number_type = 0;
    std::uint32_t render_target_tile_mode = 0;
    std::uint32_t vertex_count = 0;
    // A fast clear to apply before the draw, from the guest's clear words.
    bool clear_first = false;
    std::uint32_t clear_word0 = 0;
    std::uint32_t clear_word1 = 0;
    std::uint32_t first_vertex = 0;
    std::uint64_t index_address = 0;
    std::uint32_t index_bytes = 2;
    std::uint32_t index_max = 0;
    std::uint32_t primitive_type = 0;
    std::uint32_t flags = 0;
    std::uint32_t target_mask = 0;
    // CB_BLEND0_CONTROL for the target, from the draw's reserved1.
    std::uint32_t blend_control = 0;
    std::uint32_t shader_mask = 0;
    bool solid_white_pixel_shader = false;
    bool sample_render_surface = false;
    bool astro_copy_process = false;
    std::uint32_t astro_copy_pass_index = 0;
    bool astro_post_process = false;
    std::uint32_t astro_post_pass_index = 0;
    std::array<
        std::uint64_t,
        kAstroPostStorageBufferCount> astro_post_buffer_addresses = {};
    bool astro_state29 = false;
    std::uint64_t sampled_address = 0;
    std::uint32_t sampled_width = 0;
    std::uint32_t sampled_height = 0;
    std::uint64_t astro_state29_control_address = 0;
    std::uint64_t astro_state29_table_address = 0;
    std::uint64_t guest_buffer_address = 0;
    std::uint32_t guest_buffer_size = 0;
    std::uint64_t export_constant_address = 0;
    std::uint64_t export_resource_table_address = 0;
    float viewport[4] = {};
    int32_t scissor[2] = {};
    std::uint32_t scissor_extent[2] = {};
};

// A rect list is three vertices that describe a rectangle: the fourth
// corner is derived, v0 + v2 - v1, rather than read. This draws it as a
// strip with a fourth vertex instead, which asks the vertex shader for an
// index the guest never provided. Half the draws this title submits are
// rect lists and the targets they draw into hold one value each, so
// whether the approximation is what empties them is worth being able to
// ask directly.
// A rect list wants the triangles (v0,v1,v2) and (v0,v2,v3). A strip of
// four gives (v0,v1,v2) and (v2,v1,v3), which is a different second
// triangle - and a fan of four gives exactly the pair a rect list wants.
// The strip pipelines are shared with the real triangle strips, so this
// swaps both rather than adding a third set: it is a question, not a fix.
bool rect_list_as_fan() {
    static const auto fan = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_RECT_AS_FAN");
        return value != nullptr && value[0] == '1';
    }();
    return fan;
}

bool rect_list_as_triangles() {
    static const auto plain = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_RECT_AS_TRIANGLE");
        return value != nullptr && value[0] == '1';
    }();
    return plain;
}

bool is_nonindexed_rect_list(const GpuIrDraw& draw) {
    return
        (draw.primitive_type == 7 || draw.primitive_type == 0x11) &&
        (draw.flags & PS5GPU_NATIVE_DRAW_INDEXED) == 0;
}

bool uses_triangle_strip(const GpuIrDraw& draw) {
    if (rect_list_as_triangles() && is_nonindexed_rect_list(draw)) {
        return false;
    }
    return draw.primitive_type == 6 || is_nonindexed_rect_list(draw);
}

std::uint32_t guest_vertex_count(const GpuIrDraw& draw) {
    if (is_nonindexed_rect_list(draw) &&
        (draw.vertex_count == 1 ||
         draw.vertex_count == 3 ||
         draw.vertex_count == 4)) {
        return rect_list_as_triangles() ? 3u : 4u;
    }
    return std::max(draw.vertex_count, 1u);
}

// PS5GPU_NATIVE_FRAME_DUMP_VERTICES: dump after the draw with this many
// vertices. State numbers move from run to run; a mesh's size does not.
std::uint32_t frame_dump_vertices() {
    static const std::uint32_t value = [] {
        const auto* text = std::getenv("PS5GPU_NATIVE_FRAME_DUMP_VERTICES");
        return text == nullptr
            ? 0u
            : static_cast<std::uint32_t>(std::strtoul(text, nullptr, 0));
    }();
    return value;
}

bool mask_skip_disabled() {
    static const auto disabled = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_NO_MASK_SKIP");
        return value != nullptr && value[0] == '1';
    }();
    return disabled;
}

GpuIrDraw make_gpu_ir_draw(const Ps5GpuNativeDraw& draw) {
    GpuIrDraw ir = {};
    ir.submission_id = draw.submission_id;
    ir.draw_id = draw.draw_id;
    ir.render_target_address = draw.render_target_address;
    ir.es_address = draw.es_address;
    ir.ps_address = draw.ps_address;
    ir.shader_state_id = draw.reserved0;
    ir.render_target_width = draw.render_target_width;
    ir.render_target_height = draw.render_target_height;
    ir.render_target_format = draw.render_target_format;
    ir.render_target_number_type = draw.render_target_number_type;
    ir.render_target_tile_mode = draw.render_target_tile_mode;
    ir.vertex_count = draw.vertex_count;
    ir.first_vertex = draw.first_vertex;
    ir.index_address = draw.index_address;
    ir.index_bytes = draw.index_bytes == 4 ? 4u : 2u;
    ir.index_max = draw.index_max;
    ir.primitive_type = draw.primitive_type;
    ir.flags = draw.flags;
    ir.target_mask = draw.target_mask;
    ir.blend_control = draw.reserved1;
    ir.shader_mask = draw.shader_mask;
    ir.clear_first = (draw.flags & PS5GPU_NATIVE_DRAW_CLEAR_FIRST) != 0;
    ir.clear_word0 = draw.clear_word0;
    ir.clear_word1 = draw.clear_word1;
    ir.viewport[0] = draw.viewport_x;
    ir.viewport[1] = draw.viewport_y;
    ir.viewport[2] = draw.viewport_width;
    ir.viewport[3] = draw.viewport_height;
    ir.scissor[0] = draw.scissor_x;
    ir.scissor[1] = draw.scissor_y;
    ir.scissor_extent[0] = draw.scissor_width;
    ir.scissor_extent[1] = draw.scissor_height;
    return ir;
}

enum class VkAllocSite {
    Other = 0,
    HostStorageBuffer,
    DeviceLocalBuffer,
    Readback,
    SurfaceImage,
    Count,
};

thread_local VkAllocSite g_vk_alloc_site = VkAllocSite::Other;

struct VkAllocSiteScope {
    explicit VkAllocSiteScope(VkAllocSite site) : previous(g_vk_alloc_site) {
        g_vk_alloc_site = site;
    }
    ~VkAllocSiteScope() { g_vk_alloc_site = previous; }
    VkAllocSite previous;
};

std::atomic<std::uint64_t>
    g_vk_site_count[static_cast<std::size_t>(VkAllocSite::Count)] = {};
std::atomic<std::uint64_t>
    g_vk_site_bytes[static_cast<std::size_t>(VkAllocSite::Count)] = {};

PFN_vkAllocateMemory g_vk_allocate_raw = nullptr;
PFN_vkFreeMemory g_vk_free_raw = nullptr;
std::atomic<std::uint64_t> g_evicted_compute_states{0};
std::atomic<std::uint64_t> g_retired_graphics_states{0};
// Bytes of manifest buffer copies kept against bytes dropped because the
// buffer has an address the live read can reach.
std::atomic<std::uint64_t> g_snapshot_kept_bytes{0};
std::atomic<std::uint64_t> g_snapshot_dropped_bytes{0};
std::atomic<std::uint64_t> g_pooled_buffer_hits{0};
std::atomic<std::uint64_t> g_pooled_buffer_misses{0};
std::atomic<std::uint64_t> g_pooled_buffer_returns{0};
std::atomic<std::uint64_t> g_revived_compute_states{0};
std::atomic<std::uint64_t> g_retired_compute_manifests{0};
std::atomic<std::uint64_t> g_vk_alloc_count{0};
std::atomic<std::uint64_t> g_vk_free_count{0};
std::atomic<std::uint64_t> g_vk_alloc_bytes{0};
std::atomic<std::uint64_t> g_vk_free_bytes{0};
SRWLOCK g_vk_alloc_lock = SRWLOCK_INIT;
// Size and site of each live allocation, so the per-site figures count
// what is held now rather than everything ever allocated.
std::map<std::uint64_t, std::pair<std::uint64_t, std::size_t>>
    g_vk_alloc_sizes;

VKAPI_ATTR VkResult VKAPI_CALL counted_allocate_memory(
    VkDevice device,
    const VkMemoryAllocateInfo* info,
    const VkAllocationCallbacks* callbacks,
    VkDeviceMemory* memory) {
    if (g_vk_allocate_raw == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto result =
        g_vk_allocate_raw(device, info, callbacks, memory);
    if (result == VK_SUCCESS && info != nullptr && memory != nullptr) {
        g_vk_alloc_count.fetch_add(1, std::memory_order_relaxed);
        g_vk_alloc_bytes.fetch_add(
            info->allocationSize, std::memory_order_relaxed);
        const auto site = static_cast<std::size_t>(g_vk_alloc_site);
        if (site < static_cast<std::size_t>(VkAllocSite::Count)) {
            g_vk_site_count[site].fetch_add(1, std::memory_order_relaxed);
            g_vk_site_bytes[site].fetch_add(
                info->allocationSize, std::memory_order_relaxed);
        }
        AcquireSRWLockExclusive(&g_vk_alloc_lock);
        g_vk_alloc_sizes[reinterpret_cast<std::uint64_t>(*memory)] = {
            info->allocationSize, site};
        ReleaseSRWLockExclusive(&g_vk_alloc_lock);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL counted_free_memory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* callbacks) {
    if (memory != VK_NULL_HANDLE) {
        std::uint64_t size = 0;
        std::size_t site = static_cast<std::size_t>(VkAllocSite::Count);
        AcquireSRWLockExclusive(&g_vk_alloc_lock);
        const auto entry =
            g_vk_alloc_sizes.find(reinterpret_cast<std::uint64_t>(memory));
        if (entry != g_vk_alloc_sizes.end()) {
            size = entry->second.first;
            site = entry->second.second;
            g_vk_alloc_sizes.erase(entry);
        }
        ReleaseSRWLockExclusive(&g_vk_alloc_lock);
        if (site < static_cast<std::size_t>(VkAllocSite::Count)) {
            g_vk_site_count[site].fetch_sub(1, std::memory_order_relaxed);
            g_vk_site_bytes[site].fetch_sub(size, std::memory_order_relaxed);
        }
        g_vk_free_count.fetch_add(1, std::memory_order_relaxed);
        g_vk_free_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    if (g_vk_free_raw != nullptr) {
        g_vk_free_raw(device, memory, callbacks);
    }
}

struct VulkanBackend {
    // A graphics pipeline is only usable inside a render pass whose attachment
    // format matches the one it was built against. Guest surfaces are not all
    // one format, and a pipeline's target is not known when the shader state is
    // first seen, so each pipeline keeps its inputs and materialises a variant
    // the first time it faces a given format.
    struct GuestPipelineVariants {
        // Shared, never copied: a state's two variants and the states that
        // replace it hold the same words. Copying two hundred kilobytes of
        // SPIR-V into each variant was a fifth of the worker in the scene.
        std::shared_ptr<const std::vector<std::uint32_t>> vertex_spirv;
        std::shared_ptr<const std::vector<std::uint32_t>> fragment_spirv;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPrimitiveTopology topology =
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        // Keyed by target format in the low half and CB_BLEND0_CONTROL
        // in the high: blending is pipeline state, and one shader pair is
        // drawn both opaque and blended.
        std::map<std::uint64_t, VkPipeline> by_format;
        bool valid = false;
        // What decides whether another state's pipeline will do: the two
        // shaders, and how the layout was declared (zero when only this
        // layout object is known to match).
        std::uint64_t vertex_hash = 0;
        std::uint64_t fragment_hash = 0;
        std::uint64_t layout_signature = 0;
    };

    // Graphics pipelines by what they are made of rather than by the state
    // that asked first. After the intro the title registers a few hundred
    // new states a frame - their user data addresses change, their shaders
    // mostly do not - and each state compiled its own pipelines: the
    // worker spent most of a 2.5 second frame inside the driver's shader
    // compiler. A pipeline works with any pipeline layout declared the
    // same way, so states whose shaders, topology, target format, blend
    // and layout declaration match now share one. The cache owns them; a
    // state's variants only borrow. PS5GPU_NATIVE_SHARE_PIPELINES=0 turns
    // sharing off.
    std::map<std::array<std::uint64_t, 6>, VkPipeline>
        shared_graphics_pipelines;
    std::uint64_t shared_pipeline_hits = 0;
    std::uint64_t shared_pipeline_builds = 0;

    static std::uint64_t hash_spirv_words(
        const std::vector<std::uint32_t>& words, std::uint64_t seed) {
        std::uint64_t hash = seed ^ (words.size() * 0x9E3779B97F4A7C15ull);
        for (const auto word : words) {
            hash = (hash ^ word) * 0x100000001B3ull;
            hash ^= hash >> 29;
        }
        return hash;
    }

    HMODULE loader = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    // The window the presented buffer is shown in, when asked for.
    ps5gpu::NativeWindowPresenter window_presenter;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    // One command buffer meant one submit and one queue wait for every
    // dispatch, and the wait is the whole cost: 56 seconds of it over 28761
    // dispatches, two milliseconds each. That is not the shaders running -
    // 1187 of those dispatches are 256 threads and 856 are 512, which a
    // 2060 SUPER finishes in microseconds. It is the round trip.
    //
    // With a ring, a dispatch waits on the slot it is about to reuse, which
    // was submitted a whole ring ago and is long finished. The deferral
    // already here is a different thing: it decides whether a dispatch's
    // results are needed now. This decides when the command buffer is free.
    static constexpr std::size_t kCommandRing = 16;
    std::array<VkCommandBuffer, kCommandRing> command_ring = {};
    std::array<VkFence, kCommandRing> command_ring_fences = {};
    std::array<bool, kCommandRing> command_ring_pending = {};
    // Bumped each time a slot is submitted, so a pipeline can tell whether
    // the submission it remembers is still the one in its slot.
    std::array<std::uint64_t, kCommandRing> command_ring_serial = {};
    std::size_t command_ring_index = 0;
    bool command_ring_ready = false;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    std::filesystem::path pipeline_cache_path;
    std::uint64_t saved_pipeline_cache_hash = 0;
    std::uint32_t queue_family = 0;
    bool ready = false;

    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
    PFN_vkCreateInstance create_instance = nullptr;
    PFN_vkEnumerateInstanceVersion enumerate_instance_version = nullptr;
    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties =
        nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties
        get_physical_device_memory_properties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties
        get_physical_device_queue_family_properties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties
        get_physical_device_format_properties = nullptr;
    PFN_vkCreateDevice create_device = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc = nullptr;

    PFN_vkDestroyDevice destroy_device = nullptr;
    PFN_vkGetDeviceQueue get_device_queue = nullptr;
    PFN_vkCreateCommandPool create_command_pool = nullptr;
    PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
    PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
    PFN_vkCreateFence create_fence = nullptr;
    PFN_vkDestroyFence destroy_fence = nullptr;
    PFN_vkWaitForFences wait_for_fences = nullptr;
    PFN_vkResetFences reset_fences = nullptr;
    PFN_vkFreeCommandBuffers free_command_buffers = nullptr;
    PFN_vkResetCommandBuffer reset_command_buffer = nullptr;
    PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
    PFN_vkEndCommandBuffer end_command_buffer = nullptr;
    PFN_vkQueueSubmit queue_submit = nullptr;
    PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
    // Every compute dispatch submits and then stalls the queue, forty
    // three times a frame, and a stall costs whatever the GPU takes.
    // None of that shows in a CPU sampler, so three fixes in a row moved
    // no frame time while this was never measured at all. Count it.
    std::uint64_t gpu_wait_us_compute = 0;
    // Whether work has been submitted that nothing has waited for.
    // The worker used to drain the queue after every dispatch, so
    // the GPU sat idle while the next dispatch was prepared and the
    // CPU sat idle while the GPU ran - forty-one full stops a frame,
    // taking turns instead of working together. The wait is only
    // needed where the result is read: before the command buffer is
    // reused, and before buffers are written back to the guest.
    bool gpu_work_pending = false;
    std::uint64_t gpu_wait_us_readback = 0;
    std::uint64_t gpu_wait_us_frame = 0;
    std::uint32_t gpu_wait_n_compute = 0;
    std::uint32_t gpu_wait_n_readback = 0;
    std::uint32_t gpu_wait_n_frame = 0;

    VkResult wait_queue_idle(
        std::uint64_t& microseconds,
        std::uint32_t& count) {
        LARGE_INTEGER frequency = {};
        LARGE_INTEGER started = {};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&started);
        const auto result = queue_wait_idle(queue);
        LARGE_INTEGER finished = {};
        QueryPerformanceCounter(&finished);
        if (frequency.QuadPart != 0) {
            microseconds += static_cast<std::uint64_t>(
                (finished.QuadPart - started.QuadPart) * 1000000LL /
                frequency.QuadPart);
        }
        ++count;
        return result;
    }
    PFN_vkCreateImage create_image = nullptr;
    PFN_vkDestroyImage destroy_image = nullptr;
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements = nullptr;
    PFN_vkCreateBuffer create_buffer = nullptr;
    PFN_vkDestroyBuffer destroy_buffer = nullptr;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements =
        nullptr;
    PFN_vkAllocateMemory allocate_memory = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    PFN_vkBindImageMemory bind_image_memory = nullptr;
    PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
    PFN_vkMapMemory map_memory = nullptr;
    PFN_vkUnmapMemory unmap_memory = nullptr;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = nullptr;
    PFN_vkCmdCopyBuffer cmd_copy_buffer = nullptr;
    PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image = nullptr;
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer = nullptr;
    PFN_vkCmdCopyImage cmd_copy_image = nullptr;
    PFN_vkCmdClearColorImage cmd_clear_color_image = nullptr;
    PFN_vkCreateShaderModule create_shader_module = nullptr;
    PFN_vkDestroyShaderModule destroy_shader_module = nullptr;
    PFN_vkCreateRenderPass create_render_pass = nullptr;
    PFN_vkDestroyRenderPass destroy_render_pass = nullptr;
    PFN_vkCreatePipelineLayout create_pipeline_layout = nullptr;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout = nullptr;
    PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout =
        nullptr;
    PFN_vkCreateDescriptorPool create_descriptor_pool = nullptr;
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool = nullptr;
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets = nullptr;
    PFN_vkUpdateDescriptorSets update_descriptor_sets = nullptr;
    PFN_vkCreateSampler create_sampler = nullptr;
    PFN_vkDestroySampler destroy_sampler = nullptr;
    PFN_vkCreateGraphicsPipelines create_graphics_pipelines = nullptr;
    PFN_vkCreateComputePipelines create_compute_pipelines = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    PFN_vkCreatePipelineCache create_pipeline_cache = nullptr;
    PFN_vkGetPipelineCacheData get_pipeline_cache_data = nullptr;
    PFN_vkDestroyPipelineCache destroy_pipeline_cache = nullptr;
    PFN_vkCreateImageView create_image_view = nullptr;
    PFN_vkDestroyImageView destroy_image_view = nullptr;
    PFN_vkCreateFramebuffer create_framebuffer = nullptr;
    PFN_vkDestroyFramebuffer destroy_framebuffer = nullptr;
    PFN_vkCmdBeginRenderPass cmd_begin_render_pass = nullptr;
    PFN_vkCmdEndRenderPass cmd_end_render_pass = nullptr;
    PFN_vkCmdBindPipeline cmd_bind_pipeline = nullptr;
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets = nullptr;
    PFN_vkCmdBindVertexBuffers cmd_bind_vertex_buffers = nullptr;
    PFN_vkCmdSetViewport cmd_set_viewport = nullptr;
    PFN_vkCmdSetScissor cmd_set_scissor = nullptr;
    PFN_vkCmdDraw cmd_draw = nullptr;
    PFN_vkCmdDrawIndexed cmd_draw_indexed = nullptr;
    PFN_vkCmdBindIndexBuffer cmd_bind_index_buffer = nullptr;
    PFN_vkCmdDispatch cmd_dispatch = nullptr;
    PFN_vkCmdPushConstants cmd_push_constants = nullptr;

    std::uint64_t ir_draws = 0;
    std::uint64_t submitted_frames = 0;
    std::vector<GpuIrDraw> frame_ir;
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    VkBuffer readback_buffer = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    VkDeviceSize readback_capacity = 0;
    bool frame_dump_written = false;
    // PS5GPU_NATIVE_FRAME_DUMP_SERIES: how many flips to dump, one every
    // PS5GPU_NATIVE_FRAME_DUMP_EVERY, each to the path with the flip number
    // appended - a video is a sequence, and one dump of it says little.
    std::uint32_t frame_dump_series_done = 0;
    std::uint64_t frame_dump_next_flip = 0;
    // A render pass names its attachment format, and a pipeline is only usable
    // with a render pass that matches. Guest surfaces are not all one format,
    // so both are kept per format rather than as a single RGBA8 pair.
    std::map<VkFormat, VkRenderPass> clear_render_passes;
    std::map<VkFormat, VkRenderPass> load_render_passes;
    std::map<VkFormat, VkImageUsageFlags> surface_usage_cache;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkRenderPass load_render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    GuestPipelineVariants diagnostic_pipeline_variants;
    GuestPipelineVariants solid_white_pipeline_variants;
    VkPipeline diagnostic_pipeline = VK_NULL_HANDLE;
    VkPipeline solid_white_pipeline = VK_NULL_HANDLE;
    std::map<std::uint32_t, GuestPipelineVariants>
        resource_free_guest_pipelines;
    std::map<std::uint32_t, GuestPipelineVariants>
        resource_free_guest_strip_pipelines;
    VkDescriptorSetLayout sampled_descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool sampled_descriptor_pool = VK_NULL_HANDLE;
    VkSampler sampled_sampler = VK_NULL_HANDLE;
    VkPipelineLayout sampled_pipeline_layout = VK_NULL_HANDLE;
    GuestPipelineVariants sampled_pipeline_variants;
    VkPipeline sampled_pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout real_descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool real_descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet real_descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout real_pipeline_layout = VK_NULL_HANDLE;
    std::vector<std::uint32_t> real_composition_vertex_spirv;
    std::vector<std::uint32_t> real_composition_fragment_spirv;
    std::map<VkFormat, VkPipeline> real_composition_pipelines;
    VkPipeline real_composition_pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout astro_state29_descriptor_set_layout =
        VK_NULL_HANDLE;
    VkDescriptorPool astro_state29_descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet astro_state29_descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout astro_state29_pipeline_layout = VK_NULL_HANDLE;
    // The scalar handles below mirror the RGBA8 variant so the readiness
    // checks stay as they were; the variant maps own every handle.
    GuestPipelineVariants astro_state29_pipeline_variants;
    VkPipeline astro_state29_pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout astro_state28_descriptor_set_layout =
        VK_NULL_HANDLE;
    VkDescriptorPool astro_state28_descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet astro_state28_descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout astro_state28_pipeline_layout = VK_NULL_HANDLE;
    GuestPipelineVariants astro_state28_pipeline_variants;
    VkPipeline astro_state28_pipeline = VK_NULL_HANDLE;
    struct AstroPostPipeline {
        VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        GuestPipelineVariants pipeline_variants;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::array<
            VkBuffer,
            kAstroPostStorageBufferCount> storage_buffers = {};
        std::array<
            VkDeviceMemory,
            kAstroPostStorageBufferCount> storage_memory = {};
        std::array<
            VkDeviceSize,
            kAstroPostStorageBufferCount> storage_sizes = {
            kAstroState29AuxBufferBytes,
            kAstroState29AuxBufferBytes,
            kAstroVertexBufferBytes,
            kAstroState29AuxBufferBytes,
        };
        bool resources_ready = false;
    };
    std::array<AstroPostPipeline, kAstroPostPasses.size()>
        astro_post_pipelines = {};
    int astro_post_copy_state = -1;
    int sample_copy_state = -1;
    std::array<VkBuffer, 5> astro_state28_storage_buffers = {};
    std::array<VkDeviceMemory, 5> astro_state28_storage_memory = {};
    std::array<VkDeviceSize, 5> astro_state28_storage_sizes = {
        kAstroState29AuxBufferBytes,
        kAstroState29AuxBufferBytes,
        kAstroVertexBufferBytes,
        kAstroState29AuxBufferBytes,
        VkDeviceSize{32},
    };
    bool astro_state28_resources_ready = false;
    int astro_state28_copy_image = -1;
    std::array<VkBuffer, 3> astro_state29_storage_buffers = {};
    std::array<VkDeviceMemory, 3> astro_state29_storage_memory = {};
    std::array<VkDeviceSize, 3> astro_state29_storage_sizes = {
        kAstroState29AuxBufferBytes,
        kAstroState29AuxBufferBytes,
        kAstroState29BufferBytes,
    };
    RealCompositionMode astro_state29_mode =
        RealCompositionMode::Real;
    bool astro_state29_resources_ready = false;
    VkBuffer real_vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory real_vertex_memory = VK_NULL_HANDLE;
    std::uint64_t real_vertex_address = 0;
    std::uint32_t real_vertex_stride = 0;
    std::uint32_t real_vertex_count = 0;
    bool real_vertex_ready = false;
    std::array<
        VkBuffer,
        kAstroCompositionStorageBufferCount> real_storage_buffers = {};
    std::array<
        VkDeviceMemory,
        kAstroCompositionStorageBufferCount> real_storage_memory = {};
    std::array<
        VkDeviceSize,
        kAstroCompositionStorageBufferCount> real_storage_sizes = {
        kAstroState29AuxBufferBytes,
        kAstroPixelBufferBytes,
        kAstroState29AuxBufferBytes,
        kAstroVertexBufferBytes,
        kAstroPixelBufferBytes,
    };
    bool real_composition_enabled = false;
    RealCompositionMode real_composition_mode =
        RealCompositionMode::Real;
    RealEsBufferMode real_es_buffer_mode =
        RealEsBufferMode::ConstantTable;
    bool real_resources_ready = false;
    struct RenderSurface {
        std::uint64_t guest_address = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t unified_format = 56;
        std::uint32_t bytes_per_pixel = 4;
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkDescriptorSet sampled_descriptor_set = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkDeviceSize allocated_bytes = 0;
        bool initialized = false;
        bool device_written = false;
        // Slices, for a volume. Everything else is one.
        std::uint32_t depth = 1;
    };
    using RenderSurfaceKey =
        std::tuple<
            std::uint64_t,
            std::uint32_t,
            std::uint32_t,
            VkFormat>;
    std::map<RenderSurfaceKey, RenderSurface> render_surfaces;
    // Volumes live apart: a 3D image is never a render target, and keeping
    // them out of render_surfaces keeps every lookup of a target by its
    // address and extent from ever landing on one.
    using VolumeSurfaceKey = std::tuple<
        std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, VkFormat>;
    std::map<VolumeSurfaceKey, RenderSurface> volume_surfaces;
    // The format a draw last rendered an address and extent in. Surfaces
    // are keyed by format as well, so this is what keeps a frame that only
    // samples a target from resolving a different format and being handed
    // a second, empty surface for it.
    using TargetFormatKey =
        std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>;
    mutable std::map<TargetFormatKey, std::uint32_t> last_target_format;
    std::map<const RenderSurface*, RenderSurface> surface_read_copies;
    struct GuestBufferResource {
        std::uint32_t descriptor_index = 0;
        std::uint32_t flags = 0;
        // Where this descriptor came from in the shader's user data, recorded
        // by the recompiler. The address baked beside it was whatever the
        // offline pass happened to see; the guest rotates these buffers every
        // frame, so the live value has to be read from the user data instead.
        std::uint32_t scalar_address = 0;
        // Which stage declared this descriptor. Its user data block is the one
        // that holds the live address; the two stages have separate blocks and
        // separate base registers.
        VkShaderStageFlags stages = 0;
        std::uint64_t guest_address = 0;
        std::vector<std::uint8_t> snapshot;
        std::vector<std::uint8_t> writeback_shadow;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        // Set when `buffer` is device-local and therefore not mappable.
        // The host writes and reads these instead, and the dispatch's
        // own command buffer copies between the two. Null means the
        // buffer is host-visible itself and is used directly.
        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        bool contents_probed = false;
        // What the buffer on the device already holds: these guest bytes,
        // taken while `cached_watch` was armed with this serial. While the
        // watch is still that arming and clean, the bytes are unchanged and
        // neither the read nor the copy to the device is needed.
        std::uint64_t cached_address = 0;
        VkDeviceSize cached_size = 0;
        void* cached_watch = nullptr;
        std::uint64_t cached_serial = 0;
        bool upload_skipped = false;
        // This dispatch's bytes come from the shared device copy of the
        // range rather than from the guest, or its read from the guest
        // also fills that copy. Set per dispatch; see SharedDeviceBuffer.
        VkBuffer copy_from_shared = VK_NULL_HANDLE;
        VkBuffer fill_shared = VK_NULL_HANDLE;
        // Set per dispatch when the guest's own memory is bound instead of
        // a copy of it: the imported range, and where this buffer starts
        // in it. See GuestMemoryImport.
        VkBuffer import_buffer = VK_NULL_HANDLE;
        VkDeviceSize import_offset = 0;

        VkDeviceMemory host_memory() const {
            return staging_memory != VK_NULL_HANDLE
                ? staging_memory
                : memory;
        }
    };
    // A device copy of a large read-only guest range, shared by every
    // pipeline that binds it. The per-slot cache above keeps one address a
    // slot, and the intro's biggest buffers are double-buffered: one slot
    // bound 0x553F41DD0 and 0x554F41DD0 on alternate frames, missed every
    // time, and read sixteen unchanged megabytes a frame for it - a
    // quarter of the worker's buffer time. A slot that misses copies from
    // here on the GPU while the watch says the range is unchanged.
    struct SharedDeviceBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* watch = nullptr;
        std::uint64_t serial = 0;
    };
    std::map<std::pair<std::uint64_t, VkDeviceSize>, SharedDeviceBuffer>
        shared_device_buffers;
    VkDeviceSize shared_device_bytes = 0;
    // The guest's memory itself, imported as device memory, so a buffer
    // the shader only reads is bound where it lies instead of copied.
    //
    // After the intro the worker spent 420ms a frame copying buffers, and
    // 2.3GB a frame of it was the same read-only ranges again for the next
    // draw: a 4.3MB one at 0x400BEB440 in a region the title writes every
    // frame, so no write watch keeps it, and dozens of 126KB ones under
    // the size worth watching. On the console the GPU reads that memory
    // where it is, which is what this does.
    //
    // Imported a committed run at a time, clipped to a 64MB window so one
    // import does not pin gigabytes. A range that cannot be imported is
    // remembered as such and copied as before. PS5GPU_NATIVE_IMPORT_GUEST=1
    // turns it on; 2 enables only the extension, 3 imports without binding.
    //
    // It does not work yet, and off is the default. Measured on an RTX
    // driver (alignment 4096, storage offset alignment 16):
    //  - Binding imports crashes nvoglv64 within five flips, reproducibly,
    //    with robust buffer access on as well. The extension alone is fine.
    //  - Importing without binding leaves the title at 1.6 s a frame, so
    //    pinning the pages is itself at odds with the write watches or the
    //    commits around them.
    //  - Most refusals are ranges not committed at all when the draw names
    //    them (0x554F41DD0, 16MB), which the copy path reads as zeros, and
    //    large buffers span several 4MB commits.
    // So guest memory would first have to be committed up front and kept
    // committed, and the watches taught to leave imported pages alone.
    struct GuestMemoryImport {
        std::uint64_t end = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    std::map<std::uint64_t, GuestMemoryImport> guest_imports;
    bool guest_import_supported = false;
    VkDeviceSize guest_import_alignment = 4096;
    VkDeviceSize storage_offset_alignment = 256;
    PFN_vkGetMemoryHostPointerPropertiesEXT
        get_memory_host_pointer_properties = nullptr;
    std::uint64_t guest_import_hits = 0;
    std::uint64_t guest_import_hit_bytes = 0;
    std::uint64_t guest_import_misaligned = 0;
    std::uint64_t guest_import_outside = 0;

    static bool guest_import_requested() {
        static const auto requested = [] {
            const auto* value = std::getenv("PS5GPU_NATIVE_IMPORT_GUEST");
            return value != nullptr &&
                (value[0] >= '1' && value[0] <= '3');
        }();
        return requested;
    }

    // The committed run around `address`, clipped to its window and
    // stretched to cover `size` when the run allows.
    bool guest_committed_run(
        std::uint64_t address,
        std::uint64_t size,
        std::uint64_t& start,
        std::uint64_t& end) const {
        constexpr std::uint64_t kWindow = 64ull << 20;
        MEMORY_BASIC_INFORMATION info = {};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &info,
                         sizeof(info)) == 0 ||
            info.State != MEM_COMMIT) {
            return false;
        }
        const auto allocation =
            reinterpret_cast<std::uint64_t>(info.AllocationBase);
        const auto window = address & ~(kWindow - 1);
        const auto limit = std::max(window + kWindow, address + size);
        start = reinterpret_cast<std::uint64_t>(info.BaseAddress);
        end = start + info.RegionSize;
        // Regions split wherever a protection differs, and the write
        // watches make many of them, so walk both ways.
        while (start > window && start > allocation) {
            MEMORY_BASIC_INFORMATION before = {};
            if (VirtualQuery(reinterpret_cast<const void*>(start - 1),
                             &before, sizeof(before)) == 0 ||
                before.State != MEM_COMMIT ||
                reinterpret_cast<std::uint64_t>(before.AllocationBase) !=
                    allocation) {
                break;
            }
            start = reinterpret_cast<std::uint64_t>(before.BaseAddress);
        }
        while (end < limit) {
            MEMORY_BASIC_INFORMATION after = {};
            if (VirtualQuery(reinterpret_cast<const void*>(end), &after,
                             sizeof(after)) == 0 ||
                after.State != MEM_COMMIT ||
                reinterpret_cast<std::uint64_t>(after.AllocationBase) !=
                    allocation) {
                break;
            }
            end = reinterpret_cast<std::uint64_t>(after.BaseAddress) +
                after.RegionSize;
        }
        start = std::max(start, window);
        end = std::min(end, limit);
        const auto alignment = guest_import_alignment;
        start = (start + alignment - 1) & ~(alignment - 1);
        end &= ~(alignment - 1);
        return start <= address && address + size <= end;
    }

    GuestMemoryImport create_guest_import(std::uint64_t start,
                                          std::uint64_t end) {
        GuestMemoryImport imported;
        imported.end = end;
        VkMemoryHostPointerPropertiesEXT pointer = {};
        pointer.sType =
            VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
        auto result = get_memory_host_pointer_properties(
            device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            reinterpret_cast<const void*>(start),
            &pointer);
        if (result != VK_SUCCESS || pointer.memoryTypeBits == 0) {
            runtime_trace(
                "native_gpu.guest_import_failed start=0x%016llX "
                "end=0x%016llX step=pointer result=%d\n",
                static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end),
                static_cast<int>(result));
            return imported;
        }
        VkExternalMemoryBufferCreateInfo external = {};
        external.sType =
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        external.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.pNext = &external;
        buffer_info.size = end - start;
        buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        result = create_buffer(device, &buffer_info, nullptr, &buffer);
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.guest_import_failed start=0x%016llX "
                "end=0x%016llX step=buffer result=%d\n",
                static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end),
                static_cast<int>(result));
            return imported;
        }
        VkMemoryRequirements requirements = {};
        get_buffer_memory_requirements(device, buffer, &requirements);
        const auto types = requirements.memoryTypeBits &
            pointer.memoryTypeBits;
        std::uint32_t type = UINT32_MAX;
        for (std::uint32_t index = 0;
             index < memory_properties.memoryTypeCount; ++index) {
            if ((types & (1u << index)) == 0) {
                continue;
            }
            if (type == UINT32_MAX ||
                (memory_properties.memoryTypes[index].propertyFlags &
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
                type = index;
            }
        }
        VkImportMemoryHostPointerInfoEXT import_info = {};
        import_info.sType =
            VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
        import_info.handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import_info.pHostPointer = reinterpret_cast<void*>(start);
        VkMemoryAllocateInfo allocate_info = {};
        allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.pNext = &import_info;
        allocate_info.allocationSize = end - start;
        allocate_info.memoryTypeIndex = type;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        result = type == UINT32_MAX
            ? VK_ERROR_FEATURE_NOT_PRESENT
            : allocate_memory(device, &allocate_info, nullptr, &memory);
        if (result == VK_SUCCESS) {
            result = bind_buffer_memory(device, buffer, memory, 0);
        }
        if (result != VK_SUCCESS) {
            if (memory != VK_NULL_HANDLE) {
                free_memory(device, memory, nullptr);
            }
            destroy_buffer(device, buffer, nullptr);
            runtime_trace(
                "native_gpu.guest_import_failed start=0x%016llX "
                "end=0x%016llX step=memory type=%u types=0x%X result=%d\n",
                static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end),
                type,
                types,
                static_cast<int>(result));
            return imported;
        }
        imported.buffer = buffer;
        imported.memory = memory;
        MEMORY_BASIC_INFORMATION region = {};
        VirtualQuery(reinterpret_cast<const void*>(start), &region,
                     sizeof(region));
        runtime_trace(
            "native_gpu.guest_import start=0x%016llX end=0x%016llX "
            "MB=%llu type=%u imports=%zu region=0x%lX protect=0x%lX "
            "allocation=0x%016llX\n",
            static_cast<unsigned long long>(start),
            static_cast<unsigned long long>(end),
            static_cast<unsigned long long>((end - start) >> 20),
            type,
            guest_imports.size() + 1,
            static_cast<unsigned long>(region.Type),
            static_cast<unsigned long>(region.Protect),
            static_cast<unsigned long long>(
                reinterpret_cast<std::uint64_t>(region.AllocationBase)));
        return imported;
    }

    // The imported buffer holding [address, address + size), and where in
    // it the range starts; null when there is none and it has to be copied.
    VkBuffer find_guest_import(std::uint64_t address,
                               std::uint64_t size,
                               VkDeviceSize& offset) {
        auto found = guest_imports.upper_bound(address);
        if (found != guest_imports.begin()) {
            --found;
            if (address >= found->first && address < found->second.end) {
                if (address + size > found->second.end ||
                    found->second.buffer == VK_NULL_HANDLE) {
                    ++guest_import_outside;
                    return VK_NULL_HANDLE;
                }
                offset = address - found->first;
                if (offset % storage_offset_alignment != 0) {
                    ++guest_import_misaligned;
                    return VK_NULL_HANDLE;
                }
                return found->second.buffer;
            }
        }
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        const auto run_ok = guest_committed_run(address, size, start, end);
        static std::uint32_t refusals_shown = 0;
        if (!run_ok && refusals_shown < 24) {
            ++refusals_shown;
            runtime_trace(
                "native_gpu.guest_import_refused address=0x%016llX "
                "size=%llu run=0x%016llX-0x%016llX\n",
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end));
        }
        if (!run_ok) {
            // Nothing importable here; one page remembered as such, so
            // the next draw does not ask Windows again.
            const auto page = address & ~0xFFFull;
            GuestMemoryImport none;
            none.end = page + 0x1000;
            guest_imports.emplace(page, none);
            ++guest_import_outside;
            return VK_NULL_HANDLE;
        }
        // Whatever of the new run an earlier import already covers stays
        // with that one; the maps must not overlap.
        const auto next = guest_imports.lower_bound(start);
        if (next != guest_imports.end() && next->first < end) {
            end = next->first;
        }
        if (address + size > end) {
            ++guest_import_outside;
            return VK_NULL_HANDLE;
        }
        const auto inserted =
            guest_imports.emplace(start, create_guest_import(start, end))
                .first;
        if (inserted->second.buffer == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }
        offset = address - start;
        if (offset % storage_offset_alignment != 0) {
            ++guest_import_misaligned;
            return VK_NULL_HANDLE;
        }
        return inserted->second.buffer;
    }

    // One read per frame for each range the frame's draws only read.
    //
    // The draws of a frame are prepared together at the flip, and each
    // pipeline copied its buffers into buffers of its own: the scene after
    // the intro read and filled 2.3GB a frame this way, the same ranges
    // over and over, 420ms of a 700ms frame. Here the first draw to name a
    // range reads it into one host-visible arena and every later draw of
    // the frame binds the same bytes at their offset in it.
    //
    // The frame is submitted and waited for before the next flip begins,
    // so the arena is free again by the time it is refilled. Ranges a draw
    // snapshotted for itself keep the per-pipeline path, since those bytes
    // belong to the draw. PS5GPU_NATIVE_FRAME_ARENA=0 turns it off, and
    // PS5GPU_NATIVE_FRAME_ARENA_VERIFY=1 re-reads every reuse and counts the
    // ones whose bytes had changed.
    VkBuffer frame_arena_buffer = VK_NULL_HANDLE;
    VkDeviceMemory frame_arena_memory = VK_NULL_HANDLE;
    std::uint8_t* frame_arena_mapped = nullptr;
    VkDeviceSize frame_arena_size = 0;
    VkDeviceSize frame_arena_used = 0;
    bool frame_arena_pass = false;
    bool frame_arena_failed = false;
    std::map<std::pair<std::uint64_t, std::uint64_t>, VkDeviceSize>
        frame_arena_ranges;
    std::uint64_t frame_arena_hits = 0;
    std::uint64_t frame_arena_hit_bytes = 0;
    std::uint64_t frame_arena_reads = 0;
    std::uint64_t frame_arena_read_bytes = 0;
    std::uint64_t frame_arena_full = 0;
    std::uint64_t frame_arena_private = 0;
    std::uint64_t frame_arena_changed = 0;
    std::uint64_t frame_arena_changed_bytes = 0;
    std::vector<std::uint8_t> frame_arena_verify_scratch;

    static bool frame_arena_enabled() {
        static const auto enabled = [] {
            const auto* value = std::getenv("PS5GPU_NATIVE_FRAME_ARENA");
            return value == nullptr || value[0] != '0';
        }();
        return enabled;
    }

    static bool frame_arena_verify() {
        static const auto enabled = [] {
            const auto* value =
                std::getenv("PS5GPU_NATIVE_FRAME_ARENA_VERIFY");
            return value != nullptr && value[0] == '1';
        }();
        return enabled;
    }

    bool ensure_frame_arena() {
        if (frame_arena_buffer != VK_NULL_HANDLE) {
            return true;
        }
        if (frame_arena_failed) {
            return false;
        }
        frame_arena_failed = true;
        // A scene frame uses under 90MB of it; a range that does not fit
        // falls back to the per-state path rather than failing.
        VkDeviceSize megabytes = 256;
        if (const auto* value =
                std::getenv("PS5GPU_NATIVE_FRAME_ARENA_MB")) {
            megabytes = std::max<VkDeviceSize>(
                16, std::strtoull(value, nullptr, 10));
        }
        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = megabytes << 20;
        buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        if (create_buffer(device, &buffer_info, nullptr, &buffer) !=
            VK_SUCCESS) {
            runtime_trace("native_gpu.frame_arena_failed step=buffer\n");
            return false;
        }
        VkMemoryRequirements requirements = {};
        get_buffer_memory_requirements(device, buffer, &requirements);
        const auto type = find_memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo allocate_info = {};
        allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex = type;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        auto result = type == UINT32_MAX
            ? VK_ERROR_FEATURE_NOT_PRESENT
            : allocate_memory(device, &allocate_info, nullptr, &memory);
        if (result == VK_SUCCESS) {
            result = bind_buffer_memory(device, buffer, memory, 0);
        }
        if (result == VK_SUCCESS) {
            result = map_memory(
                device, memory, 0, requirements.size, 0, &mapped);
        }
        if (result != VK_SUCCESS || mapped == nullptr) {
            if (memory != VK_NULL_HANDLE) {
                free_memory(device, memory, nullptr);
            }
            destroy_buffer(device, buffer, nullptr);
            runtime_trace(
                "native_gpu.frame_arena_failed step=memory type=%u "
                "result=%d\n",
                type,
                static_cast<int>(result));
            return false;
        }
        frame_arena_buffer = buffer;
        frame_arena_memory = memory;
        frame_arena_mapped = static_cast<std::uint8_t*>(mapped);
        frame_arena_size = buffer_info.size;
        frame_arena_failed = false;
        runtime_trace(
            "native_gpu.frame_arena MB=%llu type=%u\n",
            static_cast<unsigned long long>(megabytes),
            type);
        return true;
    }

    // Graphics states were never let go. Compute states retire once idle,
    // but a graphics one kept its pipeline, its descriptor pool and a host
    // buffer per descriptor for the life of the process - and after the
    // intro the title makes a few hundred new ones a frame, because the
    // addresses in their user data change every frame. Twelve gigabytes of
    // host buffers in three hundred frames, then allocations failed.
    //
    // A state not drawn for this many frames goes, once the GPU is idle at
    // the start of a frame. Its shaders were consumed building it, so one
    // that comes back after that is not rebuilt; the window is wide for
    // that reason. PS5GPU_NATIVE_GRAPHICS_RETIRE_KEEP sets it, 0 keeps all.
    std::uint64_t graphics_frame_counter = 0;

    void retire_idle_graphics_pipelines() {
        static const std::uint64_t keep = [] {
            const auto* value =
                std::getenv("PS5GPU_NATIVE_GRAPHICS_RETIRE_KEEP");
            return value == nullptr
                ? std::uint64_t{60}
                : std::strtoull(value, nullptr, 0);
        }();
        if (keep == 0 || graphics_frame_counter <= keep) {
            return;
        }
        const auto oldest_kept = graphics_frame_counter - keep;
        for (auto entry = guest_descriptor_pipelines.begin();
             entry != guest_descriptor_pipelines.end();) {
            if (entry->second.last_used_frame >= oldest_kept) {
                ++entry;
                continue;
            }
            destroy_guest_descriptor_pipeline(entry->second);
            entry = guest_descriptor_pipelines.erase(entry);
            g_retired_graphics_states.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    void begin_frame_arena_pass() {
        frame_arena_used = 0;
        frame_arena_ranges.clear();
        frame_arena_pass = frame_arena_enabled() && ensure_frame_arena();
    }

    void end_frame_arena_pass() {
        frame_arena_pass = false;
        static std::uint64_t passes = 0;
        if (++passes % 50 == 0) {
            runtime_trace(
                "native_gpu.frame_arena_use passes=%llu hits=%llu "
                "hit_MB=%llu reads=%llu read_MB=%llu full=%llu "
                "changed=%llu changed_MB=%llu last_used_MB=%llu "
                "lazy_buffers=%llu graphics_live=%zu graphics_retired=%llu "
                "private=%llu captures_retired=%llu live_dropped=%llu "
                "pipelines_shared=%llu pipelines_built=%llu "
                "pipelines_cached=%zu\n",
                static_cast<unsigned long long>(passes),
                static_cast<unsigned long long>(frame_arena_hits),
                static_cast<unsigned long long>(frame_arena_hit_bytes >> 20),
                static_cast<unsigned long long>(frame_arena_reads),
                static_cast<unsigned long long>(
                    frame_arena_read_bytes >> 20),
                static_cast<unsigned long long>(frame_arena_full),
                static_cast<unsigned long long>(frame_arena_changed),
                static_cast<unsigned long long>(
                    frame_arena_changed_bytes >> 20),
                static_cast<unsigned long long>(frame_arena_used >> 20),
                static_cast<unsigned long long>(g_lazy_buffer_creates),
                guest_descriptor_pipelines.size(),
                static_cast<unsigned long long>(
                    g_retired_graphics_states.load()),
                static_cast<unsigned long long>(frame_arena_private),
                static_cast<unsigned long long>(
                    g_retired_graphics_captures.load()),
                static_cast<unsigned long long>(
                    g_dropped_live_graphics_states.load()),
                static_cast<unsigned long long>(shared_pipeline_hits),
                static_cast<unsigned long long>(shared_pipeline_builds),
                shared_graphics_pipelines.size());
        }
    }

    // Binds `buffer` to the frame's copy of its range, reading the range
    // in if this is the frame's first draw to want it. False leaves the
    // buffer to the ordinary path.
    bool bind_from_frame_arena(
        GuestBufferResource& buffer,
        std::uint32_t state_id) {
        if (!frame_arena_pass ||
            buffer.guest_address < 0x10000 ||
            buffer.size == 0 ||
            (buffer.flags & PS5GPU_RESOURCE_BUFFER_WRITABLE) != 0) {
            return false;
        }
        const auto size = static_cast<std::uint64_t>(buffer.size);
        // Bytes a draw snapshotted for itself are that draw's: a slot of
        // their own, shared with nothing. In the scene after the intro most
        // per-object constants are, and sending them down the per-state
        // path made a dozen host buffers for every new state, a thousand a
        // frame.
        if (!buffer.snapshot.empty() ||
            draw_snapshot_exists(state_id, buffer.guest_address)) {
            const auto offset =
                (frame_arena_used + 255) & ~VkDeviceSize{255};
            if (offset + size > frame_arena_size) {
                ++frame_arena_full;
                return false;
            }
            auto* destination = frame_arena_mapped + offset;
            if (!read_draw_buffer(
                    state_id, buffer.guest_address, destination, size)) {
                const auto kept = std::min<std::uint64_t>(
                    size, buffer.snapshot.size());
                if (kept != 0) {
                    std::memcpy(destination, buffer.snapshot.data(), kept);
                }
                std::memset(destination + kept, 0, size - kept);
            }
            frame_arena_used = offset + size;
            buffer.import_buffer = frame_arena_buffer;
            buffer.import_offset = offset;
            ++frame_arena_private;
            return true;
        }
        const auto key = std::make_pair(buffer.guest_address, size);
        const auto found = frame_arena_ranges.find(key);
        if (found != frame_arena_ranges.end()) {
            if (frame_arena_verify()) {
                frame_arena_verify_scratch.resize(size);
                if (read_draw_buffer(
                        state_id,
                        buffer.guest_address,
                        frame_arena_verify_scratch.data(),
                        size) &&
                    std::memcmp(
                        frame_arena_verify_scratch.data(),
                        frame_arena_mapped + found->second,
                        size) != 0) {
                    ++frame_arena_changed;
                    frame_arena_changed_bytes += size;
                }
            }
            buffer.import_buffer = frame_arena_buffer;
            buffer.import_offset = found->second;
            ++frame_arena_hits;
            frame_arena_hit_bytes += size;
            return true;
        }
        const auto offset = (frame_arena_used + 255) & ~VkDeviceSize{255};
        if (offset + size > frame_arena_size) {
            ++frame_arena_full;
            return false;
        }
        auto* destination = frame_arena_mapped + offset;
        const auto read_started = worker_phase_counter();
        if (!read_draw_buffer(
                state_id, buffer.guest_address, destination, size)) {
            // What the ordinary path gives a range it cannot read.
            std::memset(destination, 0, size);
        }
        g_buffer_read_ticks += worker_phase_counter() - read_started;
        frame_arena_used = offset + size;
        frame_arena_ranges.emplace(key, offset);
        buffer.import_buffer = frame_arena_buffer;
        buffer.import_offset = offset;
        ++frame_arena_reads;
        frame_arena_read_bytes += size;
        return true;
    }

    void destroy_frame_arena() {
        if (frame_arena_memory != VK_NULL_HANDLE) {
            unmap_memory(device, frame_arena_memory);
            free_memory(device, frame_arena_memory, nullptr);
        }
        if (frame_arena_buffer != VK_NULL_HANDLE) {
            destroy_buffer(device, frame_arena_buffer, nullptr);
        }
        frame_arena_memory = VK_NULL_HANDLE;
        frame_arena_buffer = VK_NULL_HANDLE;
        frame_arena_mapped = nullptr;
    }

    // Where the GPU was when it was lost. Every draw and dispatch leaves a
    // marker in its command buffer - what it is, its state, its index - and
    // after a device loss the driver says which markers the queue reached.
    // The scene after the intro loses the device a few hundred frames in
    // (nvlddmkm event 153), robust access or not, which reads as a shader
    // that never finishes. PS5GPU_NATIVE_CHECKPOINTS=1.
    bool checkpoints_enabled = false;
    bool checkpoints_reported = false;
    PFN_vkCmdSetCheckpointNV cmd_set_checkpoint = nullptr;
    PFN_vkGetQueueCheckpointDataNV get_queue_checkpoint_data = nullptr;

    static bool checkpoints_requested() {
        static const auto requested = [] {
            const auto* value = std::getenv("PS5GPU_NATIVE_CHECKPOINTS");
            return value != nullptr && value[0] == '1';
        }();
        return requested;
    }

    void checkpoint(VkCommandBuffer command_buffer,
                    std::uint32_t kind,
                    std::uint32_t state_id,
                    std::uint32_t index) {
        if (!checkpoints_enabled) {
            return;
        }
        const auto marker = (static_cast<std::uint64_t>(kind) << 60) |
            (static_cast<std::uint64_t>(state_id & 0x0FFFFFFFu) << 32) |
            index;
        cmd_set_checkpoint(command_buffer,
                           reinterpret_cast<const void*>(marker));
    }

    void report_checkpoints(const char* where) {
        if (!checkpoints_enabled || checkpoints_reported) {
            return;
        }
        checkpoints_reported = true;
        std::uint32_t count = 0;
        get_queue_checkpoint_data(queue, &count, nullptr);
        std::vector<VkCheckpointDataNV> data(count);
        for (auto& entry : data) {
            entry.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
            entry.pNext = nullptr;
        }
        get_queue_checkpoint_data(queue, &count, data.data());
        runtime_trace("native_gpu.checkpoints where=%s count=%u\n", where,
                      count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto marker = reinterpret_cast<std::uint64_t>(
                data[index].pCheckpointMarker);
            runtime_trace(
                "native_gpu.checkpoint stage=0x%X kind=%s state=%u "
                "index=%u\n",
                static_cast<unsigned>(data[index].stage),
                (marker >> 60) == 1 ? "draw"
                : (marker >> 60) == 2 ? "compute" : "other",
                static_cast<unsigned>((marker >> 32) & 0x0FFFFFFFu),
                static_cast<unsigned>(marker & 0xFFFFFFFFu));
        }
        runtime_trace_flush();
    }

    void destroy_guest_imports() {
        for (auto& [start, imported] : guest_imports) {
            (void)start;
            if (imported.buffer != VK_NULL_HANDLE) {
                destroy_buffer(device, imported.buffer, nullptr);
            }
            if (imported.memory != VK_NULL_HANDLE) {
                free_memory(device, imported.memory, nullptr);
            }
        }
        guest_imports.clear();
    }
    static constexpr VkDeviceSize kSharedDeviceBufferMinimumBytes =
        1024 * 1024;
    static constexpr VkDeviceSize kSharedDeviceBudgetBytes =
        512ull * 1024 * 1024;

    static bool shared_device_buffers_enabled() {
        static const auto enabled =
            std::getenv("PS5GPU_NATIVE_NO_SHARED_BUFFERS") == nullptr;
        return enabled;
    }

    // Reserved key for the shared 1x1 surface bound to image slots the guest
    // never populated. Far above any real guest allocation, so it can never
    // alias a genuine resource in the render-surface cache.
    static constexpr std::uint64_t kPlaceholderImageAddress =
        0xFFFFFFFFFFFFF000ULL;

    struct GuestImageResource {
        std::uint32_t binding = 0;
        std::uint32_t flags = 0;
        VkShaderStageFlags stages = 0;
        std::uint64_t guest_address = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t unified_format = 0;
        std::uint32_t tile_mode = 0;
        std::uint32_t type = 0;
        std::uint32_t pitch = 0;
        // Slices of a volume (type 10), from the descriptor's fifth word -
        // the same field a two dimensional image keeps its pitch in.
        std::uint32_t depth = 1;
        std::array<std::uint32_t, 8> resource_descriptor = {};
        // The guest's S#. Four dwords of filter, address mode, LOD range and
        // border colour that the runtime used to throw away, sampling
        // everything through one hardcoded LINEAR/CLAMP_TO_EDGE sampler. Of
        // 165 image bindings in this title, 109 ask for something else - most
        // of them CLAMP_BORDER, which is exactly the difference between an
        // out-of-range coordinate reading the edge and reading the border.
        std::array<std::uint32_t, 4> sampler_descriptor = {};
        RenderSurface* surface = nullptr;
        VkBuffer upload_buffer = VK_NULL_HANDLE;
        VkDeviceMemory upload_memory = VK_NULL_HANDLE;
        VkDeviceSize upload_size = 0;
        std::uint64_t upload_hash = 0;
        bool upload_ready = false;
        bool upload_pending = false;
        bool upload_unavailable_reported = false;
        bool availability_probed = false;
        bool placeholder = false;
        // Set when this binding samples the surface the draw also renders
        // into. Vulkan forbids that, so the binding reads a copy taken just
        // before the pass opens instead of the attachment itself.
        RenderSurface* read_copy = nullptr;
    };
    struct GuestDescriptorPipeline {
        // The ring slot and submission its last dispatch went out on. Its
        // buffers are rewritten for the next dispatch, and with sixteen
        // slots in flight the previous one may not have read them yet: the
        // intro video flickered black while that one read the next
        // dispatch's bytes.
        std::size_t in_flight_slot = ~std::size_t{0};
        std::uint64_t in_flight_serial = 0;
        // The descriptor carrying the dispatch's runtime scalar block, and
        // where in it the shader's per-descriptor write marks begin. -1
        // where the state has no such block.
        int scalar_block_index = -1;
        std::uint32_t write_mask_base = 0;
        std::uint32_t write_mask_count = 0;
        VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        // Compute states own this handle outright; graphics states mirror the
        // RGBA8 variant here so the readiness checks stay as they were.
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipeline strip_pipeline = VK_NULL_HANDLE;
        GuestPipelineVariants graphics_pipeline;
        GuestPipelineVariants graphics_strip_pipeline;
        bool graphics = false;
        // Whether the three program handles above are borrowed from
        // shared_compute_programs rather than owned here. A borrowed
        // handle must not be destroyed with the state that used it.
        bool shares_program = false;
        // The same for a graphics state's two layouts, which come from
        // shared_graphics_layouts.
        bool shares_layout = false;
        std::vector<GuestBufferResource> buffers;
        std::vector<GuestImageResource> images;
        bool frame_resources_ready = false;
        // Which target the resources were resolved against. Self-sampling is a
        // property of the draw, not of the state: one pipeline can render into
        // several surfaces across a frame, and a draw that samples the one it
        // writes needs its copy set up for that surface specifically.
        const RenderSurface* frame_resources_target = nullptr;
        // The dispatch this state last served. The title registers a compute
        // state per dispatch and never returns to one, so without this the
        // buffers of every dispatch a run ever made stay allocated: measured
        // at 7076 device-local and 7269 host allocations, 7.4GB, in
        // sixty-four flips, with vk_free at zero.
        std::uint64_t last_used_dispatch = 0;
        // The frame a graphics state was last drawn in, or built in.
        std::uint64_t last_used_frame = 0;
    };
    std::uint64_t guest_dispatch_counter = 0;
    std::set<std::uint32_t> retired_compute_states;
    std::map<std::uint32_t, GuestDescriptorPipeline>
        guest_descriptor_pipelines;
    std::map<std::uint32_t, GuestDescriptorPipeline>
        guest_compute_pipelines;
    // A compute pipeline is a function of the SPIR-V and the descriptor
    // layout it is built against, and of nothing else - not of which guest
    // buffers happen to be bound. The states are not: the title registers
    // a new one per dispatch, 1778 of them across twenty-one shaders in one
    // run, and building a pipeline for each cost 34.7ms a dispatch and two
    // thirds of the worker. Keyed on what the pipeline actually depends on,
    // the states that differ only in their bindings share one.
    struct SharedComputeProgram {
        VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };
    std::map<std::uint64_t, SharedComputeProgram> shared_compute_programs;
    // A graphics state's descriptor set layout and pipeline layout, by the
    // signature of its bindings. The scene builds forty states a frame and
    // most declare one of a few layouts; making both for each was a good
    // part of what a state cost to build.
    std::map<std::uint64_t, std::pair<VkDescriptorSetLayout, VkPipelineLayout>>
        shared_graphics_layouts;
    // Scratch storage for the guest bytes read behind each buffer binding,
    // kept from dispatch to dispatch. A vector allocated per dispatch means
    // the kernel faults in and zeroes as much as sixteen megabytes that
    // read_current_process_memory then overwrites completely.
    std::vector<std::vector<std::uint8_t>> guest_payload_arena;
    // Scratch for one guest image upload at a time. Every one of these used
    // to be a fresh allocation the size of the image - eight megabytes for a
    // 1080p texture, three of them per upload, each one page-faulted in on
    // first touch and handed straight back to the allocator. Uploads happen
    // only on the worker thread, so one set of buffers that grows to the
    // largest image seen and never shrinks serves all of them.
    std::vector<std::uint8_t> upload_source_bytes;
    std::vector<std::uint8_t> upload_linear_bytes;
    std::vector<std::uint8_t> upload_rgba_bytes;
    // A shader state is identified by a hash over its registers, and something
    // in those registers moves every frame, so the same shaders are handed to
    // us under a fresh state id each time - 109 ids over three frames for 19
    // actual shader pairs. Pipelines are loaded from files named after the id
    // that was current when they were dumped, so from frame two onward every
    // draw missed its pipeline and was dropped: only the first frame ever
    // rendered. The shader addresses do not move, so bind pipelines to those
    // and let the renumbering happen.
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint32_t>
        pipeline_state_by_shaders;
    // User data registers per shader state, copied in at registration. A
    // buffer descriptor lives here as a four dword V#, and the guest moves it
    // every frame.
    struct GuestUserData {
        std::vector<std::uint32_t> vertex;
        std::vector<std::uint32_t> pixel;
    };
    std::map<std::uint32_t, GuestUserData> guest_user_data;
    // One VkSampler per distinct guest S#. Field layout per the RDNA sampler
    // resource; five distinct descriptors appear across this title's shaders.
    std::map<std::array<std::uint32_t, 4>, VkSampler> guest_samplers;

    static VkSamplerAddressMode guest_address_mode(std::uint32_t clamp) {
        switch (clamp) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case 4:
        case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 5:
        case 7: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        }
    }

    VkSampler ensure_guest_sampler(
        const std::array<std::uint32_t, 4>& descriptor) {
        // An unbound slot decodes to all zeroes, which would ask for REPEAT and
        // NEAREST. Keep the shared sampler for those rather than inventing a
        // state the guest never asked for.
        if (descriptor[0] == 0 && descriptor[1] == 0 &&
            descriptor[2] == 0 && descriptor[3] == 0) {
            return sampled_sampler;
        }
        if (const auto known = guest_samplers.find(descriptor);
            known != guest_samplers.end()) {
            return known->second;
        }
        const auto filter = [](std::uint32_t value) {
            return value == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        };
        const auto anisotropy = (descriptor[0] >> 9) & 0x7u;
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = filter((descriptor[2] >> 20) & 0x3u);
        info.minFilter = filter((descriptor[2] >> 22) & 0x3u);
        info.mipmapMode = ((descriptor[2] >> 26) & 0x3u) >= 2
            ? VK_SAMPLER_MIPMAP_MODE_LINEAR
            : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = guest_address_mode(descriptor[0] & 0x7u);
        info.addressModeV =
            guest_address_mode((descriptor[0] >> 3) & 0x7u);
        info.addressModeW =
            guest_address_mode((descriptor[0] >> 6) & 0x7u);
        // Both LOD fields are 4.8 fixed point.
        info.minLod =
            static_cast<float>(descriptor[1] & 0xFFFu) / 256.0f;
        info.maxLod =
            static_cast<float>((descriptor[1] >> 12) & 0xFFFu) / 256.0f;
        // Anisotropy is left off: the device was not created with the
        // samplerAnisotropy feature, so asking for it here would be invalid.
        // Filtering quality is lower than the guest asked for; the sampled
        // texel is still the right one, which is the trade this project
        // allows. The ratio is traced so the gap is visible.
        (void)anisotropy;
        switch ((descriptor[3] >> 30) & 0x3u) {
        case 1:
            info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
            break;
        case 2:
            info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            break;
        default:
            info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            break;
        }
        VkSampler sampler = VK_NULL_HANDLE;
        if (create_sampler(device, &info, nullptr, &sampler) !=
            VK_SUCCESS) {
            sampler = sampled_sampler;
        }
        runtime_trace(
            "native_gpu.guest_sampler descriptor=%08X,%08X,%08X,%08X "
            "mag=%d min=%d addressU=%d minLod=%.2f maxLod=%.2f "
            "aniso=%u border=%d\n",
            descriptor[0], descriptor[1], descriptor[2], descriptor[3],
            static_cast<int>(info.magFilter),
            static_cast<int>(info.minFilter),
            static_cast<int>(info.addressModeU),
            static_cast<double>(info.minLod),
            static_cast<double>(info.maxLod),
            anisotropy,
            static_cast<int>(info.borderColor));
        guest_samplers.emplace(descriptor, sampler);
        return sampler;
    }

    void set_guest_user_data(
        std::uint32_t state,
        bool pixel_stage,
        const std::uint32_t* values,
        std::size_t count) {
        if (values == nullptr || count == 0) {
            return;
        }
        auto& entry = guest_user_data[state];
        auto& target = pixel_stage ? entry.pixel : entry.vertex;
        target.assign(values, values + count);
    }

    // A GCN/RDNA buffer resource: 48 bit base in the first two dwords, record
    // count in the third. Returns false when the slot plainly does not hold
    // one, in which case the caller keeps whatever the manifest said.
    bool decode_guest_buffer_descriptor(
        std::uint32_t state,
        VkShaderStageFlags stages,
        std::uint32_t scalar_address,
        std::uint64_t& address,
        std::uint64_t& size) const {
        const auto user_data = guest_user_data.find(state);
        if (user_data == guest_user_data.end()) {
            return false;
        }
        const auto& values =
            (stages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0
                ? user_data->second.pixel
                : user_data->second.vertex;
        if (scalar_address + 3 >= values.size()) {
            return false;
        }
        const auto base =
            static_cast<std::uint64_t>(values[scalar_address]) |
            (static_cast<std::uint64_t>(
                 values[scalar_address + 1] & 0xFFFFu)
             << 32);
        const auto records = values[scalar_address + 2];
        if (base < 0x10000 || base > 0x0000FFFFFFFFFFFFULL ||
            records == 0 || records > (256u * 1024u * 1024u)) {
            return false;
        }
        address = base;
        size = records;
        return true;
    }

    std::uint32_t resolve_pipeline_state(const GpuIrDraw& draw) {
        const auto known_here =
            guest_descriptor_pipelines.count(draw.shader_state_id) != 0 ||
            resource_free_guest_pipelines.count(draw.shader_state_id) != 0;
        if (draw.es_address == 0 || draw.ps_address == 0) {
            return draw.shader_state_id;
        }
        const std::pair<std::uint64_t, std::uint64_t> key = {
            draw.es_address,
            draw.ps_address,
        };
        if (known_here) {
            pipeline_state_by_shaders[key] = draw.shader_state_id;
            return draw.shader_state_id;
        }
        const auto known = pipeline_state_by_shaders.find(key);
        if (known == pipeline_state_by_shaders.end()) {
            return draw.shader_state_id;
        }
        static std::set<std::uint32_t> reported;
        if (reported.insert(draw.shader_state_id).second) {
            runtime_trace(
                "native_gpu.pipeline_state_remapped state=%u to=%u "
                "es=0x%016llX ps=0x%016llX\n",
                draw.shader_state_id,
                known->second,
                static_cast<unsigned long long>(draw.es_address),
                static_cast<unsigned long long>(draw.ps_address));
        }
        return known->second;
    }

    template <typename T>
    T load_instance(const char* name) const {
        return reinterpret_cast<T>(get_instance_proc(instance, name));
    }

    template <typename T>
    T load_device(const char* name) const {
        return reinterpret_cast<T>(get_device_proc(device, name));
    }

    bool initialize_pipeline_cache() {
        pipeline_cache_path.clear();
        saved_pipeline_cache_hash = 0;
        // PS5GPU_NATIVE_NO_PIPELINE_CACHE=1: none at all, to see whether
        // the driver's cache is what grows with every new state.
        if (const auto* off = std::getenv("PS5GPU_NATIVE_NO_PIPELINE_CACHE");
            off != nullptr && off[0] == '1') {
            pipeline_cache = VK_NULL_HANDLE;
            return true;
        }
        if (const auto* value =
                std::getenv("PS5GPU_NATIVE_PIPELINE_CACHE_PATH");
            value != nullptr && value[0] != '\0') {
            pipeline_cache_path = value;
        }

        constexpr std::uint64_t kMaximumPipelineCacheBytes =
            64ULL * 1024ULL * 1024ULL;
        std::vector<std::uint8_t> initial_data;
        if (!pipeline_cache_path.empty()) {
            std::ifstream input(
                pipeline_cache_path,
                std::ios::binary | std::ios::ate);
            if (input) {
                const auto length = input.tellg();
                if (length > 0 &&
                    static_cast<std::uint64_t>(length) <=
                        kMaximumPipelineCacheBytes) {
                    initial_data.resize(
                        static_cast<std::size_t>(length));
                    input.seekg(0);
                    input.read(
                        reinterpret_cast<char*>(
                            initial_data.data()),
                        static_cast<std::streamsize>(
                            initial_data.size()));
                    if (!input) {
                        initial_data.clear();
                    }
                }
            }
        }

        VkPipelineCacheCreateInfo create_info = {};
        create_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        create_info.initialDataSize = initial_data.size();
        create_info.pInitialData = initial_data.empty()
            ? nullptr
            : initial_data.data();
        auto result = create_pipeline_cache(
            device,
            &create_info,
            nullptr,
            &pipeline_cache);
        if (result != VK_SUCCESS && !initial_data.empty()) {
            runtime_trace(
                "native_gpu.pipeline_cache_rejected path=%s "
                "bytes=%llu result=%d retry=empty\n",
                pipeline_cache_path.string().c_str(),
                static_cast<unsigned long long>(
                    initial_data.size()),
                static_cast<int>(result));
            pipeline_cache = VK_NULL_HANDLE;
            create_info.initialDataSize = 0;
            create_info.pInitialData = nullptr;
            result = create_pipeline_cache(
                device,
                &create_info,
                nullptr,
                &pipeline_cache);
            initial_data.clear();
        }
        if (result != VK_SUCCESS ||
            pipeline_cache == VK_NULL_HANDLE) {
            runtime_trace(
                "native_gpu.pipeline_cache_failed result=%d\n",
                static_cast<int>(result));
            pipeline_cache = VK_NULL_HANDLE;
            return false;
        }
        if (!initial_data.empty()) {
            saved_pipeline_cache_hash = hash_bytes(
                initial_data.data(),
                initial_data.size());
        }
        runtime_trace(
            "native_gpu.pipeline_cache_ready path=%s bytes=%llu\n",
            pipeline_cache_path.empty()
                ? "(memory-only)"
                : pipeline_cache_path.string().c_str(),
            static_cast<unsigned long long>(initial_data.size()));
        return true;
    }

    bool save_pipeline_cache() {
        if (pipeline_cache == VK_NULL_HANDLE ||
            pipeline_cache_path.empty() ||
            get_pipeline_cache_data == nullptr) {
            return true;
        }

        std::size_t byte_count = 0;
        auto result = get_pipeline_cache_data(
            device,
            pipeline_cache,
            &byte_count,
            nullptr);
        if (result != VK_SUCCESS || byte_count == 0) {
            runtime_trace(
                "native_gpu.pipeline_cache_query_failed result=%d "
                "bytes=%llu\n",
                static_cast<int>(result),
                static_cast<unsigned long long>(byte_count));
            return false;
        }
        std::vector<std::uint8_t> bytes(byte_count);
        result = get_pipeline_cache_data(
            device,
            pipeline_cache,
            &byte_count,
            bytes.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            runtime_trace(
                "native_gpu.pipeline_cache_read_failed result=%d\n",
                static_cast<int>(result));
            return false;
        }
        bytes.resize(byte_count);
        const auto cache_hash =
            hash_bytes(bytes.data(), bytes.size());
        if (cache_hash == saved_pipeline_cache_hash) {
            return true;
        }

        std::error_code directory_error;
        if (!pipeline_cache_path.parent_path().empty()) {
            std::filesystem::create_directories(
                pipeline_cache_path.parent_path(),
                directory_error);
        }
        if (directory_error) {
            runtime_trace(
                "native_gpu.pipeline_cache_directory_failed "
                "path=%s error=%d\n",
                pipeline_cache_path.string().c_str(),
                directory_error.value());
            return false;
        }

        auto temporary_path = pipeline_cache_path;
        temporary_path += L".tmp";
        {
            std::ofstream output(
                temporary_path,
                std::ios::binary | std::ios::trunc);
            output.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (!output) {
                runtime_trace(
                    "native_gpu.pipeline_cache_write_failed "
                    "path=%s\n",
                    temporary_path.string().c_str());
                return false;
            }
        }
        if (!MoveFileExW(
                temporary_path.c_str(),
                pipeline_cache_path.c_str(),
                MOVEFILE_REPLACE_EXISTING |
                    MOVEFILE_WRITE_THROUGH)) {
            const auto error = GetLastError();
            std::error_code remove_error;
            std::filesystem::remove(
                temporary_path,
                remove_error);
            runtime_trace(
                "native_gpu.pipeline_cache_replace_failed "
                "path=%s error=%lu\n",
                pipeline_cache_path.string().c_str(),
                static_cast<unsigned long>(error));
            return false;
        }
        saved_pipeline_cache_hash = cache_hash;
        runtime_trace(
            "native_gpu.pipeline_cache_saved path=%s "
            "bytes=%llu hash=0x%016llX\n",
            pipeline_cache_path.string().c_str(),
            static_cast<unsigned long long>(bytes.size()),
            static_cast<unsigned long long>(cache_hash));
        return true;
    }

    // Writing the cache is not cheap: it queries every byte out of the
    // driver, hashes them to see whether anything changed, and rewrites
    // the file. The cache had grown past thirty megabytes, and this ran
    // after every pipeline created - two hundred times in one run, about
    // seventy milliseconds each, three seconds of every frame - to
    // persist something that only pays off on the next launch. Throttle
    // it. destroy() still writes unconditionally, so a normal exit loses
    // nothing.
    static constexpr std::uint64_t kPipelineCacheSaveIntervalMs = 15000;
    std::uint64_t last_pipeline_cache_save_ms = 0;

    void save_pipeline_cache_throttled() {
        const auto now = GetTickCount64();
        if (last_pipeline_cache_save_ms != 0 &&
            now - last_pipeline_cache_save_ms <
                kPipelineCacheSaveIntervalMs) {
            return;
        }
        last_pipeline_cache_save_ms = now;
        (void)save_pipeline_cache();
    }


    bool initialize() {
        loader = LoadLibraryW(L"vulkan-1.dll");
        if (loader == nullptr) {
            runtime_trace("native_gpu.vulkan_unavailable error=%lu\n",
                          static_cast<unsigned long>(GetLastError()));
            return false;
        }
        get_instance_proc = load_module_export<PFN_vkGetInstanceProcAddr>(
            loader,
            "vkGetInstanceProcAddr");
        if (get_instance_proc == nullptr) {
            runtime_trace("native_gpu.vulkan_missing_get_proc\n");
            destroy();
            return false;
        }
        create_instance = reinterpret_cast<PFN_vkCreateInstance>(
            get_instance_proc(nullptr, "vkCreateInstance"));
        enumerate_instance_version =
            reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                get_instance_proc(nullptr, "vkEnumerateInstanceVersion"));
        if (create_instance == nullptr) {
            runtime_trace("native_gpu.vulkan_missing_instance_exports\n");
            destroy();
            return false;
        }

        VkApplicationInfo application_info = {};
        application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application_info.pApplicationName = "ps5recomp-native-gpu";
        application_info.applicationVersion = 1;
        application_info.pEngineName = "ps5recomp";
        application_info.engineVersion = 1;
        application_info.apiVersion = VK_API_VERSION_1_0;
        if (enumerate_instance_version != nullptr) {
            std::uint32_t version = VK_API_VERSION_1_0;
            if (enumerate_instance_version(&version) == VK_SUCCESS) {
                application_info.apiVersion = std::min(
                    version,
                    VK_MAKE_API_VERSION(0, 1, 2, 0));
            }
        }

        VkInstanceCreateInfo instance_info = {};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &application_info;
        if (ps5gpu::native_window_requested()) {
            instance_info.ppEnabledExtensionNames =
                ps5gpu::NativeWindowPresenter::instance_extensions(
                    instance_info.enabledExtensionCount);
        }
        auto result = create_instance(
            &instance_info,
            nullptr,
            &instance);
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.vulkan_create_instance_failed result=%d\n",
                static_cast<int>(result));
            destroy();
            return false;
        }

        destroy_instance = load_instance<PFN_vkDestroyInstance>(
            "vkDestroyInstance");
        enumerate_physical_devices =
            load_instance<PFN_vkEnumeratePhysicalDevices>(
                "vkEnumeratePhysicalDevices");
        get_physical_device_properties =
            load_instance<PFN_vkGetPhysicalDeviceProperties>(
                "vkGetPhysicalDeviceProperties");
        get_physical_device_memory_properties =
            load_instance<PFN_vkGetPhysicalDeviceMemoryProperties>(
                "vkGetPhysicalDeviceMemoryProperties");
        get_physical_device_queue_family_properties =
            load_instance<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                "vkGetPhysicalDeviceQueueFamilyProperties");
        get_physical_device_format_properties =
            load_instance<PFN_vkGetPhysicalDeviceFormatProperties>(
                "vkGetPhysicalDeviceFormatProperties");
        create_device = load_instance<PFN_vkCreateDevice>("vkCreateDevice");
        get_device_proc = load_instance<PFN_vkGetDeviceProcAddr>(
            "vkGetDeviceProcAddr");
        if (destroy_instance == nullptr ||
            enumerate_physical_devices == nullptr ||
            get_physical_device_properties == nullptr ||
            get_physical_device_memory_properties == nullptr ||
            get_physical_device_queue_family_properties == nullptr ||
            get_physical_device_format_properties == nullptr ||
            create_device == nullptr ||
            get_device_proc == nullptr) {
            runtime_trace("native_gpu.vulkan_missing_instance_functions\n");
            destroy();
            return false;
        }

        std::uint32_t physical_count = 0;
        result = enumerate_physical_devices(
            instance,
            &physical_count,
            nullptr);
        if (result != VK_SUCCESS || physical_count == 0) {
            runtime_trace(
                "native_gpu.vulkan_no_physical_devices result=%d count=%u\n",
                static_cast<int>(result),
                physical_count);
            destroy();
            return false;
        }
        std::vector<VkPhysicalDevice> physical_devices(physical_count);
        result = enumerate_physical_devices(
            instance,
            &physical_count,
            physical_devices.data());
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.vulkan_enumerate_devices_failed result=%d\n",
                static_cast<int>(result));
            destroy();
            return false;
        }

        for (const auto candidate : physical_devices) {
            std::uint32_t family_count = 0;
            get_physical_device_queue_family_properties(
                candidate,
                &family_count,
                nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            get_physical_device_queue_family_properties(
                candidate,
                &family_count,
                families.data());
            for (std::uint32_t index = 0; index < family_count; ++index) {
                if ((families[index].queueFlags &
                     (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                    (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                    physical_device = candidate;
                    queue_family = index;
                    break;
                }
            }
            if (physical_device != VK_NULL_HANDLE) {
                break;
            }
        }
        if (physical_device == VK_NULL_HANDLE) {
            runtime_trace("native_gpu.vulkan_no_graphics_queue\n");
            destroy();
            return false;
        }
        get_physical_device_memory_properties(
            physical_device,
            &memory_properties);

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        // The features the translated modules rely on. The device was made
        // with none enabled, so every module using a 64-bit integer, every
        // vertex or pixel stage with a storage buffer and every storage
        // image read without a format was one the driver owed nothing to -
        // the validation layer lists them all, and the intro video's colour
        // conversion wrote nothing at all. Enabled where the device has
        // them.
        VkPhysicalDeviceFeatures supported_features = {};
        if (const auto get_features =
                load_instance<PFN_vkGetPhysicalDeviceFeatures>(
                    "vkGetPhysicalDeviceFeatures");
            get_features != nullptr) {
            get_features(physical_device, &supported_features);
        }
        VkPhysicalDeviceFeatures enabled_features = {};
        enabled_features.shaderInt64 = supported_features.shaderInt64;
        enabled_features.shaderInt16 = supported_features.shaderInt16;
        enabled_features.vertexPipelineStoresAndAtomics =
            supported_features.vertexPipelineStoresAndAtomics;
        enabled_features.fragmentStoresAndAtomics =
            supported_features.fragmentStoresAndAtomics;
        enabled_features.shaderStorageImageReadWithoutFormat =
            supported_features.shaderStorageImageReadWithoutFormat;
        enabled_features.shaderStorageImageWriteWithoutFormat =
            supported_features.shaderStorageImageWriteWithoutFormat;
        enabled_features.independentBlend = supported_features.independentBlend;
        enabled_features.dualSrcBlend = supported_features.dualSrcBlend;
        // Block compressed textures are sampled as they are.
        enabled_features.textureCompressionBC =
            supported_features.textureCompressionBC;
        enabled_features.imageCubeArray = supported_features.imageCubeArray;
        enabled_features.shaderImageGatherExtended =
            supported_features.shaderImageGatherExtended;
        enabled_features.shaderClipDistance =
            supported_features.shaderClipDistance;
        // A binding of the guest's memory ends where the buffer does, not
        // at a padded allocation, so a read past it is a page fault on the
        // device rather than stale bytes. Robust access makes it read zero.
        // PS5GPU_NATIVE_ROBUST=1 turns it on by itself, to tell a device
        // lost from an out-of-bounds read.
        const auto* robust = std::getenv("PS5GPU_NATIVE_ROBUST");
        if (guest_import_requested() ||
            (robust != nullptr && robust[0] == '1')) {
            enabled_features.robustBufferAccess =
                supported_features.robustBufferAccess;
        }
        runtime_trace(
            "native_gpu.device_features int64=%u vertex_stores=%u "
            "fragment_stores=%u read_without_format=%u "
            "write_without_format=%u dual_source=%u\n",
            enabled_features.shaderInt64,
            enabled_features.vertexPipelineStoresAndAtomics,
            enabled_features.fragmentStoresAndAtomics,
            enabled_features.shaderStorageImageReadWithoutFormat,
            enabled_features.shaderStorageImageWriteWithoutFormat,
            enabled_features.dualSrcBlend);
        VkDeviceCreateInfo device_info = {};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.pEnabledFeatures = &enabled_features;
        std::vector<const char*> device_extension_names;
        if (ps5gpu::native_window_requested()) {
            std::uint32_t count = 0;
            const auto* names =
                ps5gpu::NativeWindowPresenter::device_extensions(count);
            device_extension_names.assign(names, names + count);
        }
        if (guest_import_requested()) {
            const auto enumerate_extensions =
                load_instance<PFN_vkEnumerateDeviceExtensionProperties>(
                    "vkEnumerateDeviceExtensionProperties");
            std::uint32_t count = 0;
            if (enumerate_extensions != nullptr &&
                enumerate_extensions(physical_device, nullptr, &count,
                                     nullptr) == VK_SUCCESS) {
                std::vector<VkExtensionProperties> available(count);
                enumerate_extensions(physical_device, nullptr, &count,
                                     available.data());
                for (const auto& extension : available) {
                    if (std::strcmp(
                            extension.extensionName,
                            VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) ==
                        0) {
                        guest_import_supported = true;
                    }
                }
            }
            if (guest_import_supported) {
                device_extension_names.push_back(
                    VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
                VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {};
                host.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
                VkPhysicalDeviceProperties2 properties2 = {};
                properties2.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                properties2.pNext = &host;
                if (const auto get_properties2 =
                        load_instance<PFN_vkGetPhysicalDeviceProperties2>(
                            "vkGetPhysicalDeviceProperties2");
                    get_properties2 != nullptr) {
                    get_properties2(physical_device, &properties2);
                    guest_import_alignment = std::max<VkDeviceSize>(
                        host.minImportedHostPointerAlignment, 4096);
                    storage_offset_alignment = std::max<VkDeviceSize>(
                        properties2.properties.limits
                            .minStorageBufferOffsetAlignment,
                        1);
                } else {
                    guest_import_supported = false;
                    device_extension_names.pop_back();
                }
            }
            runtime_trace(
                "native_gpu.guest_import_setup supported=%u "
                "alignment=%llu storage_offset_alignment=%llu\n",
                guest_import_supported ? 1u : 0u,
                static_cast<unsigned long long>(guest_import_alignment),
                static_cast<unsigned long long>(storage_offset_alignment));
        }
        if (checkpoints_requested()) {
            const auto enumerate_extensions =
                load_instance<PFN_vkEnumerateDeviceExtensionProperties>(
                    "vkEnumerateDeviceExtensionProperties");
            std::uint32_t count = 0;
            if (enumerate_extensions != nullptr &&
                enumerate_extensions(physical_device, nullptr, &count,
                                     nullptr) == VK_SUCCESS) {
                std::vector<VkExtensionProperties> available(count);
                enumerate_extensions(physical_device, nullptr, &count,
                                     available.data());
                for (const auto& extension : available) {
                    if (std::strcmp(
                            extension.extensionName,
                            VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) ==
                        0) {
                        checkpoints_enabled = true;
                    }
                }
            }
            if (checkpoints_enabled) {
                device_extension_names.push_back(
                    VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
            }
            runtime_trace("native_gpu.checkpoints_setup supported=%u\n",
                          checkpoints_enabled ? 1u : 0u);
        }
        device_info.enabledExtensionCount =
            static_cast<std::uint32_t>(device_extension_names.size());
        device_info.ppEnabledExtensionNames =
            device_extension_names.empty()
                ? nullptr
                : device_extension_names.data();
        result = create_device(
            physical_device,
            &device_info,
            nullptr,
            &device);
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.vulkan_create_device_failed result=%d\n",
                static_cast<int>(result));
            destroy();
            return false;
        }

        destroy_device = load_device<PFN_vkDestroyDevice>(
            "vkDestroyDevice");
        get_device_queue = load_device<PFN_vkGetDeviceQueue>(
            "vkGetDeviceQueue");
        create_command_pool = load_device<PFN_vkCreateCommandPool>(
            "vkCreateCommandPool");
        destroy_command_pool = load_device<PFN_vkDestroyCommandPool>(
            "vkDestroyCommandPool");
        allocate_command_buffers =
            load_device<PFN_vkAllocateCommandBuffers>(
                "vkAllocateCommandBuffers");
        create_fence = load_device<PFN_vkCreateFence>("vkCreateFence");
        destroy_fence = load_device<PFN_vkDestroyFence>("vkDestroyFence");
        wait_for_fences =
            load_device<PFN_vkWaitForFences>("vkWaitForFences");
        reset_fences = load_device<PFN_vkResetFences>("vkResetFences");
        free_command_buffers = load_device<PFN_vkFreeCommandBuffers>(
            "vkFreeCommandBuffers");
        reset_command_buffer = load_device<PFN_vkResetCommandBuffer>(
            "vkResetCommandBuffer");
        begin_command_buffer = load_device<PFN_vkBeginCommandBuffer>(
            "vkBeginCommandBuffer");
        end_command_buffer = load_device<PFN_vkEndCommandBuffer>(
            "vkEndCommandBuffer");
        queue_submit = load_device<PFN_vkQueueSubmit>("vkQueueSubmit");
        queue_wait_idle = load_device<PFN_vkQueueWaitIdle>(
            "vkQueueWaitIdle");
        if (checkpoints_enabled) {
            cmd_set_checkpoint = load_device<PFN_vkCmdSetCheckpointNV>(
                "vkCmdSetCheckpointNV");
            get_queue_checkpoint_data =
                load_device<PFN_vkGetQueueCheckpointDataNV>(
                    "vkGetQueueCheckpointDataNV");
            checkpoints_enabled = cmd_set_checkpoint != nullptr &&
                get_queue_checkpoint_data != nullptr;
        }
        if (guest_import_supported) {
            get_memory_host_pointer_properties =
                load_device<PFN_vkGetMemoryHostPointerPropertiesEXT>(
                    "vkGetMemoryHostPointerPropertiesEXT");
            guest_import_supported =
                get_memory_host_pointer_properties != nullptr;
            // 2: the extension without the imports, to tell the two apart.
            if (const auto* value = std::getenv("PS5GPU_NATIVE_IMPORT_GUEST");
                value != nullptr && value[0] == '2') {
                guest_import_supported = false;
            }
        }
        create_image = load_device<PFN_vkCreateImage>("vkCreateImage");
        destroy_image = load_device<PFN_vkDestroyImage>("vkDestroyImage");
        get_image_memory_requirements =
            load_device<PFN_vkGetImageMemoryRequirements>(
                "vkGetImageMemoryRequirements");
        create_buffer = load_device<PFN_vkCreateBuffer>("vkCreateBuffer");
        destroy_buffer = load_device<PFN_vkDestroyBuffer>(
            "vkDestroyBuffer");
        get_buffer_memory_requirements =
            load_device<PFN_vkGetBufferMemoryRequirements>(
                "vkGetBufferMemoryRequirements");
        g_vk_allocate_raw = load_device<PFN_vkAllocateMemory>(
            "vkAllocateMemory");
        allocate_memory = g_vk_allocate_raw != nullptr
            ? &counted_allocate_memory
            : nullptr;
        g_vk_free_raw = load_device<PFN_vkFreeMemory>("vkFreeMemory");
        free_memory =
            g_vk_free_raw != nullptr ? &counted_free_memory : nullptr;
        bind_image_memory = load_device<PFN_vkBindImageMemory>(
            "vkBindImageMemory");
        bind_buffer_memory = load_device<PFN_vkBindBufferMemory>(
            "vkBindBufferMemory");
        map_memory = load_device<PFN_vkMapMemory>("vkMapMemory");
        unmap_memory = load_device<PFN_vkUnmapMemory>("vkUnmapMemory");
        cmd_pipeline_barrier = load_device<PFN_vkCmdPipelineBarrier>(
            "vkCmdPipelineBarrier");
        cmd_copy_buffer =
            load_device<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
        cmd_copy_buffer_to_image =
            load_device<PFN_vkCmdCopyBufferToImage>(
                "vkCmdCopyBufferToImage");
        cmd_copy_image_to_buffer =
            load_device<PFN_vkCmdCopyImageToBuffer>(
                "vkCmdCopyImageToBuffer");
        cmd_copy_image = load_device<PFN_vkCmdCopyImage>("vkCmdCopyImage");
        cmd_clear_color_image =
            load_device<PFN_vkCmdClearColorImage>("vkCmdClearColorImage");
        create_shader_module = load_device<PFN_vkCreateShaderModule>(
            "vkCreateShaderModule");
        destroy_shader_module = load_device<PFN_vkDestroyShaderModule>(
            "vkDestroyShaderModule");
        create_render_pass = load_device<PFN_vkCreateRenderPass>(
            "vkCreateRenderPass");
        destroy_render_pass = load_device<PFN_vkDestroyRenderPass>(
            "vkDestroyRenderPass");
        create_pipeline_layout = load_device<PFN_vkCreatePipelineLayout>(
            "vkCreatePipelineLayout");
        destroy_pipeline_layout =
            load_device<PFN_vkDestroyPipelineLayout>(
                "vkDestroyPipelineLayout");
        create_descriptor_set_layout =
            load_device<PFN_vkCreateDescriptorSetLayout>(
                "vkCreateDescriptorSetLayout");
        destroy_descriptor_set_layout =
            load_device<PFN_vkDestroyDescriptorSetLayout>(
                "vkDestroyDescriptorSetLayout");
        create_descriptor_pool = load_device<PFN_vkCreateDescriptorPool>(
            "vkCreateDescriptorPool");
        destroy_descriptor_pool = load_device<PFN_vkDestroyDescriptorPool>(
            "vkDestroyDescriptorPool");
        allocate_descriptor_sets = load_device<PFN_vkAllocateDescriptorSets>(
            "vkAllocateDescriptorSets");
        update_descriptor_sets = load_device<PFN_vkUpdateDescriptorSets>(
            "vkUpdateDescriptorSets");
        create_sampler = load_device<PFN_vkCreateSampler>(
            "vkCreateSampler");
        destroy_sampler = load_device<PFN_vkDestroySampler>(
            "vkDestroySampler");
        create_graphics_pipelines =
            load_device<PFN_vkCreateGraphicsPipelines>(
                "vkCreateGraphicsPipelines");
        create_compute_pipelines =
            load_device<PFN_vkCreateComputePipelines>(
                "vkCreateComputePipelines");
        destroy_pipeline = load_device<PFN_vkDestroyPipeline>(
            "vkDestroyPipeline");
        create_pipeline_cache = load_device<PFN_vkCreatePipelineCache>(
            "vkCreatePipelineCache");
        get_pipeline_cache_data =
            load_device<PFN_vkGetPipelineCacheData>(
                "vkGetPipelineCacheData");
        destroy_pipeline_cache =
            load_device<PFN_vkDestroyPipelineCache>(
                "vkDestroyPipelineCache");
        create_image_view = load_device<PFN_vkCreateImageView>(
            "vkCreateImageView");
        destroy_image_view = load_device<PFN_vkDestroyImageView>(
            "vkDestroyImageView");
        create_framebuffer = load_device<PFN_vkCreateFramebuffer>(
            "vkCreateFramebuffer");
        destroy_framebuffer = load_device<PFN_vkDestroyFramebuffer>(
            "vkDestroyFramebuffer");
        cmd_begin_render_pass = load_device<PFN_vkCmdBeginRenderPass>(
            "vkCmdBeginRenderPass");
        cmd_end_render_pass = load_device<PFN_vkCmdEndRenderPass>(
            "vkCmdEndRenderPass");
        cmd_bind_pipeline = load_device<PFN_vkCmdBindPipeline>(
            "vkCmdBindPipeline");
        cmd_bind_descriptor_sets =
            load_device<PFN_vkCmdBindDescriptorSets>(
                "vkCmdBindDescriptorSets");
        cmd_bind_vertex_buffers =
            load_device<PFN_vkCmdBindVertexBuffers>(
                "vkCmdBindVertexBuffers");
        cmd_set_viewport = load_device<PFN_vkCmdSetViewport>(
            "vkCmdSetViewport");
        cmd_set_scissor = load_device<PFN_vkCmdSetScissor>(
            "vkCmdSetScissor");
        cmd_draw = load_device<PFN_vkCmdDraw>("vkCmdDraw");
        cmd_draw_indexed =
            load_device<PFN_vkCmdDrawIndexed>("vkCmdDrawIndexed");
        cmd_bind_index_buffer =
            load_device<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer");
        cmd_dispatch = load_device<PFN_vkCmdDispatch>(
            "vkCmdDispatch");
        cmd_push_constants = load_device<PFN_vkCmdPushConstants>(
            "vkCmdPushConstants");
        if (destroy_device == nullptr ||
            get_device_queue == nullptr ||
            create_command_pool == nullptr ||
            destroy_command_pool == nullptr ||
            allocate_command_buffers == nullptr ||
            free_command_buffers == nullptr ||
            reset_command_buffer == nullptr ||
            begin_command_buffer == nullptr ||
            end_command_buffer == nullptr ||
            queue_submit == nullptr ||
            queue_wait_idle == nullptr ||
            create_image == nullptr ||
            destroy_image == nullptr ||
            get_image_memory_requirements == nullptr ||
            create_buffer == nullptr ||
            destroy_buffer == nullptr ||
            get_buffer_memory_requirements == nullptr ||
            allocate_memory == nullptr ||
            free_memory == nullptr ||
            bind_image_memory == nullptr ||
            bind_buffer_memory == nullptr ||
            map_memory == nullptr ||
            unmap_memory == nullptr ||
            cmd_pipeline_barrier == nullptr ||
            cmd_copy_buffer == nullptr ||
            cmd_copy_buffer_to_image == nullptr ||
            cmd_copy_image_to_buffer == nullptr ||
            cmd_copy_image == nullptr ||
            create_shader_module == nullptr ||
            destroy_shader_module == nullptr ||
            create_render_pass == nullptr ||
            destroy_render_pass == nullptr ||
            create_pipeline_layout == nullptr ||
            destroy_pipeline_layout == nullptr ||
            create_descriptor_set_layout == nullptr ||
            destroy_descriptor_set_layout == nullptr ||
            create_descriptor_pool == nullptr ||
            destroy_descriptor_pool == nullptr ||
            allocate_descriptor_sets == nullptr ||
            update_descriptor_sets == nullptr ||
            create_sampler == nullptr ||
            destroy_sampler == nullptr ||
            create_graphics_pipelines == nullptr ||
            create_compute_pipelines == nullptr ||
            destroy_pipeline == nullptr ||
            create_pipeline_cache == nullptr ||
            get_pipeline_cache_data == nullptr ||
            destroy_pipeline_cache == nullptr ||
            create_image_view == nullptr ||
            destroy_image_view == nullptr ||
            create_framebuffer == nullptr ||
            destroy_framebuffer == nullptr ||
            cmd_begin_render_pass == nullptr ||
            cmd_end_render_pass == nullptr ||
            cmd_bind_pipeline == nullptr ||
            cmd_bind_descriptor_sets == nullptr ||
            cmd_bind_vertex_buffers == nullptr ||
            cmd_set_viewport == nullptr ||
            cmd_set_scissor == nullptr ||
            cmd_draw == nullptr ||
            cmd_dispatch == nullptr ||
            cmd_push_constants == nullptr) {
            runtime_trace("native_gpu.vulkan_missing_device_functions\n");
            destroy();
            return false;
        }

        get_device_queue(device, queue_family, 0, &queue);
        if (ps5gpu::native_window_requested()) {
            const auto shown = window_presenter.start(
                instance, get_instance_proc, physical_device, device,
                get_device_proc, queue_family, queue);
            runtime_trace("native_gpu.native_window started=%d\n",
                          shown ? 1 : 0);
        }
        VkCommandPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family;
        result = create_command_pool(
            device,
            &pool_info,
            nullptr,
            &command_pool);
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.vulkan_create_command_pool_failed result=%d\n",
                static_cast<int>(result));
            destroy();
            return false;
        }

        VkCommandBufferAllocateInfo allocation_info = {};
        allocation_info.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation_info.commandPool = command_pool;
        allocation_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation_info.commandBufferCount = 1;
        result = allocate_command_buffers(
            device,
            &allocation_info,
            &command_buffer);
        if (result == VK_SUCCESS &&
            create_fence != nullptr &&
            wait_for_fences != nullptr &&
            reset_fences != nullptr) {
            command_ring_ready = true;
            for (std::size_t slot = 0; slot < kCommandRing; ++slot) {
                VkCommandBufferAllocateInfo ring_info = allocation_info;
                if (allocate_command_buffers(
                        device,
                        &ring_info,
                        &command_ring[slot]) != VK_SUCCESS) {
                    command_ring_ready = false;
                    break;
                }
                VkFenceCreateInfo fence_info = {};
                fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                if (create_fence(
                        device,
                        &fence_info,
                        nullptr,
                        &command_ring_fences[slot]) != VK_SUCCESS) {
                    command_ring_ready = false;
                    break;
                }
                command_ring_pending[slot] = false;
            }
            if (!command_ring_ready) {
                runtime_trace(
                    "native_gpu.command_ring_unavailable\n");
            }
        }
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.vulkan_allocate_command_buffer_failed "
                "result=%d\n",
                static_cast<int>(result));
            destroy();
            return false;
        }
        if (!initialize_pipeline_cache()) {
            destroy();
            return false;
        }
        sample_copy_state =
            sample_copy_state_from_environment();
        if (!create_diagnostic_pipeline() ||
            !create_solid_white_pipeline() ||
            !create_sampled_pipeline() ||
            !create_real_composition_pipeline() ||
            !create_astro_post_pipelines() ||
            !create_astro_state28_pipeline() ||
            !create_astro_state29_pipeline() ||
            !create_guest_descriptor_pipelines() ||
            !create_resource_free_guest_pipelines()) {
            runtime_trace("native_gpu.vulkan_pipeline_failed\n");
            destroy();
            return false;
        }
        (void)save_pipeline_cache();
        last_pipeline_cache_save_ms = GetTickCount64();

        VkPhysicalDeviceProperties properties = {};
        get_physical_device_properties(physical_device, &properties);
        runtime_trace(
            "native_gpu.vulkan_ready api=%u.%u.%u device=%s "
            "queue_family=%u\n",
            VK_VERSION_MAJOR(application_info.apiVersion),
            VK_VERSION_MINOR(application_info.apiVersion),
            VK_VERSION_PATCH(application_info.apiVersion),
            properties.deviceName,
            queue_family);
        ready = true;
        return true;
    }

    VkRenderPass ensure_render_pass(VkFormat format, bool load) {
        if (device == VK_NULL_HANDLE || create_render_pass == nullptr) {
            return VK_NULL_HANDLE;
        }
        auto& cache = load ? load_render_passes : clear_render_passes;
        if (const auto existing = cache.find(format);
            existing != cache.end()) {
            return existing->second;
        }
        VkAttachmentDescription attachment = {};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = load
            ? VK_ATTACHMENT_LOAD_OP_LOAD
            : VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color_reference = {};
        color_reference.attachment = 0;
        color_reference.layout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_reference;
        VkRenderPassCreateInfo render_pass_info = {};
        render_pass_info.sType =
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments = &attachment;
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;
        VkRenderPass created = VK_NULL_HANDLE;
        const auto result = create_render_pass(
            device,
            &render_pass_info,
            nullptr,
            &created);
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.render_pass_failed format=%s load=%u "
                "result=%d\n",
                render_surface_format_name(format),
                load ? 1u : 0u,
                static_cast<int>(result));
            return VK_NULL_HANDLE;
        }
        cache.emplace(format, created);
        return created;
    }

    bool create_diagnostic_pipeline() {
        const auto vertex_spirv = ps5gpu::fixed_spirv::fullscreen_vertex();
        const auto fragment_spirv =
            ps5gpu::fixed_spirv::solid_green_fragment();

        render_pass =
            ensure_render_pass(VK_FORMAT_R8G8B8A8_UNORM, false);
        load_render_pass =
            ensure_render_pass(VK_FORMAT_R8G8B8A8_UNORM, true);
        if (render_pass == VK_NULL_HANDLE ||
            load_render_pass == VK_NULL_HANDLE) {
            return false;
        }

        VkPipelineLayoutCreateInfo layout_info = {};
        layout_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const auto result = create_pipeline_layout(
            device,
            &layout_info,
            nullptr,
            &pipeline_layout);
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.vulkan_graphics_pipeline_failed result=%d\n",
                static_cast<int>(result));
            return false;
        }
        if (!describe_guest_pipeline(
                diagnostic_pipeline_variants,
                vertex_spirv,
                fragment_spirv,
                pipeline_layout)) {
            runtime_trace(
                "native_gpu.vulkan_graphics_pipeline_failed result=%d\n",
                static_cast<int>(VK_ERROR_INITIALIZATION_FAILED));
            return false;
        }
        diagnostic_pipeline = ensure_guest_pipeline(
            diagnostic_pipeline_variants,
            VK_FORMAT_R8G8B8A8_UNORM);
        runtime_trace(
            "native_gpu.vulkan_pipeline_ready vertex_words=%llu "
            "fragment_words=%llu\n",
            static_cast<unsigned long long>(vertex_spirv.size()),
            static_cast<unsigned long long>(fragment_spirv.size()));
        return true;
    }

    bool create_solid_white_pipeline() {
        const auto vertex_spirv = ps5gpu::fixed_spirv::fullscreen_vertex();
        const auto fragment_spirv =
            ps5gpu::fixed_spirv::solid_white_fragment();
        if (!describe_guest_pipeline(
                solid_white_pipeline_variants,
                vertex_spirv,
                fragment_spirv,
                pipeline_layout)) {
            runtime_trace(
                "native_gpu.vulkan_white_pipeline_failed result=%d\n",
                static_cast<int>(VK_ERROR_INITIALIZATION_FAILED));
            return false;
        }
        solid_white_pipeline = ensure_guest_pipeline(
            solid_white_pipeline_variants,
            VK_FORMAT_R8G8B8A8_UNORM);
        runtime_trace(
            "native_gpu.vulkan_guest_clear_pipeline_ready "
            "fragment_words=%llu\n",
            static_cast<unsigned long long>(fragment_spirv.size()));
        return true;
    }

    // Replaces the guest's shaders with a full-screen triangle and a solid
    // colour. If the targets stay empty with these, then nothing a shader
    // computes is the reason - the pixels are being rejected by the state
    // around the draw, or the draw is not reaching the target it names. If
    // they fill, the guest's own shaders are producing nothing, and the
    // question moves to what they compute.
    // "1" replaces both, "vertex" only the vertex shader, "fragment" only
    // the fragment one. Replacing both said the pipeline writes pixels;
    // replacing one at a time says which of the guest's two shaders is the
    // one producing nothing.
    const char* fixed_shader_mode() {
        static const auto* mode = [] {
            const auto* value =
                std::getenv("PS5GPU_NATIVE_FORCE_FIXED_SHADERS");
            return value == nullptr ? "" : value;
        }();
        return mode;
    }

    bool force_fixed_vertex() {
        const auto* mode = fixed_shader_mode();
        return mode[0] == '1' || std::strstr(mode, "vertex") != nullptr;
    }

    bool force_fixed_fragment() {
        const auto* mode = fixed_shader_mode();
        return mode[0] == '1' || std::strstr(mode, "fragment") != nullptr;
    }

    bool force_fixed_shaders() {
        return force_fixed_vertex() || force_fixed_fragment();
    }

    bool create_guest_pipeline(
        const std::vector<std::uint32_t>& vertex_spirv_in,
        const std::vector<std::uint32_t>& fragment_spirv_in,
        VkPipelineLayout layout,
        VkPipeline& pipeline,
        VkPrimitiveTopology topology =
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VkFormat target_format = VK_FORMAT_R8G8B8A8_UNORM,
        std::uint32_t blend_control = 0) {
        const auto target_render_pass =
            ensure_render_pass(target_format, false);
        if (target_render_pass == VK_NULL_HANDLE) {
            return false;
        }
        const auto forced = force_fixed_shaders();
        const auto fixed_vertex = forced
            ? ps5gpu::fixed_spirv::fullscreen_vertex()
            : std::vector<std::uint32_t>{};
        const auto fixed_fragment = forced
            ? ps5gpu::fixed_spirv::solid_green_fragment()
            : std::vector<std::uint32_t>{};
        const auto& vertex_spirv =
            forced && force_fixed_vertex() ? fixed_vertex : vertex_spirv_in;
        // A diagnostic: a fragment module from a file, for every guest
        // pipeline - a hand-written shader put against the translated
        // vertex stage to find which part of a pair breaks it.
        static const auto fragment_override = [] {
            std::vector<std::uint32_t> words;
            if (const auto* path =
                    std::getenv("PS5GPU_NATIVE_FRAGMENT_OVERRIDE_SPV");
                path != nullptr && path[0] != '\0') {
                (void)load_spirv_file(path, words);
            }
            return words;
        }();
        const auto& fragment_spirv = !fragment_override.empty()
            ? fragment_override
            : forced && force_fixed_fragment()
            ? fixed_fragment
            : fragment_spirv_in;
        VkShaderModuleCreateInfo vertex_info = {};
        vertex_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vertex_info.codeSize =
            vertex_spirv.size() * sizeof(std::uint32_t);
        vertex_info.pCode = vertex_spirv.data();
        VkShaderModuleCreateInfo fragment_info = vertex_info;
        fragment_info.codeSize =
            fragment_spirv.size() * sizeof(std::uint32_t);
        fragment_info.pCode = fragment_spirv.data();
        VkShaderModule vertex_module = VK_NULL_HANDLE;
        VkShaderModule fragment_module = VK_NULL_HANDLE;
        auto result = create_shader_module(
            device,
            &vertex_info,
            nullptr,
            &vertex_module);
        if (result == VK_SUCCESS) {
            result = create_shader_module(
                device,
                &fragment_info,
                nullptr,
                &fragment_module);
        }
        if (result != VK_SUCCESS) {
            if (fragment_module != VK_NULL_HANDLE) {
                destroy_shader_module(device, fragment_module, nullptr);
            }
            if (vertex_module != VK_NULL_HANDLE) {
                destroy_shader_module(device, vertex_module, nullptr);
            }
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_module;
        stages[0].pName = "main";
        stages[1].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_module;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertex_input = {};
        vertex_input.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
        input_assembly.sType =
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = topology;
        VkPipelineViewportStateCreateInfo viewport_state = {};
        viewport_state.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterization = {};
        rasterization.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType =
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment = {};
        blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;
        apply_guest_blend(blend_attachment, blend_control);
        apply_forced_blend(blend_attachment);
        VkPipelineColorBlendStateCreateInfo blend = {};
        blend.sType =
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        const VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };
        VkPipelineDynamicStateCreateInfo dynamic = {};
        dynamic.sType =
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount =
            static_cast<std::uint32_t>(std::size(dynamic_states));
        dynamic.pDynamicStates = dynamic_states;
        VkGraphicsPipelineCreateInfo pipeline_info = {};
        pipeline_info.sType =
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = stages;
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = layout;
        pipeline_info.renderPass = target_render_pass;
        pipeline_info.subpass = 0;
        result = create_graphics_pipelines(
            device,
            pipeline_cache,
            1,
            &pipeline_info,
            nullptr,
            &pipeline);
        destroy_shader_module(device, fragment_module, nullptr);
        destroy_shader_module(device, vertex_module, nullptr);
        return result == VK_SUCCESS;
    }

    VkPipeline ensure_guest_pipeline(
        GuestPipelineVariants& variants,
        VkFormat target_format,
        std::uint32_t blend_control = 0) {
        if (variants.layout == VK_NULL_HANDLE ||
            variants.vertex_spirv == nullptr ||
            variants.fragment_spirv == nullptr ||
            variants.vertex_spirv->empty() ||
            variants.fragment_spirv->empty()) {
            return VK_NULL_HANDLE;
        }
        // Only the enabled form matters; a disabled control is opaque
        // whatever its factors say.
        if ((blend_control & (1u << 30)) == 0) {
            blend_control = 0;
        }
        const auto key =
            static_cast<std::uint64_t>(target_format) |
            (static_cast<std::uint64_t>(blend_control) << 32);
        if (const auto existing = variants.by_format.find(key);
            existing != variants.by_format.end()) {
            return existing->second;
        }
        static const bool share = [] {
            const auto* value = std::getenv("PS5GPU_NATIVE_SHARE_PIPELINES");
            return value == nullptr || value[0] != '0';
        }();
        const std::array<std::uint64_t, 6> shared_key = {
            variants.vertex_hash,
            variants.fragment_hash,
            variants.layout_signature != 0
                ? variants.layout_signature
                : reinterpret_cast<std::uint64_t>(variants.layout),
            static_cast<std::uint64_t>(variants.topology),
            static_cast<std::uint64_t>(target_format),
            static_cast<std::uint64_t>(blend_control),
        };
        if (share) {
            if (const auto existing =
                    shared_graphics_pipelines.find(shared_key);
                existing != shared_graphics_pipelines.end()) {
                ++shared_pipeline_hits;
                variants.by_format.emplace(key, existing->second);
                return existing->second;
            }
        }
        ++shared_pipeline_builds;
        VkPipeline created = VK_NULL_HANDLE;
        if (blend_control != 0) {
            runtime_trace(
                "native_gpu.guest_pipeline_blend format=%s control=0x%08X\n",
                render_surface_format_name(target_format),
                blend_control);
        }
        if (!create_guest_pipeline(
                *variants.vertex_spirv,
                *variants.fragment_spirv,
                variants.layout,
                created,
                variants.topology,
                target_format,
                blend_control)) {
            runtime_trace(
                "native_gpu.guest_pipeline_variant_failed format=%s\n",
                render_surface_format_name(target_format));
            created = VK_NULL_HANDLE;
        }
        // Remember the failure too, so a format that cannot be built is not
        // retried once per draw for the rest of the run.
        variants.by_format.emplace(key, created);
        if (shared_graphics_pipelines.count(shared_key) == 0) {
            shared_graphics_pipelines.emplace(shared_key, created);
        } else if (created != VK_NULL_HANDLE && destroy_pipeline != nullptr) {
            // Sharing off and a twin already cached: this one stays with
            // the cache's copy so that only the cache destroys pipelines.
            destroy_pipeline(device, created, nullptr);
            created = shared_graphics_pipelines[shared_key];
            variants.by_format[key] = created;
        }
        return created;
    }

    bool describe_guest_pipeline(
        GuestPipelineVariants& variants,
        const std::vector<std::uint32_t>& vertex_spirv,
        const std::vector<std::uint32_t>& fragment_spirv,
        VkPipelineLayout layout,
        VkPrimitiveTopology topology =
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        std::uint64_t layout_signature = 0) {
        return describe_guest_pipeline(
            variants,
            std::make_shared<const std::vector<std::uint32_t>>(vertex_spirv),
            std::make_shared<const std::vector<std::uint32_t>>(
                fragment_spirv),
            layout,
            topology,
            layout_signature);
    }

    // The same over words already shared. Hashes the caller already has -
    // the strip variant of a state has its list variant's - are taken as
    // given rather than computed over the words again.
    bool describe_guest_pipeline(
        GuestPipelineVariants& variants,
        std::shared_ptr<const std::vector<std::uint32_t>> vertex_spirv,
        std::shared_ptr<const std::vector<std::uint32_t>> fragment_spirv,
        VkPipelineLayout layout,
        VkPrimitiveTopology topology,
        std::uint64_t layout_signature,
        std::uint64_t known_vertex_hash = 0,
        std::uint64_t known_fragment_hash = 0) {
        destroy_guest_pipeline_variants(variants);
        variants.vertex_hash = known_vertex_hash != 0
            ? known_vertex_hash
            : hash_spirv_words(*vertex_spirv, 0xCBF29CE484222325ull);
        variants.fragment_hash = known_fragment_hash != 0
            ? known_fragment_hash
            : hash_spirv_words(*fragment_spirv, 0x84222325CBF29CE4ull);
        variants.vertex_spirv = std::move(vertex_spirv);
        variants.fragment_spirv = std::move(fragment_spirv);
        variants.layout = layout;
        variants.topology = topology;
        variants.layout_signature = layout_signature;
        // Build the RGBA8 variant eagerly: it is what almost every surface
        // still is, and it keeps a broken shader failing where it used to.
        variants.valid =
            ensure_guest_pipeline(variants, VK_FORMAT_R8G8B8A8_UNORM) !=
            VK_NULL_HANDLE;
        return variants.valid;
    }

    void destroy_guest_pipeline_variants(
        GuestPipelineVariants& variants) {
        // The pipelines belong to shared_graphics_pipelines.
        variants.by_format.clear();
        variants.valid = false;
    }

    void destroy_shared_graphics_pipelines() {
        if (destroy_pipeline != nullptr) {
            for (const auto& [key, pipeline] : shared_graphics_pipelines) {
                (void)key;
                if (pipeline != VK_NULL_HANDLE) {
                    destroy_pipeline(device, pipeline, nullptr);
                }
            }
        }
        shared_graphics_pipelines.clear();
    }

    void destroy_guest_descriptor_pipeline(
        GuestDescriptorPipeline& guest) {
        // Borrowed from shared_compute_programs, which outlives every state
        // that used it and is torn down with the device.
        if (guest.shares_program) {
            guest.descriptor_set_layout = VK_NULL_HANDLE;
            guest.pipeline_layout = VK_NULL_HANDLE;
            guest.pipeline = VK_NULL_HANDLE;
            guest.shares_program = false;
        }
        if (guest.shares_layout) {
            guest.descriptor_set_layout = VK_NULL_HANDLE;
            guest.pipeline_layout = VK_NULL_HANDLE;
            guest.shares_layout = false;
        }
        // On a graphics state the two scalar handles only mirror the RGBA8
        // variants, which the variant maps own and destroy.
        destroy_guest_pipeline_variants(guest.graphics_pipeline);
        destroy_guest_pipeline_variants(guest.graphics_strip_pipeline);
        if (guest.graphics) {
            guest.pipeline = VK_NULL_HANDLE;
            guest.strip_pipeline = VK_NULL_HANDLE;
        }
        if (guest.pipeline != VK_NULL_HANDLE &&
            destroy_pipeline != nullptr) {
            destroy_pipeline(device, guest.pipeline, nullptr);
        }
        if (guest.strip_pipeline != VK_NULL_HANDLE &&
            destroy_pipeline != nullptr) {
            destroy_pipeline(
                device,
                guest.strip_pipeline,
                nullptr);
        }
        if (guest.pipeline_layout != VK_NULL_HANDLE &&
            destroy_pipeline_layout != nullptr) {
            destroy_pipeline_layout(
                device,
                guest.pipeline_layout,
                nullptr);
        }
        if (guest.descriptor_pool != VK_NULL_HANDLE &&
            destroy_descriptor_pool != nullptr) {
            destroy_descriptor_pool(
                device,
                guest.descriptor_pool,
                nullptr);
        }
        if (guest.descriptor_set_layout != VK_NULL_HANDLE &&
            destroy_descriptor_set_layout != nullptr) {
            destroy_descriptor_set_layout(
                device,
                guest.descriptor_set_layout,
                nullptr);
        }
        for (auto& buffer : guest.buffers) {
            release_compute_storage_buffer(
                buffer.size,
                buffer.buffer,
                buffer.memory,
                buffer.staging_buffer,
                buffer.staging_memory);
            if (buffer.buffer != VK_NULL_HANDLE &&
                destroy_buffer != nullptr) {
                destroy_buffer(device, buffer.buffer, nullptr);
            }
            if (buffer.memory != VK_NULL_HANDLE &&
                free_memory != nullptr) {
                free_memory(device, buffer.memory, nullptr);
            }
            if (buffer.staging_buffer != VK_NULL_HANDLE &&
                destroy_buffer != nullptr) {
                destroy_buffer(device, buffer.staging_buffer, nullptr);
            }
            if (buffer.staging_memory != VK_NULL_HANDLE &&
                free_memory != nullptr) {
                free_memory(device, buffer.staging_memory, nullptr);
            }
        }
        for (auto& image : guest.images) {
            if (image.upload_buffer != VK_NULL_HANDLE &&
                destroy_buffer != nullptr) {
                destroy_buffer(device, image.upload_buffer, nullptr);
            }
            if (image.upload_memory != VK_NULL_HANDLE &&
                free_memory != nullptr) {
                free_memory(device, image.upload_memory, nullptr);
            }
        }
        guest = {};
    }

    bool append_guest_manifest(
        const LoadedResourceManifest& manifest,
        VkShaderStageFlags stage,
        GuestDescriptorPipeline& guest) {
        // A manifest may arrive without the data section it describes: the
        // snapshot in there is a fallback for a failed read of the guest's
        // live memory, and measuring says that fallback is reached zero
        // times against 456 failed reads, because the buffers whose reads
        // fail are the ones with no data section to begin with. Keeping it
        // cost 8.5GB across 2365 states and an out-of-memory exit at 113
        // seconds, so registration drops it. The records still describe it,
        // which is how the sizes below survive.
        const auto data_present =
            manifest.header.data_offset < manifest.bytes.size();
        for (const auto& record : manifest.globals) {
            if (record.descriptor_index > 4096 ||
                (data_present &&
                 (record.data_offset > manifest.bytes.size() ||
                  record.data_size >
                      manifest.bytes.size() - record.data_offset))) {
                runtime_trace(
                    "native_gpu.manifest_global_rejected stage=0x%X "
                    "descriptor=%u offset=%u size=%u bytes=%llu\n",
                    stage,
                    record.descriptor_index,
                    record.data_offset,
                    record.data_size,
                    static_cast<unsigned long long>(
                        manifest.bytes.size()));
                return false;
            }
            if (guest.buffers.size() <= record.descriptor_index) {
                guest.buffers.resize(record.descriptor_index + 1);
            }
            auto& buffer = guest.buffers[record.descriptor_index];
            if (buffer.size != 0) {
                runtime_trace(
                    "native_gpu.manifest_global_duplicate stage=0x%X "
                    "descriptor=%u\n",
                    stage,
                    record.descriptor_index);
                return false;
            }
            buffer.descriptor_index = record.descriptor_index;
            buffer.flags = record.flags;
            buffer.scalar_address = record.scalar_address;
            buffer.stages |= stage;
            buffer.guest_address = record.base_address;
            buffer.size = record.data_size != 0
                ? std::max<VkDeviceSize>(record.data_size, 4)
                : record.base_address >= 0x10000
                ? VkDeviceSize{4096}
                : VkDeviceSize{4};
            // The snapshot is the copy of the buffer the manifest carries,
            // and it is only ever reached when reading the title's live
            // memory fails. It used to fail often, because a buffer
            // straddling memory the lazy commit had only reserved could not
            // be read at all; now that the read walks such a range instead
            // of refusing it, a buffer with a real address is always
            // readable and its copy is dead weight. It is not small weight:
            // the copies of one state run to seventeen megabytes, 1850
            // states were live in a run, and the process died of a full
            // commit at a hundred seconds with 16.7GB of them.
            //
            // A buffer with no address is the exception. That is the scalar
            // block, which is assembled from the manifest itself rather
            // than read from anywhere, so its bytes are the only copy there
            // is and must be kept.
            static const auto keep_all_snapshots =
                environment_flag_enabled("PS5GPU_NATIVE_KEEP_SNAPSHOTS");
            if (data_present &&
                (keep_all_snapshots || record.base_address < 0x10000)) {
                buffer.snapshot.assign(
                    manifest.bytes.begin() + record.data_offset,
                    manifest.bytes.begin() +
                        record.data_offset + record.data_size);
                g_snapshot_kept_bytes.fetch_add(
                    record.data_size, std::memory_order_relaxed);
            } else if (data_present) {
                g_snapshot_dropped_bytes.fetch_add(
                    record.data_size, std::memory_order_relaxed);
            }
        }
        const auto scalar_descriptor = manifest.header.reserved[2];
        if (scalar_descriptor != 0) {
            const auto index = scalar_descriptor - 1;
            const auto offset = manifest.header.reserved[0];
            const auto dwords = manifest.header.reserved[1];
            const auto bytes = static_cast<std::size_t>(dwords) * 4;
            if (index > 4096 || dwords == 0 ||
                offset > manifest.bytes.size() ||
                bytes > manifest.bytes.size() - offset) {
                runtime_trace(
                    "native_gpu.manifest_scalar_rejected stage=0x%X "
                    "descriptor=%u offset=%u dwords=%u bytes=%llu\n",
                    stage,
                    index,
                    offset,
                    dwords,
                    static_cast<unsigned long long>(
                        manifest.bytes.size()));
                return false;
            }
            if (guest.buffers.size() <= index) {
                guest.buffers.resize(index + 1);
            }
            // 256 registers, then one byte bias per descriptor, then one
            // write mark per descriptor - the same split the translator
            // computes from the same two numbers.
            const auto descriptors = dwords > 256 ? (dwords - 256) / 2 : 0;
            guest.scalar_block_index = static_cast<int>(index);
            guest.write_mask_base = 256 + descriptors;
            guest.write_mask_count = descriptors;

            auto& buffer = guest.buffers[index];
            buffer.descriptor_index = index;
            buffer.flags = 0;
            buffer.scalar_address = 0;
            buffer.stages |= stage;
            buffer.guest_address = 0;
            buffer.size = bytes;
            buffer.snapshot.assign(
                manifest.bytes.begin() + offset,
                manifest.bytes.begin() + offset + bytes);
        }

        for (const auto& record : manifest.images) {
            // A shader may declare an image slot the guest never bound; the
            // descriptor then decodes as all zeroes. That is a legal hardware
            // state, not a malformed manifest, and the translator still emits
            // a variable for the slot - so the descriptor set layout has to
            // cover it. Bind a placeholder instead of discarding every other
            // resource this pipeline needs.
            const auto unbound_slot =
                record.flags == 0 && record.base_address == 0;
            const char* rejection = nullptr;
            if (record.binding == 0 || record.binding > 4096) {
                rejection = "binding";
            } else if (
                std::any_of(
                    guest.images.begin(),
                    guest.images.end(),
                    [&record](const GuestImageResource& image) {
                        return image.binding == record.binding;
                    })) {
                rejection = "duplicate-binding";
            } else if (!unbound_slot) {
                if ((record.flags &
                        PS5GPU_RESOURCE_IMAGE_VALID_EXTENT) == 0) {
                    rejection = "extent";
                } else if (record.base_address < 0x10000) {
                    rejection = "address";
                } else if (
                    record.width == 0 ||
                    record.height == 0 ||
                    record.width > 16384 ||
                    record.height > 16384) {
                    rejection = "size";
                }
            }
            if (rejection != nullptr) {
                runtime_trace(
                    "native_gpu.manifest_image_rejected stage=0x%X "
                    "reason=%s binding=%u flags=0x%08X "
                    "address=0x%016llX size=%ux%u\n",
                    stage,
                    rejection,
                    record.binding,
                    record.flags,
                    static_cast<unsigned long long>(
                        record.base_address),
                    record.width,
                    record.height);
                return false;
            }
            GuestImageResource image;
            image.binding = record.binding;
            image.flags = record.flags;
            image.stages = stage;
            if (unbound_slot) {
                image.placeholder = true;
                image.guest_address = kPlaceholderImageAddress;
                image.width = 1;
                image.height = 1;
                image.pitch = 1;
                image.unified_format = 56;
                runtime_trace(
                    "native_gpu.manifest_image_placeholder stage=0x%X "
                    "binding=%u\n",
                    stage,
                    record.binding);
                guest.images.push_back(image);
                continue;
            }
            image.guest_address = record.base_address;
            std::copy(
                std::begin(record.sampler_descriptor),
                std::end(record.sampler_descriptor),
                image.sampler_descriptor.begin());
            image.width = record.width;
            image.height = record.height;
            image.unified_format =
                (record.resource_descriptor[1] >> 20) & 0x1FFu;
            image.tile_mode =
                (record.resource_descriptor[3] >> 20) & 0x1Fu;
            image.type =
                (record.resource_descriptor[3] >> 28) & 0xFu;
            image.pitch =
                image.type == 8 ||
                    image.type == 9 ||
                    image.type == 14
                ? (record.resource_descriptor[4] != 0
                    ? (record.resource_descriptor[4] & 0x3FFFu) + 1
                    : image.width)
                : image.width;
            image.depth = image.type == 10
                ? (record.resource_descriptor[4] & 0x1FFFu) + 1
                : 1u;
            std::copy(
                std::begin(record.resource_descriptor),
                std::end(record.resource_descriptor),
                image.resource_descriptor.begin());
            guest.images.push_back(image);
        }
        return true;
    }

    static std::uint64_t compute_program_key(
        const std::vector<std::uint32_t>& spirv,
        const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
        auto key = hash_bytes(
            spirv.data(),
            spirv.size() * sizeof(std::uint32_t));
        for (const auto& binding : bindings) {
            const std::uint32_t fields[] = {
                binding.binding,
                static_cast<std::uint32_t>(binding.descriptorType),
                binding.descriptorCount,
                static_cast<std::uint32_t>(binding.stageFlags),
            };
            key = hash_bytes(fields, sizeof(fields), key);
        }
        return key;
    }

    bool ensure_guest_compute_pipeline(
        const RegisteredComputeState& state) {
        // The first thing this worker does, because a fault between a
        // state being submitted and its module being created leaves no
        // other mark.
        {
            static std::atomic<std::uint64_t> sampled_compute_worker_begin{0};
            const auto seen = sampled_compute_worker_begin.fetch_add(1, std::memory_order_relaxed);
            // Once per dispatch filled the log and cost the worker a
            // thirtieth of its time writing it: the first few, then a sample.
            if (seen < 256 || (seen % 1024) == 0) runtime_trace(
            "native_gpu.compute_worker_begin state=%u words=%zu\n",
            state.state_id,
            state.spirv.size());
        }
        // Flushed, because these exist to survive a crash and the trace is
        // buffered - the first round of them was lost with the process and
        // made it look as though the worker was never entered.
        runtime_trace_flush();
        if (retired_compute_states.erase(state.state_id) != 0) {
            const auto count = g_revived_compute_states.fetch_add(
                1, std::memory_order_relaxed);
            if (count < 8) {
                runtime_trace(
                    "native_gpu.compute_state_revived state=%u\n",
                    state.state_id);
            }
        }
        if (guest_compute_pipelines.contains(state.state_id)) {
            ++g_pipeline_cached;
            return true;
        }

        ++g_pipeline_built;
        const auto decode_started = worker_phase_counter();
        LoadedResourceManifest manifest;
        if (!decode_resource_manifest(
                state.resource_manifest.data(),
                state.resource_manifest.size(),
                manifest) ||
            manifest.header.stage != PS5GPU_STAGE_COMPUTE) {
            runtime_trace(
                "native_gpu.compute_manifest_invalid state=%u "
                "shader=0x%016llX bytes=%llu\n",
                state.state_id,
                static_cast<unsigned long long>(
                    state.shader_address),
                static_cast<unsigned long long>(
                    state.resource_manifest.size()));
            return false;
        }

        GuestDescriptorPipeline guest;
        if (!append_guest_manifest(
                manifest,
                VK_SHADER_STAGE_COMPUTE_BIT,
                guest) ||
            guest.buffers.size() > 4096 ||
            guest.images.size() > 4096) {
            runtime_trace(
                "native_gpu.compute_resources_invalid state=%u\n",
                state.state_id);
            return false;
        }
        g_pipeline_decode_ticks += worker_phase_counter() - decode_started;
        std::sort(
            guest.images.begin(),
            guest.images.end(),
            [](const GuestImageResource& left,
               const GuestImageResource& right) {
                return left.binding < right.binding;
            });
        for (std::size_t index = 0;
             index < guest.buffers.size();
             ++index) {
            auto& buffer = guest.buffers[index];
            if (buffer.size == 0) {
                buffer.descriptor_index =
                    static_cast<std::uint32_t>(index);
                buffer.size = 4;
            }
        }

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.reserve(guest.images.size() + 1);
        if (!guest.buffers.empty()) {
            VkDescriptorSetLayoutBinding buffer_binding = {};
            buffer_binding.binding = 0;
            buffer_binding.descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            buffer_binding.descriptorCount =
                static_cast<std::uint32_t>(guest.buffers.size());
            buffer_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings.push_back(buffer_binding);
        }
        std::uint32_t sampled_count = 0;
        std::uint32_t storage_image_count = 0;
        for (const auto& image : guest.images) {
            VkDescriptorSetLayoutBinding image_binding = {};
            image_binding.binding = image.binding;
            image_binding.descriptorType =
                (image.flags & PS5GPU_RESOURCE_IMAGE_STORAGE) != 0
                ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            image_binding.descriptorCount = 1;
            image_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings.push_back(image_binding);
            if (image_binding.descriptorType ==
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                ++storage_image_count;
            } else {
                ++sampled_count;
            }
        }

        // The debug override swaps the SPIR-V out from under the key, so
        // a state using it keeps its own program.
        const auto overriding =
            state.shader_address == 0x000000050054A400ULL &&
            environment_flag_enabled(
                "PS5GPU_NATIVE_OVERRIDE_COMPUTE_STATE10");
        const auto program_key = compute_program_key(state.spirv, bindings);
        if (!overriding) {
            const auto shared = shared_compute_programs.find(program_key);
            if (shared != shared_compute_programs.end()) {
                guest.descriptor_set_layout =
                    shared->second.descriptor_set_layout;
                guest.pipeline_layout = shared->second.pipeline_layout;
                guest.pipeline = shared->second.pipeline;
                guest.shares_program = true;
                ++g_pipeline_shared;
            }
        }

        const auto create_started = worker_phase_counter();
        auto result = VK_SUCCESS;
        if (!bindings.empty() && !guest.shares_program) {
            ++g_pipeline_created;
            VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {};
            descriptor_layout_info.sType =
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descriptor_layout_info.bindingCount =
                static_cast<std::uint32_t>(bindings.size());
            descriptor_layout_info.pBindings = bindings.data();
            result = create_descriptor_set_layout(
                device,
                &descriptor_layout_info,
                nullptr,
                &guest.descriptor_set_layout);
        }

        VkPipelineLayoutCreateInfo layout_info = {};
        layout_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (guest.descriptor_set_layout != VK_NULL_HANDLE) {
            layout_info.setLayoutCount = 1;
            layout_info.pSetLayouts = &guest.descriptor_set_layout;
        }
        VkPushConstantRange dispatch_limit_range = {};
        dispatch_limit_range.stageFlags =
            VK_SHADER_STAGE_COMPUTE_BIT;
        dispatch_limit_range.offset = 0;
        dispatch_limit_range.size =
            3 * sizeof(std::uint32_t);
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges =
            &dispatch_limit_range;
        if (result == VK_SUCCESS && !guest.shares_program) {
            result = create_pipeline_layout(
                device,
                &layout_info,
                nullptr,
                &guest.pipeline_layout);
        }

        VkShaderModule shader_module = VK_NULL_HANDLE;
        std::vector<std::uint32_t> override_spirv;
        const auto* pipeline_spirv = &state.spirv;
        if (state.shader_address ==
                0x000000050054A400ULL &&
            environment_flag_enabled(
                "PS5GPU_NATIVE_OVERRIDE_COMPUTE_STATE10")) {
            override_spirv =
                ps5gpu::fixed_spirv::solid_green_compute_storage(3);
            pipeline_spirv = &override_spirv;
            runtime_trace(
                "native_gpu.compute_override state=%u "
                "shader=0x%016llX binding=3 mode=solid-green\n",
                state.state_id,
                static_cast<unsigned long long>(
                    state.shader_address));
        }
        VkShaderModuleCreateInfo shader_info = {};
        shader_info.sType =
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize =
            pipeline_spirv->size() * sizeof(std::uint32_t);
        shader_info.pCode = pipeline_spirv->data();
        if (result == VK_SUCCESS && !guest.shares_program) {
            // Either side of the driver, so that a fault inside it is
            // distinguishable from one before or after. A module that is
            // structurally sound and semantically wrong takes the process
            // down here with nothing said.
            runtime_trace(
                "native_gpu.compute_module_begin state=%u words=%zu\n",
                state.state_id,
                pipeline_spirv->size());
            runtime_trace_flush();
            result = create_shader_module(
                device,
                &shader_info,
                nullptr,
                &shader_module);
            runtime_trace(
                "native_gpu.compute_module_end state=%u result=%d\n",
                state.state_id,
                static_cast<int>(result));
            runtime_trace_flush();
        }
        VkPipelineShaderStageCreateInfo stage_info = {};
        stage_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = shader_module;
        stage_info.pName = "main";
        VkComputePipelineCreateInfo pipeline_info = {};
        pipeline_info.sType =
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage = stage_info;
        pipeline_info.layout = guest.pipeline_layout;
        if (result == VK_SUCCESS && !guest.shares_program) {
            runtime_trace(
                "native_gpu.compute_pipeline_begin state=%u\n",
                state.state_id);
            runtime_trace_flush();
            result = create_compute_pipelines(
                device,
                pipeline_cache,
                1,
                &pipeline_info,
                nullptr,
                &guest.pipeline);
            runtime_trace(
                "native_gpu.compute_pipeline_end state=%u result=%d\n",
                state.state_id,
                static_cast<int>(result));
            runtime_trace_flush();
            if (result == VK_SUCCESS && !overriding) {
                shared_compute_programs.emplace(
                    program_key,
                    SharedComputeProgram{
                        guest.descriptor_set_layout,
                        guest.pipeline_layout,
                        guest.pipeline,
                    });
                guest.shares_program = true;
            }
        }
        g_pipeline_create_ticks += worker_phase_counter() - create_started;
        if (shader_module != VK_NULL_HANDLE) {
            destroy_shader_module(device, shader_module, nullptr);
        }

        const auto pipeline_descriptors_started = worker_phase_counter();
        if (result == VK_SUCCESS && !bindings.empty()) {
            std::vector<VkDescriptorPoolSize> pool_sizes;
            if (!guest.buffers.empty()) {
                pool_sizes.push_back({
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    static_cast<std::uint32_t>(
                        guest.buffers.size()),
                });
            }
            if (sampled_count != 0) {
                pool_sizes.push_back({
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    sampled_count,
                });
            }
            if (storage_image_count != 0) {
                pool_sizes.push_back({
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    storage_image_count,
                });
            }
            VkDescriptorPoolCreateInfo pool_info = {};
            pool_info.sType =
                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_info.maxSets = 1;
            pool_info.poolSizeCount =
                static_cast<std::uint32_t>(pool_sizes.size());
            pool_info.pPoolSizes = pool_sizes.data();
            result = create_descriptor_pool(
                device,
                &pool_info,
                nullptr,
                &guest.descriptor_pool);

            VkDescriptorSetAllocateInfo allocate_info = {};
            allocate_info.sType =
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate_info.descriptorPool = guest.descriptor_pool;
            allocate_info.descriptorSetCount = 1;
            allocate_info.pSetLayouts =
                &guest.descriptor_set_layout;
            if (result == VK_SUCCESS) {
                result = allocate_descriptor_sets(
                    device,
                    &allocate_info,
                    &guest.descriptor_set);
            }
        }
        g_pipeline_descriptor_ticks +=
            worker_phase_counter() - pipeline_descriptors_started;
        // The program a state ends up with, and the sizes it wants
        // beside it. Counting only - nothing reads this yet.
        auto shape = program_key;
        for (const auto& buffer : guest.buffers) {
            const std::uint64_t size = buffer.size;
            shape = hash_bytes(&size, sizeof(size), shape);
        }
        for (const auto& image : guest.images) {
            const std::uint32_t fields[] = {
                image.binding,
                image.width,
                image.height,
                image.flags,
            };
            shape = hash_bytes(fields, sizeof(fields), shape);
        }
        g_pipeline_shapes.insert(shape);

        // 823 states are built in a run and they come to 71 distinct
        // shapes, so almost every buffer made here is the same size as one
        // that already exists, and 2285ms of the 2742ms this phase costs is
        // making 6198 of them.
        //
        // Sharing one buffer per size and slot across states works and is
        // worth having: 6198 created falls to 502-691, the phase from
        // 2742ms to 879-1014ms. It is not here because it cannot live
        // beside the deferred write-back. The upload for the next dispatch
        // runs while the previous one is still on the GPU - that is what
        // the deferral bought - and with the buffers shared it writes over
        // what that dispatch is still reading. Measured, throughput falls
        // rather than rises: 21, 46 and 56 flips against 59 to 70.
        //
        // Both together need the flush moved back in front of the upload,
        // which returns the 1.4 seconds the deferral won against the 1.8
        // this saves. Worth doing when the two can be settled together;
        // worth nothing done separately. The predicate is also worth
        // writing down: private exactly when both flags are set, because
        // WRITE_BACK alone means the recompiler managed to read the buffer
        // rather than that the shader writes it, and 82 per cent carry it.
        const auto pipeline_buffers_started = worker_phase_counter();
        for (auto& buffer : guest.buffers) {
            ++g_pipeline_buffer_n;
            if (result == VK_SUCCESS &&
                !acquire_compute_storage_buffer(
                    buffer.size,
                    buffer.buffer,
                    buffer.memory,
                    buffer.staging_buffer,
                    buffer.staging_memory)) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        g_pipeline_buffer_ticks +=
            worker_phase_counter() - pipeline_buffers_started;
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.compute_pipeline_failed state=%u "
                "shader=0x%016llX result=%d\n",
                state.state_id,
                static_cast<unsigned long long>(
                    state.shader_address),
                static_cast<int>(result));
            destroy_guest_descriptor_pipeline(guest);
            return false;
        }

        const auto [inserted, success] =
            guest_compute_pipelines.emplace(
                state.state_id,
                std::move(guest));
        if (!success) {
            destroy_guest_descriptor_pipeline(guest);
            return false;
        }
        runtime_trace(
            "native_gpu.compute_pipeline_ready state=%u "
            "shader=0x%016llX buffers=%llu images=%llu "
            "sampled=%u storage_images=%u\n",
            state.state_id,
            static_cast<unsigned long long>(state.shader_address),
            static_cast<unsigned long long>(
                inserted->second.buffers.size()),
            static_cast<unsigned long long>(
                inserted->second.images.size()),
            sampled_count,
            storage_image_count);
        save_pipeline_cache_throttled();
        return true;
    }

    enum class GuestPipelineBuild {
        Ready,
        MissingManifest,
        InvalidManifest,
        Failed,
    };

    // Builds the descriptor pipeline for one state out of its two modules
    // and their manifests. Shared by the load at start, which reads what an
    // earlier translation left on disk, and by the draw loop, which builds
    // a state compiled during this run the first time it is drawn - every
    // mesh in the scene is one of those, and until this existed none of
    // them had a pipeline at all.
    GuestPipelineBuild build_guest_descriptor_pipeline(
        std::uint32_t state,
        std::vector<std::uint32_t> vertex_spirv,
        std::vector<std::uint32_t> fragment_spirv,
        const LoadedResourceManifest& es_manifest,
        const LoadedResourceManifest& ps_manifest,
        bool force_early_vertex,
        bool force_fragment_states,
        std::uint32_t force_fragment_first,
        std::uint32_t force_fragment_last) {
        return build_guest_descriptor_pipeline(
            state,
            std::make_shared<const std::vector<std::uint32_t>>(
                std::move(vertex_spirv)),
            std::make_shared<const std::vector<std::uint32_t>>(
                std::move(fragment_spirv)),
            es_manifest,
            ps_manifest,
            force_early_vertex,
            force_fragment_states,
            force_fragment_first,
            force_fragment_last);
    }

    GuestPipelineBuild build_guest_descriptor_pipeline(
        std::uint32_t state,
        SharedSpirv vertex_spirv,
        SharedSpirv fragment_spirv,
        const LoadedResourceManifest& es_manifest,
        const LoadedResourceManifest& ps_manifest,
        bool force_early_vertex,
        bool force_fragment_states,
        std::uint32_t force_fragment_first,
        std::uint32_t force_fragment_last) {
        // Which locations a module reads or writes, by storage class. A
        // pixel stage reading a location its vertex stage does not write is
        // undefined behaviour the validation layer reports without naming
        // the pipeline; this names the state.
        const auto locations = [](const std::vector<std::uint32_t>& words,
                                  std::uint32_t storage_class) {
            std::map<std::uint32_t, std::uint32_t> decorated;
            std::set<std::uint32_t> result;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> variables;
            for (std::size_t at = 5; at < words.size();) {
                const auto length = words[at] >> 16;
                const auto opcode = words[at] & 0xFFFFu;
                if (length == 0 || at + length > words.size()) {
                    break;
                }
                if (opcode == 71 && length >= 4 && words[at + 2] == 30) {
                    decorated[words[at + 1]] = words[at + 3];
                } else if (opcode == 59 && length >= 4) {
                    variables.push_back({words[at + 2], words[at + 3]});
                }
                at += length;
            }
            for (const auto& [id, storage] : variables) {
                const auto found = decorated.find(id);
                if (storage == storage_class && found != decorated.end()) {
                    result.insert(found->second);
                }
            }
            return result;
        };
        // Once for a pair of modules: they are shared between states now,
        // and parsing both for every state a frame registers was a tenth
        // of the worker.
        static std::set<std::pair<const void*, const void*>> checked_pairs;
        if (checked_pairs.size() > 65536) {
            checked_pairs.clear();
        }
        if (checked_pairs.insert({vertex_spirv.get(), fragment_spirv.get()})
                .second) {
            const auto outputs = locations(*vertex_spirv, 3);
            const auto inputs = locations(*fragment_spirv, 1);
            std::string missing;
            for (const auto location : inputs) {
                if (outputs.count(location) == 0) {
                    missing += std::to_string(location) + " ";
                }
            }
            if (!missing.empty()) {
                static std::atomic<std::uint32_t> shown{0};
                if (shown.fetch_add(1, std::memory_order_relaxed) < 64) {
                    runtime_trace(
                        "native_gpu.stage_interface_mismatch state=%u "
                        "vertex_outputs=%zu pixel_inputs=%zu missing=%s\n",
                        state,
                        outputs.size(),
                        inputs.size(),
                        missing.c_str());
                }
            }
        }
        if (force_early_vertex && state == 11) {
            vertex_spirv = std::make_shared<const std::vector<std::uint32_t>>(
                ps5gpu::fixed_spirv::fullscreen_vertex());
            runtime_trace(
                "native_gpu.guest_vertex_override state=%u\n",
                state);
        }
        if (force_fragment_states &&
            state >= force_fragment_first &&
            state <= force_fragment_last) {
            fragment_spirv =
                std::make_shared<const std::vector<std::uint32_t>>(
                    ps5gpu::fixed_spirv::solid_green_fragment());
            runtime_trace(
                "native_gpu.guest_fragment_override state=%u\n",
                state);
        }
        // A diagnostic: one state's fragment module from a file, so a
        // single shader can be taken apart in a frame whose other passes
        // still run as they are.
        static const auto override_state = [] {
            const auto* value =
                std::getenv("PS5GPU_NATIVE_FRAGMENT_OVERRIDE_STATE");
            return value == nullptr
                ? 0u
                : static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0));
        }();
        if (override_state != 0 && state == override_state) {
            std::vector<std::uint32_t> words;
            if (const auto* path =
                    std::getenv("PS5GPU_NATIVE_FRAGMENT_OVERRIDE_STATE_SPV");
                path != nullptr && load_spirv_file(path, words)) {
                fragment_spirv =
                    std::make_shared<const std::vector<std::uint32_t>>(
                        std::move(words));
                runtime_trace(
                    "native_gpu.guest_fragment_file_override state=%u\n",
                    state);
            }
            words.clear();
            if (const auto* path =
                    std::getenv("PS5GPU_NATIVE_VERTEX_OVERRIDE_STATE_SPV");
                path != nullptr && load_spirv_file(path, words)) {
                vertex_spirv =
                    std::make_shared<const std::vector<std::uint32_t>>(
                        std::move(words));
                runtime_trace(
                    "native_gpu.guest_vertex_file_override state=%u\n",
                    state);
            }
        }

        GuestDescriptorPipeline guest;
        if (!append_guest_manifest(
                es_manifest,
                VK_SHADER_STAGE_VERTEX_BIT,
                guest) ||
            !append_guest_manifest(
                ps_manifest,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                guest) ||
            guest.buffers.size() > 4096 ||
            guest.images.size() > 4096) {
            runtime_trace(
                "native_gpu.guest_descriptor_resources_invalid "
                "state=%u\n",
                state);
            return GuestPipelineBuild::InvalidManifest;
        }
        std::sort(
            guest.images.begin(),
            guest.images.end(),
            [](const GuestImageResource& left,
               const GuestImageResource& right) {
                return left.binding < right.binding;
            });
        for (std::size_t index = 0;
             index < guest.buffers.size();
             ++index) {
            auto& buffer = guest.buffers[index];
            if (buffer.size == 0) {
                buffer.descriptor_index =
                    static_cast<std::uint32_t>(index);
                buffer.size = 4;
            }
        }

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.reserve(guest.images.size() + 1);
        if (!guest.buffers.empty()) {
            VkDescriptorSetLayoutBinding buffer_binding = {};
            buffer_binding.binding = 0;
            buffer_binding.descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            buffer_binding.descriptorCount =
                static_cast<std::uint32_t>(guest.buffers.size());
            buffer_binding.stageFlags =
                VK_SHADER_STAGE_VERTEX_BIT |
                VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings.push_back(buffer_binding);
        }
        std::uint32_t sampled_count = 0;
        std::uint32_t storage_image_count = 0;
        for (const auto& image : guest.images) {
            VkDescriptorSetLayoutBinding image_binding = {};
            image_binding.binding = image.binding;
            image_binding.descriptorType =
                (image.flags &
                    PS5GPU_RESOURCE_IMAGE_STORAGE) != 0
                ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            image_binding.descriptorCount = 1;
            image_binding.stageFlags = image.stages;
            bindings.push_back(image_binding);
            if (image_binding.descriptorType ==
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                ++storage_image_count;
            } else {
                ++sampled_count;
            }
        }
        if (bindings.empty()) {
            return GuestPipelineBuild::InvalidManifest;
        }

        // How the layout was declared, which is what decides whether a
        // pipeline built against another state's layout can be bound
        // with this one. Never zero.
        std::uint64_t layout_signature = 0x6C61796F75740001ull;
        for (const auto& binding : bindings) {
            for (const std::uint64_t part :
                 {static_cast<std::uint64_t>(binding.binding),
                  static_cast<std::uint64_t>(binding.descriptorType),
                  static_cast<std::uint64_t>(binding.descriptorCount),
                  static_cast<std::uint64_t>(binding.stageFlags)}) {
                layout_signature =
                    (layout_signature ^ part) * 0x100000001B3ull;
            }
        }
        layout_signature |= 1;

        auto result = VK_SUCCESS;
        if (const auto shared = shared_graphics_layouts.find(layout_signature);
            shared != shared_graphics_layouts.end()) {
            guest.descriptor_set_layout = shared->second.first;
            guest.pipeline_layout = shared->second.second;
            guest.shares_layout = true;
        } else {
            VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {};
            descriptor_layout_info.sType =
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descriptor_layout_info.bindingCount =
                static_cast<std::uint32_t>(bindings.size());
            descriptor_layout_info.pBindings = bindings.data();
            result = create_descriptor_set_layout(
                device,
                &descriptor_layout_info,
                nullptr,
                &guest.descriptor_set_layout);
            VkPipelineLayoutCreateInfo layout_info = {};
            layout_info.sType =
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layout_info.setLayoutCount = 1;
            layout_info.pSetLayouts = &guest.descriptor_set_layout;
            if (result == VK_SUCCESS) {
                result = create_pipeline_layout(
                    device,
                    &layout_info,
                    nullptr,
                    &guest.pipeline_layout);
            }
            if (result == VK_SUCCESS) {
                shared_graphics_layouts.emplace(
                    layout_signature,
                    std::make_pair(
                        guest.descriptor_set_layout, guest.pipeline_layout));
                guest.shares_layout = true;
            }
        }
        // One shared copy both variants hold.
        const auto& shared_vertex = vertex_spirv;
        const auto& shared_fragment = fragment_spirv;
        if (result == VK_SUCCESS &&
            !describe_guest_pipeline(
                guest.graphics_pipeline,
                shared_vertex,
                shared_fragment,
                guest.pipeline_layout,
                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                layout_signature)) {
            result = VK_ERROR_INITIALIZATION_FAILED;
        }
        if (result == VK_SUCCESS &&
            !describe_guest_pipeline(
                guest.graphics_strip_pipeline,
                shared_vertex,
                shared_fragment,
                guest.pipeline_layout,
                rect_list_as_fan()
                    ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
                    : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                layout_signature,
                guest.graphics_pipeline.vertex_hash,
                guest.graphics_pipeline.fragment_hash)) {
            result = VK_ERROR_INITIALIZATION_FAILED;
        }
        if (result == VK_SUCCESS) {
            guest.graphics = true;
            guest.pipeline = ensure_guest_pipeline(
                guest.graphics_pipeline,
                VK_FORMAT_R8G8B8A8_UNORM);
            guest.strip_pipeline = ensure_guest_pipeline(
                guest.graphics_strip_pipeline,
                VK_FORMAT_R8G8B8A8_UNORM);
        }

        std::vector<VkDescriptorPoolSize> pool_sizes;
        if (!guest.buffers.empty()) {
            pool_sizes.push_back({
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                static_cast<std::uint32_t>(guest.buffers.size()),
            });
        }
        if (sampled_count != 0) {
            pool_sizes.push_back({
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                sampled_count,
            });
        }
        if (storage_image_count != 0) {
            pool_sizes.push_back({
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                storage_image_count,
            });
        }
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount =
            static_cast<std::uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        if (result == VK_SUCCESS) {
            result = create_descriptor_pool(
                device,
                &pool_info,
                nullptr,
                &guest.descriptor_pool);
        }

        VkDescriptorSetAllocateInfo allocate_info = {};
        allocate_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate_info.descriptorPool = guest.descriptor_pool;
        allocate_info.descriptorSetCount = 1;
        allocate_info.pSetLayouts =
            &guest.descriptor_set_layout;
        if (result == VK_SUCCESS) {
            result = allocate_descriptor_sets(
                device,
                &allocate_info,
                &guest.descriptor_set);
        }
        for (auto& buffer : guest.buffers) {
            // A buffer the shader only reads is bound from the frame arena,
            // so its own copy is made only if that ever fails. Made up front
            // for every state, they were twelve of the twenty-six gigabytes
            // the process reached a few hundred frames into the scene,
            // after which allocations failed and the title fell over.
            if (frame_arena_enabled() &&
                (buffer.flags & PS5GPU_RESOURCE_BUFFER_WRITABLE) == 0) {
                continue;
            }
            if (result == VK_SUCCESS &&
                !create_host_storage_buffer(
                    buffer.size,
                    buffer.buffer,
                    buffer.memory)) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.guest_descriptor_pipeline_failed "
                "state=%u result=%d\n",
                state,
                static_cast<int>(result));
            destroy_guest_descriptor_pipeline(guest);
            return GuestPipelineBuild::Failed;
        }

        guest.last_used_frame = graphics_frame_counter;
        guest_descriptor_pipelines.emplace(
            state,
            std::move(guest));
        runtime_trace(
            "native_gpu.guest_descriptor_pipeline_ready "
            "state=%u buffers=%llu images=%llu "
            "sampled=%u storage_images=%u\n",
            state,
            static_cast<unsigned long long>(
                guest_descriptor_pipelines[state].buffers.size()),
            static_cast<unsigned long long>(
                guest_descriptor_pipelines[state].images.size()),
            sampled_count,
            storage_image_count);
        return GuestPipelineBuild::Ready;
    }

    bool create_guest_descriptor_pipelines() {
        if (shader_search_directories().empty()) {
            return true;
        }
        const auto force_early_fragment =
            environment_flag_enabled(
                "PS5GPU_NATIVE_FORCE_EARLY_FRAGMENT");
        const auto force_early_vertex =
            environment_flag_enabled(
                "PS5GPU_NATIVE_FORCE_EARLY_VERTEX");
        // Which states the solid-colour fragment override applies to.
        // PS5GPU_NATIVE_FORCE_EARLY_FRAGMENT keeps its historical 11-14 range;
        // PS5GPU_NATIVE_FORCE_FRAGMENT_STATES overrides it with "N" or "N-M"
        // so any draw can be bisected without rebuilding.
        std::uint32_t force_fragment_first = 11;
        std::uint32_t force_fragment_last = 14;
        auto force_fragment_states = force_early_fragment;
        if (const auto* value =
                std::getenv("PS5GPU_NATIVE_FORCE_FRAGMENT_STATES");
            value != nullptr && value[0] != '\0') {
            char* end = nullptr;
            const auto first = std::strtoul(value, &end, 0);
            if (end != value && first >= 1 && first <= 4096) {
                auto last = first;
                if (end != nullptr && end[0] == '-') {
                    const auto* range_end = end + 1;
                    char* tail = nullptr;
                    const auto parsed =
                        std::strtoul(range_end, &tail, 0);
                    if (tail != range_end &&
                        parsed >= first &&
                        parsed <= 4096) {
                        last = parsed;
                    }
                }
                force_fragment_first =
                    static_cast<std::uint32_t>(first);
                force_fragment_last =
                    static_cast<std::uint32_t>(last);
                force_fragment_states = true;
                runtime_trace(
                    "native_gpu.force_fragment_states first=%u last=%u\n",
                    force_fragment_first,
                    force_fragment_last);
            }
        }
        std::uint32_t loaded = 0;
        std::uint32_t missing_manifest = 0;
        std::uint32_t invalid_manifest = 0;
        std::uint32_t failed = 0;
        for (std::uint32_t state = 1; state <= 4096; ++state) {
            std::vector<std::uint32_t> vertex_spirv;
            std::vector<std::uint32_t> fragment_spirv;
            const auto stem = "state-" + std::to_string(state);
            if (!load_spirv_file(
                    resolve_shader_path(stem + "-es.spv"),
                    vertex_spirv) ||
                !load_spirv_file(
                    resolve_shader_path(stem + "-ps.spv"),
                    fragment_spirv)) {
                continue;
            }
            if (!spirv_uses_descriptors(vertex_spirv) &&
                !spirv_uses_descriptors(fragment_spirv)) {
                continue;
            }

            LoadedResourceManifest es_manifest;
            LoadedResourceManifest ps_manifest;
            const auto es_path =
                resolve_shader_path(stem + "-es.resources.bin");
            const auto ps_path =
                resolve_shader_path(stem + "-ps.resources.bin");
            if (!std::filesystem::exists(es_path) ||
                !std::filesystem::exists(ps_path)) {
                ++missing_manifest;
                continue;
            }
            if (!load_resource_manifest_file(es_path, es_manifest) ||
                !load_resource_manifest_file(ps_path, ps_manifest) ||
                es_manifest.header.stage != PS5GPU_STAGE_VERTEX ||
                ps_manifest.header.stage != PS5GPU_STAGE_PIXEL) {
                ++invalid_manifest;
                runtime_trace(
                    "native_gpu.guest_descriptor_manifest_invalid "
                    "state=%u\n",
                    state);
                continue;
            }
            switch (build_guest_descriptor_pipeline(
                        state,
                        std::move(vertex_spirv),
                        std::move(fragment_spirv),
                        es_manifest,
                        ps_manifest,
                        force_early_vertex,
                        force_fragment_states,
                        force_fragment_first,
                        force_fragment_last)) {
                case GuestPipelineBuild::Ready: ++loaded; break;
                case GuestPipelineBuild::MissingManifest:
                    ++missing_manifest;
                    break;
                case GuestPipelineBuild::InvalidManifest:
                    ++invalid_manifest;
                    break;
                case GuestPipelineBuild::Failed: ++failed; break;
            }
        }
        runtime_trace(
            "native_gpu.guest_descriptor_pipelines_ready "
            "loaded=%u missing_manifest=%u invalid_manifest=%u "
            "failed=%u\n",
            loaded,
            missing_manifest,
            invalid_manifest,
            failed);
        return true;
    }

    bool create_resource_free_guest_pipelines() {
        if (shader_search_directories().empty()) {
            return true;
        }
        std::uint32_t loaded = 0;
        std::uint32_t skipped_descriptors = 0;
        std::uint32_t failed = 0;
        for (std::uint32_t state = 1; state <= 4096; ++state) {
            std::vector<std::uint32_t> vertex_spirv;
            std::vector<std::uint32_t> fragment_spirv;
            const auto stem = "state-" + std::to_string(state);
            if (!load_spirv_file(
                    resolve_shader_path(stem + "-es.spv"),
                    vertex_spirv) ||
                !load_spirv_file(
                    resolve_shader_path(stem + "-ps.spv"),
                    fragment_spirv)) {
                continue;
            }
            if (spirv_uses_descriptors(vertex_spirv) ||
                spirv_uses_descriptors(fragment_spirv)) {
                ++skipped_descriptors;
                continue;
            }
            GuestPipelineVariants pipeline;
            GuestPipelineVariants strip_pipeline;
            if (!describe_guest_pipeline(
                    pipeline,
                    vertex_spirv,
                    fragment_spirv,
                    pipeline_layout) ||
                !describe_guest_pipeline(
                    strip_pipeline,
                    vertex_spirv,
                    fragment_spirv,
                    pipeline_layout,
                    rect_list_as_fan()
                        ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)) {
                destroy_guest_pipeline_variants(pipeline);
                destroy_guest_pipeline_variants(strip_pipeline);
                ++failed;
                runtime_trace(
                    "native_gpu.guest_pipeline_failed state=%u\n",
                    state);
                continue;
            }
            resource_free_guest_pipelines.emplace(
                state,
                std::move(pipeline));
            resource_free_guest_strip_pipelines.emplace(
                state,
                std::move(strip_pipeline));
            ++loaded;
        }
        runtime_trace(
            "native_gpu.guest_pipelines_ready loaded=%u "
            "descriptor_states=%u failed=%u\n",
            loaded,
            skipped_descriptors,
            failed);
        return true;
    }

    bool create_astro_post_pipelines() {
        if (shader_search_directories().empty()) {
            return true;
        }
        astro_post_copy_state =
            astro_post_copy_state_from_environment();
        std::uint32_t loaded = 0;
        for (std::size_t pass_index = 0;
             pass_index < kAstroPostPasses.size();
             ++pass_index) {
            const auto& config = kAstroPostPasses[pass_index];
            auto& pass = astro_post_pipelines[pass_index];
            std::vector<std::uint32_t> vertex_spirv;
            std::vector<std::uint32_t> fragment_spirv;
            const auto stem =
                "state-" + std::to_string(config.shader_state_file);
            if (!load_spirv_file(
                    resolve_shader_path(stem + "-es.spv"),
                    vertex_spirv) ||
                !load_spirv_file(
                    resolve_shader_path(stem + "-ps.spv"),
                    fragment_spirv)) {
                runtime_trace(
                    "native_gpu.astro_post_spirv_missing "
                    "state=%u dir=%s\n",
                    config.shader_state_file,
                    shader_search_summary().c_str());
                continue;
            }
            const auto copy_source =
                static_cast<int>(config.shader_state_file) ==
                astro_post_copy_state;
            if (copy_source) {
                vertex_spirv =
                    ps5gpu::fixed_spirv::fullscreen_vertex_uv();
                fragment_spirv =
                    ps5gpu::fixed_spirv::sampled_texture_fragment(1);
            }

            std::vector<VkDescriptorSetLayoutBinding> bindings(
                static_cast<std::size_t>(config.image_count) + 1);
            bindings[0].binding = 0;
            bindings[0].descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[0].descriptorCount =
                static_cast<std::uint32_t>(
                    kAstroPostStorageBufferCount);
            bindings[0].stageFlags =
                VK_SHADER_STAGE_VERTEX_BIT |
                VK_SHADER_STAGE_FRAGMENT_BIT;
            for (std::uint32_t image = 0;
                 image < config.image_count;
                 ++image) {
                auto& binding = bindings[image + 1];
                binding.binding = image + 1;
                binding.descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                binding.descriptorCount = 1;
                binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {};
            descriptor_layout_info.sType =
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descriptor_layout_info.bindingCount =
                static_cast<std::uint32_t>(bindings.size());
            descriptor_layout_info.pBindings = bindings.data();
            auto result = create_descriptor_set_layout(
                device,
                &descriptor_layout_info,
                nullptr,
                &pass.descriptor_set_layout);

            VkPipelineLayoutCreateInfo layout_info = {};
            layout_info.sType =
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layout_info.setLayoutCount = 1;
            layout_info.pSetLayouts = &pass.descriptor_set_layout;
            if (result == VK_SUCCESS) {
                result = create_pipeline_layout(
                    device,
                    &layout_info,
                    nullptr,
                    &pass.pipeline_layout);
            }
            if (result == VK_SUCCESS &&
                !describe_guest_pipeline(
                    pass.pipeline_variants,
                    vertex_spirv,
                    fragment_spirv,
                    pass.pipeline_layout,
                    rect_list_as_fan()
                        ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)) {
                result = VK_ERROR_INITIALIZATION_FAILED;
            }
            if (result == VK_SUCCESS) {
                pass.pipeline = ensure_guest_pipeline(
                    pass.pipeline_variants,
                    VK_FORMAT_R8G8B8A8_UNORM);
            }

            std::array<VkDescriptorPoolSize, 2> pool_sizes = {};
            pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            pool_sizes[0].descriptorCount =
                static_cast<std::uint32_t>(
                    kAstroPostStorageBufferCount);
            pool_sizes[1].type =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            pool_sizes[1].descriptorCount = config.image_count;
            VkDescriptorPoolCreateInfo pool_info = {};
            pool_info.sType =
                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_info.maxSets = 1;
            pool_info.poolSizeCount =
                static_cast<std::uint32_t>(pool_sizes.size());
            pool_info.pPoolSizes = pool_sizes.data();
            if (result == VK_SUCCESS) {
                result = create_descriptor_pool(
                    device,
                    &pool_info,
                    nullptr,
                    &pass.descriptor_pool);
            }

            VkDescriptorSetAllocateInfo allocate_info = {};
            allocate_info.sType =
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate_info.descriptorPool = pass.descriptor_pool;
            allocate_info.descriptorSetCount = 1;
            allocate_info.pSetLayouts = &pass.descriptor_set_layout;
            if (result == VK_SUCCESS) {
                result = allocate_descriptor_sets(
                    device,
                    &allocate_info,
                    &pass.descriptor_set);
            }
            for (std::size_t buffer = 0;
                 result == VK_SUCCESS &&
                 buffer < pass.storage_buffers.size();
                 ++buffer) {
                if (!create_host_storage_buffer(
                        pass.storage_sizes[buffer],
                        pass.storage_buffers[buffer],
                        pass.storage_memory[buffer])) {
                    result = VK_ERROR_OUT_OF_HOST_MEMORY;
                }
            }
            if (result != VK_SUCCESS) {
                runtime_trace(
                    "native_gpu.astro_post_pipeline_failed "
                    "state=%u result=%d\n",
                    config.shader_state_file,
                    static_cast<int>(result));
                return false;
            }
            ++loaded;
            runtime_trace(
                "native_gpu.astro_post_pipeline_ready "
                "state=%u copy_source=%u buffers=%llu images=%u "
                "vertex_bytes=%llu fragment_bytes=%llu\n",
                config.shader_state_file,
                copy_source ? 1u : 0u,
                static_cast<unsigned long long>(
                    pass.storage_buffers.size()),
                config.image_count,
                static_cast<unsigned long long>(
                    vertex_spirv.size() * sizeof(std::uint32_t)),
                static_cast<unsigned long long>(
                    fragment_spirv.size() * sizeof(std::uint32_t)));
        }
        runtime_trace(
            "native_gpu.astro_post_pipelines_ready loaded=%u/%llu\n",
            loaded,
            static_cast<unsigned long long>(
                kAstroPostPasses.size()));
        return true;
    }

    bool create_astro_state28_pipeline() {
        if (shader_search_directories().empty()) {
            return true;
        }
        std::vector<std::uint32_t> vertex_spirv;
        std::vector<std::uint32_t> fragment_spirv;
        if (!load_spirv_file(
                resolve_shader_path("state-28-es.spv"),
                vertex_spirv) ||
            !load_spirv_file(
                resolve_shader_path("state-28-ps.spv"),
                fragment_spirv)) {
            runtime_trace(
                "native_gpu.astro_state28_spirv_missing dir=%s\n",
                shader_search_summary().c_str());
            return true;
        }
        astro_state28_copy_image =
            astro_state28_copy_image_from_environment();
        if (astro_state28_copy_image >= 0) {
            vertex_spirv =
                ps5gpu::fixed_spirv::fullscreen_vertex_uv();
            fragment_spirv =
                ps5gpu::fixed_spirv::sampled_texture_fragment(
                    static_cast<std::uint32_t>(
                        astro_state28_copy_image + 1));
        }

        std::array<VkDescriptorSetLayoutBinding, 5> bindings = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount =
            static_cast<std::uint32_t>(
                astro_state28_storage_buffers.size());
        bindings[0].stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT;
        for (std::uint32_t index = 1; index < bindings.size(); ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {};
        descriptor_layout_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptor_layout_info.bindingCount =
            static_cast<std::uint32_t>(bindings.size());
        descriptor_layout_info.pBindings = bindings.data();
        auto result = create_descriptor_set_layout(
            device,
            &descriptor_layout_info,
            nullptr,
            &astro_state28_descriptor_set_layout);

        VkPipelineLayoutCreateInfo layout_info = {};
        layout_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &astro_state28_descriptor_set_layout;
        if (result == VK_SUCCESS) {
            result = create_pipeline_layout(
                device,
                &layout_info,
                nullptr,
                &astro_state28_pipeline_layout);
        }
        if (result == VK_SUCCESS &&
            !describe_guest_pipeline(
                astro_state28_pipeline_variants,
                vertex_spirv,
                fragment_spirv,
                astro_state28_pipeline_layout,
                rect_list_as_fan()
                        ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)) {
            result = VK_ERROR_INITIALIZATION_FAILED;
        }
        if (result == VK_SUCCESS) {
            astro_state28_pipeline = ensure_guest_pipeline(
                astro_state28_pipeline_variants,
                VK_FORMAT_R8G8B8A8_UNORM);
        }

        std::array<VkDescriptorPoolSize, 2> pool_sizes = {};
        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[0].descriptorCount =
            static_cast<std::uint32_t>(
                astro_state28_storage_buffers.size());
        pool_sizes[1].type =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_sizes[1].descriptorCount =
            static_cast<std::uint32_t>(
                kAstroState28ImageAddresses.size());
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount =
            static_cast<std::uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        if (result == VK_SUCCESS) {
            result = create_descriptor_pool(
                device,
                &pool_info,
                nullptr,
                &astro_state28_descriptor_pool);
        }

        VkDescriptorSetAllocateInfo allocate_info = {};
        allocate_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate_info.descriptorPool = astro_state28_descriptor_pool;
        allocate_info.descriptorSetCount = 1;
        allocate_info.pSetLayouts =
            &astro_state28_descriptor_set_layout;
        if (result == VK_SUCCESS) {
            result = allocate_descriptor_sets(
                device,
                &allocate_info,
                &astro_state28_descriptor_set);
        }
        for (std::size_t index = 0;
             result == VK_SUCCESS &&
             index < astro_state28_storage_buffers.size();
             ++index) {
            if (!create_host_storage_buffer(
                    astro_state28_storage_sizes[index],
                    astro_state28_storage_buffers[index],
                    astro_state28_storage_memory[index])) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.astro_state28_pipeline_failed result=%d\n",
                static_cast<int>(result));
            return false;
        }
        runtime_trace(
            "native_gpu.astro_state28_pipeline_ready "
            "copy_image=%d "
            "vertex_bytes=%llu fragment_bytes=%llu "
            "buffers=%llu images=%llu\n",
            astro_state28_copy_image,
            static_cast<unsigned long long>(
                vertex_spirv.size() * sizeof(std::uint32_t)),
            static_cast<unsigned long long>(
                fragment_spirv.size() * sizeof(std::uint32_t)),
            static_cast<unsigned long long>(
                astro_state28_storage_buffers.size()),
            static_cast<unsigned long long>(
                kAstroState28ImageAddresses.size()));
        return true;
    }

    bool create_astro_state29_pipeline() {
        if (shader_search_directories().empty()) {
            return true;
        }
        std::vector<std::uint32_t> vertex_spirv;
        std::vector<std::uint32_t> fragment_spirv;
        if (!load_spirv_file(
                resolve_shader_path("state-29-es.spv"),
                vertex_spirv) ||
            !load_spirv_file(
                resolve_shader_path("state-29-ps.spv"),
                fragment_spirv)) {
            runtime_trace(
                "native_gpu.astro_state29_spirv_missing dir=%s\n",
                shader_search_summary().c_str());
            return true;
        }
        astro_state29_mode =
            astro_state29_mode_from_environment();
        switch (astro_state29_mode) {
        case RealCompositionMode::Real:
            break;
        case RealCompositionMode::RealEsGreen:
            fragment_spirv =
                ps5gpu::fixed_spirv::solid_green_fragment();
            break;
        case RealCompositionMode::FullscreenRealPs:
            vertex_spirv =
                ps5gpu::fixed_spirv::fullscreen_vertex_vec4_attribute();
            break;
        case RealCompositionMode::FullscreenCopySource:
            vertex_spirv =
                ps5gpu::fixed_spirv::fullscreen_vertex_uv();
            fragment_spirv =
                ps5gpu::fixed_spirv::sampled_texture_fragment(1);
            break;
        }

        std::array<VkDescriptorSetLayoutBinding, 2> bindings = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount =
            static_cast<std::uint32_t>(
                astro_state29_storage_buffers.size());
        bindings[0].stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {};
        descriptor_layout_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptor_layout_info.bindingCount =
            static_cast<std::uint32_t>(bindings.size());
        descriptor_layout_info.pBindings = bindings.data();
        auto result = create_descriptor_set_layout(
            device,
            &descriptor_layout_info,
            nullptr,
            &astro_state29_descriptor_set_layout);

        VkPipelineLayoutCreateInfo layout_info = {};
        layout_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts =
            &astro_state29_descriptor_set_layout;
        if (result == VK_SUCCESS) {
            result = create_pipeline_layout(
                device,
                &layout_info,
                nullptr,
                &astro_state29_pipeline_layout);
        }
        if (result == VK_SUCCESS &&
            !describe_guest_pipeline(
                astro_state29_pipeline_variants,
                vertex_spirv,
                fragment_spirv,
                astro_state29_pipeline_layout,
                rect_list_as_fan()
                        ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)) {
            result = VK_ERROR_INITIALIZATION_FAILED;
        }
        if (result == VK_SUCCESS) {
            astro_state29_pipeline = ensure_guest_pipeline(
                astro_state29_pipeline_variants,
                VK_FORMAT_R8G8B8A8_UNORM);
        }

        std::array<VkDescriptorPoolSize, 2> pool_sizes = {};
        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[0].descriptorCount =
            static_cast<std::uint32_t>(
                astro_state29_storage_buffers.size());
        pool_sizes[1].type =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_sizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount =
            static_cast<std::uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        if (result == VK_SUCCESS) {
            result = create_descriptor_pool(
                device,
                &pool_info,
                nullptr,
                &astro_state29_descriptor_pool);
        }

        VkDescriptorSetAllocateInfo allocate_info = {};
        allocate_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate_info.descriptorPool =
            astro_state29_descriptor_pool;
        allocate_info.descriptorSetCount = 1;
        allocate_info.pSetLayouts =
            &astro_state29_descriptor_set_layout;
        if (result == VK_SUCCESS) {
            result = allocate_descriptor_sets(
                device,
                &allocate_info,
                &astro_state29_descriptor_set);
        }
        for (std::size_t index = 0;
             result == VK_SUCCESS &&
             index < astro_state29_storage_buffers.size();
             ++index) {
            if (!create_host_storage_buffer(
                    astro_state29_storage_sizes[index],
                    astro_state29_storage_buffers[index],
                    astro_state29_storage_memory[index])) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.astro_state29_pipeline_failed result=%d\n",
                static_cast<int>(result));
            return false;
        }
        runtime_trace(
            "native_gpu.astro_state29_pipeline_ready "
            "mode=%s "
            "vertex_bytes=%llu fragment_bytes=%llu "
            "buffers=3 images=1\n",
            real_composition_mode_name(astro_state29_mode),
            static_cast<unsigned long long>(
                vertex_spirv.size() * sizeof(std::uint32_t)),
            static_cast<unsigned long long>(
                fragment_spirv.size() * sizeof(std::uint32_t)));
        return true;
    }

    bool create_sampled_pipeline() {
        VkDescriptorSetLayoutBinding texture_binding = {};
        texture_binding.binding = 0;
        texture_binding.descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texture_binding.descriptorCount = 1;
        texture_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {};
        descriptor_layout_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptor_layout_info.bindingCount = 1;
        descriptor_layout_info.pBindings = &texture_binding;
        auto result = create_descriptor_set_layout(
            device,
            &descriptor_layout_info,
            nullptr,
            &sampled_descriptor_set_layout);

        VkDescriptorPoolSize pool_size = {};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = kMaximumSampledSurfaces;
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = kMaximumSampledSurfaces;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        if (result == VK_SUCCESS) {
            result = create_descriptor_pool(
                device,
                &pool_info,
                nullptr,
                &sampled_descriptor_pool);
        }

        VkSamplerCreateInfo sampler_info = {};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.minLod = 0.0f;
        sampler_info.maxLod = 0.0f;
        if (result == VK_SUCCESS) {
            result = create_sampler(
                device,
                &sampler_info,
                nullptr,
                &sampled_sampler);
        }

        VkPipelineLayoutCreateInfo layout_info = {};
        layout_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &sampled_descriptor_set_layout;
        if (result == VK_SUCCESS) {
            result = create_pipeline_layout(
                device,
                &layout_info,
                nullptr,
                &sampled_pipeline_layout);
        }

        const auto vertex_spirv =
            ps5gpu::fixed_spirv::fullscreen_vertex_uv();
        const auto fragment_spirv =
            ps5gpu::fixed_spirv::sampled_texture_fragment();
        if (result != VK_SUCCESS ||
            !describe_guest_pipeline(
                sampled_pipeline_variants,
                vertex_spirv,
                fragment_spirv,
                sampled_pipeline_layout)) {
            runtime_trace(
                "native_gpu.vulkan_sampled_pipeline_failed result=%d\n",
                static_cast<int>(result));
            return false;
        }
        sampled_pipeline = ensure_guest_pipeline(
            sampled_pipeline_variants,
            VK_FORMAT_R8G8B8A8_UNORM);
        runtime_trace(
            "native_gpu.vulkan_sampled_pipeline_ready "
            "vertex_words=%llu fragment_words=%llu descriptors=%u\n",
            static_cast<unsigned long long>(vertex_spirv.size()),
            static_cast<unsigned long long>(fragment_spirv.size()),
            kMaximumSampledSurfaces);
        return true;
    }

    // The composition pipeline reads a vertex buffer, so it cannot share the
    // resource-free builder's fixed state; it gets its own per-format cache.
    VkPipeline ensure_real_composition_pipeline(VkFormat target_format) {
        if (const auto existing =
                real_composition_pipelines.find(target_format);
            existing != real_composition_pipelines.end()) {
            return existing->second;
        }
        const auto& vertex_spirv = real_composition_vertex_spirv;
        const auto& fragment_spirv = real_composition_fragment_spirv;
        if (vertex_spirv.empty() || fragment_spirv.empty() ||
            real_pipeline_layout == VK_NULL_HANDLE) {
            runtime_trace(
                "native_gpu.real_composition_pipeline_unbuildable "
                "vertex_words=%llu fragment_words=%llu layout=%u\n",
                static_cast<unsigned long long>(vertex_spirv.size()),
                static_cast<unsigned long long>(fragment_spirv.size()),
                real_pipeline_layout != VK_NULL_HANDLE ? 1u : 0u);
            return VK_NULL_HANDLE;
        }
        const auto target_render_pass =
            ensure_render_pass(target_format, false);
        if (target_render_pass == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }
        VkPipeline created = VK_NULL_HANDLE;
        auto result = VK_SUCCESS;
        VkShaderModuleCreateInfo vertex_info = {};
        vertex_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vertex_info.codeSize =
            vertex_spirv.size() * sizeof(std::uint32_t);
        vertex_info.pCode = vertex_spirv.data();
        VkShaderModuleCreateInfo fragment_info = vertex_info;
        fragment_info.codeSize =
            fragment_spirv.size() * sizeof(std::uint32_t);
        fragment_info.pCode = fragment_spirv.data();
        VkShaderModule vertex_module = VK_NULL_HANDLE;
        VkShaderModule fragment_module = VK_NULL_HANDLE;
        if (result == VK_SUCCESS) {
            result = create_shader_module(
                device,
                &vertex_info,
                nullptr,
                &vertex_module);
        }
        if (result == VK_SUCCESS) {
            result = create_shader_module(
                device,
                &fragment_info,
                nullptr,
                &fragment_module);
        }
        if (result != VK_SUCCESS) {
            if (fragment_module != VK_NULL_HANDLE) {
                destroy_shader_module(device, fragment_module, nullptr);
            }
            if (vertex_module != VK_NULL_HANDLE) {
                destroy_shader_module(device, vertex_module, nullptr);
            }
            runtime_trace(
                "native_gpu.real_composition_shader_failed result=%d\n",
                static_cast<int>(result));
            return VK_NULL_HANDLE;
        }

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_module;
        stages[0].pName = "main";
        stages[1].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_module;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertex_input = {};
        vertex_input.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkVertexInputBindingDescription vertex_binding = {};
        vertex_binding.binding = 0;
        vertex_binding.stride = 24;
        vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        std::array<VkVertexInputAttributeDescription, 2>
            vertex_attributes = {};
        vertex_attributes[0].location = 0;
        vertex_attributes[0].binding = 0;
        vertex_attributes[0].format =
            VK_FORMAT_R32G32B32A32_SFLOAT;
        vertex_attributes[0].offset = 0;
        vertex_attributes[1].location = 1;
        vertex_attributes[1].binding = 0;
        vertex_attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
        vertex_attributes[1].offset = 16;
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &vertex_binding;
        vertex_input.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(vertex_attributes.size());
        vertex_input.pVertexAttributeDescriptions =
            vertex_attributes.data();
        VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
        input_assembly.sType =
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport_state = {};
        viewport_state.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterization = {};
        rasterization.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType =
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment = {};
        blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;
        apply_forced_blend(blend_attachment);
        VkPipelineColorBlendStateCreateInfo blend = {};
        blend.sType =
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        const VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };
        VkPipelineDynamicStateCreateInfo dynamic = {};
        dynamic.sType =
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount =
            static_cast<std::uint32_t>(std::size(dynamic_states));
        dynamic.pDynamicStates = dynamic_states;
        VkGraphicsPipelineCreateInfo pipeline_info = {};
        pipeline_info.sType =
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = stages;
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = real_pipeline_layout;
        pipeline_info.renderPass = target_render_pass;
        pipeline_info.subpass = 0;
        result = create_graphics_pipelines(
            device,
            pipeline_cache,
            1,
            &pipeline_info,
            nullptr,
            &created);
        destroy_shader_module(device, fragment_module, nullptr);
        destroy_shader_module(device, vertex_module, nullptr);
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.real_composition_pipeline_failed result=%d "
                "format=%s\n",
                static_cast<int>(result),
                render_surface_format_name(target_format));
            created = VK_NULL_HANDLE;
        }
        // Remember a failure too, so a format that cannot be built is not
        // retried once per frame for the rest of the run.
        real_composition_pipelines.emplace(target_format, created);
        return created;
    }

    bool create_real_composition_pipeline() {
        if (shader_search_directories().empty()) {
            runtime_trace(
                "native_gpu.real_composition_pipeline_disabled\n");
            return true;
        }
        real_composition_enabled =
            environment_flag_enabled("PS5GPU_NATIVE_REAL_COMPOSITION");
        real_composition_mode =
            real_composition_mode_from_environment();
        real_es_buffer_mode =
            real_es_buffer_mode_from_environment();
        const auto load_spirv = [](
            const std::filesystem::path& path,
            std::vector<std::uint32_t>& words) {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) {
                return false;
            }
            const auto size = input.tellg();
            if (size <= 0 ||
                static_cast<std::uint64_t>(size) %
                    sizeof(std::uint32_t) != 0) {
                return false;
            }
            words.resize(
                static_cast<std::size_t>(size) /
                sizeof(std::uint32_t));
            input.seekg(0);
            input.read(
                reinterpret_cast<char*>(words.data()),
                size);
            return static_cast<bool>(input) &&
                !words.empty() &&
                words[0] == 0x07230203u;
        };

        std::vector<std::uint32_t> vertex_spirv;
        std::vector<std::uint32_t> fragment_spirv;
        if (!load_spirv(
                resolve_shader_path("state-30-es.spv"),
                vertex_spirv) ||
            !load_spirv(
                resolve_shader_path("state-30-ps.spv"),
                fragment_spirv)) {
            runtime_trace(
                "native_gpu.real_composition_spirv_missing dir=%s\n",
                shader_search_summary().c_str());
            return false;
        }
        switch (real_composition_mode) {
        case RealCompositionMode::Real:
            break;
        case RealCompositionMode::RealEsGreen:
            fragment_spirv =
                ps5gpu::fixed_spirv::solid_green_fragment();
            break;
        case RealCompositionMode::FullscreenRealPs:
            vertex_spirv =
                ps5gpu::fixed_spirv::fullscreen_vertex_vec4_attribute();
            break;
        case RealCompositionMode::FullscreenCopySource:
            vertex_spirv =
                ps5gpu::fixed_spirv::fullscreen_vertex_uv();
            fragment_spirv =
                ps5gpu::fixed_spirv::sampled_texture_fragment(1);
            break;
        }

        std::array<VkDescriptorSetLayoutBinding, 4> bindings = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount =
            static_cast<std::uint32_t>(real_storage_buffers.size());
        bindings[0].stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT;
        for (std::uint32_t index = 1; index < bindings.size(); ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {};
        descriptor_layout_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptor_layout_info.bindingCount =
            static_cast<std::uint32_t>(bindings.size());
        descriptor_layout_info.pBindings = bindings.data();
        auto result = create_descriptor_set_layout(
            device,
            &descriptor_layout_info,
            nullptr,
            &real_descriptor_set_layout);

        VkPipelineLayoutCreateInfo layout_info = {};
        layout_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &real_descriptor_set_layout;
        if (result == VK_SUCCESS) {
            result = create_pipeline_layout(
                device,
                &layout_info,
                nullptr,
                &real_pipeline_layout);
        }

        real_composition_vertex_spirv = vertex_spirv;
        real_composition_fragment_spirv = fragment_spirv;
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.real_composition_pipeline_failed result=%d\n",
                static_cast<int>(result));
            return false;
        }
        real_composition_pipeline =
            ensure_real_composition_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
        if (real_composition_pipeline == VK_NULL_HANDLE) {
            return false;
        }

        std::array<VkDescriptorPoolSize, 2> pool_sizes = {};
        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[0].descriptorCount =
            static_cast<std::uint32_t>(real_storage_buffers.size());
        pool_sizes[1].type =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_sizes[1].descriptorCount = 3;
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount =
            static_cast<std::uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        result = create_descriptor_pool(
            device,
            &pool_info,
            nullptr,
            &real_descriptor_pool);

        VkDescriptorSetAllocateInfo allocate_info = {};
        allocate_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate_info.descriptorPool = real_descriptor_pool;
        allocate_info.descriptorSetCount = 1;
        allocate_info.pSetLayouts = &real_descriptor_set_layout;
        if (result == VK_SUCCESS) {
            result = allocate_descriptor_sets(
                device,
                &allocate_info,
                &real_descriptor_set);
        }
        for (std::size_t index = 0;
             result == VK_SUCCESS && index < real_storage_buffers.size();
             ++index) {
            if (!create_host_storage_buffer(
                    real_storage_sizes[index],
                    real_storage_buffers[index],
                    real_storage_memory[index])) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        if (result == VK_SUCCESS &&
            !create_host_storage_buffer(
                kAstroVertexBufferBytes,
                real_vertex_buffer,
                real_vertex_memory,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.real_composition_resources_failed result=%d\n",
                static_cast<int>(result));
            return false;
        }
        runtime_trace(
            "native_gpu.real_composition_pipeline_ready "
            "mode=%s "
            "vertex_bytes=%llu fragment_bytes=%llu "
            "buffers=%llu images=3 enabled=%u\n",
            real_composition_mode_name(real_composition_mode),
            static_cast<unsigned long long>(
                vertex_spirv.size() * sizeof(std::uint32_t)),
            static_cast<unsigned long long>(
                fragment_spirv.size() * sizeof(std::uint32_t)),
            static_cast<unsigned long long>(
                real_storage_buffers.size()),
            real_composition_enabled ? 1u : 0u);
        return true;
    }

    std::uint32_t find_memory_type(
        std::uint32_t type_bits,
        VkMemoryPropertyFlags required) const {
        for (std::uint32_t index = 0;
             index < memory_properties.memoryTypeCount;
             ++index) {
            if ((type_bits & (1u << index)) != 0 &&
                (memory_properties.memoryTypes[index].propertyFlags &
                 required) == required) {
                return index;
            }
        }
        return UINT32_MAX;
    }

    // `preferred` names properties that are wanted but not required: the
    // first memory type carrying them is taken, and if there is none the
    // search falls back to plain host-visible coherent memory. Staging
    // buffers ask for HOST_CACHED this way, because the host reads them
    // back as well as writing them and uncached reads are an order of
    // magnitude slower.
    bool create_host_storage_buffer(
        VkDeviceSize size,
        VkBuffer& buffer,
        VkDeviceMemory& memory,
        VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VkMemoryPropertyFlags preferred = 0) {
        const VkAllocSiteScope site(VkAllocSite::HostStorageBuffer);
        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = size;
        buffer_info.usage = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        auto result = create_buffer(
            device,
            &buffer_info,
            nullptr,
            &buffer);
        if (result != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements requirements = {};
        get_buffer_memory_requirements(
            device,
            buffer,
            &requirements);
        constexpr VkMemoryPropertyFlags host_required =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto memory_type = UINT32_MAX;
        if (preferred != 0) {
            memory_type = find_memory_type(
                requirements.memoryTypeBits,
                host_required | preferred);
        }
        // A buffer the host only fills and the GPU only copies from goes in
        // system memory. The first host-visible type on this card is the
        // GPU's own memory seen through the PCIe window, and filling a 12MB
        // video frame there took 27ms of the worker's frame.
        if (memory_type == UINT32_MAX &&
            usage == VK_BUFFER_USAGE_TRANSFER_SRC_BIT) {
            for (std::uint32_t index = 0;
                 index < memory_properties.memoryTypeCount;
                 ++index) {
                const auto flags =
                    memory_properties.memoryTypes[index].propertyFlags;
                if ((requirements.memoryTypeBits & (1u << index)) != 0 &&
                    (flags & host_required) == host_required &&
                    (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
                    memory_type = index;
                    break;
                }
            }
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                runtime_trace(
                    "native_gpu.upload_memory_type index=%u flags=0x%08X\n",
                    memory_type,
                    memory_type < memory_properties.memoryTypeCount
                        ? memory_properties.memoryTypes[memory_type]
                              .propertyFlags
                        : 0u);
                for (std::uint32_t index = 0;
                     index < memory_properties.memoryTypeCount;
                     ++index) {
                    runtime_trace(
                        "native_gpu.memory_type index=%u flags=0x%08X "
                        "heap=%u\n",
                        index,
                        memory_properties.memoryTypes[index].propertyFlags,
                        memory_properties.memoryTypes[index].heapIndex);
                }
            }
        }
        if (memory_type == UINT32_MAX) {
            memory_type = find_memory_type(
                requirements.memoryTypeBits,
                host_required);
        }
        if (memory_type == UINT32_MAX) {
            destroy_buffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }

        if (preferred != 0) {
            static std::set<std::uint32_t> reported;
            if (reported.insert(memory_type).second) {
                VkPhysicalDeviceMemoryProperties memory_properties = {};
                if (get_physical_device_memory_properties != nullptr) {
                    get_physical_device_memory_properties(
                        physical_device,
                        &memory_properties);
                }
                runtime_trace(
                    "native_gpu.host_memory_type index=%u flags=0x%08X "
                    "wanted=0x%08X cached=%u\n",
                    memory_type,
                    memory_type < memory_properties.memoryTypeCount
                        ? memory_properties.memoryTypes[memory_type]
                              .propertyFlags
                        : 0u,
                    host_required | preferred,
                    memory_type < memory_properties.memoryTypeCount &&
                            (memory_properties.memoryTypes[memory_type]
                                 .propertyFlags &
                             VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0
                        ? 1u
                        : 0u);
            }
        }

        VkMemoryAllocateInfo allocation = {};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        result = allocate_memory(
            device,
            &allocation,
            nullptr,
            &memory);
        if (result != VK_SUCCESS ||
            bind_buffer_memory(device, buffer, memory, 0) != VK_SUCCESS) {
            if (memory != VK_NULL_HANDLE) {
                free_memory(device, memory, nullptr);
            }
            destroy_buffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            memory = VK_NULL_HANDLE;
            return false;
        }

        void* mapped = nullptr;
        result = map_memory(device, memory, 0, size, 0, &mapped);
        if (result != VK_SUCCESS || mapped == nullptr) {
            free_memory(device, memory, nullptr);
            destroy_buffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            memory = VK_NULL_HANDLE;
            return false;
        }
        std::memset(mapped, 0, static_cast<std::size_t>(size));
        unmap_memory(device, memory);
        return true;
    }

    bool create_device_local_buffer(
        VkDeviceSize size,
        VkBuffer& buffer,
        VkDeviceMemory& memory) {
        const VkAllocSiteScope site(VkAllocSite::DeviceLocalBuffer);
        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = size;
        buffer_info.usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (create_buffer(device, &buffer_info, nullptr, &buffer) !=
            VK_SUCCESS) {
            buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryRequirements requirements = {};
        get_buffer_memory_requirements(device, buffer, &requirements);
        const auto memory_type = find_memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memory_type == UINT32_MAX) {
            destroy_buffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo allocation = {};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        if (allocate_memory(device, &allocation, nullptr, &memory) !=
                VK_SUCCESS ||
            bind_buffer_memory(device, buffer, memory, 0) != VK_SUCCESS) {
            if (memory != VK_NULL_HANDLE) {
                free_memory(device, memory, nullptr);
            }
            destroy_buffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            memory = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    // A compute storage buffer the dispatch can read at device speed.
    // Host-visible memory on a discrete card is system RAM, so every load
    // and store in the shader crosses PCIe. The title's hottest compute
    // shader was costing 8.6 us per 64-thread workgroup and scaling
    // strictly linearly with workgroup count - the shape of a bandwidth
    // limit, and the bandwidth it implies is a few gigabytes a second
    // against a card that has four hundred. So the buffer the shader binds
    // is device-local, and a host-visible staging buffer of the same size
    // carries the guest's bytes in and the results back out, copied inside
    // the dispatch's own command buffer.
    //
    // If either half fails to allocate - the device-local heap is finite
    // and these buffers reach sixteen megabytes each - the pair collapses
    // back to the single host-visible buffer this used to be. That is
    // slower, not wrong, so the fallback needs no other handling.
    struct PooledStorageBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    };
    std::map<VkDeviceSize, std::vector<PooledStorageBuffer>>
        compute_buffer_pool;
    std::uint64_t compute_buffer_pool_bytes = 0;

    // Measured both ways on one build, two runs each, 40 seconds:
    //   pool on  247 and 244 flips, 2.34 ms a dispatch, 9022 and 8622
    //            device allocations, pl_buf 1646 and 1711 ms
    //   pool off 145 and 156 flips, 3.66 and 3.36 ms, 42136 allocations,
    //            pl_buf 6586 ms
    // Private usage is the same or slightly lower with it on.
    // PS5GPU_NATIVE_BUFFER_POOL=0 turns it off.
    static bool compute_buffer_pool_enabled() {
        static const auto enabled = [] {
            char value[8] = {};
            const auto length = GetEnvironmentVariableA(
                "PS5GPU_NATIVE_BUFFER_POOL",
                value,
                static_cast<DWORD>(sizeof(value)));
            return !(length == 1 && value[0] == '0');
        }();
        return enabled;
    }

    bool acquire_compute_storage_buffer(
        VkDeviceSize size,
        VkBuffer& buffer,
        VkDeviceMemory& memory,
        VkBuffer& staging_buffer,
        VkDeviceMemory& staging_memory) {
        if (compute_buffer_pool_enabled()) {
            const auto bucket = compute_buffer_pool.find(size);
            if (bucket != compute_buffer_pool.end() &&
                !bucket->second.empty()) {
                const auto taken = bucket->second.back();
                bucket->second.pop_back();
                compute_buffer_pool_bytes -= size;
                buffer = taken.buffer;
                memory = taken.memory;
                staging_buffer = taken.staging_buffer;
                staging_memory = taken.staging_memory;
                g_pooled_buffer_hits.fetch_add(
                    1, std::memory_order_relaxed);
                return true;
            }
        }
        g_pooled_buffer_misses.fetch_add(1, std::memory_order_relaxed);
        return create_compute_storage_buffer(
            size, buffer, memory, staging_buffer, staging_memory);
    }

    // Called only where nothing is in flight. Anything over the cap is
    // destroyed rather than held, so the pool cannot grow without bound in
    // a title that keeps inventing shapes.
    void release_compute_storage_buffer(
        VkDeviceSize size,
        VkBuffer& buffer,
        VkDeviceMemory& memory,
        VkBuffer& staging_buffer,
        VkDeviceMemory& staging_memory) {
        constexpr std::uint64_t kPoolByteCap = 512ULL * 1024ULL * 1024ULL;
        constexpr std::size_t kPerSizeCap = 64;
        if (compute_buffer_pool_enabled() &&
            buffer != VK_NULL_HANDLE &&
            size != 0 &&
            compute_buffer_pool_bytes + size <= kPoolByteCap) {
            auto& bucket = compute_buffer_pool[size];
            if (bucket.size() < kPerSizeCap) {
                bucket.push_back(
                    PooledStorageBuffer{
                        buffer, memory, staging_buffer, staging_memory});
                compute_buffer_pool_bytes += size;
                buffer = VK_NULL_HANDLE;
                memory = VK_NULL_HANDLE;
                staging_buffer = VK_NULL_HANDLE;
                staging_memory = VK_NULL_HANDLE;
                g_pooled_buffer_returns.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
        }
    }

    void destroy_compute_buffer_pool() {
        for (auto& [size, bucket] : compute_buffer_pool) {
            (void)size;
            for (auto& entry : bucket) {
                if (entry.buffer != VK_NULL_HANDLE &&
                    destroy_buffer != nullptr) {
                    destroy_buffer(device, entry.buffer, nullptr);
                }
                if (entry.memory != VK_NULL_HANDLE &&
                    free_memory != nullptr) {
                    free_memory(device, entry.memory, nullptr);
                }
                if (entry.staging_buffer != VK_NULL_HANDLE &&
                    destroy_buffer != nullptr) {
                    destroy_buffer(device, entry.staging_buffer, nullptr);
                }
                if (entry.staging_memory != VK_NULL_HANDLE &&
                    free_memory != nullptr) {
                    free_memory(device, entry.staging_memory, nullptr);
                }
            }
            bucket.clear();
        }
        compute_buffer_pool.clear();
        compute_buffer_pool_bytes = 0;
    }

    // One index buffer per guest address, kept across draws and refilled
    // from guest memory each time it is used, because a mesh the title
    // animates rewrites its indices in place. They are small - the geometry
    // this title draws carries 24192 sixteen-bit indices, 48KB.
    struct GuestIndexBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };
    std::map<std::uint64_t, GuestIndexBuffer> guest_index_buffers;

    VkBuffer ensure_guest_index_buffer(
        std::uint32_t state,
        std::uint64_t address,
        std::uint32_t index_bytes,
        std::uint32_t index_count) {
        const auto stride = index_bytes == 4 ? VkDeviceSize{4} : VkDeviceSize{2};
        const VkDeviceSize wanted =
            static_cast<VkDeviceSize>(index_count) * stride;
        if (address == 0 || wanted == 0) {
            return VK_NULL_HANDLE;
        }
        auto& entry = guest_index_buffers[address];
        if (entry.buffer == VK_NULL_HANDLE || entry.size < wanted) {
            if (entry.buffer != VK_NULL_HANDLE && destroy_buffer != nullptr) {
                destroy_buffer(device, entry.buffer, nullptr);
            }
            if (entry.memory != VK_NULL_HANDLE && free_memory != nullptr) {
                free_memory(device, entry.memory, nullptr);
            }
            entry = {};
            if (!create_host_storage_buffer(
                    wanted,
                    entry.buffer,
                    entry.memory,
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
                entry = {};
                g_index_buffer_failures.fetch_add(
                    1, std::memory_order_relaxed);
                return VK_NULL_HANDLE;
            }
            entry.size = wanted;
            g_index_buffers_created.fetch_add(1, std::memory_order_relaxed);
        }
        void* mapped = nullptr;
        if (map_memory(device, entry.memory, 0, wanted, 0, &mapped) !=
                VK_SUCCESS ||
            mapped == nullptr) {
            g_index_buffer_failures.fetch_add(1, std::memory_order_relaxed);
            return VK_NULL_HANDLE;
        }
        if (!read_draw_buffer(
                state, address, mapped, static_cast<std::size_t>(wanted))) {
            // Zero indices all name the first vertex, which draws nothing -
            // better than handing the device whatever was there before.
            std::memset(mapped, 0, static_cast<std::size_t>(wanted));
            g_index_buffer_read_failures.fetch_add(
                1, std::memory_order_relaxed);
        }
        unmap_memory(device, entry.memory);
        g_index_buffer_uploads.fetch_add(1, std::memory_order_relaxed);
        g_index_buffer_bytes.fetch_add(wanted, std::memory_order_relaxed);
        return entry.buffer;
    }

    void destroy_guest_index_buffers() {
        for (auto& [address, entry] : guest_index_buffers) {
            (void)address;
            if (entry.buffer != VK_NULL_HANDLE && destroy_buffer != nullptr) {
                destroy_buffer(device, entry.buffer, nullptr);
            }
            if (entry.memory != VK_NULL_HANDLE && free_memory != nullptr) {
                free_memory(device, entry.memory, nullptr);
            }
        }
        guest_index_buffers.clear();
    }

    bool create_compute_storage_buffer(
        VkDeviceSize size,
        VkBuffer& buffer,
        VkDeviceMemory& memory,
        VkBuffer& staging_buffer,
        VkDeviceMemory& staging_memory) {
        staging_buffer = VK_NULL_HANDLE;
        staging_memory = VK_NULL_HANDLE;
        if (create_device_local_buffer(size, buffer, memory)) {
            if (create_host_storage_buffer(
                    size,
                    staging_buffer,
                    staging_memory,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
                return true;
            }
            free_memory(device, memory, nullptr);
            destroy_buffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            memory = VK_NULL_HANDLE;
            staging_buffer = VK_NULL_HANDLE;
            staging_memory = VK_NULL_HANDLE;
        }
        return create_host_storage_buffer(size, buffer, memory);
    }

    // Derived from the Vulkan format rather than kept as a second table. The
    // hand-written version knew six formats while guest_format_to_vulkan knew
    // forty-four, and an unlisted format made prepare_guest_image_upload
    // return without a word: 226 image bindings in one frame were created as
    // surfaces and never received any data, against 21 that uploaded. Two
    // tables of the same thing drift; one derived from the other cannot.
    static std::uint32_t linear_guest_bytes_per_pixel(
        std::uint32_t unified_format) {
        const auto format = guest_format_to_vulkan(unified_format);
        if (format == VK_FORMAT_UNDEFINED ||
            render_surface_is_compressed(format)) {
            return 0;
        }
        return render_surface_bytes_per_pixel(format);
    }

    // Guest colour formats are the GFX10 unified buffer-format enumeration:
    // one 9-bit number that names both the channel layout and the numeric
    // interpretation. Texture descriptors carry it directly; render targets do
    // not - CB_COLOR_INFO splits it back into a channel layout and a channel
    // type, so that pair has to be folded into the unified value before
    // anything here can be looked up.
    static VkFormat guest_format_to_vulkan(
        std::uint32_t unified_format) {
        switch (unified_format) {
        case 1:
            return VK_FORMAT_R8_UNORM;
        case 2:
            return VK_FORMAT_R8_SNORM;
        case 5:
            return VK_FORMAT_R8_UINT;
        case 6:
            return VK_FORMAT_R8_SINT;
        case 7:
            return VK_FORMAT_R16_UNORM;
        case 8:
            return VK_FORMAT_R16_SNORM;
        case 11:
            return VK_FORMAT_R16_UINT;
        case 12:
            return VK_FORMAT_R16_SINT;
        case 13:
            return VK_FORMAT_R16_SFLOAT;
        case 14:
            return VK_FORMAT_R8G8_UNORM;
        case 15:
            return VK_FORMAT_R8G8_SNORM;
        case 18:
            return VK_FORMAT_R8G8_UINT;
        case 19:
            return VK_FORMAT_R8G8_SINT;
        case 20:
            return VK_FORMAT_R32_UINT;
        case 21:
            return VK_FORMAT_R32_SINT;
        case 22:
            return VK_FORMAT_R32_SFLOAT;
        case 23:
            return VK_FORMAT_R16G16_UNORM;
        case 24:
            return VK_FORMAT_R16G16_SNORM;
        case 27:
            return VK_FORMAT_R16G16_UINT;
        case 28:
            return VK_FORMAT_R16G16_SINT;
        case 29:
            return VK_FORMAT_R16G16_SFLOAT;
        case 36:
            return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case 50:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 54:
            return VK_FORMAT_A2B10G10R10_UINT_PACK32;
        case 56:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case 57:
            return VK_FORMAT_R8G8B8A8_SNORM;
        case 60:
            return VK_FORMAT_R8G8B8A8_UINT;
        case 61:
            return VK_FORMAT_R8G8B8A8_SINT;
        case 62:
            return VK_FORMAT_R32G32_UINT;
        case 63:
            return VK_FORMAT_R32G32_SINT;
        case 64:
            return VK_FORMAT_R32G32_SFLOAT;
        case 65:
            return VK_FORMAT_R16G16B16A16_UNORM;
        case 66:
            return VK_FORMAT_R16G16B16A16_SNORM;
        case 69:
            return VK_FORMAT_R16G16B16A16_UINT;
        case 70:
            return VK_FORMAT_R16G16B16A16_SINT;
        case 71:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 75:
            return VK_FORMAT_R32G32B32A32_UINT;
        case 76:
            return VK_FORMAT_R32G32B32A32_SINT;
        case 77:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 130:
            return VK_FORMAT_R8G8B8A8_SRGB;
        // Block compressed. Sampled only - never a render target - and the
        // data is copied by 4x4 block rather than by pixel.
        case 169:
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case 170:
            return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case 171:
            return VK_FORMAT_BC2_UNORM_BLOCK;
        case 172:
            return VK_FORMAT_BC2_SRGB_BLOCK;
        case 173:
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case 174:
            return VK_FORMAT_BC3_SRGB_BLOCK;
        case 175:
            return VK_FORMAT_BC4_UNORM_BLOCK;
        case 176:
            return VK_FORMAT_BC4_SNORM_BLOCK;
        case 177:
            return VK_FORMAT_BC5_UNORM_BLOCK;
        case 178:
            return VK_FORMAT_BC5_SNORM_BLOCK;
        case 179:
            return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case 180:
            return VK_FORMAT_BC6H_SFLOAT_BLOCK;
        case 181:
            return VK_FORMAT_BC7_UNORM_BLOCK;
        // The intro's "Sony Interactive Entertainment presents" is one of
        // these, 1600x256; unmapped, it was an empty texture and the text a
        // black rectangle.
        case 182:
            return VK_FORMAT_BC7_SRGB_BLOCK;
        case 132:
            return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        case 133:
            return VK_FORMAT_B5G6R5_UNORM_PACK16;
        case 134:
            return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
        case 136:
            return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    // CB_COLOR_INFO.FORMAT selects a channel layout; CB_COLOR_INFO.NUMBER_TYPE
    // selects the numeric type. Within a layout the unified enumeration lists
    // the numeric types in ChannelType order, so the linear members are reached
    // by offset from the layout's first entry. Layouts whose float member sits
    // outside that run carry it separately.
    struct RenderTargetLayoutInfo {
        std::uint32_t layout;
        std::uint32_t first_linear_format;
        std::uint32_t linear_type_mask;
        std::uint32_t extra_format;
        std::uint32_t extra_type;
    };

    static std::uint32_t resolve_render_target_format(
        std::uint32_t layout,
        std::uint32_t number_type) {
        // ChannelType: UNorm 0, SNorm 1, UInt 4, SInt 5, Srgb 6, Float 7.
        constexpr std::uint32_t kUnorm = 1u << 0;
        constexpr std::uint32_t kIntegers = (1u << 4) | (1u << 5);
        constexpr std::uint32_t kNormalizedIntegers =
            kUnorm | (1u << 1) | kIntegers;
        static constexpr RenderTargetLayoutInfo kLayouts[] = {
            {1, 1, kNormalizedIntegers, 128, 6},
            {2, 7, kNormalizedIntegers, 13, 7},
            {3, 14, kNormalizedIntegers, 129, 6},
            {4, 20, kIntegers, 22, 7},
            {5, 23, kNormalizedIntegers, 29, 7},
            {6, 30, kNormalizedIntegers, 36, 7},
            {7, 37, kNormalizedIntegers, 43, 7},
            {8, 44, kNormalizedIntegers, 0, 0},
            {9, 50, kNormalizedIntegers, 0, 0},
            {10, 56, kNormalizedIntegers, 130, 6},
            {11, 62, kIntegers, 64, 7},
            {12, 65, kNormalizedIntegers, 71, 7},
            {14, 75, kIntegers, 77, 7},
            {16, 133, kUnorm, 0, 0},
            {17, 134, kUnorm, 0, 0},
            {18, 135, kUnorm, 0, 0},
            {19, 136, kUnorm, 0, 0},
            {31, 0, 0, 131, 7},
        };
        if (number_type >= 8) {
            return 0;
        }
        for (const auto& info : kLayouts) {
            if (info.layout != layout) {
                continue;
            }
            if (info.extra_format != 0 &&
                number_type == info.extra_type) {
                return info.extra_format;
            }
            if ((info.linear_type_mask & (1u << number_type)) == 0) {
                return 0;
            }
            const auto first_type = static_cast<std::uint32_t>(
                std::countr_zero(info.linear_type_mask));
            return info.first_linear_format + number_type - first_type;
        }
        return 0;
    }

    // Draws that carry no decoded colour-target registers keep the historical
    // RGBA8 assumption, which is what the display buffer actually is.
    static constexpr std::uint32_t kDefaultGuestFormat = 56;

    static std::uint32_t draw_render_target_unified_format(
        const GpuIrDraw& draw) {
        const auto resolved = resolve_render_target_format(
            draw.render_target_format,
            draw.render_target_number_type);
        if (resolved == 0 && draw.render_target_format != 0) {
            report_unresolved_render_target_format(
                draw.render_target_format,
                draw.render_target_number_type);
        }
        return resolved != 0 ? resolved : kDefaultGuestFormat;
    }

    // A layout the table does not carry means the frame is being drawn into
    // the wrong kind of image, not merely traced imprecisely - report it, but
    // only once per pair so a per-draw defect cannot flood the log.
    static void report_unresolved_render_target_format(
        std::uint32_t layout,
        std::uint32_t number_type) {
        static std::mutex unresolved_mutex;
        static std::set<std::uint32_t> unresolved_reported;
        const auto key = (layout << 8) | number_type;
        bool first_sighting = false;
        {
            std::lock_guard<std::mutex> guard(unresolved_mutex);
            first_sighting = unresolved_reported.insert(key).second;
        }
        if (first_sighting) {
            runtime_trace(
                "native_gpu.render_target_format_unresolved "
                "layout=%u number_type=%u fallback=%u\n",
                layout,
                number_type,
                kDefaultGuestFormat);
        }
    }

    // Anything the table does not name still has to produce a surface, so it
    // falls back to the historical RGBA8 guess - but silently guessing is how
    // an entire frame ends up the wrong colour, so say so once per value.
    static VkFormat render_surface_format(
        std::uint32_t unified_format) {
        const auto mapped = guest_format_to_vulkan(unified_format);
        if (mapped != VK_FORMAT_UNDEFINED) {
            return mapped;
        }
        static std::mutex unmapped_mutex;
        static std::set<std::uint32_t> unmapped_reported;
        bool first_sighting = false;
        {
            std::lock_guard<std::mutex> guard(unmapped_mutex);
            first_sighting =
                unmapped_reported.insert(unified_format).second;
        }
        if (first_sighting) {
            runtime_trace(
                "native_gpu.guest_format_unmapped unified=%u "
                "fallback=R8G8B8A8_UNORM\n",
                unified_format);
        }
        return VK_FORMAT_R8G8B8A8_UNORM;
    }

    static bool render_surface_is_compressed(VkFormat format) {
        return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK &&
            format <= VK_FORMAT_BC7_SRGB_BLOCK;
    }

    // Bytes in one 4x4 block: eight for BC1 and BC4, sixteen for the rest.
    static std::uint32_t compressed_block_bytes(VkFormat format) {
        switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
            return 8;
        default:
            return 16;
        }
    }

    // For a compressed format this is the average over a block: BC6H packs a
    // 4x4 block into 16 bytes, so one byte per pixel. Every buffer size built
    // from width * height * this stays right in total, which is what the
    // upload and readback paths need; per-pixel addressing does not apply and
    // those paths check render_surface_is_compressed before indexing.
    static std::uint32_t render_surface_bytes_per_pixel(
        VkFormat format) {
        // A block format's average, rounded up: sizes built from it are
        // allocations, which may be generous, never copies.
        if (render_surface_is_compressed(format)) {
            return 1;
        }
        switch (format) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT:
            return 1;
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
            return 2;
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return 8;
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16;
        default:
            return 4;
        }
    }

    static const char* render_surface_format_name(VkFormat format) {
        switch (format) {
        case VK_FORMAT_R8_UNORM:
            return "R8_UNORM";
        case VK_FORMAT_R8_SNORM:
            return "R8_SNORM";
        case VK_FORMAT_R8_UINT:
            return "R8_UINT";
        case VK_FORMAT_R8_SINT:
            return "R8_SINT";
        case VK_FORMAT_R16_UNORM:
            return "R16_UNORM";
        case VK_FORMAT_R16_SNORM:
            return "R16_SNORM";
        case VK_FORMAT_R16_UINT:
            return "R16_UINT";
        case VK_FORMAT_R16_SINT:
            return "R16_SINT";
        case VK_FORMAT_R16_SFLOAT:
            return "R16_SFLOAT";
        case VK_FORMAT_R8G8_UNORM:
            return "R8G8_UNORM";
        case VK_FORMAT_R8G8_SNORM:
            return "R8G8_SNORM";
        case VK_FORMAT_R8G8_UINT:
            return "R8G8_UINT";
        case VK_FORMAT_R8G8_SINT:
            return "R8G8_SINT";
        case VK_FORMAT_R32_UINT:
            return "R32_UINT";
        case VK_FORMAT_R32_SINT:
            return "R32_SINT";
        case VK_FORMAT_R32_SFLOAT:
            return "R32_SFLOAT";
        case VK_FORMAT_R16G16_UNORM:
            return "R16G16_UNORM";
        case VK_FORMAT_R16G16_SNORM:
            return "R16G16_SNORM";
        case VK_FORMAT_R16G16_UINT:
            return "R16G16_UINT";
        case VK_FORMAT_R16G16_SINT:
            return "R16G16_SINT";
        case VK_FORMAT_R16G16_SFLOAT:
            return "R16G16_SFLOAT";
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return "B10G11R11_UFLOAT_PACK32";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return "A2B10G10R10_UNORM_PACK32";
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            return "A2B10G10R10_UINT_PACK32";
        case VK_FORMAT_R8G8B8A8_SNORM:
            return "R8G8B8A8_SNORM";
        case VK_FORMAT_R8G8B8A8_UINT:
            return "R8G8B8A8_UINT";
        case VK_FORMAT_R8G8B8A8_SINT:
            return "R8G8B8A8_SINT";
        case VK_FORMAT_R8G8B8A8_SRGB:
            return "R8G8B8A8_SRGB";
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
            return "BC6H_UFLOAT_BLOCK";
        // Never had an entry: the old default returned this name for anything
        // unknown, so the commonest format in the runtime printed correctly by
        // accident and the gap stayed invisible.
        case VK_FORMAT_R8G8B8A8_UNORM:
            return "R8G8B8A8_UNORM";
        case VK_FORMAT_R32G32_UINT:
            return "R32G32_UINT";
        case VK_FORMAT_R32G32_SINT:
            return "R32G32_SINT";
        case VK_FORMAT_R32G32_SFLOAT:
            return "R32G32_SFLOAT";
        case VK_FORMAT_R16G16B16A16_UNORM:
            return "R16G16B16A16_UNORM";
        case VK_FORMAT_R16G16B16A16_SNORM:
            return "R16G16B16A16_SNORM";
        case VK_FORMAT_R16G16B16A16_UINT:
            return "R16G16B16A16_UINT";
        case VK_FORMAT_R16G16B16A16_SINT:
            return "R16G16B16A16_SINT";
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return "R16G16B16A16_SFLOAT";
        case VK_FORMAT_R32G32B32A32_UINT:
            return "R32G32B32A32_UINT";
        case VK_FORMAT_R32G32B32A32_SINT:
            return "R32G32B32A32_SINT";
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return "R32G32B32A32_SFLOAT";
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
            return "E5B9G9R9_UFLOAT_PACK32";
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
            return "B5G6R5_UNORM_PACK16";
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
            return "R5G5B5A1_UNORM_PACK16";
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
            return "R4G4B4A4_UNORM_PACK16";
        default:
            // Naming an unknown format after the commonest one turns every
            // trace that prints it into a false report. A BC6H surface was
            // created correctly and read back from the log as R8G8B8A8_UNORM,
            // which cost a probe to see through.
            return "UNNAMED_FORMAT";
        }
    }

    struct ExactDetilePattern {
        std::array<std::uint32_t, 16> x_masks;
        std::array<std::uint32_t, 16> y_masks;
    };

    static const ExactDetilePattern* exact_detile_pattern(
        std::uint32_t tile_mode,
        std::uint32_t bytes_per_pixel) {
        static constexpr ExactDetilePattern depth_x_4 = {
            {
                0, 0, 1u << 0, 0,
                1u << 1, 0, 1u << 2, 0,
                1u << 7, 1u << 4, 1u << 6, 1u << 5,
                1u << 3, 0, 1u << 6, 1u << 7,
            },
            {
                0, 0, 0, 1u << 0,
                0, 1u << 1, 0, 1u << 2,
                (1u << 4) | (1u << 7), 1u << 4,
                1u << 5, 1u << 6,
                0, 1u << 3, 1u << 7, 1u << 6,
            },
        };
        static constexpr ExactDetilePattern render_x_8 = {
            {
                0, 0, 0, 1u << 0,
                0, 1u << 1, 1u << 2, 0,
                1u << 7, 1u << 4, 1u << 6, 1u << 5,
                0, 1u << 3, 1u << 7, 1u << 6,
            },
            {
                0, 0, 0, 0,
                1u << 0, 0, 0, 1u << 1,
                (1u << 4) | (1u << 7), 1u << 4,
                1u << 5, 1u << 6,
                1u << 2, 0, 1u << 3, 1u << 6,
            },
        };
        static constexpr ExactDetilePattern standard_4k_8 = {
            {
                0, 0, 0, 1u << 0,
                0, 0, 1u << 1, 1u << 2,
                0, 1u << 3, 0, 1u << 4,
                0, 0, 0, 0,
            },
            {
                0, 0, 0, 0,
                1u << 0, 1u << 1, 0, 0,
                1u << 2, 0, 1u << 3, 0,
                0, 0, 0, 0,
            },
        };
        // Tile 24 at one and two bytes, converted from KytyPS5's
        // Depth64KB8/16 X and Y offset functions (GPL-2.0). The
        // conversion is mechanical but transposed - Kyty maps source bit
        // to output bit, this table is indexed by output bit and holds the
        // source bits that XOR into it - so each was checked by rebuilding
        // the offset from the masks and comparing against Kyty's formula
        // over every coordinate in a block. All four agree.
        //
        // Caution for whoever verifies these against real data: the same
        // conversion applied to Kyty's Depth64KB32 functions does NOT
        // reproduce depth_x_4 above, which is the entry already in use for
        // this tile mode at four bytes. One of the two is wrong, or tile 24
        // is not this Kyty family after all. That is unresolved, and these
        // entries inherit the assumption.
        static constexpr ExactDetilePattern depth_x_1 = {
            {
                1u << 0, 0, 1u << 1, 0,
                1u << 2, 0, 1u << 3, 0,
                1u << 3, 1u << 4, 1u << 6, 1u << 5,
                0, 1u << 6, 0, 1u << 7,
            },
            {
                0, 1u << 0, 0, 1u << 1,
                0, 1u << 2, 0, 1u << 4,
                1u << 3, 1u << 4, 1u << 5, 1u << 6,
                1u << 6, 0, 1u << 7, 0,
            },
        };
        static constexpr ExactDetilePattern depth_x_2 = {
            {
                0, 1u << 0, 0, 1u << 1,
                0, 1u << 2, 0, 1u << 3,
                1u << 3, 1u << 4, 1u << 6, 1u << 5,
                0, 1u << 6, 0, 1u << 7,
            },
            {
                0, 0, 1u << 0, 0,
                1u << 1, 0, 1u << 2, 0,
                1u << 3, 1u << 4, 1u << 5, 1u << 6,
                1u << 4, 0, 1u << 6, 0,
            },
        };
        // Tile 27 at four bytes, the size 163 of the 176 reported tile-27
        // skips ask for, converted from KytyPS5's
        // Gen5RenderTargetOffsetInBlock<uint32_t> (GPL-2.0) and checked by
        // rebuilding the offset from the masks across a whole block.
        //
        // The same conversion applied to Kyty's eight-byte case reproduces
        // 14 of the 16 x masks in render_x_8 above, byte-indexed and with
        // no shift - which is what fixes the convention used here.
        //
        // The rest of that difference is smaller than it looks. A 64KB
        // block at eight bytes an element is 128x64 - which is what
        // detile_block_bytes and the block_width/block_height derivation
        // below compute - so within a block x7, y6 and y7 are always zero
        // and every mask entry naming them is dead. render_x_8 names x7 at
        // offset bits 8 and 14 and y7 at bit 8; Kyty names none of them.
        // Dropping the dead bits leaves the two patterns identical on 14 of
        // the 16 offset bits, and differing on exactly two:
        //
        //     offset bit  8:  here y4        Kyty x3^y3
        //     offset bit 14:  here y3        Kyty y4
        //
        // Bit 15 does not differ at all once y6 is dropped, so the earlier
        // note naming bits 8, 14 and 15 overstated it.
        //
        // Both patterns are bijections over the 128x64 block - checked by
        // enumerating it - so neither is broken by construction and the
        // question cannot be settled by reading them. It takes an eight
        // byte tiled surface rendered both ways, and only bits 8 and 14
        // need swapping to try it. The note on depth_x_4 says the same for
        // tile 24 at four bytes and has not had this treatment.
        static constexpr ExactDetilePattern render_x_4 = {
            {
                0, 0, 1u << 0, 1u << 1,
                0, 0, 0, 1u << 2,
                1u << 3, 1u << 4, 1u << 6, 1u << 5,
                0, 1u << 4, 0, 1u << 6,
            },
            {
                0, 0, 0, 0,
                1u << 0, 1u << 1, 1u << 2, 0,
                1u << 3, 1u << 4, 1u << 5, 1u << 6,
                1u << 3, 0, 1u << 6, 0,
            },
        };
        // Tile 5 at sixteen bytes, which is what a BC block is. Converted
        // from Kyty's Gen5Standard4KBOffsetInBlock and checked the same way;
        // the eight byte case of that same function reproduces standard_4k_8
        // above mask for mask, which is what says the conversion is right.
        static constexpr ExactDetilePattern standard_4k_16 = {
            {
                0, 0, 0, 0,
                0, 0, 1u << 0, 1u << 1,
                0, 1u << 2, 0, 1u << 3,
                0, 0, 0, 0,
            },
            {
                0, 0, 0, 0,
                1u << 0, 1u << 1, 0, 0,
                1u << 2, 0, 1u << 3, 0,
                0, 0, 0, 0,
            },
        };
        // Tile 27 at two and sixteen bytes, converted from the same Kyty
        // function for uint16_t and Uint128. The converter was checked by
        // rebuilding the byte offset from the masks and comparing against
        // Kyty's own arithmetic at every coordinate of a 256x256 block, for
        // all four element sizes at once: two, four, eight and sixteen bytes
        // all agree exactly.
        //
        // That check also settles the question the note above leaves open.
        // The conversion reproduces render_x_4 mask for mask, so it is not
        // the conversion that is off; render_x_8 differs from it at bits 8,
        // 14 and 15 and is therefore the odd one out. Left as it is until
        // there is a measurement that says which draws it changes - the
        // eight byte path is in use and this is not the moment to disturb it.
        static constexpr ExactDetilePattern render_x_2 = {
            {
                0, 1u << 0, 1u << 1, 1u << 2,
                0, 0, 0, 1u << 3,
                1u << 3, 1u << 4, 1u << 6, 1u << 5,
                0, 1u << 6, 0, 1u << 7,
            },
            {
                0, 0, 0, 0,
                1u << 0, 1u << 1, 1u << 2, 0,
                1u << 3, 1u << 4, 1u << 5, 1u << 6,
                1u << 4, 0, 1u << 6, 0,
            },
        };
        static constexpr ExactDetilePattern render_x_16 = {
            {
                0, 0, 0, 0,
                1u << 0, 0, 1u << 1, 0,
                1u << 3, 1u << 4, 1u << 6, 1u << 5,
                0, 1u << 2, 0, 1u << 4,
            },
            {
                0, 0, 0, 0,
                0, 1u << 0, 0, 1u << 1,
                1u << 3, 1u << 4, 1u << 5, 1u << 6,
                1u << 2, 0, 1u << 3, 0,
            },
        };
        // Tile 9 is SW_64KB_S: the standard swizzle of tile 5 carried on
        // to a 64KB block. The four kilobyte pattern is the low twelve
        // bits; above them Y and X keep alternating, one bit each, which is
        // what the 4KB pattern already does from bit eight up. Found on
        // the intro's "Sony Interactive Entertainment presents", a 1600x256
        // BC7 texture.
        static constexpr ExactDetilePattern standard_64k_16 = {
            {
                0, 0, 0, 0,
                0, 0, 1u << 0, 1u << 1,
                0, 1u << 2, 0, 1u << 3,
                0, 1u << 4, 0, 1u << 5,
            },
            {
                0, 0, 0, 0,
                1u << 0, 1u << 1, 0, 0,
                1u << 2, 0, 1u << 3, 0,
                1u << 4, 0, 1u << 5, 0,
            },
        };
        static constexpr ExactDetilePattern standard_64k_8 = {
            {
                0, 0, 0, 1u << 0,
                0, 0, 1u << 1, 1u << 2,
                0, 1u << 3, 0, 1u << 4,
                0, 1u << 5, 0, 1u << 6,
            },
            {
                0, 0, 0, 0,
                1u << 0, 1u << 1, 0, 0,
                1u << 2, 0, 1u << 3, 0,
                1u << 4, 0, 1u << 5, 0,
            },
        };
        if (tile_mode == 9 && bytes_per_pixel == 16) {
            return &standard_64k_16;
        }
        if (tile_mode == 9 && bytes_per_pixel == 8) {
            return &standard_64k_8;
        }
        if (tile_mode == 5 && bytes_per_pixel == 8) {
            return &standard_4k_8;
        }
        if (tile_mode == 5 && bytes_per_pixel == 16) {
            return &standard_4k_16;
        }
        if (tile_mode == 24 && bytes_per_pixel == 4) {
            return &depth_x_4;
        }
        if (tile_mode == 24 && bytes_per_pixel == 2) {
            return &depth_x_2;
        }
        if (tile_mode == 24 && bytes_per_pixel == 1) {
            return &depth_x_1;
        }
        if (tile_mode == 27 && bytes_per_pixel == 8) {
            return &render_x_8;
        }
        if (tile_mode == 27 && bytes_per_pixel == 4) {
            return &render_x_4;
        }
        if (tile_mode == 27 && bytes_per_pixel == 2) {
            return &render_x_2;
        }
        if (tile_mode == 27 && bytes_per_pixel == 16) {
            return &render_x_16;
        }
        return nullptr;
    }

    // Grows the scratch to hold at least the size asked for and never
    // shrinks it. The caller carries the logical size itself, because the
    // size of the buffer is whatever the largest image so far needed.
    static std::uint8_t* grow_scratch(
        std::vector<std::uint8_t>& scratch,
        std::uint64_t size) {
        if (scratch.size() < size) {
            scratch.resize(static_cast<std::size_t>(size));
        }
        return scratch.data();
    }

    static std::uint32_t detile_block_bytes(
        std::uint32_t tile_mode) {
        return tile_mode >= 1 && tile_mode <= 3
            ? 256u
            : (tile_mode >= 4 && tile_mode <= 7) ||
                    (tile_mode >= 20 && tile_mode <= 23)
                ? 4096u
                : 65536u;
    }

    static std::uint32_t detile_axis_term(
        std::uint32_t coordinate,
        const std::array<std::uint32_t, 16>& masks) {
        std::uint32_t offset = 0;
        for (std::size_t bit = 0; bit < masks.size(); ++bit) {
            offset |=
                (std::popcount(coordinate & masks[bit]) & 1u) << bit;
        }
        return offset;
    }

    static bool exact_detile_guest_image(
        const std::uint8_t* tiled,
        std::uint64_t tiled_size,
        std::uint32_t tile_mode,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t bytes_per_pixel,
        std::vector<std::uint8_t>& linear) {
        const auto* pattern =
            exact_detile_pattern(tile_mode, bytes_per_pixel);
        if (pattern == nullptr ||
            width == 0 ||
            height == 0) {
            return false;
        }
        const std::uint64_t block_bytes =
            detile_block_bytes(tile_mode);
        const auto block_elements =
            block_bytes / bytes_per_pixel;
        const auto total_bits = std::countr_zero(block_elements);
        const auto block_width =
            1u << ((total_bits + 1) / 2);
        const auto block_height =
            1u << (total_bits / 2);
        const auto blocks_per_row =
            (width + block_width - 1) / block_width;
        const auto linear_size =
            static_cast<std::uint64_t>(width) *
            height *
            bytes_per_pixel;
        if (linear_size >
            std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        // The copy below skips any pixel whose tiled address falls outside
        // the source and leaves the zero written here standing in for it, so
        // this fill is load-bearing rather than tidiness.
        std::memset(
            grow_scratch(linear, linear_size),
            0,
            static_cast<std::size_t>(linear_size));

        std::array<std::uint32_t, 256> x_terms = {};
        std::array<std::uint32_t, 256> y_terms = {};
        for (std::uint32_t coordinate = 0;
             coordinate < x_terms.size();
             ++coordinate) {
            x_terms[coordinate] = detile_axis_term(
                coordinate,
                pattern->x_masks);
            y_terms[coordinate] = detile_axis_term(
                coordinate,
                pattern->y_masks);
        }

        for (std::uint32_t y = 0; y < height; ++y) {
            const auto block_y = y / block_height;
            const auto y_term = y_terms[y & 0xFFu];
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto block_x = x / block_width;
                const auto block_index =
                    static_cast<std::uint64_t>(block_y) *
                    blocks_per_row +
                    block_x;
                const auto source_offset =
                    block_index * block_bytes +
                    (x_terms[x & 0xFFu] ^ y_term);
                const auto destination_offset =
                    (static_cast<std::uint64_t>(y) * width + x) *
                    bytes_per_pixel;
                if (source_offset + bytes_per_pixel > tiled_size) {
                    continue;
                }
                std::memcpy(
                    linear.data() + destination_offset,
                    tiled + source_offset,
                    bytes_per_pixel);
            }
        }
        return true;
    }

    static float half_to_float(std::uint16_t value) {
        const auto sign = (value & 0x8000u) != 0 ? -1.0f : 1.0f;
        const auto exponent = (value >> 10) & 0x1Fu;
        const auto mantissa = value & 0x03FFu;
        if (exponent == 0) {
            return mantissa == 0
                ? std::copysign(0.0f, sign)
                : sign * std::ldexp(
                    static_cast<float>(mantissa),
                    -24);
        }
        if (exponent == 0x1Fu) {
            return mantissa == 0
                ? std::copysign(
                    std::numeric_limits<float>::infinity(),
                    sign)
                : std::numeric_limits<float>::quiet_NaN();
        }
        return sign * std::ldexp(
            1.0f + static_cast<float>(mantissa) / 1024.0f,
            static_cast<int>(exponent) - 15);
    }

    static std::uint8_t float_to_unorm8(float value) {
        if (!std::isfinite(value)) {
            return value > 0.0f ? 255 : 0;
        }
        return static_cast<std::uint8_t>(
            std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    // Once per (state, binding, reason): a texture that silently never loads
    // is indistinguishable from one that loaded zeroes, and both look like a
    // rendering bug much further downstream.
    static void report_guest_upload_skipped(
        std::uint32_t state,
        const GuestImageResource& image,
        const char* reason) {
        static std::mutex skipped_mutex;
        static std::set<std::uint64_t> skipped_reported;
        const auto key =
            (static_cast<std::uint64_t>(state) << 32) |
            (static_cast<std::uint64_t>(image.binding) << 8) |
            static_cast<std::uint64_t>(reason[0]);
        bool first = false;
        {
            std::lock_guard<std::mutex> guard(skipped_mutex);
            first = skipped_reported.insert(key).second;
        }
        if (first) {
            runtime_trace(
                "native_gpu.guest_image_upload_skipped state=%u binding=%u "
                "address=0x%016llX size=%ux%u unified=%u tile=%u reason=%s\n",
                state,
                image.binding,
                static_cast<unsigned long long>(image.guest_address),
                image.width,
                image.height,
                image.unified_format,
                image.tile_mode,
                reason);
        }
    }

    bool prepare_guest_image_upload(
        std::uint32_t state,
        GuestImageResource& image,
        bool graphics = false) {
        // A storage image nothing on the device has written yet is filled
        // from guest memory like any other, in a compute stage as well as a
        // graphics one. Compute used to be left out, and a dispatch that
        // reads its storage image before writing it read zeros: the depth
        // chain behind the intro's fog starts with a compute pass reading
        // the depth buffer that way, found it all near plane, and the fog
        // painted the "Sony Interactive Entertainment" screen magenta. The
        // caller asks only while the surface is not device-written, so an
        // output pays for one read, before its first dispatch.
        (void)graphics;
        // A block compressed image has no bytes per pixel, but it does have
        // bytes per 4x4 block, and everything below - the size arithmetic,
        // the detiler, the copy - works on those blocks exactly as it works
        // on pixels for an uncompressed image. The tiler addresses blocks
        // too, which is why this needs no special detile pattern, only the
        // one for the block's size. Vulkan takes the blocks as they are, so
        // the copy is direct and no conversion applies.
        const auto surface_format =
            image.surface != nullptr
            ? image.surface->format
            : guest_format_to_vulkan(image.unified_format);
        const auto compressed = render_surface_is_compressed(surface_format);
        const auto kCompressedBlockBytes =
            compressed ? compressed_block_bytes(surface_format) : 16u;
        constexpr std::uint32_t kCompressedBlockExtent = 4;
        const auto copy_width = compressed
            ? (image.width + kCompressedBlockExtent - 1) /
                kCompressedBlockExtent
            : image.width;
        // A volume's slices follow one another, so a linear one is its
        // rows times its slices. A tiled volume is swizzled in three
        // dimensions, which nothing here detiles yet.
        const auto slices = std::max<std::uint32_t>(1u, image.depth);
        if (slices > 1 && image.tile_mode != 0) {
            report_guest_upload_skipped(state, image, "tiled-volume");
            return false;
        }
        const auto copy_height = (compressed
            ? (image.height + kCompressedBlockExtent - 1) /
                kCompressedBlockExtent
            : image.height) * slices;
        const auto source_bytes_per_pixel = compressed
            ? kCompressedBlockBytes
            : linear_guest_bytes_per_pixel(image.unified_format);
        if (source_bytes_per_pixel == 0) {
            report_guest_upload_skipped(
                state, image, "no-linear-bytes-per-pixel");
            return false;
        }
        const auto tiled = image.tile_mode != 0;
        if (compressed && !tiled) {
            // A linear compressed image would need its pitch read as blocks
            // rather than texels, and nothing in this title asks for one.
            report_guest_upload_skipped(state, image, "linear-compressed");
            return false;
        }
        if (tiled &&
            exact_detile_pattern(
                image.tile_mode,
                source_bytes_per_pixel) == nullptr) {
            report_guest_upload_skipped(state, image, "no-detile-pattern");
            return false;
        }
        const auto source_pitch = tiled
            ? copy_width
            : std::max(image.pitch, image.width);
        std::uint64_t source_size = 0;
        // Where the top level starts. A swizzled image stores its mip
        // chain smallest first - the tail, then each level up to the
        // largest - so the base address is where the smallest levels are
        // and the full-size one is last. Read from the base, the intro's
        // "Sony Interactive Entertainment presents" came out as its own
        // mip chain, a row of ever larger copies of the text.
        std::uint64_t level_offset = 0;
        if (tiled) {
            const auto block_bytes =
                detile_block_bytes(image.tile_mode);
            const auto block_elements =
                block_bytes / source_bytes_per_pixel;
            const auto total_bits =
                std::countr_zero(block_elements);
            const auto block_width =
                1ULL << ((total_bits + 1) / 2);
            const auto block_height =
                1ULL << (total_bits / 2);
            source_size =
                ((copy_width + block_width - 1) / block_width) *
                ((copy_height + block_height - 1) / block_height) *
                block_bytes;
            // The levels below the top one, from the descriptor: base level
            // in bits 12-15 of the fourth word, last level in 16-19. A level
            // small enough for the tail - half a block wide on a square
            // block - shares one block with everything smaller.
            const auto base_level =
                (image.resource_descriptor[3] >> 12) & 0xFu;
            const auto last_level =
                (image.resource_descriptor[3] >> 16) & 0xFu;
            const auto tail_width = block_width >= block_height
                ? block_width / 2
                : block_width;
            const auto tail_height = block_width >= block_height
                ? block_height
                : block_height / 2;
            const auto element_extent = [&](std::uint32_t pixels,
                                            std::uint32_t level) {
                const auto size = std::max<std::uint32_t>(1u, pixels >> level);
                return compressed
                    ? static_cast<std::uint64_t>(
                          (size + kCompressedBlockExtent - 1) /
                          kCompressedBlockExtent)
                    : static_cast<std::uint64_t>(size);
            };
            for (auto level = base_level + 1; level <= last_level; ++level) {
                const auto width = element_extent(image.width, level);
                const auto height = element_extent(image.height, level);
                if (width <= tail_width && height <= tail_height) {
                    level_offset += block_bytes;
                    break;
                }
                level_offset +=
                    ((width + block_width - 1) / block_width) *
                    ((height + block_height - 1) / block_height) *
                    block_bytes;
            }
        } else {
            source_size =
                static_cast<std::uint64_t>(source_pitch) *
                image.height * slices *
                source_bytes_per_pixel;
        }
        const auto output_bytes_per_pixel = compressed
            ? kCompressedBlockBytes
            : (image.surface != nullptr
                ? image.surface->bytes_per_pixel
                : (image.unified_format == 71 ? 8u : 4u));
        const auto direct_copy =
            compressed ||
            (image.surface != nullptr &&
             image.surface->format ==
                 guest_format_to_vulkan(image.unified_format) &&
             output_bytes_per_pixel == source_bytes_per_pixel);
        const auto output_size =
            static_cast<std::uint64_t>(copy_width) *
            copy_height *
            output_bytes_per_pixel;
        if (source_size == 0 ||
            output_size == 0 ||
            source_size > kMaximumGuestImageUploadBytes ||
            output_size > kMaximumGuestImageUploadBytes ||
            source_size > std::numeric_limits<std::size_t>::max() ||
            output_size > std::numeric_limits<std::size_t>::max()) {
            return false;
        }

        // Nothing has written this texture since it was uploaded, so
        // there is nothing to read, detile, convert or copy. This is
        // the whole point of the watch: the question is answered
        // without touching the bytes.
        const auto level_address = image.guest_address + level_offset;
        auto& watch = guest_watch_for(level_address, source_size);
        if (traced_image_address() == image.guest_address) {
            runtime_trace(
                "native_gpu.traced_image_prepare state=%u address=0x%016llX "
                "armed=%d dirty=%d initialized=%d\n",
                state,
                static_cast<unsigned long long>(image.guest_address),
                watch.armed.load(std::memory_order_acquire) ? 1 : 0,
                watch.dirty.load(std::memory_order_acquire) ? 1 : 0,
                image.surface != nullptr && image.surface->initialized ? 1 : 0);
        }
        if (watch.armed.load(std::memory_order_acquire) &&
            !watch.dirty.load(std::memory_order_acquire) &&
            image.surface != nullptr &&
            image.surface->initialized) {
            image.upload_pending = false;
            ++g_image_watch_skipped;
            return true;
        }
        // This surface was read in full earlier and found to be nothing
        // but zeroes - this frame, or before it with the watch armed since
        // and nothing written. Reading it again to find the same is what
        // eighteen gigabytes a run is spent on; the render targets the
        // intro samples before drawing them came to sixty megabytes a
        // frame of it.
        const auto zero_epoch =
            g_image_zero_epoch.load(std::memory_order_relaxed);
        if (image.surface != nullptr) {
            const auto seen = g_image_zero_seen.find(image.guest_address);
            if (seen != g_image_zero_seen.end() &&
                seen->second.size == source_size &&
                (seen->second.epoch == zero_epoch ||
                 (watch.armed.load(std::memory_order_acquire) &&
                  !watch.dirty.load(std::memory_order_acquire) &&
                  watch.arm_serial.load(std::memory_order_acquire) ==
                      seen->second.serial))) {
                image.upload_pending = false;
                ++g_image_zero_cached;
                return true;
            }
        }
        static const auto tally_reads =
            environment_flag_enabled("PS5GPU_NATIVE_TRACE_IMAGE_TOP");
        if (tally_reads) {
            auto& tally = g_image_read_tally[level_address];
            ++tally.reads;
            tally.bytes += source_size;
            tally.unarmed += watch.armed.load(std::memory_order_acquire) ? 0 : 1;
            tally.dirty += watch.dirty.load(std::memory_order_acquire) ? 1 : 0;
            tally.uninitialized +=
                image.surface == nullptr || !image.surface->initialized ? 1 : 0;
            tally.width = image.width;
            tally.height = image.height;
            tally.unified_format = image.unified_format;
            tally.tile_mode = image.tile_mode;
        }
        // Armed before the bytes are taken. The title's thread runs ahead
        // of this one and writes what we are about to read - a video frame
        // copied into this texture, a ring rewritten - and a write that
        // lands between a read and a later arming went unseen: the texture
        // was kept as read, the intro's video went green a frame at a time.
        // Armed first, a write during or after the read leaves the watch
        // dirty and the next preparation reads again.
        const auto read_serial = guest_watch_arm(watch)
            ? watch.arm_serial.load(std::memory_order_acquire)
            : ~std::uint64_t{0};

        // A linear image in the surface's own format goes from guest memory
        // to the upload buffer in one copy. The general path below reads it
        // into scratch, copies it into a second scratch, hashes that and
        // copies it again into the mapping - four passes over a twelve
        // megabyte video frame, every frame of the intro. The hash is there
        // to skip an upload whose bytes did not change, and nothing reaches
        // this point without the watch having seen them change.
        static const auto direct_disabled =
            environment_flag_enabled("PS5GPU_NATIVE_NO_DIRECT_UPLOAD");
        if (!direct_disabled && !tiled && !compressed && direct_copy &&
            source_pitch == copy_width &&
            output_size == source_size &&
            image.surface != nullptr &&
            image.unified_format != 22 &&
            guest_range_usable(
                level_address, static_cast<std::size_t>(source_size), false) &&
            !bytes_all_zero(
                reinterpret_cast<const void*>(level_address),
                static_cast<std::size_t>(source_size))) {
            if (image.upload_buffer == VK_NULL_HANDLE ||
                image.upload_memory == VK_NULL_HANDLE ||
                image.upload_size != output_size) {
                if (image.upload_buffer != VK_NULL_HANDLE) {
                    destroy_buffer(device, image.upload_buffer, nullptr);
                }
                if (image.upload_memory != VK_NULL_HANDLE) {
                    free_memory(device, image.upload_memory, nullptr);
                }
                image.upload_buffer = VK_NULL_HANDLE;
                image.upload_memory = VK_NULL_HANDLE;
                image.upload_size = 0;
                if (!create_host_storage_buffer(
                        output_size,
                        image.upload_buffer,
                        image.upload_memory,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
                    return false;
                }
                image.upload_size = output_size;
            }
            void* mapped = nullptr;
            if (map_memory(
                    device, image.upload_memory, 0, image.upload_size, 0,
                    &mapped) != VK_SUCCESS ||
                mapped == nullptr) {
                return false;
            }
            const auto copy_started = worker_phase_counter();
            std::memcpy(
                mapped,
                reinterpret_cast<const void*>(level_address),
                static_cast<std::size_t>(source_size));
            unmap_memory(device, image.upload_memory);
            g_image_upload_ticks += worker_phase_counter() - copy_started;
            ++g_image_prepared;
            ++g_image_direct_uploads;
            g_image_bytes_read += source_size;
            // Nothing compares this any more on this path; a value no
            // hash gives keeps the general path from mistaking it for one.
            image.upload_hash = ~static_cast<std::uint64_t>(0) -
                g_image_direct_uploads;
            image.upload_ready = true;
            image.upload_pending = true;
            image.upload_unavailable_reported = false;
            return true;
        }
        auto* source = grow_scratch(upload_source_bytes, source_size);
        ++g_image_prepared;
        g_image_bytes_read += source_size;
        const auto read_started = worker_phase_counter();
        const auto read_ok = read_current_process_memory(
            level_address,
            source,
            static_cast<std::size_t>(source_size),
            nullptr);
        g_image_read_ticks += worker_phase_counter() - read_started;
        if (!read_ok) {
            if (!image.upload_unavailable_reported) {
                // ReadProcessMemory fails the whole range if any page
                // in it is not committed, so the failure alone does not
                // say whether the allocation is missing, reserved, or
                // merely shorter than the surface. VirtualQuery does.
                MEMORY_BASIC_INFORMATION region = {};
                const auto queried = VirtualQuery(
                    reinterpret_cast<const void*>(image.guest_address),
                    &region,
                    sizeof(region)) == sizeof(region);
                runtime_trace(
                    "native_gpu.guest_upload_region address=0x%016llX "
                    "queried=%d state=0x%08lX protect=0x%08lX "
                    "type=0x%08lX region_bytes=%llu wanted=%llu\n",
                    static_cast<unsigned long long>(
                        image.guest_address),
                    queried ? 1 : 0,
                    static_cast<unsigned long>(region.State),
                    static_cast<unsigned long>(region.Protect),
                    static_cast<unsigned long>(region.Type),
                    static_cast<unsigned long long>(
                        region.RegionSize),
                    static_cast<unsigned long long>(source_size));
                runtime_trace(
                    "native_gpu.guest_%s_upload_unavailable "
                    "state=%u binding=%u address=0x%016llX "
                    "size=%ux%u pitch=%u unified=%u tile=%u bytes=%llu\n",
                    tiled ? "tiled" : "linear",
                    state,
                    image.binding,
                    static_cast<unsigned long long>(
                        image.guest_address),
                    image.width,
                    image.height,
                    source_pitch,
                    image.unified_format,
                    image.tile_mode,
                    static_cast<unsigned long long>(source_size));
                image.upload_unavailable_reported = true;
            }
            return false;
        }
        image.upload_unavailable_reported = false;

        // Detiling only permutes the guest's own bytes and the conversion
        // below only widens them, so an all-zero source can only ever
        // produce an all-zero image - and the rule right underneath then
        // throws every byte of it away. Deciding here instead of four passes
        // later costs one scan that stops at the first nonzero byte, and
        // saves two allocations the size of the image and two full walks
        // over it. Measured on a 126 s run, 1618 of 3582 preparations took
        // this exit, all of them after paying for the detile and the
        // conversion first.
        const auto zero_started = worker_phase_counter();
        auto source_all_zero = bytes_all_zero(
            source,
            static_cast<std::size_t>(source_size));
        g_image_zero_ticks += worker_phase_counter() - zero_started;

        // A depth buffer this renderer never drew and the title never
        // wrote. The depth block clears it on the console - to 1.0, the far
        // plane, which the title's fog pass tests for as "nothing here" -
        // and draws into it; here it is only ever the zeros guest memory
        // starts as, the near plane everywhere, and the fog pass turned
        // the black behind the intro into a purple and blue smear. Read as
        // cleared instead: a 32-bit float image in a Z swizzle mode, all
        // zeros, nothing on the device.
        const auto z_swizzle = image.tile_mode == 4 ||
            image.tile_mode == 8 || image.tile_mode == 16 ||
            image.tile_mode == 20 || image.tile_mode == 24;
        constexpr std::uint32_t kUnified32Float = 22;
        // Not only when it is all zeros. The console clears depth through
        // HTILE metadata and leaves the bytes as they were, so a depth
        // buffer's memory holds whatever an earlier frame left - the
        // loading screen's, for the intro - and the fog pass behind "Sony
        // Interactive Entertainment" read it as geometry everywhere: its
        // reconstruction came out as a magenta and blue wedge. This
        // renderer draws no depth, so nothing in that memory is ever the
        // frame's own; cleared is the only reading that is.
        if (z_swizzle &&
            image.unified_format == kUnified32Float &&
            source_bytes_per_pixel == 4 &&
            (image.surface == nullptr || !image.surface->device_written)) {
            const float far_plane = 1.0f;
            std::uint32_t bits = 0;
            std::memcpy(&bits, &far_plane, sizeof(bits));
            auto* words = reinterpret_cast<std::uint32_t*>(source);
            for (std::uint64_t index = 0; index < source_size / 4; ++index) {
                words[index] = bits;
            }
            source_all_zero = false;
        }

        // A render target sampled as a texture lives only in the surface we
        // rendered into - the guest never writes it through the CPU, so
        // reading its address yields zeros. Uploading those zeros erases the
        // frame's own work. Measured on one run, 65 of 70 uploads were
        // entirely zero, which is how a whole bloom chain ended up sampling
        // black.
        //
        // The rule used to require the surface to have been rendered into
        // already, and that is exactly the case this never catches:
        // descriptors are built when a shader state is submitted, before any
        // of the frame's draws run, so the first time a pass samples a
        // surface the surface is still empty. Measured, every one of the
        // forty-five uploads of nothing in a frame reported
        // `initialized=0 device_written=0` - the surface was found, it just
        // had not been drawn into yet, and the black upload was cached from
        // then on.
        //
        // Dropping that half of the condition costs nothing, because there is
        // no case where uploading zeros is needed: a surface nobody has drawn
        // into is already cleared to black, and one that has been drawn into
        // must not be erased.
        if (source_all_zero && image.surface != nullptr) {
            image.upload_pending = false;
            ++g_image_zero_exits;
            // Watched from here, so later frames can take the verdict
            // without the read for as long as nothing writes the range.
            static const auto watch_empty =
                environment_flag_enabled(
                    "PS5GPU_NATIVE_TRACE_EMPTY_WATCH");
            const auto armed_before =
                watch.armed.load(std::memory_order_acquire);
            const auto written_since =
                watch.dirty.load(std::memory_order_acquire);
            const auto armed_now =
                watch.armed.load(std::memory_order_acquire);
            if (watch_empty) {
                runtime_trace(
                    "native_gpu.guest_empty_watch address=0x%016llX "
                    "bytes=%llu armed_before=%u written_since=%u "
                    "armed_now=%u\n",
                    static_cast<unsigned long long>(
                        image.guest_address),
                    static_cast<unsigned long long>(source_size),
                    armed_before ? 1u : 0u,
                    written_since ? 1u : 0u,
                    armed_now ? 1u : 0u);
            }
            g_image_zero_seen[image.guest_address] =
                ImageZeroVerdict{source_size, zero_epoch, read_serial};
            if (!image.upload_unavailable_reported) {
                image.upload_unavailable_reported = true;
                // Six of the empty images in a run are written by nothing
                // else in it - not a draw, not a dispatch - so their bytes
                // ought to have arrived from asset loading. What the
                // allocation looks like says whether the guest reserved the
                // memory and never filled it, or something else entirely.
                MEMORY_BASIC_INFORMATION region = {};
                const auto queried = VirtualQuery(
                    reinterpret_cast<const void*>(image.guest_address),
                    &region,
                    sizeof(region)) == sizeof(region);
                runtime_trace(
                    "native_gpu.guest_empty_region address=0x%016llX "
                    "queried=%d base=0x%016llX state=0x%08lX "
                    "protect=0x%08lX type=0x%08lX region_bytes=%llu\n",
                    static_cast<unsigned long long>(image.guest_address),
                    queried ? 1 : 0,
                    static_cast<unsigned long long>(
                        reinterpret_cast<std::uintptr_t>(
                            region.AllocationBase)),
                    static_cast<unsigned long>(region.State),
                    static_cast<unsigned long>(region.Protect),
                    static_cast<unsigned long>(region.Type),
                    static_cast<unsigned long long>(region.RegionSize));
                // Twenty-one of the addresses a shader samples are
                // zero, never a draw target, and never uploaded once in a
                // run - so either the asset never arrived or the descriptor
                // names the wrong place. The neighbourhood tells them
                // apart: bytes all around and nothing here means the
                // address is wrong, nothing anywhere means nothing was
                // loaded. Behind PS5GPU_NATIVE_TRACE_EMPTY_NEIGHBOURHOOD
                // because it walks up to sixteen megabytes.
                static const auto scan_neighbourhood =
                    environment_flag_enabled(
                        "PS5GPU_NATIVE_TRACE_EMPTY_NEIGHBOURHOOD");
                if (scan_neighbourhood) {
                    constexpr std::int64_t kPage = 4096;
                    constexpr std::int64_t kReach = 8 << 20;
                    std::int64_t nearest = 0;
                    auto found = false;
                    std::array<std::uint8_t, kPage> page = {};
                    for (std::int64_t step = kPage;
                         step <= kReach && !found;
                         step += kPage) {
                        for (const auto sign : {-1, 1}) {
                            const auto offset = sign * step;
                            const auto probe =
                                static_cast<std::int64_t>(
                                    image.guest_address) + offset;
                            if (probe < 0x10000) {
                                continue;
                            }
                            if (!read_current_process_memory(
                                    static_cast<std::uint64_t>(probe),
                                    page.data(),
                                    page.size(),
                                    nullptr)) {
                                continue;
                            }
                            if (!bytes_all_zero(
                                    page.data(),
                                    page.size())) {
                                nearest = offset;
                                found = true;
                                break;
                            }
                        }
                    }
                    std::uint64_t present_pages = 0;
                    std::uint64_t wanted_pages = 0;
                    if (found) {
                        wanted_pages =
                            (source_size + kPage - 1) / kPage;
                        std::array<std::uint8_t, kPage> run = {};
                        for (std::uint64_t index = 0;
                             index < wanted_pages;
                             ++index) {
                            const auto probe =
                                static_cast<std::int64_t>(
                                    image.guest_address) +
                                nearest +
                                static_cast<std::int64_t>(
                                    index * kPage);
                            if (probe < 0x10000 ||
                                !read_current_process_memory(
                                    static_cast<std::uint64_t>(probe),
                                    run.data(),
                                    run.size(),
                                    nullptr)) {
                                break;
                            }
                            if (bytes_all_zero(run.data(), run.size())) {
                                continue;
                            }
                            ++present_pages;
                        }
                    }
                    std::array<char, 49> head_text = {};
                    if (found) {
                        std::array<std::uint8_t, 16> head = {};
                        if (read_current_process_memory(
                                static_cast<std::uint64_t>(
                                    static_cast<std::int64_t>(
                                        image.guest_address) + nearest),
                                head.data(),
                                head.size(),
                                nullptr)) {
                            for (std::size_t i = 0; i < head.size(); ++i) {
                                std::snprintf(
                                    head_text.data() + i * 3,
                                    4,
                                    "%02X ",
                                    head[i]);
                            }
                        }
                    }
                    runtime_trace(
                        "native_gpu.guest_empty_neighbourhood "
                        "address=0x%016llX found=%d nearest=%lld "
                        "boundary=0x%llX present=%llu/%llu head=%s\n",
                        static_cast<unsigned long long>(
                            image.guest_address),
                        found ? 1 : 0,
                        static_cast<long long>(nearest),
                        static_cast<unsigned long long>(
                            image.guest_address & 0xFFFFULL),
                        static_cast<unsigned long long>(present_pages),
                        static_cast<unsigned long long>(wanted_pages),
                        head_text.data());
                }
                runtime_trace(
                    "native_gpu.guest_image_upload_skipped_empty "
                    "state=%u binding=%u address=0x%016llX "
                    "size=%ux%u unified=%u bytes=%llu\n",
                    state,
                    image.binding,
                    static_cast<unsigned long long>(
                        image.guest_address),
                    image.width,
                    image.height,
                    image.unified_format,
                    static_cast<unsigned long long>(source_size));
            }
            return true;
        }

        const auto* pixel_source = source;
        const auto detile_started = worker_phase_counter();
        if (tiled) {
            if (!exact_detile_guest_image(
                    source,
                    source_size,
                    image.tile_mode,
                    copy_width,
                    copy_height,
                    source_bytes_per_pixel,
                    upload_linear_bytes)) {
                return false;
            }
            pixel_source = upload_linear_bytes.data();
        }
        g_image_detile_ticks += worker_phase_counter() - detile_started;
        const auto convert_started = worker_phase_counter();

        // Every byte of this one is written by the conversion below, so
        // unlike the output of the detiler it needs no fill first.
        auto* rgba = grow_scratch(upload_rgba_bytes, output_size);
        // Pixels that need no conversion go across a row at a time. One
        // call per pixel was twelve million calls for a frame of the intro
        // video, every frame.
        const auto row_copy =
            direct_copy && output_bytes_per_pixel == source_bytes_per_pixel;
        for (std::uint32_t y = 0; row_copy && y < copy_height; ++y) {
            std::memcpy(
                rgba +
                    static_cast<std::size_t>(y) * copy_width *
                        output_bytes_per_pixel,
                pixel_source +
                    static_cast<std::size_t>(y) * source_pitch *
                        source_bytes_per_pixel,
                static_cast<std::size_t>(copy_width) *
                    output_bytes_per_pixel);
        }
        for (std::uint32_t y = 0; !row_copy && y < copy_height; ++y) {
            const auto* source_row =
                pixel_source +
                static_cast<std::size_t>(y) *
                    source_pitch *
                    source_bytes_per_pixel;
            auto* destination_row =
                rgba +
                static_cast<std::size_t>(y) *
                    copy_width *
                    output_bytes_per_pixel;
            for (std::uint32_t x = 0; x < copy_width; ++x) {
                const auto* source_pixel =
                    source_row +
                    static_cast<std::size_t>(x) *
                        source_bytes_per_pixel;
                auto* destination_pixel =
                    destination_row +
                    static_cast<std::size_t>(x) *
                        output_bytes_per_pixel;
                // When the surface carries the guest format exactly, the
                // pixel needs no conversion at all. The expansions below
                // exist only for a surface that fell back to RGBA8, and they
                // write four bytes unconditionally - which would overrun a
                // one- or two-byte destination.
                if (direct_copy) {
                    std::memcpy(
                        destination_pixel,
                        source_pixel,
                        source_bytes_per_pixel);
                    continue;
                }
                switch (image.unified_format) {
                case 1:
                    destination_pixel[0] = source_pixel[0];
                    destination_pixel[1] = 0;
                    destination_pixel[2] = 0;
                    destination_pixel[3] = 255;
                    break;
                case 14:
                    destination_pixel[0] = source_pixel[0];
                    destination_pixel[1] = source_pixel[1];
                    destination_pixel[2] = 0;
                    destination_pixel[3] = 255;
                    break;
                case 22: {
                    float value = 0.0f;
                    std::memcpy(&value, source_pixel, sizeof(value));
                    const auto channel = float_to_unorm8(value);
                    destination_pixel[0] = channel;
                    destination_pixel[1] = channel;
                    destination_pixel[2] = channel;
                    destination_pixel[3] = 255;
                    break;
                }
                case 56:
                    std::memcpy(destination_pixel, source_pixel, 4);
                    break;
                case 65:
                    std::memcpy(destination_pixel, source_pixel, 8);
                    break;
                case 71:
                    if (output_bytes_per_pixel == 8) {
                        std::memcpy(
                            destination_pixel,
                            source_pixel,
                            8);
                    } else {
                        for (std::size_t component = 0;
                             component < 4;
                             ++component) {
                            std::uint16_t half = 0;
                            std::memcpy(
                                &half,
                                source_pixel + component * 2,
                                sizeof(half));
                            destination_pixel[component] =
                                float_to_unorm8(
                                    half_to_float(half));
                        }
                    }
                    break;
                default:
                    return false;
                }
            }
        }

        // Blocks, not pixels, for a compressed image - and the counts
        // gathered from them then describe blocks, which is the honest thing
        // for them to describe when the bytes are not pixels at all.
        const auto pixel_count =
            static_cast<std::uint64_t>(copy_width) * copy_height;
        const auto force_early_texture_white =
            (((state == 11 || state == 13) &&
                 (image.binding == 1 || image.binding == 2)) ||
             (state == 14 && image.binding == 1)) &&
            environment_flag_enabled(
                "PS5GPU_NATIVE_FORCE_EARLY_TEXTURE_WHITE");
        const auto force_early_texture_pattern =
            (((state == 11 || state == 13) &&
                 (image.binding == 1 || image.binding == 2)) ||
             (state == 14 && image.binding == 1)) &&
            environment_flag_enabled(
                "PS5GPU_NATIVE_FORCE_EARLY_TEXTURE_PATTERN");
        if (force_early_texture_white) {
            if (output_bytes_per_pixel == 8) {
                for (std::uint64_t pixel = 0;
                     pixel < pixel_count;
                     ++pixel) {
                    const std::array<std::uint16_t, 4> white = {
                        0x3C00u,
                        0x3C00u,
                        0x3C00u,
                        0x3C00u,
                    };
                    std::memcpy(
                        rgba + pixel * output_bytes_per_pixel,
                        white.data(),
                        sizeof(white));
                }
            } else {
                std::memset(
                    rgba,
                    255,
                    static_cast<std::size_t>(output_size));
            }
        } else if (force_early_texture_pattern) {
            for (std::uint64_t pixel = 0;
                 pixel < pixel_count;
                 ++pixel) {
                const auto offset =
                    static_cast<std::size_t>(
                        pixel * output_bytes_per_pixel);
                if (output_bytes_per_pixel == 8) {
                    const std::array<std::uint16_t, 4> color =
                        image.guest_address ==
                            0x0000000513560000ULL
                        ? std::array<std::uint16_t, 4>{
                            0x3800u,
                            0x3800u,
                            0x3800u,
                            0x3C00u}
                        : std::array<std::uint16_t, 4>{
                            0x3C00u,
                            0,
                            0x3C00u,
                            0x3C00u};
                    std::memcpy(
                        rgba + offset,
                        color.data(),
                        sizeof(color));
                } else if (output_bytes_per_pixel != 4) {
                    // The four-channel patterns below have nowhere to go in a
                    // narrower format; fill it solid instead of writing past
                    // the pixel.
                    for (std::uint32_t byte = 0;
                         byte < output_bytes_per_pixel;
                         ++byte) {
                        rgba[offset + byte] = 255;
                    }
                } else if (
                    image.guest_address ==
                        0x0000000513560000ULL) {
                    rgba[offset] = 128;
                    rgba[offset + 1] = 128;
                    rgba[offset + 2] = 128;
                    rgba[offset + 3] = 255;
                } else {
                    rgba[offset] = 255;
                    rgba[offset + 1] = 0;
                    rgba[offset + 2] = 255;
                    rgba[offset + 3] = 255;
                }
            }
        }

        g_image_convert_ticks +=
            worker_phase_counter() - convert_started;
        struct UploadTimer {
            std::int64_t started;
            ~UploadTimer() {
                g_image_upload_ticks +=
                    worker_phase_counter() - started;
            }
        } upload_timer{worker_phase_counter()};
        const auto upload_hash = hash_words(
            rgba,
            static_cast<std::size_t>(output_size));
        if (image.upload_buffer == VK_NULL_HANDLE ||
            image.upload_memory == VK_NULL_HANDLE ||
            image.upload_size != output_size) {
            if (image.upload_buffer != VK_NULL_HANDLE) {
                destroy_buffer(device, image.upload_buffer, nullptr);
            }
            if (image.upload_memory != VK_NULL_HANDLE) {
                free_memory(device, image.upload_memory, nullptr);
            }
            image.upload_buffer = VK_NULL_HANDLE;
            image.upload_memory = VK_NULL_HANDLE;
            image.upload_size = 0;
            image.upload_ready = false;
            image.upload_pending = false;
            if (!create_host_storage_buffer(
                    output_size,
                    image.upload_buffer,
                    image.upload_memory,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
                runtime_trace(
                    "native_gpu.guest_%s_upload_buffer_failed "
                    "state=%u binding=%u bytes=%llu\n",
                    tiled ? "tiled" : "linear",
                    state,
                    image.binding,
                    static_cast<unsigned long long>(output_size));
                return false;
            }
            image.upload_size = output_size;
        }

        const auto changed =
            !image.upload_ready ||
            image.upload_hash != upload_hash;
        if (changed) {
            void* mapped = nullptr;
            const auto result = map_memory(
                device,
                image.upload_memory,
                0,
                image.upload_size,
                0,
                &mapped);
            if (result != VK_SUCCESS || mapped == nullptr) {
                return false;
            }
            std::memcpy(
                mapped,
                rgba,
                static_cast<std::size_t>(output_size));
            unmap_memory(device, image.upload_memory);

            // These two walks exist only to fill in the trace below, and the
            // trace only prints for an image whose bytes actually changed.
            // Counting them before that was decided meant walking every
            // image of every frame - eight megabytes a piece for a 1080p
            // one - to describe the ones that had not moved. They now run
            // where they are read, which also means they describe the pixels
            // that were just handed to the GPU rather than the ones before
            // the debug fills overwrote them.
            //
            // And only for the first few hundred: past that the log has
            // stopped caring, while the walks - two byte-at-a-time passes
            // over a twelve megabyte video frame - were most of what an
            // upload cost.
            //
            // A cap on the count was not enough: the intro video's frames
            // arrive before the count runs out, and each was still walked a
            // byte at a time. PS5GPU_NATIVE_TRACE_IMAGE_PIXELS asks for it.
            static const auto trace_pixels =
                environment_flag_enabled("PS5GPU_NATIVE_TRACE_IMAGE_PIXELS");
            static std::atomic<std::uint32_t> pixel_traces{0};
            const auto count_pixels = trace_pixels &&
                pixel_traces.fetch_add(1, std::memory_order_relaxed) < 256;
            std::uint64_t source_nonzero_bytes = 0;
            for (std::uint64_t offset = 0;
                 count_pixels && offset < source_size;
                 ++offset) {
                source_nonzero_bytes += source[offset] != 0 ? 1 : 0;
            }
            std::uint64_t nonblack_pixels = 0;
            std::uint64_t nonopaque_pixels = 0;
            for (std::uint64_t pixel = 0;
                 count_pixels && pixel < pixel_count;
                 ++pixel) {
                const auto offset = static_cast<std::size_t>(
                    pixel * output_bytes_per_pixel);
                if (output_bytes_per_pixel == 8) {
                    const auto* components =
                        reinterpret_cast<const std::uint16_t*>(
                            rgba + offset);
                    nonblack_pixels +=
                        components[0] != 0 ||
                        components[1] != 0 ||
                        components[2] != 0
                        ? 1
                        : 0;
                    nonopaque_pixels +=
                        components[3] != 0x3C00u ? 1 : 0;
                } else if (output_bytes_per_pixel == 4) {
                    nonblack_pixels +=
                        rgba[offset] != 0 ||
                        rgba[offset + 1] != 0 ||
                        rgba[offset + 2] != 0
                        ? 1
                        : 0;
                    nonopaque_pixels +=
                        rgba[offset + 3] != 255 ? 1 : 0;
                } else {
                    // One- and two-byte formats have no alpha channel
                    // to read; indexing four bytes in would also run off
                    // the buffer on the last pixel.
                    bool any_set = false;
                    for (std::uint32_t byte = 0;
                         byte < output_bytes_per_pixel;
                         ++byte) {
                        if (rgba[offset + byte] != 0) {
                            any_set = true;
                            break;
                        }
                    }
                    nonblack_pixels += any_set ? 1 : 0;
                }
            }
            image.upload_hash = upload_hash;
            image.upload_pending = true;
            runtime_trace(
                "native_gpu.guest_%s_upload_ready "
                "state=%u binding=%u address=0x%016llX "
                "size=%ux%u unified=%u tile=%u bytes=%llu "
                "hash=0x%016llX\n",
                tiled ? "tiled" : "linear",
                state,
                image.binding,
                static_cast<unsigned long long>(
                    image.guest_address),
                image.width,
                image.height,
                image.unified_format,
                image.tile_mode,
                static_cast<unsigned long long>(output_size),
                static_cast<unsigned long long>(upload_hash));
            if (count_pixels) runtime_trace(
                "native_gpu.guest_image_pixels "
                "state=%u binding=%u source_nonzero_bytes=%llu/%llu "
                "nonblack=%llu/%llu nonopaque=%llu/%llu "
                "forced_white=%u forced_pattern=%u "
                "surface=%d initialized=%d device_written=%d "
                "address=0x%016llX size=%ux%u unified=%u storage=%u\n",
                state,
                image.binding,
                static_cast<unsigned long long>(source_nonzero_bytes),
                static_cast<unsigned long long>(source_size),
                static_cast<unsigned long long>(nonblack_pixels),
                static_cast<unsigned long long>(pixel_count),
                static_cast<unsigned long long>(nonopaque_pixels),
                static_cast<unsigned long long>(pixel_count),
                force_early_texture_white ? 1u : 0u,
                force_early_texture_pattern ? 1u : 0u,
                image.surface != nullptr ? 1 : 0,
                image.surface != nullptr && image.surface->initialized
                    ? 1
                    : 0,
                image.surface != nullptr && image.surface->device_written
                    ? 1
                    : 0,
                static_cast<unsigned long long>(image.guest_address),
                image.width,
                image.height,
                image.unified_format,
                (image.flags & PS5GPU_RESOURCE_IMAGE_STORAGE) != 0 ? 1u : 0u);
        }
        image.upload_ready = true;
        // The watch was armed before the read; arming again here would
        // clear a write that landed during it.
        return true;
    }

    void destroy_render_surface(RenderSurface& surface) {
        if (surface.framebuffer != VK_NULL_HANDLE &&
            destroy_framebuffer != nullptr) {
            destroy_framebuffer(device, surface.framebuffer, nullptr);
        }
        if (surface.view != VK_NULL_HANDLE &&
            destroy_image_view != nullptr) {
            destroy_image_view(device, surface.view, nullptr);
        }
        if (surface.image != VK_NULL_HANDLE && destroy_image != nullptr) {
            destroy_image(device, surface.image, nullptr);
        }
        if (surface.memory != VK_NULL_HANDLE && free_memory != nullptr) {
            free_memory(device, surface.memory, nullptr);
        }
        surface = {};
    }

    void destroy_output_resources() {
        for (auto& [key, surface] : render_surfaces) {
            (void)key;
            destroy_render_surface(surface);
        }
        render_surfaces.clear();
        for (auto& [key, surface] : volume_surfaces) {
            (void)key;
            destroy_render_surface(surface);
        }
        volume_surfaces.clear();
        for (auto& [source, copy] : surface_read_copies) {
            (void)source;
            destroy_render_surface(copy);
        }
        surface_read_copies.clear();
        if (readback_buffer != VK_NULL_HANDLE && destroy_buffer != nullptr) {
            destroy_buffer(device, readback_buffer, nullptr);
        }
        if (readback_memory != VK_NULL_HANDLE && free_memory != nullptr) {
            free_memory(device, readback_memory, nullptr);
        }
        readback_buffer = VK_NULL_HANDLE;
        readback_memory = VK_NULL_HANDLE;
        readback_capacity = 0;
    }

    bool ensure_readback_resources(VkDeviceSize required_size) {
        const VkAllocSiteScope site(VkAllocSite::Readback);
        required_size = std::max<VkDeviceSize>(required_size, 4);
        if (readback_buffer != VK_NULL_HANDLE &&
            readback_memory != VK_NULL_HANDLE &&
            readback_capacity >= required_size) {
            return true;
        }
        if (readback_buffer != VK_NULL_HANDLE) {
            destroy_buffer(device, readback_buffer, nullptr);
            readback_buffer = VK_NULL_HANDLE;
        }
        if (readback_memory != VK_NULL_HANDLE) {
            free_memory(device, readback_memory, nullptr);
            readback_memory = VK_NULL_HANDLE;
        }
        readback_capacity = 0;
        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = required_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        auto result = create_buffer(
            device,
            &buffer_info,
            nullptr,
            &readback_buffer);
        if (result != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements buffer_requirements = {};
        get_buffer_memory_requirements(
            device,
            readback_buffer,
            &buffer_requirements);
        const auto buffer_memory_type = find_memory_type(
            buffer_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (buffer_memory_type == UINT32_MAX) {
            destroy_buffer(device, readback_buffer, nullptr);
            readback_buffer = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo buffer_allocation = {};
        buffer_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        buffer_allocation.allocationSize = buffer_requirements.size;
        buffer_allocation.memoryTypeIndex = buffer_memory_type;
        result = allocate_memory(
            device,
            &buffer_allocation,
            nullptr,
            &readback_memory);
        if (result != VK_SUCCESS ||
            bind_buffer_memory(
                device,
                readback_buffer,
                readback_memory,
                0) != VK_SUCCESS) {
            if (readback_memory != VK_NULL_HANDLE) {
                free_memory(device, readback_memory, nullptr);
            }
            destroy_buffer(device, readback_buffer, nullptr);
            readback_buffer = VK_NULL_HANDLE;
            readback_memory = VK_NULL_HANDLE;
            return false;
        }
        readback_capacity = required_size;
        return true;
    }

    // Surfaces are cached per (address, extent, format), so a surface reached
    // outside the draw loop - the presented buffer, a dump target - has to be
    // asked for under the same format the draws will write it with, or the
    // frame is composed from a second, empty image at the same address.
    std::uint32_t frame_target_unified_format(
        std::uint64_t address,
        std::uint32_t width,
        std::uint32_t height) const {
        const TargetFormatKey key = {address, width, height};
        for (const auto& draw : frame_ir) {
            if (draw.render_target_address == address &&
                draw.render_target_width == width &&
                draw.render_target_height == height) {
                const auto format = draw_render_target_unified_format(draw);
                last_target_format[key] = format;
                return format;
            }
        }
        // No draw this frame writes it, which does not make it a different
        // surface. render_surfaces is keyed by format alongside address and
        // extent, so answering with the default here hands back a second,
        // empty surface for an address that already has a full one - and
        // the composition source, which a frame samples far more often than
        // it renders into, then reads as undefined. Measured before this:
        // real= ran 1 for four frames, 0 for the next six and 1 again, and
        // of the eleven terms behind it the only one that moved was that
        // surface's initialized flag.
        if (const auto seen = last_target_format.find(key);
            seen != last_target_format.end()) {
            return seen->second;
        }
        return kDefaultGuestFormat;
    }

    // Surfaces ask for colour attachment, sampled image and storage image at
    // once. Storage is the demanding one: the packed 32-bit colour formats the
    // guest renders into are attachable and sampleable everywhere but rarely
    // storable, so requiring it would push every one of them back onto the
    // RGBA8 fallback - which is the defect this table exists to remove. Drop
    // the storage bit instead, and only reject a format outright when it
    // cannot be an attachment or be sampled at all.
    VkImageUsageFlags surface_usage_for_format(VkFormat format) {
        if (const auto cached = surface_usage_cache.find(format);
            cached != surface_usage_cache.end()) {
            return cached->second;
        }
        const auto usage = query_surface_usage_for_format(format);
        surface_usage_cache.emplace(format, usage);
        return usage;
    }

    VkImageUsageFlags query_surface_usage_for_format(VkFormat format) const {
        constexpr VkImageUsageFlags base =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (get_physical_device_format_properties == nullptr ||
            physical_device == VK_NULL_HANDLE) {
            return format == VK_FORMAT_R8G8B8A8_UNORM
                ? base | VK_IMAGE_USAGE_STORAGE_BIT
                : 0;
        }
        VkFormatProperties properties = {};
        get_physical_device_format_properties(
            physical_device,
            format,
            &properties);
        // A compressed format can be sampled but never drawn into. Demanding
        // colour attachment support of every format sent BC6H down the
        // fallback path, so twenty-four bindings across four drawing states
        // sampled an R8G8B8A8 reading of compressed data.
        if ((properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
            return 0;
        }
        if ((properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) {
            return VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        return (properties.optimalTilingFeatures &
                   VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0
            ? base | VK_IMAGE_USAGE_STORAGE_BIT
            : base;
    }

    VkFormat supported_surface_format(
        std::uint32_t unified_format,
        bool require_storage = false) {
        // One surface per piece of guest memory. The same target is drawn
        // as sRGB and sampled as UNORM, and keying surfaces on the exact
        // format made those two separate images: the intro video was drawn
        // into one and read, black, out of the other. sRGB is stored as its
        // UNORM twin, so a write and a read of the same memory meet.
        const auto requested = [&] {
            const auto format = render_surface_format(unified_format);
            switch (format) {
                case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
                case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
                case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
                    return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
                default: return format;
            }
        }();
        const auto usage = surface_usage_for_format(requested);
        const auto usable = usage != 0 &&
            (!require_storage ||
             (usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0);
        if (requested == VK_FORMAT_R8G8B8A8_UNORM || usable) {
            return requested;
        }
        static std::mutex unsupported_mutex;
        static std::set<std::uint32_t> unsupported_reported;
        bool first_sighting = false;
        {
            std::lock_guard<std::mutex> guard(unsupported_mutex);
            first_sighting = unsupported_reported
                                 .insert(
                                     (unified_format << 1) |
                                     (require_storage ? 1u : 0u))
                                 .second;
        }
        if (first_sighting) {
            runtime_trace(
                "native_gpu.surface_format_unsupported unified=%u "
                "format=%s storage=%u fallback=R8G8B8A8_UNORM\n",
                unified_format,
                render_surface_format_name(requested),
                require_storage ? 1u : 0u);
        }
        return VK_FORMAT_R8G8B8A8_UNORM;
    }

    // Image, memory and view for a surface. Kept separate from
    // ensure_render_surface because a read copy needs exactly this and no
    // framebuffer: it is only ever sampled and copied into.
    bool create_surface_image(RenderSurface& surface) {
        const VkAllocSiteScope site(VkAllocSite::SurfaceImage);
        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        const auto volume = surface.depth > 1;
        image_info.imageType =
            volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        image_info.format = surface.format;
        image_info.extent = {surface.width, surface.height, surface.depth};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = surface_usage_for_format(surface.format);
        if (volume) {
            // Sampled, stored to and copied in and out of; never drawn into.
            image_info.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto result = create_image(
            device,
            &image_info,
            nullptr,
            &surface.image);
        if (result != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements image_requirements = {};
        get_image_memory_requirements(
            device,
            surface.image,
            &image_requirements);
        const auto image_memory_type = find_memory_type(
            image_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (image_memory_type == UINT32_MAX) {
            destroy_render_surface(surface);
            return false;
        }
        surface.allocated_bytes = image_requirements.size;
        VkMemoryAllocateInfo image_allocation = {};
        image_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        image_allocation.allocationSize = image_requirements.size;
        image_allocation.memoryTypeIndex = image_memory_type;
        result = allocate_memory(
            device,
            &image_allocation,
            nullptr,
            &surface.memory);
        if (result != VK_SUCCESS ||
            bind_image_memory(
                device,
                surface.image,
                surface.memory,
                0) != VK_SUCCESS) {
            destroy_render_surface(surface);
            return false;
        }

        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = surface.image;
        view_info.viewType =
            volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = surface.format;
        view_info.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        result = create_image_view(
            device,
            &view_info,
            nullptr,
            &surface.view);
        if (result != VK_SUCCESS) {
            destroy_render_surface(surface);
            return false;
        }
        return true;
    }

    // A draw that samples the surface it renders into cannot read the
    // attachment directly, so it reads a copy taken immediately before the
    // pass opens. One copy per source surface, reused for the life of the
    // surface.
    RenderSurface* ensure_surface_read_copy(const RenderSurface& source) {
        auto& copy = surface_read_copies[&source];
        if (copy.image != VK_NULL_HANDLE &&
            copy.format == source.format &&
            copy.width == source.width &&
            copy.height == source.height) {
            return &copy;
        }
        destroy_render_surface(copy);
        copy.guest_address = source.guest_address;
        copy.width = source.width;
        copy.height = source.height;
        copy.unified_format = source.unified_format;
        copy.format = source.format;
        copy.bytes_per_pixel = source.bytes_per_pixel;
        if (!create_surface_image(copy)) {
            destroy_render_surface(copy);
            runtime_trace(
                "native_gpu.surface_read_copy_failed "
                "address=0x%016llX size=%ux%u format=%s\n",
                static_cast<unsigned long long>(source.guest_address),
                source.width,
                source.height,
                render_surface_format_name(source.format));
            return nullptr;
        }
        return &copy;
    }

    // Stage the copy so the binding sees the target's contents as they were
    // before this draw. Callers must have closed any open pass first.
    void record_surface_read_copy(
        VkCommandBuffer commands,
        RenderSurface& source,
        RenderSurface& copy) {
        transition_surface(
            commands,
            source,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        transition_surface(
            commands,
            copy,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy region = {};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent = {source.width, source.height, 1};
        cmd_copy_image(
            commands,
            source.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            copy.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &region);
        copy.initialized = true;
        transition_surface(
            commands,
            copy,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    // A volume: the froxel fog behind the intro and the colour grading
    // tables are 3D images. Made as 3D images now, where they used to be
    // their first slice - the fog pass sampled a volume nothing had filled
    // and painted the "Sony Interactive Entertainment" screen with it.
    RenderSurface* ensure_volume_surface(
        std::uint64_t guest_address,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t depth,
        std::uint32_t unified_format,
        bool require_storage) {
        if (guest_address < 0x10000 || width == 0 || height == 0 ||
            depth == 0) {
            return nullptr;
        }
        const auto surface_format =
            supported_surface_format(unified_format, require_storage);
        const VolumeSurfaceKey key = {
            guest_address, width, height, depth, surface_format};
        if (auto existing = volume_surfaces.find(key);
            existing != volume_surfaces.end()) {
            return &existing->second;
        }
        RenderSurface surface;
        surface.guest_address = guest_address;
        surface.width = width;
        surface.height = height;
        surface.depth = depth;
        surface.unified_format = unified_format;
        surface.format = surface_format;
        surface.bytes_per_pixel =
            render_surface_bytes_per_pixel(surface_format);
        if (!create_surface_image(surface)) {
            destroy_render_surface(surface);
            return nullptr;
        }
        const auto [inserted, success] =
            volume_surfaces.emplace(key, std::move(surface));
        if (!success) {
            return nullptr;
        }
        runtime_trace(
            "native_gpu.vulkan_volume_created address=0x%016llX "
            "size=%ux%ux%u unified=%u format=%s bytes=%llu\n",
            static_cast<unsigned long long>(guest_address),
            width,
            height,
            depth,
            unified_format,
            render_surface_format_name(surface_format),
            static_cast<unsigned long long>(
                inserted->second.allocated_bytes));
        return &inserted->second;
    }

    RenderSurface* ensure_render_surface(
        std::uint64_t guest_address,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t unified_format = kDefaultGuestFormat,
        bool require_storage = false) {
        if (guest_address < 0x10000 || width == 0 || height == 0) {
            return nullptr;
        }
        const auto surface_format =
            supported_surface_format(unified_format, require_storage);
        const RenderSurfaceKey key = {
            guest_address,
            width,
            height,
            surface_format,
        };
        if (auto existing = render_surfaces.find(key);
            existing != render_surfaces.end()) {
            return &existing->second;
        }

        RenderSurface surface;
        surface.guest_address = guest_address;
        surface.width = width;
        surface.height = height;
        surface.unified_format = unified_format;
        surface.format = surface_format;
        surface.bytes_per_pixel =
            render_surface_bytes_per_pixel(surface_format);
        if (!create_surface_image(surface)) {
            destroy_render_surface(surface);
            return nullptr;
        }
        // A framebuffer is tied to its render pass, and the two passes for a
        // format differ only in load op - which does not affect compatibility -
        // so one framebuffer per surface serves both. Colour attachment usage
        // is what decides whether the surface can be drawn into at all; a
        // sampled-only guest texture simply goes without.
        VkResult result = VK_SUCCESS;
        const auto attachable =
            (surface_usage_for_format(surface.format) &
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        const auto surface_render_pass = attachable
            ? ensure_render_pass(surface.format, false)
            : VK_NULL_HANDLE;
        if (surface_render_pass != VK_NULL_HANDLE) {
            VkFramebufferCreateInfo framebuffer_info = {};
            framebuffer_info.sType =
                VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer_info.renderPass = surface_render_pass;
            framebuffer_info.attachmentCount = 1;
            framebuffer_info.pAttachments = &surface.view;
            framebuffer_info.width = width;
            framebuffer_info.height = height;
            framebuffer_info.layers = 1;
            result = create_framebuffer(
                device,
                &framebuffer_info,
                nullptr,
                &surface.framebuffer);
            if (result != VK_SUCCESS) {
                destroy_render_surface(surface);
                return nullptr;
            }
        }

        VkDescriptorSetAllocateInfo descriptor_allocate = {};
        descriptor_allocate.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descriptor_allocate.descriptorPool = sampled_descriptor_pool;
        descriptor_allocate.descriptorSetCount = 1;
        descriptor_allocate.pSetLayouts = &sampled_descriptor_set_layout;
        result = allocate_descriptor_sets(
            device,
            &descriptor_allocate,
            &surface.sampled_descriptor_set);
        if (result != VK_SUCCESS) {
            destroy_render_surface(surface);
            return nullptr;
        }
        VkDescriptorImageInfo descriptor_image = {};
        descriptor_image.sampler = sampled_sampler;
        descriptor_image.imageView = surface.view;
        descriptor_image.imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet descriptor_write = {};
        descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_write.dstSet = surface.sampled_descriptor_set;
        descriptor_write.dstBinding = 0;
        descriptor_write.descriptorCount = 1;
        descriptor_write.descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor_write.pImageInfo = &descriptor_image;
        update_descriptor_sets(
            device,
            1,
            &descriptor_write,
            0,
            nullptr);

        const auto [inserted, success] = render_surfaces.emplace(
            key,
            std::move(surface));
        if (!success) {
            return nullptr;
        }
        runtime_trace(
            "native_gpu.vulkan_surface_created address=0x%016llX "
            "size=%ux%u unified=%u format=%s bpp=%u "
            "framebuffer=%u bytes=%llu\n",
            static_cast<unsigned long long>(guest_address),
            width,
            height,
            unified_format,
            render_surface_format_name(surface_format),
            surface.bytes_per_pixel,
            surface.framebuffer != VK_NULL_HANDLE ? 1u : 0u,
            static_cast<unsigned long long>(surface.allocated_bytes));
        return &inserted->second;
    }

    bool update_guest_descriptor_resources(
        std::uint32_t state,
        GuestDescriptorPipeline& guest,
        const RenderSurface* target = nullptr,
        std::uint32_t live_state = 0) {
        // Re-read every buffer address from the live user data before touching
        // it. The manifest records where each descriptor sits in user data
        // (scalar_address) alongside the address the offline recompiler saw;
        // the guest rotates these buffers each frame, so after frame one the
        // baked address points at last frame's data and the picture freezes.
        // Off by default: reading the export stage's user data rebases the
        // pixel stage's buffers to the wrong place, and the frame loses its UI
        // panels - 190 distinct colours down to 53. The plumbing is right and
        // the addresses it finds are real and captured; what is wrong is
        // assuming one user data block serves every descriptor. Enable with
        // PS5GPU_NATIVE_LIVE_BUFFER_REBASE while working on it.
        static const auto rebase_enabled =
            environment_flag_enabled("PS5GPU_NATIVE_LIVE_BUFFER_REBASE");
        if (rebase_enabled && live_state != 0) {
            for (auto& buffer : guest.buffers) {
                std::uint64_t address = 0;
                std::uint64_t records = 0;
                if (!decode_guest_buffer_descriptor(
                        live_state,
                        buffer.stages,
                        buffer.scalar_address,
                        address,
                        records) ||
                    address == buffer.guest_address) {
                    continue;
                }
                static std::set<std::uint64_t> reported;
                const auto key =
                    (static_cast<std::uint64_t>(state) << 32) |
                    buffer.descriptor_index;
                if (reported.insert(key).second) {
                    runtime_trace(
                        "native_gpu.guest_buffer_rebased state=%u "
                        "descriptor=%u baked=0x%016llX live=0x%016llX\n",
                        state,
                        buffer.descriptor_index,
                        static_cast<unsigned long long>(
                            buffer.guest_address),
                        static_cast<unsigned long long>(address));
                }
                buffer.guest_address = address;
                buffer.contents_probed = false;
            }
        }
        if (guest.pipeline == VK_NULL_HANDLE ||
            ((!guest.buffers.empty() || !guest.images.empty()) &&
             guest.descriptor_set == VK_NULL_HANDLE)) {
            return false;
        }

        struct SharedGuestBufferPayload {
            const std::uint8_t* bytes = nullptr;
            std::size_t size = 0;
            bool loaded_guest = false;
        };
        using GuestBufferPayloadKey =
            std::pair<std::uint64_t, std::uint64_t>;
        std::map<GuestBufferPayloadKey, SharedGuestBufferPayload>
            shared_guest_buffers;
        std::uint32_t unique_guest_buffer_reads = 0;
        std::uint32_t reused_guest_buffer_reads = 0;
        // How many descriptors of this dispatch share each address and
        // size. One means nothing else will want the bytes afterwards.
        //
        // Holding the shared ones to the end of the guest submission was
        // tried, which costs no copy anywhere - the bytes are simply still
        // there - and is sound for descriptors the shader cannot write,
        // because a submission reaches the GPU as a unit. It saves almost
        // nothing: 879 hits and 271MB of 8208, reads 8933MB to 8208MB, the
        // buffer phase 5837ms to 5424ms. The repeats the counters report
        // are within a dispatch, which this already shares, or across
        // submissions, which is not safe to share. One run of three also
        // ended in an access violation that was not chased down, and 400ms
        // of 25 seconds does not buy the chasing.
        std::map<GuestBufferPayloadKey, std::uint32_t> payload_users;
        for (const auto& buffer : guest.buffers) {
            if (buffer.guest_address >= 0x10000) {
                ++payload_users[{
                    buffer.guest_address,
                    static_cast<std::uint64_t>(buffer.size),
                }];
            }
        }
        std::size_t arena_used = 0;
        std::vector<std::uint8_t> zero_block;
        const auto buffer_phase_started = worker_phase_counter();
        for (auto& buffer : guest.buffers) {
            ++g_buffer_n;
            g_buffer_bytes += buffer.size;
            // Large read-only buffers the title has not touched since they
            // were last taken - a 32MB one and a dozen of several MB are
            // read by every frame of the intro, 89MB a frame in all.
            buffer.upload_skipped = false;
            buffer.copy_from_shared = VK_NULL_HANDLE;
            buffer.fill_shared = VK_NULL_HANDLE;
            buffer.import_buffer = VK_NULL_HANDLE;
            if (guest_import_supported &&
                buffer.guest_address >= 0x10000 &&
                buffer.size != 0 &&
                (buffer.flags & PS5GPU_RESOURCE_BUFFER_WRITABLE) == 0 &&
                buffer.snapshot.empty()) {
                VkDeviceSize offset = 0;
                const auto imported = find_guest_import(
                    buffer.guest_address,
                    static_cast<std::uint64_t>(buffer.size),
                    offset);
                static const bool bind_imports = [] {
                    const auto* value =
                        std::getenv("PS5GPU_NATIVE_IMPORT_GUEST");
                    return value == nullptr || value[0] != '3';
                }();
                if (imported != VK_NULL_HANDLE && bind_imports) {
                    buffer.import_buffer = imported;
                    buffer.import_offset = offset;
                    buffer.upload_skipped = true;
                    ++guest_import_hits;
                    guest_import_hit_bytes += buffer.size;
                    if (guest_import_hits % 50000 == 0) {
                        runtime_trace(
                            "native_gpu.guest_import_use hits=%llu MB=%llu "
                            "misaligned=%llu outside=%llu imports=%zu\n",
                            static_cast<unsigned long long>(
                                guest_import_hits),
                            static_cast<unsigned long long>(
                                guest_import_hit_bytes >> 20),
                            static_cast<unsigned long long>(
                                guest_import_misaligned),
                            static_cast<unsigned long long>(
                                guest_import_outside),
                            guest_imports.size());
                    }
                    continue;
                }
            }
            if (bind_from_frame_arena(
                    buffer, live_state != 0 ? live_state : state)) {
                buffer.upload_skipped = true;
                continue;
            }
            if (buffer.buffer == VK_NULL_HANDLE &&
                buffer.staging_buffer == VK_NULL_HANDLE) {
                if (!create_host_storage_buffer(
                        buffer.size, buffer.buffer, buffer.memory)) {
                    runtime_trace(
                        "native_gpu.guest_buffer_lazy_failed state=%u "
                        "size=%llu\n",
                        state,
                        static_cast<unsigned long long>(buffer.size));
                    return false;
                }
                ++g_lazy_buffer_creates;
            }
            const auto cacheable =
                buffer.guest_address >= 0x10000 &&
                buffer.size >= kCachedBufferMinimumBytes &&
                (buffer.flags & PS5GPU_RESOURCE_BUFFER_WRITABLE) == 0 &&
                buffer.snapshot.empty() &&
                buffer_cache_enabled();
            if (cacheable &&
                buffer.cached_watch != nullptr &&
                buffer.cached_address == buffer.guest_address &&
                buffer.cached_size == buffer.size) {
                auto* watch =
                    static_cast<GuestWriteWatch*>(buffer.cached_watch);
                if (watch->armed.load(std::memory_order_acquire) &&
                    !watch->dirty.load(std::memory_order_acquire) &&
                    watch->arm_serial.load(std::memory_order_acquire) ==
                        buffer.cached_serial) {
                    buffer.upload_skipped = true;
                    ++g_buffer_cache_hits;
                    g_buffer_cache_bytes += buffer.size;
                    continue;
                }
            }
            const auto shareable =
                cacheable &&
                buffer.staging_buffer != VK_NULL_HANDLE &&
                buffer.buffer != VK_NULL_HANDLE &&
                buffer.size >= kSharedDeviceBufferMinimumBytes &&
                shared_device_buffers_enabled();
            if (shareable) {
                const auto shared = shared_device_buffers.find(
                    {buffer.guest_address, buffer.size});
                if (shared != shared_device_buffers.end() &&
                    shared->second.watch != nullptr) {
                    auto* watch =
                        static_cast<GuestWriteWatch*>(shared->second.watch);
                    if (watch->armed.load(std::memory_order_acquire) &&
                        !watch->dirty.load(std::memory_order_acquire) &&
                        watch->arm_serial.load(std::memory_order_acquire) ==
                            shared->second.serial) {
                        buffer.copy_from_shared = shared->second.buffer;
                        buffer.upload_skipped = true;
                        buffer.cached_watch = watch;
                        buffer.cached_address = buffer.guest_address;
                        buffer.cached_size = buffer.size;
                        buffer.cached_serial = shared->second.serial;
                        ++g_shared_buffer_hits;
                        g_shared_buffer_bytes += buffer.size;
                        continue;
                    }
                }
            }
            buffer.cached_watch = nullptr;
            // Armed before the read, not after it: the title's own thread
            // runs frames ahead of this one and rewrites its rings while we
            // read. A write that lands between a read and a later arming is
            // never seen, and the stale bytes were kept - the intro video
            // went green whole frames at a time. Armed first, any write
            // during or after the read leaves the watch dirty.
            GuestWriteWatch* armed_watch = nullptr;
            std::uint64_t armed_serial = 0;
            if (cacheable) {
                auto& watch = guest_watch_for(
                    buffer.guest_address,
                    static_cast<std::uint64_t>(buffer.size));
                if (guest_watch_arm(watch)) {
                    armed_watch = &watch;
                    armed_serial =
                        watch.arm_serial.load(std::memory_order_acquire);
                }
            }
            void* mapped = nullptr;
            const auto map_started = worker_phase_counter();
            const auto result = map_memory(
                device,
                buffer.host_memory(),
                0,
                buffer.size,
                0,
                &mapped);
            g_buffer_map_ticks += worker_phase_counter() - map_started;
            if (result != VK_SUCCESS || mapped == nullptr) {
                return false;
            }
            // Whatever is copied in below covers the buffer from its start,
            // so only the tail past it needs zeroing. Zeroing all of it first
            // was a second full pass over as much as sixteen megabytes of
            // uncached mapping, every dispatch.
            std::size_t filled = 0;
            auto loaded_guest = false;
            const SharedGuestBufferPayload* shared_payload = nullptr;
            if (buffer.guest_address >= 0x10000) {
                const GuestBufferPayloadKey key = {
                    buffer.guest_address,
                    static_cast<std::uint64_t>(buffer.size),
                };
                if (++g_buffer_frame_reads[key] > 1) {
                    ++g_buffer_repeat_n;
                    g_buffer_repeat_bytes += buffer.size;
                    if ((buffer.flags &
                         PS5GPU_RESOURCE_BUFFER_WRITABLE) == 0) {
                        ++g_buffer_repeat_readonly_n;
                        g_buffer_repeat_readonly_bytes += buffer.size;
                    }
                }
                auto [payload, inserted] =
                    shared_guest_buffers.try_emplace(key);
                if (inserted && payload_users[key] == 1) {
                    // Nothing else in this dispatch wants these bytes, so
                    // they go where they are needed and stay there.
                    const auto wanted =
                        static_cast<std::size_t>(buffer.size);
                    ++g_buffer_read_n;
                    g_buffer_read_bytes += wanted;
                    tally_buffer_read(buffer.guest_address, wanted, buffer.flags);
                    const auto read_started = worker_phase_counter();
                    payload->second.loaded_guest =
                        read_draw_buffer(
                            live_state != 0 ? live_state : state,
                            buffer.guest_address,
                            mapped,
                            wanted);
                    g_buffer_read_ticks +=
                        worker_phase_counter() - read_started;
                    if (payload->second.loaded_guest) {
                        payload->second.bytes =
                            static_cast<const std::uint8_t*>(mapped);
                        payload->second.size = wanted;
                        filled = wanted;
                        tally_buffer_pages(
                            buffer.guest_address, mapped, wanted);
                    }
                    ++unique_guest_buffer_reads;
                } else if (inserted) {
                    if (guest_payload_arena.size() <= arena_used) {
                        guest_payload_arena.emplace_back();
                    }
                    // Moving the outer vector moves the inner ones, and a
                    // moved vector keeps its buffer, so pointers handed out
                    // to earlier payloads stay good.
                    auto& scratch = guest_payload_arena[arena_used++];
                    const auto wanted =
                        static_cast<std::size_t>(buffer.size);
                    if (scratch.size() < wanted) {
                        scratch.resize(wanted);
                    }
                    ++g_buffer_read_n;
                    g_buffer_read_bytes += wanted;
                    tally_buffer_read(buffer.guest_address, wanted, buffer.flags);
                    const auto read_started = worker_phase_counter();
                    payload->second.loaded_guest =
                        read_draw_buffer(
                            live_state != 0 ? live_state : state,
                            buffer.guest_address,
                            scratch.data(),
                            wanted);
                    g_buffer_read_ticks +=
                        worker_phase_counter() - read_started;
                    if (payload->second.loaded_guest) {
                        payload->second.bytes = scratch.data();
                        payload->second.size = wanted;
                        tally_buffer_pages(
                            buffer.guest_address, scratch.data(), wanted);
                    }
                    ++unique_guest_buffer_reads;
                } else {
                    ++reused_guest_buffer_reads;
                }
                shared_payload = &payload->second;
                loaded_guest = shared_payload->loaded_guest;
                static const auto repeat_hash_enabled =
                    environment_flag_enabled(
                        "PS5GPU_NATIVE_TRACE_REPEAT_HASH");
                if (repeat_hash_enabled && loaded_guest &&
                    shared_payload->bytes != nullptr) {
                    const auto hash = hash_bytes(
                        shared_payload->bytes,
                        shared_payload->size);
                    std::lock_guard<std::mutex> hash_guard(
                        g_buffer_frame_hash_lock);
                    const auto seen = g_buffer_frame_hashes.find(key);
                    if (seen == g_buffer_frame_hashes.end()) {
                        g_buffer_frame_hashes.emplace(key, hash);
                    } else if (seen->second == hash) {
                        g_buffer_repeat_same_n.fetch_add(
                            1, std::memory_order_relaxed);
                        g_buffer_repeat_same_bytes.fetch_add(
                            buffer.size,
                            std::memory_order_relaxed);
                    } else {
                        g_buffer_repeat_differ_n.fetch_add(
                            1, std::memory_order_relaxed);
                        g_buffer_repeat_differ_bytes.fetch_add(
                            buffer.size,
                            std::memory_order_relaxed);
                        if ((buffer.flags &
                             PS5GPU_RESOURCE_BUFFER_WRITABLE) == 0) {
                            g_buffer_repeat_differ_ro_n.fetch_add(
                                1, std::memory_order_relaxed);
                            g_buffer_repeat_differ_ro_bytes.fetch_add(
                                buffer.size,
                                std::memory_order_relaxed);
                            static std::mutex differ_mutex;
                            static std::set<std::uint64_t> differ_seen;
                            bool first_sighting = false;
                            {
                                std::lock_guard<std::mutex> guard(
                                    differ_mutex);
                                first_sighting =
                                    differ_seen
                                        .insert(buffer.guest_address)
                                        .second;
                            }
                            if (first_sighting) {
                                runtime_trace(
                                    "native_gpu.buffer_repeat_differs "
                                    "address=0x%016llX bytes=%llu "
                                    "flags=0x%08X\n",
                                    static_cast<unsigned long long>(
                                        buffer.guest_address),
                                    static_cast<unsigned long long>(
                                        buffer.size),
                                    buffer.flags);
                            }
                        }
                        seen->second = hash;
                    }
                }
                if (loaded_guest &&
                    shared_payload->bytes !=
                        static_cast<const std::uint8_t*>(mapped)) {
                    const auto fill_started = worker_phase_counter();
                    g_buffer_fill_bytes += shared_payload->size;
                    std::memcpy(
                        mapped,
                        shared_payload->bytes,
                        shared_payload->size);
                    g_buffer_fill_ticks +=
                        worker_phase_counter() - fill_started;
                    filled = shared_payload->size;
                }
            }
            if (!loaded_guest) {
                ++g_buffer_guest_read_failed;
            }
            if (!loaded_guest && !buffer.snapshot.empty()) {
                ++g_buffer_snapshot_used;
                std::memcpy(
                    mapped,
                    buffer.snapshot.data(),
                    buffer.snapshot.size());
                filled = buffer.snapshot.size();
            }
            filled = std::min(
                filled, static_cast<std::size_t>(buffer.size));
            if (filled < static_cast<std::size_t>(buffer.size)) {
                const auto zero_started = worker_phase_counter();
                std::memset(
                    static_cast<std::uint8_t*>(mapped) + filled,
                    0,
                    static_cast<std::size_t>(buffer.size) - filled);
                g_buffer_fill_ticks +=
                    worker_phase_counter() - zero_started;
            }
            // The same bytes that were just written into the mapping,
            // on the host side. Everything below wants to look at the
            // buffer's contents, and reading them back out of mapped
            // device memory is the expensive way to get them: that
            // memory is uncached, so the probe's byte-at-a-time scan was
            // taking thirteen milliseconds for a megabyte. Whatever the
            // mapping holds is either the guest payload, the snapshot,
            // or the zeroes memset above, and all three are known here.
            const std::uint8_t* host_bytes = nullptr;
            std::size_t host_size = 0;
            if (loaded_guest && shared_payload != nullptr) {
                host_bytes = shared_payload->bytes;
                host_size = shared_payload->size;
            } else if (!buffer.snapshot.empty()) {
                host_bytes = buffer.snapshot.data();
                host_size = buffer.snapshot.size();
            }
            const bool host_covers_buffer =
                host_size == static_cast<std::size_t>(buffer.size);
            if ((buffer.flags &
                    (PS5GPU_RESOURCE_BUFFER_WRITABLE |
                     PS5GPU_RESOURCE_BUFFER_WRITE_BACK)) ==
                    (PS5GPU_RESOURCE_BUFFER_WRITABLE |
                     PS5GPU_RESOURCE_BUFFER_WRITE_BACK) &&
                buffer.guest_address >= 0x10000) {
                if (buffer.writeback_shadow.size() !=
                    static_cast<std::size_t>(buffer.size)) {
                    buffer.writeback_shadow.assign(
                        static_cast<std::size_t>(buffer.size),
                        0);
                }
                const auto shadowed = host_bytes != nullptr
                    ? std::min<std::size_t>(
                          host_size,
                          static_cast<std::size_t>(buffer.size))
                    : std::size_t{0};
                if (shadowed != 0) {
                    std::memcpy(
                        buffer.writeback_shadow.data(),
                        host_bytes,
                        shadowed);
                }
                if (shadowed < buffer.writeback_shadow.size()) {
                    std::memset(
                        buffer.writeback_shadow.data() + shadowed,
                        0,
                        buffer.writeback_shadow.size() - shadowed);
                }
            } else {
                buffer.writeback_shadow.clear();
            }
            static const auto trace_buffer_contents =
                environment_flag_enabled(
                    "PS5GPU_NATIVE_TRACE_BUFFER_CONTENTS");
            if (trace_buffer_contents && !buffer.contents_probed) {
                struct ProbeTimer {
                    std::int64_t started;
                    ~ProbeTimer() {
                        g_buffer_probe_ticks +=
                            worker_phase_counter() - started;
                    }
                } probe_timer{worker_phase_counter()};
                std::uint64_t nonzero_bytes = 0;
                std::uint64_t contents_hash = 0;
                if (host_bytes == nullptr) {
                    // Nothing was copied in, so the mapping is the
                    // zeroes written above and the hash is the hash of
                    // that many zeroes. Reading it back would say the
                    // same thing at uncached speed. The block is kept
                    // across buffers so an empty one costs no
                    // allocation at all.
                    if (zero_block.size() <
                        static_cast<std::size_t>(buffer.size)) {
                        zero_block.assign(
                            static_cast<std::size_t>(buffer.size),
                            0);
                    }
                    nonzero_bytes = 0;
                    contents_hash = hash_bytes(
                        zero_block.data(),
                        static_cast<std::size_t>(buffer.size));
                } else if (host_covers_buffer) {
                    for (std::size_t offset = 0;
                         offset < host_size;
                         ++offset) {
                        nonzero_bytes +=
                            host_bytes[offset] != 0 ? 1 : 0;
                    }
                    contents_hash = hash_bytes(host_bytes, host_size);
                } else {
                    // A snapshot shorter than the buffer leaves the
                    // tail zeroed, and the hash has always covered the
                    // whole buffer, so keep reading the mapping in that
                    // one case rather than changing what it reports.
                    const auto* bytes =
                        static_cast<const std::uint8_t*>(mapped);
                    for (VkDeviceSize offset = 0;
                         offset < buffer.size;
                         ++offset) {
                        nonzero_bytes += bytes[offset] != 0 ? 1 : 0;
                    }
                    contents_hash = hash_bytes(
                        bytes,
                        static_cast<std::size_t>(buffer.size));
                }
                runtime_trace(
                    "native_gpu.guest_buffer_resource "
                    "state=%u descriptor=%u address=0x%016llX "
                    "bytes=%llu snapshot=%llu loaded_guest=%u "
                    "nonzero=%llu hash=0x%016llX\n",
                    state,
                    buffer.descriptor_index,
                    static_cast<unsigned long long>(
                        buffer.guest_address),
                    static_cast<unsigned long long>(buffer.size),
                    static_cast<unsigned long long>(
                        buffer.snapshot.size()),
                    loaded_guest ? 1u : 0u,
                    static_cast<unsigned long long>(nonzero_bytes),
                    static_cast<unsigned long long>(contents_hash));
                buffer.contents_probed = true;
            }
            // A diagnostic: the first sixteen words of the buffer at
            // PS5GPU_NATIVE_TRACE_BUFFER_ADDRESS as the GPU will see them.
            static const auto traced_buffer = [] {
                const auto* value =
                    std::getenv("PS5GPU_NATIVE_TRACE_BUFFER_ADDRESS");
                return value == nullptr ? std::uint64_t{0}
                                        : std::strtoull(value, nullptr, 0);
            }();
            // PS5GPU_NATIVE_TRACE_TARGET_BUFFERS: every buffer of the draws
            // into one render target, the display buffer being the usual.
            static const auto traced_target = [] {
                const auto* value =
                    std::getenv("PS5GPU_NATIVE_TRACE_TARGET_BUFFERS");
                return value == nullptr ? std::uint64_t{0}
                                        : std::strtoull(value, nullptr, 0);
            }();
            const auto for_target = traced_target != 0 && target != nullptr &&
                target->guest_address == traced_target;
            // "1" traces every buffer, to find which address one lives at.
            if ((for_target ||
                 (traced_buffer != 0 &&
                  (traced_buffer == 1 ||
                   buffer.guest_address == traced_buffer))) &&
                mapped != nullptr) {
                const auto* words = static_cast<const std::uint32_t*>(mapped);
                static const VkDeviceSize wanted_floats = [] {
                    const auto* value =
                        std::getenv("PS5GPU_NATIVE_TRACE_BUFFER_FLOATS");
                    return value == nullptr
                        ? VkDeviceSize{16}
                        : static_cast<VkDeviceSize>(
                              std::strtoull(value, nullptr, 0));
                }();
                const auto count =
                    std::min<VkDeviceSize>(buffer.size / 4, wanted_floats);
                std::string text;
                char item[32] = {};
                for (VkDeviceSize index = 0; index < count; ++index) {
                    float value = 0.0f;
                    std::memcpy(&value, &words[index], sizeof(value));
                    std::snprintf(item, sizeof(item), " %g", value);
                    text += item;
                }
                runtime_trace(
                    "native_gpu.traced_buffer state=%u address=0x%016llX "
                    "floats=%s\n",
                    state,
                    static_cast<unsigned long long>(buffer.guest_address),
                    text.c_str());
            }
            if (armed_watch != nullptr && loaded_guest &&
                armed_watch->armed.load(std::memory_order_acquire) &&
                !armed_watch->dirty.load(std::memory_order_acquire) &&
                armed_watch->arm_serial.load(std::memory_order_acquire) ==
                    armed_serial) {
                buffer.cached_watch = armed_watch;
                buffer.cached_address = buffer.guest_address;
                buffer.cached_size = buffer.size;
                buffer.cached_serial = armed_serial;
                if (shareable) {
                    // What was just read goes to the shared copy as well,
                    // copied from the staging buffer by this dispatch.
                    // Later command buffers copy out of it after a full
                    // barrier, so they see it filled.
                    const std::pair<std::uint64_t, VkDeviceSize> key = {
                        buffer.guest_address, buffer.size};
                    auto shared = shared_device_buffers.find(key);
                    if (shared == shared_device_buffers.end() &&
                        shared_device_bytes + buffer.size <=
                            kSharedDeviceBudgetBytes) {
                        SharedDeviceBuffer created;
                        if (create_device_local_buffer(
                                buffer.size,
                                created.buffer,
                                created.memory)) {
                            shared_device_bytes += buffer.size;
                            shared = shared_device_buffers
                                         .emplace(key, created)
                                         .first;
                        }
                    }
                    if (shared != shared_device_buffers.end()) {
                        shared->second.watch = armed_watch;
                        shared->second.serial = armed_serial;
                        buffer.fill_shared = shared->second.buffer;
                    }
                }
            }
            unmap_memory(device, buffer.host_memory());
        }
        if (reused_guest_buffer_reads != 0) {
            runtime_trace(
                "native_gpu.guest_buffer_reads state=%u "
                "unique=%u reused=%u\n",
                state,
                unique_guest_buffer_reads,
                reused_guest_buffer_reads);
        }

        using GuestImageUploadKey = std::tuple<
            std::uint64_t,
            std::uint32_t,
            std::uint32_t,
            std::uint32_t,
            std::uint32_t,
            std::uint32_t>;
        g_resource_buffer_ticks +=
            worker_phase_counter() - buffer_phase_started;
        std::set<GuestImageUploadKey> prepared_image_uploads;
        const auto image_phase_started = worker_phase_counter();
        for (auto& image : guest.images) {
            ++g_image_loop_n;
            image.surface = image.type == 10
                ? ensure_volume_surface(
                      image.guest_address,
                      image.width,
                      image.height,
                      image.depth,
                      image.unified_format,
                      (image.flags & PS5GPU_RESOURCE_IMAGE_STORAGE) != 0)
                : ensure_render_surface(
                      image.guest_address,
                      image.width,
                      image.height,
                      image.unified_format,
                      (image.flags & PS5GPU_RESOURCE_IMAGE_STORAGE) != 0);
            if (image.surface == nullptr) {
                runtime_trace(
                    "native_gpu.guest_descriptor_image_failed "
                    "state=%u binding=%u address=0x%016llX "
                    "size=%ux%u\n",
                    state,
                    image.binding,
                    static_cast<unsigned long long>(
                        image.guest_address),
                    image.width,
                    image.height);
                return false;
            }
            if (image.placeholder) {
                // Nothing to read back or upload for an unbound slot; the
                // shared surface is still transitioned with every other image
                // before the dispatch.
                image.availability_probed = true;
                image.upload_pending = false;
                continue;
            }
            if (!image.availability_probed) {
                std::array<std::uint8_t, 64> head = {};
                const auto readable = read_current_process_memory(
                    image.guest_address,
                    head.data(),
                    head.size(),
                    nullptr);
                runtime_trace(
                    "native_gpu.guest_image_resource "
                    "state=%u binding=%u address=0x%016llX "
                    "size=%ux%u pitch=%u unified=%u tile=%u "
                    "type=%u readable=%u head_hash=0x%016llX\n",
                    state,
                    image.binding,
                    static_cast<unsigned long long>(
                        image.guest_address),
                    image.width,
                    image.height,
                    image.pitch,
                    image.unified_format,
                    image.tile_mode,
                    image.type,
                    readable ? 1u : 0u,
                    static_cast<unsigned long long>(
                        readable
                            ? hash_bytes(head.data(), head.size())
                            : 0));
                image.availability_probed = true;
            }
            if (!image.surface->device_written) {
                const auto storage =
                    (image.flags & PS5GPU_RESOURCE_IMAGE_STORAGE) != 0;
                const GuestImageUploadKey upload_key = {
                    image.guest_address,
                    image.width,
                    image.height,
                    image.unified_format,
                    image.tile_mode,
                    image.pitch,
                };
                if (!storage &&
                    prepared_image_uploads.contains(upload_key)) {
                    image.upload_pending = false;
                } else if (
                    prepare_guest_image_upload(state, image, guest.graphics)) {
                    prepared_image_uploads.insert(upload_key);
                }
            } else {
                image.upload_pending = false;
            }
        }

        g_resource_image_ticks +=
            worker_phase_counter() - image_phase_started;
        std::vector<VkDescriptorBufferInfo> buffer_infos(
            guest.buffers.size());
        for (std::size_t index = 0;
             index < guest.buffers.size();
             ++index) {
            const auto& bound = guest.buffers[index];
            buffer_infos[index].buffer = bound.import_buffer != VK_NULL_HANDLE
                ? bound.import_buffer
                : bound.buffer;
            buffer_infos[index].offset = bound.import_buffer != VK_NULL_HANDLE
                ? bound.import_offset
                : 0;
            buffer_infos[index].range = bound.size;
        }
        std::vector<VkDescriptorImageInfo> image_infos(
            guest.images.size());
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(guest.images.size() + 1);
        if (!buffer_infos.empty()) {
            VkWriteDescriptorSet write = {};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = guest.descriptor_set;
            write.dstBinding = 0;
            write.descriptorCount =
                static_cast<std::uint32_t>(buffer_infos.size());
            write.descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = buffer_infos.data();
            writes.push_back(write);
        }
        for (std::size_t index = 0;
             index < guest.images.size();
             ++index) {
            const auto storage =
                (guest.images[index].flags &
                    PS5GPU_RESOURCE_IMAGE_STORAGE) != 0;
            image_infos[index].sampler = storage
                ? VK_NULL_HANDLE
                : ensure_guest_sampler(
                      guest.images[index].sampler_descriptor);
            // Vulkan cannot sample the attachment a pass is writing, so
            // a binding that names the render target reads a copy instead.
            auto& image = guest.images[index];
            image.read_copy =
                (!storage && target != nullptr && image.surface == target)
                    ? ensure_surface_read_copy(*target)
                    : nullptr;
            if (image.read_copy != nullptr) {
                runtime_trace(
                    "native_gpu.guest_descriptor_read_copy state=%u "
                    "binding=%u address=0x%016llX size=%ux%u\n",
                    state,
                    image.binding,
                    static_cast<unsigned long long>(
                        target->guest_address),
                    target->width,
                    target->height);
            }
            image_infos[index].imageView = image.read_copy != nullptr
                ? image.read_copy->view
                : image.surface->view;
            image_infos[index].imageLayout = storage
                ? VK_IMAGE_LAYOUT_GENERAL
                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet write = {};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = guest.descriptor_set;
            write.dstBinding = guest.images[index].binding;
            write.descriptorCount = 1;
            write.descriptorType = storage
                ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image_infos[index];
            writes.push_back(write);
        }
        // What a draw reads, named by guest address. The picture question
        // keeps coming back to one full-screen pass whose output is black,
        // and the only way to tell "it samples the wrong surface" from "it
        // samples the right one and that surface is empty" is to print
        // what it was handed.
        static const auto trace_draw_inputs =
            environment_flag_enabled("PS5GPU_NATIVE_TRACE_DRAW_INPUTS");
        if (trace_draw_inputs) {
            char text[576] = {};
            int used = 0;
            for (std::size_t index = 0;
                 index < guest.buffers.size() && used >= 0;
                 ++index) {
                const auto& buffer = guest.buffers[index];
                const auto written = std::snprintf(
                    text + used,
                    sizeof(text) - static_cast<std::size_t>(used),
                    " s%u=0x%llX/%lluK",
                    buffer.descriptor_index,
                    static_cast<unsigned long long>(buffer.guest_address),
                    static_cast<unsigned long long>(buffer.size >> 10));
                if (written <= 0 ||
                    static_cast<std::size_t>(used + written) >=
                        sizeof(text) - 1) {
                    break;
                }
                used += written;
            }
            for (std::size_t index = 0;
                 index < guest.images.size() && used >= 0;
                 ++index) {
                const auto& image = guest.images[index];
                if (image.surface == nullptr) {
                    continue;
                }
                const auto written = std::snprintf(
                    text + used,
                    sizeof(text) - static_cast<std::size_t>(used),
                    " b%u=0x%llX/%ux%u%s",
                    image.binding,
                    static_cast<unsigned long long>(
                        image.surface->guest_address),
                    image.surface->width,
                    image.surface->height,
                    image.read_copy != nullptr ? "(copy)" : "");
                if (written <= 0 ||
                    static_cast<std::size_t>(used + written) >=
                        sizeof(text) - 1) {
                    break;
                }
                used += written;
            }
            runtime_trace(
                "native_gpu.draw_inputs state=%u target=0x%016llX "
                "size=%ux%u buffers=%llu images=%llu%s\n",
                state,
                static_cast<unsigned long long>(
                    target != nullptr ? target->guest_address : 0),
                target != nullptr ? target->width : 0,
                target != nullptr ? target->height : 0,
                static_cast<unsigned long long>(guest.buffers.size()),
                static_cast<unsigned long long>(guest.images.size()),
                text);
        }
        update_descriptor_sets(
            device,
            static_cast<std::uint32_t>(writes.size()),
            writes.data(),
            0,
            nullptr);
        guest.frame_resources_ready = true;
        return true;
    }

    bool write_back_guest_buffers(
        std::uint32_t state,
        GuestDescriptorPipeline& guest,
        const std::vector<std::uint32_t>& write_mask,
        std::uint32_t& writeback_count) {
        constexpr std::size_t page_size = 4096;
        // Whether a descriptor the shader never marked may go unexamined.
        // The condition set here was that wb_missed - the mark saying
        // nothing was written while the comparison finds bytes moved -
        // stays at zero for a run. It is zero across seven runs on
        // 2026-09-11, covering 528 to 1540 unmarked descriptors and 2.6 to
        // 7.6 GB each, so this is on now.
        //
        // Measured either way over two runs each, same build, 40 seconds:
        // wb_compare falls from 884 and 960 ms to 320 ms, and the cost of a
        // dispatch from 4.54 and 4.65 ms to 3.80 and 4.33. Flip counts are
        // far too noisy to attribute - 48, 51 against 137, 51 - and are not
        // claimed. PS5GPU_NATIVE_WRITE_MASK=0 returns to comparing
        // everything.
        static const auto skip_unmarked = [] {
            char value[8] = {};
            const auto length = GetEnvironmentVariableA(
                "PS5GPU_NATIVE_WRITE_MASK",
                value,
                static_cast<DWORD>(sizeof(value)));
            return !(length == 1 && value[0] == '0');
        }();
        writeback_count = 0;
        auto all_written = true;
        std::vector<std::uint8_t> live_page(page_size);

        for (auto& buffer : guest.buffers) {
            if ((buffer.flags &
                    (PS5GPU_RESOURCE_BUFFER_WRITABLE |
                     PS5GPU_RESOURCE_BUFFER_WRITE_BACK)) !=
                    (PS5GPU_RESOURCE_BUFFER_WRITABLE |
                     PS5GPU_RESOURCE_BUFFER_WRITE_BACK) ||
                buffer.guest_address < 0x10000 ||
                buffer.writeback_shadow.size() != buffer.size) {
                continue;
            }

            // Zero means the shader took no store to this descriptor,
            // so there is nothing the comparison could find. Skipping is
            // off until the mark and the comparison have been held against
            // each other over a run and agreed.
            const auto marked =
                buffer.descriptor_index >= write_mask.size() ||
                write_mask[buffer.descriptor_index] != 0;
            if (!marked) {
                ++g_writeback_unmarked_n;
                g_writeback_unmarked_bytes += buffer.size;
                if (skip_unmarked) {
                    continue;
                }
            }

            ++g_writeback_n;
            g_writeback_bytes += buffer.size;
            if (buffer.size >= 1024ULL * 1024ULL) {
                ++g_writeback_large_n;
                g_writeback_large_bytes += buffer.size;
            } else {
                ++g_writeback_small_n;
                g_writeback_small_bytes += buffer.size;
            }
            g_writeback_largest =
                std::max<std::uint64_t>(g_writeback_largest, buffer.size);
            void* mapped = nullptr;
            const auto writeback_map_started = worker_phase_counter();
            const auto result = map_memory(
                device,
                buffer.host_memory(),
                0,
                buffer.size,
                0,
                &mapped);
            g_writeback_map_ticks +=
                worker_phase_counter() - writeback_map_started;
            if (result != VK_SUCCESS || mapped == nullptr) {
                runtime_trace(
                    "native_gpu.compute_buffer_writeback_map_failed "
                    "state=%u descriptor=%u result=%d\n",
                    state,
                    buffer.descriptor_index,
                    static_cast<int>(result));
                all_written = false;
                continue;
            }

            const auto* output =
                static_cast<const std::uint8_t*>(mapped);
            auto changed_bytes = std::uint64_t{0};
            auto changed_runs = std::uint64_t{0};
            auto changed_pages = std::uint64_t{0};
            auto written_pages = std::uint64_t{0};
            auto failed_pages = std::uint64_t{0};
            auto first_changed =
                std::numeric_limits<std::size_t>::max();

            const auto compare_started = worker_phase_counter();
            for (std::size_t page_start = 0;
                 page_start < buffer.writeback_shadow.size();
                 page_start += page_size) {
                const auto page_length = std::min(
                    page_size,
                    buffer.writeback_shadow.size() - page_start);
                const auto* output_page = output + page_start;
                auto* shadow_page =
                    buffer.writeback_shadow.data() + page_start;
                const auto scan_started = worker_phase_counter();
                const auto same = std::memcmp(
                    output_page,
                    shadow_page,
                    page_length) == 0;
                g_writeback_scan_ticks +=
                    worker_phase_counter() - scan_started;
                if (same) {
                    continue;
                }

                const auto splice_started = worker_phase_counter();
                ++g_writeback_splice_n;
                ++changed_pages;
                ++g_writeback_changed_pages;
                if (!read_current_process_memory(
                        buffer.guest_address + page_start,
                        live_page.data(),
                        page_length,
                        nullptr)) {
                    std::memcpy(
                        live_page.data(),
                        shadow_page,
                        page_length);
                }

                auto cursor = std::size_t{0};
                while (cursor < page_length) {
                    while (cursor < page_length &&
                           output_page[cursor] == shadow_page[cursor]) {
                        ++cursor;
                    }
                    if (cursor == page_length) {
                        break;
                    }
                    const auto run_start = cursor;
                    while (cursor < page_length &&
                           output_page[cursor] != shadow_page[cursor]) {
                        live_page[cursor] = output_page[cursor];
                        ++cursor;
                    }
                    changed_bytes += cursor - run_start;
                    ++changed_runs;
                    first_changed = std::min(
                        first_changed,
                        page_start + run_start);
                }

                if (write_current_process_memory(
                        buffer.guest_address + page_start,
                        live_page.data(),
                        page_length)) {
                    std::memcpy(
                        shadow_page,
                        output_page,
                        page_length);
                    ++written_pages;
                } else {
                    ++failed_pages;
                    all_written = false;
                }
                g_writeback_splice_ticks +=
                    worker_phase_counter() - splice_started;
            }

            g_writeback_compare_ticks +=
                worker_phase_counter() - compare_started;
            if (changed_pages == 0 && failed_pages == 0) {
                ++g_writeback_clean_n;
            } else if (!marked) {
                // The shader said it wrote nothing and the bytes moved
                // anyway. Nothing may be skipped on the mark until this
                // stays at zero for a run.
                ++g_writeback_false_clean;
                runtime_trace(
                    "native_gpu.writeback_mark_missed state=%u "
                    "descriptor=%u bytes=%llu pages=%llu\n",
                    state,
                    buffer.descriptor_index,
                    static_cast<unsigned long long>(buffer.size),
                    static_cast<unsigned long long>(changed_pages));
            }

            // Only the trace below reads this, and the trace only runs when
            // something changed. Hashing the whole mapping unconditionally
            // was another full pass per dispatch for nothing.
            std::uint64_t output_hash = 0;
            if (changed_pages != 0 || failed_pages != 0) {
                output_hash = hash_bytes(
                    output,
                    static_cast<std::size_t>(buffer.size));
            }
            std::array<std::uint8_t, 16> changed_head = {};
            auto changed_head_size = std::size_t{0};
            if (first_changed !=
                std::numeric_limits<std::size_t>::max()) {
                changed_head_size = std::min(
                    changed_head.size(),
                    static_cast<std::size_t>(buffer.size) -
                        first_changed);
                std::memcpy(
                    changed_head.data(),
                    output + first_changed,
                    changed_head_size);
            }
            unmap_memory(device, buffer.host_memory());

            if (changed_pages != 0 || failed_pages != 0) {
                ++writeback_count;
                runtime_trace(
                    "native_gpu.compute_buffer_writeback "
                    "state=%u descriptor=%u address=0x%016llX "
                    "bytes=%llu changed_bytes=%llu runs=%llu "
                    "pages=%llu/%llu failed=%llu "
                    "first_changed=%lld head=%02X%02X%02X%02X "
                    "hash=0x%016llX\n",
                    state,
                    buffer.descriptor_index,
                    static_cast<unsigned long long>(
                        buffer.guest_address),
                    static_cast<unsigned long long>(buffer.size),
                    static_cast<unsigned long long>(changed_bytes),
                    static_cast<unsigned long long>(changed_runs),
                    static_cast<unsigned long long>(written_pages),
                    static_cast<unsigned long long>(changed_pages),
                    static_cast<unsigned long long>(failed_pages),
                    first_changed ==
                            std::numeric_limits<std::size_t>::max()
                        ? -1LL
                        : static_cast<long long>(first_changed),
                    changed_head_size > 0 ? changed_head[0] : 0,
                    changed_head_size > 1 ? changed_head[1] : 0,
                    changed_head_size > 2 ? changed_head[2] : 0,
                    changed_head_size > 3 ? changed_head[3] : 0,
                    static_cast<unsigned long long>(output_hash));
            }
        }
        return all_written;
    }

    // Rejecting a draw here means it does not run at all, so say which image
    // and which condition did it - once per (state, binding), because the
    // answer is a property of the state, not of the frame.
    static bool guest_descriptor_images_ready(
        const GuestDescriptorPipeline& guest,
        const RenderSurface& target,
        std::uint32_t state) {
        if (!guest.frame_resources_ready) {
            report_guest_descriptor_rejected(state, 0, "resources");
            return false;
        }
        bool ready = true;
        for (const auto& image : guest.images) {
            const char* reason = nullptr;
            // A storage image in a graphics stage is how a pixel shader
            // reads texels by integer coordinate - the intro video's
            // colour conversion reads its NV12 frame that way. Refusing
            // every one of them refused that draw, and the video never
            // reached the screen. Only an image that is also the target
            // cannot be bound this way.
            if ((image.flags & PS5GPU_RESOURCE_IMAGE_STORAGE) != 0 &&
                image.surface == &target) {
                reason = "storage-target";
            } else if (image.surface == nullptr) {
                reason = "no-surface";
            } else if (image.surface == &target &&
                       image.read_copy == nullptr) {
                reason = "samples-target";
            } else if (!image.surface->initialized) {
                reason = "uninitialized";
            } else if ((image.flags & PS5GPU_RESOURCE_IMAGE_STORAGE) != 0) {
                if (image.surface->layout != VK_IMAGE_LAYOUT_GENERAL) {
                    reason = "storage-layout";
                }
            } else if (
                // The copy is what gets sampled only when this draw is the one
                // writing that surface. A pipeline shared across targets keeps
                // its copy from an earlier draw, and judging an ordinary
                // sample by that copy's layout rejected draws that were fine.
                ((image.surface == &target && image.read_copy != nullptr)
                     ? image.read_copy->layout
                     : image.surface->layout) !=
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                reason = "layout";
            }
            if (reason != nullptr) {
                report_guest_descriptor_rejected(
                    state,
                    image.binding,
                    reason);
                ready = false;
            }
        }
        return ready;
    }

    static void report_guest_descriptor_rejected(
        std::uint32_t state,
        std::uint32_t binding,
        const char* reason) {
        static std::mutex rejected_mutex;
        static std::set<std::uint64_t> rejected_reported;
        const auto key =
            (static_cast<std::uint64_t>(state) << 32) | binding;
        bool first_sighting = false;
        {
            std::lock_guard<std::mutex> guard(rejected_mutex);
            first_sighting = rejected_reported.insert(key).second;
        }
        if (first_sighting) {
            runtime_trace(
                "native_gpu.guest_descriptor_rejected state=%u "
                "binding=%u reason=%s\n",
                state,
                binding,
                reason);
        }
    }

    bool update_astro_post_descriptors(
        const GpuIrDraw& draw,
        RenderSurface& source) {
        if (!draw.astro_post_process ||
            draw.astro_post_pass_index >=
                astro_post_pipelines.size() ||
            sampled_sampler == VK_NULL_HANDLE) {
            return false;
        }
        auto& pass =
            astro_post_pipelines[draw.astro_post_pass_index];
        const auto& config =
            kAstroPostPasses[draw.astro_post_pass_index];
        if (pass.descriptor_set == VK_NULL_HANDLE ||
            pass.pipeline == VK_NULL_HANDLE ||
            std::any_of(
                pass.storage_memory.begin(),
                pass.storage_memory.end(),
                [](VkDeviceMemory memory) {
                    return memory == VK_NULL_HANDLE;
                })) {
            return false;
        }

        for (std::size_t index = 0;
             index < pass.storage_memory.size();
             ++index) {
            const auto address =
                draw.astro_post_buffer_addresses[index];
            if (address < 0x10000) {
                return false;
            }
            void* mapped = nullptr;
            const auto result = map_memory(
                device,
                pass.storage_memory[index],
                0,
                pass.storage_sizes[index],
                0,
                &mapped);
            if (result != VK_SUCCESS || mapped == nullptr) {
                return false;
            }
            std::memset(
                mapped,
                0,
                static_cast<std::size_t>(
                    pass.storage_sizes[index]));
            const auto read = read_current_process_memory(
                address,
                mapped,
                static_cast<std::size_t>(
                    pass.storage_sizes[index]),
                nullptr);
            unmap_memory(device, pass.storage_memory[index]);
            if (!read) {
                runtime_trace(
                    "native_gpu.astro_post_buffer_unavailable "
                    "state=%u binding=%llu address=0x%016llX "
                    "bytes=%llu\n",
                    config.shader_state_file,
                    static_cast<unsigned long long>(index),
                    static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(
                        pass.storage_sizes[index]));
                return false;
            }
        }

        std::array<
            VkDescriptorBufferInfo,
            kAstroPostStorageBufferCount> buffer_infos = {};
        for (std::size_t index = 0;
             index < buffer_infos.size();
             ++index) {
            buffer_infos[index].buffer =
                pass.storage_buffers[index];
            buffer_infos[index].offset = 0;
            buffer_infos[index].range =
                pass.storage_sizes[index];
        }
        std::array<
            VkDescriptorImageInfo,
            kAstroPostMaximumImageCount> image_infos = {};
        for (std::uint32_t image = 0;
             image < config.image_count;
             ++image) {
            image_infos[image].sampler = sampled_sampler;
            image_infos[image].imageView = source.view;
            image_infos[image].imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        std::array<
            VkWriteDescriptorSet,
            kAstroPostMaximumImageCount + 1> writes = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = pass.descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount =
            static_cast<std::uint32_t>(buffer_infos.size());
        writes[0].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = buffer_infos.data();
        for (std::uint32_t image = 0;
             image < config.image_count;
             ++image) {
            auto& write = writes[image + 1];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = pass.descriptor_set;
            write.dstBinding = image + 1;
            write.descriptorCount = 1;
            write.descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image_infos[image];
        }
        update_descriptor_sets(
            device,
            config.image_count + 1,
            writes.data(),
            0,
            nullptr);
        if (!pass.resources_ready) {
            runtime_trace(
                "native_gpu.astro_post_resources_ready "
                "state=%u buffers=0x%016llX,0x%016llX,"
                "0x%016llX,0x%016llX "
                "source=0x%016llX/%ux%u images=%u\n",
                config.shader_state_file,
                static_cast<unsigned long long>(
                    draw.astro_post_buffer_addresses[0]),
                static_cast<unsigned long long>(
                    draw.astro_post_buffer_addresses[1]),
                static_cast<unsigned long long>(
                    draw.astro_post_buffer_addresses[2]),
                static_cast<unsigned long long>(
                    draw.astro_post_buffer_addresses[3]),
                static_cast<unsigned long long>(
                    source.guest_address),
                source.width,
                source.height,
                config.image_count);
        }
        pass.resources_ready = true;
        return true;
    }

    bool update_astro_state28_descriptors(
        const std::array<RenderSurface*, 4>& images) {
        if (astro_state28_descriptor_set == VK_NULL_HANDLE ||
            sampled_sampler == VK_NULL_HANDLE ||
            std::any_of(
                astro_state28_storage_memory.begin(),
                astro_state28_storage_memory.end(),
                [](VkDeviceMemory memory) {
                    return memory == VK_NULL_HANDLE;
                }) ||
            std::any_of(
                images.begin(),
                images.end(),
                [](const RenderSurface* image) {
                    return image == nullptr;
                })) {
            return false;
        }

        for (std::size_t index = 0;
             index < astro_state28_storage_memory.size();
             ++index) {
            void* mapped = nullptr;
            const auto result = map_memory(
                device,
                astro_state28_storage_memory[index],
                0,
                astro_state28_storage_sizes[index],
                0,
                &mapped);
            if (result != VK_SUCCESS || mapped == nullptr) {
                return false;
            }
            std::memset(
                mapped,
                0,
                static_cast<std::size_t>(
                    astro_state28_storage_sizes[index]));
            const auto read = read_current_process_memory(
                kAstroState28BufferAddresses[index],
                mapped,
                static_cast<std::size_t>(
                    astro_state28_storage_sizes[index]),
                nullptr);
            unmap_memory(
                device,
                astro_state28_storage_memory[index]);
            if (!read) {
                runtime_trace(
                    "native_gpu.astro_state28_buffer_unavailable "
                    "binding=%llu address=0x%016llX bytes=%llu\n",
                    static_cast<unsigned long long>(index),
                    static_cast<unsigned long long>(
                        kAstroState28BufferAddresses[index]),
                    static_cast<unsigned long long>(
                        astro_state28_storage_sizes[index]));
                return false;
            }
        }

        std::array<VkDescriptorBufferInfo, 5> buffer_infos = {};
        for (std::size_t index = 0;
             index < buffer_infos.size();
             ++index) {
            buffer_infos[index].buffer =
                astro_state28_storage_buffers[index];
            buffer_infos[index].offset = 0;
            buffer_infos[index].range =
                astro_state28_storage_sizes[index];
        }
        std::array<VkDescriptorImageInfo, 4> image_infos = {};
        for (std::size_t index = 0;
             index < image_infos.size();
             ++index) {
            image_infos[index].sampler = sampled_sampler;
            image_infos[index].imageView = images[index]->view;
            image_infos[index].imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        std::array<VkWriteDescriptorSet, 5> writes = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = astro_state28_descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount =
            static_cast<std::uint32_t>(buffer_infos.size());
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = buffer_infos.data();
        for (std::uint32_t index = 0;
             index < image_infos.size();
             ++index) {
            writes[index + 1].sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index + 1].dstSet =
                astro_state28_descriptor_set;
            writes[index + 1].dstBinding = index + 1;
            writes[index + 1].descriptorCount = 1;
            writes[index + 1].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[index + 1].pImageInfo = &image_infos[index];
        }
        update_descriptor_sets(
            device,
            static_cast<std::uint32_t>(writes.size()),
            writes.data(),
            0,
            nullptr);
        if (!astro_state28_resources_ready) {
            runtime_trace(
                "native_gpu.astro_state28_resources_ready "
                "buffers=0x%016llX,0x%016llX,0x%016llX,"
                "0x%016llX,0x%016llX "
                "images=0x%016llX,0x%016llX,0x%016llX,0x%016llX\n",
                static_cast<unsigned long long>(
                    kAstroState28BufferAddresses[0]),
                static_cast<unsigned long long>(
                    kAstroState28BufferAddresses[1]),
                static_cast<unsigned long long>(
                    kAstroState28BufferAddresses[2]),
                static_cast<unsigned long long>(
                    kAstroState28BufferAddresses[3]),
                static_cast<unsigned long long>(
                    kAstroState28BufferAddresses[4]),
                static_cast<unsigned long long>(
                    kAstroState28ImageAddresses[0]),
                static_cast<unsigned long long>(
                    kAstroState28ImageAddresses[1]),
                static_cast<unsigned long long>(
                    kAstroState28ImageAddresses[2]),
                static_cast<unsigned long long>(
                    kAstroState28ImageAddresses[3]));
        }
        astro_state28_resources_ready = true;
        return true;
    }

    bool update_astro_state29_descriptors(
        const GpuIrDraw& draw,
        RenderSurface& source) {
        if (!draw.astro_state29 ||
            draw.astro_state29_control_address < 0x10000 ||
            draw.astro_state29_table_address < 0x10000 ||
            draw.guest_buffer_address < 0x10000 ||
            draw.guest_buffer_size == 0 ||
            draw.guest_buffer_size > kAstroState29BufferBytes ||
            astro_state29_descriptor_set == VK_NULL_HANDLE ||
            std::any_of(
                astro_state29_storage_memory.begin(),
                astro_state29_storage_memory.end(),
                [](VkDeviceMemory memory) {
                    return memory == VK_NULL_HANDLE;
                }) ||
            sampled_sampler == VK_NULL_HANDLE) {
            return false;
        }

        const std::array<std::uint64_t, 3> addresses = {
            draw.astro_state29_control_address,
            draw.astro_state29_table_address,
            draw.guest_buffer_address,
        };
        const std::array<std::uint32_t, 3> byte_sizes = {
            static_cast<std::uint32_t>(
                kAstroState29AuxBufferBytes),
            static_cast<std::uint32_t>(
                kAstroState29AuxBufferBytes),
            draw.guest_buffer_size,
        };
        for (std::size_t index = 0;
             index < addresses.size();
             ++index) {
            void* mapped = nullptr;
            const auto result = map_memory(
                device,
                astro_state29_storage_memory[index],
                0,
                astro_state29_storage_sizes[index],
                0,
                &mapped);
            if (result != VK_SUCCESS || mapped == nullptr) {
                return false;
            }
            std::memset(
                mapped,
                0,
                static_cast<std::size_t>(
                    astro_state29_storage_sizes[index]));
            const auto read = read_current_process_memory(
                addresses[index],
                mapped,
                byte_sizes[index],
                nullptr);
            unmap_memory(
                device,
                astro_state29_storage_memory[index]);
            if (!read) {
                runtime_trace(
                    "native_gpu.astro_state29_buffer_unavailable "
                    "binding=%llu address=0x%016llX bytes=%u\n",
                    static_cast<unsigned long long>(index),
                    static_cast<unsigned long long>(
                        addresses[index]),
                    byte_sizes[index]);
                return false;
            }
        }

        std::array<VkDescriptorBufferInfo, 3> buffer_infos = {};
        for (std::size_t index = 0;
             index < buffer_infos.size();
             ++index) {
            buffer_infos[index].buffer =
                astro_state29_storage_buffers[index];
            buffer_infos[index].offset = 0;
            buffer_infos[index].range = byte_sizes[index];
        }
        VkDescriptorImageInfo image_info = {};
        image_info.sampler = sampled_sampler;
        image_info.imageView = source.view;
        image_info.imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        std::array<VkWriteDescriptorSet, 2> writes = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = astro_state29_descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount =
            static_cast<std::uint32_t>(buffer_infos.size());
        writes[0].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = buffer_infos.data();
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = astro_state29_descriptor_set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &image_info;
        update_descriptor_sets(
            device,
            static_cast<std::uint32_t>(writes.size()),
            writes.data(),
            0,
            nullptr);
        if (!astro_state29_resources_ready) {
            runtime_trace(
                "native_gpu.astro_state29_resources_ready "
                "buffers=0x%016llX/%u,0x%016llX/%u,"
                "0x%016llX/%u "
                "image=0x%016llX/%ux%u\n",
                static_cast<unsigned long long>(
                    draw.astro_state29_control_address),
                byte_sizes[0],
                static_cast<unsigned long long>(
                    draw.astro_state29_table_address),
                byte_sizes[1],
                static_cast<unsigned long long>(
                    draw.guest_buffer_address),
                byte_sizes[2],
                static_cast<unsigned long long>(
                    source.guest_address),
                source.width,
                source.height);
        }
        astro_state29_resources_ready = true;
        return true;
    }

    bool update_real_composition_descriptors(
        const GpuIrDraw& composition_draw,
        RenderSurface& source,
        RenderSurface& texture1,
        RenderSurface& texture2) {
        if (real_descriptor_set == VK_NULL_HANDLE ||
            sampled_sampler == VK_NULL_HANDLE ||
            real_storage_memory[
                kAstroCompositionPixelBufferIndex] == VK_NULL_HANDLE) {
            return false;
        }

        struct Vertex {
            float position[4];
            float uv[2];
        };
        static_assert(sizeof(Vertex) == 24);
        static constexpr std::array<Vertex, 3> fallback_vertices = {{
            {{-1.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
            {{3.0f, -1.0f, 0.0f, 1.0f}, {2.0f, 0.0f}},
            {{-1.0f, 3.0f, 0.0f, 1.0f}, {0.0f, 2.0f}},
        }};
        std::array<Vertex, 3> vertices = fallback_vertices;
        std::array<std::uint32_t, 4> descriptor = {};
        const auto descriptor_read =
            composition_draw.export_resource_table_address >= 0x10000 &&
            read_current_process_memory(
                composition_draw.export_resource_table_address,
                descriptor.data(),
                sizeof(descriptor),
                nullptr);
        const auto vertex_address = descriptor_read
            ? static_cast<std::uint64_t>(descriptor[0]) |
                (static_cast<std::uint64_t>(descriptor[1] & 0xFFFFu)
                 << 32)
            : 0;
        const auto vertex_stride = descriptor_read
            ? (descriptor[1] >> 16) & 0x3FFFu
            : 0;
        const auto vertex_count = descriptor_read
            ? descriptor[2]
            : 0;
        const auto live_vertices =
            vertex_address >= 0x10000 &&
            vertex_stride == sizeof(Vertex) &&
            vertex_count >= vertices.size() &&
            read_current_process_memory(
                vertex_address,
                vertices.data(),
                sizeof(vertices),
                nullptr);

        const std::array<
            std::uint64_t,
            kAstroCompositionStorageBufferCount> guest_buffers = {
            kAstroState28BufferAddresses[0],
            composition_draw.export_constant_address,
            composition_draw.export_resource_table_address,
            vertex_address,
            kAstroCompositionPixelBufferAddress,
        };
        const auto uses_vertex_data = [&](std::size_t index) {
            switch (real_es_buffer_mode) {
            case RealEsBufferMode::ConstantTable:
                return false;
            case RealEsBufferMode::ConstantVertex:
                return index == 1;
            case RealEsBufferMode::TableVertex:
                return index == 1;
            case RealEsBufferMode::VertexVertex:
                return index < 2;
            }
            return false;
        };
        for (std::size_t index = 0; index < guest_buffers.size(); ++index) {
            void* mapped = nullptr;
            const auto result = map_memory(
                device,
                real_storage_memory[index],
                0,
                real_storage_sizes[index],
                0,
                &mapped);
            if (result != VK_SUCCESS || mapped == nullptr) {
                return false;
            }
            std::memset(
                mapped,
                0,
                static_cast<std::size_t>(real_storage_sizes[index]));
            auto address = guest_buffers[index];
            bool read = false;
            if (index < 2 && uses_vertex_data(index)) {
                std::memcpy(mapped, vertices.data(), sizeof(vertices));
                address = vertex_address;
                read = true;
            } else if (index == kAstroCompositionVertexBufferIndex &&
                       real_storage_sizes[index] >= sizeof(vertices)) {
                // Whatever `vertices` holds is what the draw is going to
                // use, live or fallback, so the binding says the same.
                std::memcpy(mapped, vertices.data(), sizeof(vertices));
                address = live_vertices ? vertex_address : 0;
                read = true;
            } else {
                if (index == 0 &&
                    real_es_buffer_mode ==
                        RealEsBufferMode::TableVertex) {
                    address =
                        composition_draw.export_resource_table_address;
                }
                read = address >= 0x10000 &&
                    read_current_process_memory(
                        address,
                        mapped,
                        static_cast<std::size_t>(
                            real_storage_sizes[index]),
                        nullptr);
            }
            unmap_memory(device, real_storage_memory[index]);
            const auto buffer_required =
                real_composition_mode !=
                    RealCompositionMode::FullscreenCopySource &&
                (index == kAstroCompositionPixelBufferIndex ||
                 real_composition_mode !=
                     RealCompositionMode::FullscreenRealPs);
            if (!read && buffer_required) {
                runtime_trace(
                    "native_gpu.real_composition_buffer_unavailable "
                    "binding=%llu address=0x%016llX bytes=%llu\n",
                    static_cast<unsigned long long>(index),
                    static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(
                        real_storage_sizes[index]));
                return false;
            }
        }

        real_vertex_ready = false;
        if (real_vertex_memory != VK_NULL_HANDLE) {
            void* mapped = nullptr;
            if (map_memory(
                    device,
                    real_vertex_memory,
                    0,
                    kAstroVertexBufferBytes,
                    0,
                    &mapped) == VK_SUCCESS &&
                mapped != nullptr) {
                std::memcpy(mapped, vertices.data(), sizeof(vertices));
                unmap_memory(device, real_vertex_memory);
                real_vertex_address = vertex_address;
                real_vertex_stride = sizeof(Vertex);
                real_vertex_count =
                    static_cast<std::uint32_t>(vertices.size());
                real_vertex_ready = true;
                runtime_trace(
                    "native_gpu.real_composition_vertices "
                    "address=0x%016llX stride=%u count=%u "
                    "source=%s positions="
                    "%.1f,%.1f;%.1f,%.1f;%.1f,%.1f\n",
                    static_cast<unsigned long long>(
                        real_vertex_address),
                    real_vertex_stride,
                    real_vertex_count,
                    live_vertices ? "guest" : "fallback",
                    static_cast<double>(vertices[0].position[0]),
                    static_cast<double>(vertices[0].position[1]),
                    static_cast<double>(vertices[1].position[0]),
                    static_cast<double>(vertices[1].position[1]),
                    static_cast<double>(vertices[2].position[0]),
                    static_cast<double>(vertices[2].position[1]));
            }
        }

        std::array<
            VkDescriptorBufferInfo,
            kAstroCompositionStorageBufferCount> buffer_infos = {};
        for (std::size_t index = 0;
             index < buffer_infos.size();
             ++index) {
            buffer_infos[index].buffer = real_storage_buffers[index];
            buffer_infos[index].offset = 0;
            buffer_infos[index].range = real_storage_sizes[index];
        }
        std::array<VkDescriptorImageInfo, 3> image_infos = {};
        const std::array<RenderSurface*, 3> images = {
            &source,
            &texture1,
            &texture2,
        };
        for (std::size_t index = 0; index < images.size(); ++index) {
            image_infos[index].sampler = sampled_sampler;
            image_infos[index].imageView = images[index]->view;
            image_infos[index].imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        std::array<VkWriteDescriptorSet, 4> writes = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = real_descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount =
            static_cast<std::uint32_t>(buffer_infos.size());
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = buffer_infos.data();
        for (std::uint32_t index = 0; index < image_infos.size(); ++index) {
            writes[index + 1].sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index + 1].dstSet = real_descriptor_set;
            writes[index + 1].dstBinding = index + 1;
            writes[index + 1].descriptorCount = 1;
            writes[index + 1].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[index + 1].pImageInfo = &image_infos[index];
        }
        update_descriptor_sets(
            device,
            static_cast<std::uint32_t>(writes.size()),
            writes.data(),
            0,
            nullptr);
        if (!real_resources_ready) {
            runtime_trace(
                "native_gpu.real_composition_resources_ready "
                "es_buffers=%s "
                "buffer0=0x%016llX buffer1=0x%016llX "
                "buffer2=0x%016llX buffer3=0x%016llX "
                "buffer4=0x%016llX/%llu "
                "images=0x%016llX,0x%016llX,0x%016llX\n",
                real_es_buffer_mode_name(real_es_buffer_mode),
                static_cast<unsigned long long>(
                    kAstroState28BufferAddresses[0]),
                static_cast<unsigned long long>(
                    composition_draw.export_constant_address),
                static_cast<unsigned long long>(
                    composition_draw.export_resource_table_address),
                static_cast<unsigned long long>(vertex_address),
                static_cast<unsigned long long>(
                    kAstroCompositionPixelBufferAddress),
                static_cast<unsigned long long>(
                    kAstroPixelBufferBytes),
                static_cast<unsigned long long>(source.guest_address),
                static_cast<unsigned long long>(texture1.guest_address),
                static_cast<unsigned long long>(texture2.guest_address));
        }
        real_resources_ready = true;
        return true;
    }

    void transition_surface(
        VkCommandBuffer commands,
        RenderSurface& surface,
        VkImageLayout new_layout,
        VkAccessFlags destination_access,
        VkPipelineStageFlags destination_stage) {
        if (surface.layout == new_layout) {
            return;
        }
        VkAccessFlags source_access = 0;
        VkPipelineStageFlags source_stage =
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        switch (surface.layout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            source_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            source_stage =
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            source_access = VK_ACCESS_SHADER_READ_BIT;
            source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            source_access = VK_ACCESS_TRANSFER_READ_BIT;
            source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            source_access = VK_ACCESS_TRANSFER_WRITE_BIT;
            source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_GENERAL:
            source_access =
                VK_ACCESS_MEMORY_READ_BIT |
                VK_ACCESS_MEMORY_WRITE_BIT;
            source_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            break;
        default:
            break;
        }
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = destination_access;
        barrier.oldLayout = surface.layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = surface.image;
        barrier.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        cmd_pipeline_barrier(
            commands,
            source_stage,
            destination_stage,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);
        surface.layout = new_layout;
    }

    // Waits for anything submitted and not yet waited for. Everything
    // that reuses the command buffer or reads what the GPU produced calls
    // this first.
    // The state whose results are still on the GPU, or 0. Its buffers
    // are read back into staging by its own command buffer; what is left
    // is to wait, ask the shader's write marks what moved, and put it in
    // the guest's memory.
    std::uint32_t pending_writeback_state = 0;

    bool flush_pending_writeback() {
        if (pending_writeback_state == 0) {
            return true;
        }
        const auto state_id = pending_writeback_state;
        pending_writeback_state = 0;
        const auto pipeline = guest_compute_pipelines.find(state_id);
        if (pipeline == guest_compute_pipelines.end()) {
            return true;
        }
        auto& guest = pipeline->second;
        if (finish_pending_gpu_work() != VK_SUCCESS) {
            return false;
        }

        // What the shader says it wrote. Four kilobytes against the
        // megabytes the comparison reads.
        std::vector<std::uint32_t> write_mask;
        if (guest.scalar_block_index >= 0 &&
            guest.write_mask_count != 0) {
            auto& block = guest.buffers[guest.scalar_block_index];
            void* mask_mapped = nullptr;
            if (map_memory(
                    device,
                    block.host_memory(),
                    0,
                    block.size,
                    0,
                    &mask_mapped) == VK_SUCCESS &&
                mask_mapped != nullptr) {
                const auto* words =
                    static_cast<const std::uint32_t*>(mask_mapped);
                const auto available =
                    static_cast<std::size_t>(block.size / 4);
                write_mask.assign(guest.write_mask_count, 1u);
                for (std::uint32_t slot = 0;
                     slot < guest.write_mask_count;
                     ++slot) {
                    const std::size_t word = guest.write_mask_base + slot;
                    write_mask[slot] = word < available ? words[word] : 1u;
                }
                unmap_memory(device, block.host_memory());
            }
        }

        std::uint32_t buffer_writebacks = 0;
        struct WritebackTimer {
            std::int64_t started;
            ~WritebackTimer() {
                g_record_writeback_ticks +=
                    worker_phase_counter() - started;
            }
        } writeback_timer{worker_phase_counter()};
        if (!write_back_guest_buffers(
                state_id,
                guest,
                write_mask,
                buffer_writebacks)) {
            runtime_trace(
                "native_gpu.compute_buffer_writeback_failed state=%u\n",
                state_id);
            return false;
        }
        return true;
    }

    // A compute state is used by one dispatch and never again, but its
    // buffers are only released when the device goes away. Retire the ones
    // the queue is long past: this runs where the caller has just waited
    // for the GPU, so nothing being destroyed can still be in flight. The
    // window is generous because a state that is somehow revisited would
    // otherwise be rebuilt, which is correct but slow.
    void retire_spent_compute_states() {
        // On by default at a window of 512 dispatches, which is where the
        // trade sits: at 64 the title keeps coming back to states that have
        // been retired - vk_revived 2311 in a run - and rebuilding them
        // halves the frame rate, 85 flips against 159. At 512 the revivals
        // fall to 342 and 493 over two runs, the flips come back to 156 and
        // 148, and vk_live still holds at 5.1-5.4GB where it otherwise
        // reaches 9.7GB by flip 91 and keeps climbing.
        //
        // One early run with the window at 64 ended after ten flips with
        // exit code zero. It has not happened again in four runs since, and
        // nothing was found to explain it, so it is recorded rather than
        // claimed fixed. PS5GPU_NATIVE_RETIRE_COMPUTE=0 turns this off.
        static const auto enabled = [] {
            char value[8] = {};
            const auto length = GetEnvironmentVariableA(
                "PS5GPU_NATIVE_RETIRE_COMPUTE",
                value,
                static_cast<DWORD>(sizeof(value)));
            return !(length == 1 && value[0] == '0');
        }();
        if (!enabled) {
            return;
        }
        // Tunable while the trade is being measured: too small and states
        // the title comes back to are rebuilt, too large and the memory is
        // not returned. vk_revived says how often the window was too short.
        static const std::uint64_t kKeepDispatches = [] {
            char value[16] = {};
            const auto length = GetEnvironmentVariableA(
                "PS5GPU_NATIVE_RETIRE_KEEP",
                value,
                static_cast<DWORD>(sizeof(value)));
            // 512 was a few frames once the intro ran at seventy
            // dispatches a frame: 6.5 states retired and 4.6 rebuilt every
            // frame. At 2048 the rebuilds fall to 0.7, the worker's compute
            // time from 17.4 to 12.5ms a frame, for 0.6GB more held.
            if (length == 0 || length >= sizeof(value)) {
                return std::uint64_t{2048};
            }
            const auto parsed = std::strtoull(value, nullptr, 0);
            return parsed == 0 ? std::uint64_t{2048} : parsed;
        }();
        if (guest_dispatch_counter <= kKeepDispatches) {
            return;
        }
        const auto oldest_kept = guest_dispatch_counter - kKeepDispatches;
        for (auto entry = guest_compute_pipelines.begin();
             entry != guest_compute_pipelines.end();) {
            if (entry->first == pending_writeback_state ||
                entry->second.last_used_dispatch >= oldest_kept) {
                ++entry;
                continue;
            }
            destroy_guest_descriptor_pipeline(entry->second);
            retired_compute_states.insert(entry->first);
            entry = guest_compute_pipelines.erase(entry);
            g_evicted_compute_states.fetch_add(
                1, std::memory_order_relaxed);
        }
        retire_compute_states_over_budget();
    }

    // The dispatch window alone does not bound the memory. After the intro
    // the title registers a new compute state for most dispatches - the
    // addresses in its user data change - and each builds a device buffer
    // and a staging buffer per descriptor: 2048 dispatches of them was
    // 2.8GB more in twenty frames, and the process ran the machine out of
    // memory. So the states also go, least recently used first, once all
    // of them together hold more than the budget. A state used within the
    // last 64 dispatches is kept whatever the total, since the command ring
    // may still be reading it. PS5GPU_NATIVE_COMPUTE_BUDGET_MB sets the
    // budget (1536); at 768 states were rebuilt 4264 times in a run and
    // a scene frame took 2.5s.
    void retire_compute_states_over_budget() {
        static const std::uint64_t budget = [] {
            const auto* value =
                std::getenv("PS5GPU_NATIVE_COMPUTE_BUDGET_MB");
            const auto parsed =
                value == nullptr ? 0ull : std::strtoull(value, nullptr, 0);
            return (parsed == 0 ? 1536ull : parsed) << 20;
        }();
        const auto bytes_of = [](const GuestDescriptorPipeline& guest) {
            std::uint64_t bytes = 0;
            for (const auto& buffer : guest.buffers) {
                const auto copies =
                    buffer.staging_buffer != VK_NULL_HANDLE ? 2u : 1u;
                bytes += static_cast<std::uint64_t>(buffer.size) * copies;
            }
            return bytes;
        };
        std::uint64_t total = 0;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> by_age;
        by_age.reserve(guest_compute_pipelines.size());
        for (const auto& [state, guest] : guest_compute_pipelines) {
            total += bytes_of(guest);
            by_age.emplace_back(guest.last_used_dispatch, state);
        }
        if (total <= budget) {
            return;
        }
        std::sort(by_age.begin(), by_age.end());
        for (const auto& [last_used, state] : by_age) {
            if (total <= budget) {
                break;
            }
            if (state == pending_writeback_state ||
                last_used + 64 >= guest_dispatch_counter) {
                continue;
            }
            const auto entry = guest_compute_pipelines.find(state);
            if (entry == guest_compute_pipelines.end()) {
                continue;
            }
            total -= std::min(total, bytes_of(entry->second));
            destroy_guest_descriptor_pipeline(entry->second);
            retired_compute_states.insert(state);
            guest_compute_pipelines.erase(entry);
            g_evicted_compute_states.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    VkResult finish_pending_gpu_work() {
        // Anything the ring still has in flight has to be finished too, or
        // a caller that asked for results would read a buffer the GPU is
        // still writing.
        if (command_ring_ready) {
            for (std::size_t slot = 0; slot < kCommandRing; ++slot) {
                if (!command_ring_pending[slot]) {
                    continue;
                }
                wait_for_fences(
                    device,
                    1,
                    &command_ring_fences[slot],
                    VK_TRUE,
                    UINT64_MAX);
                reset_fences(device, 1, &command_ring_fences[slot]);
                command_ring_pending[slot] = false;
            }
        }
        if (!gpu_work_pending) {
            return VK_SUCCESS;
        }
        gpu_work_pending = false;
        return wait_queue_idle(gpu_wait_us_compute, gpu_wait_n_compute);
    }

    // Takes the next slot, waiting only for that one. Returns false when
    // there is no ring, so the caller keeps the old behaviour.
    // Commands in different submissions to one queue may overlap unless
    // something orders them, and nothing did: a frame could start reading
    // what a compute dispatch submitted just before it was still writing -
    // the intro video's picture, converted by compute and drawn by the
    // frame, came out black whenever the two overlapped. That was rare
    // while the worker waited for every dispatch; with a ring and a faster
    // title it was one frame in three. A barrier at the top of every
    // command buffer covers everything submitted before it.
    void order_after_earlier_submissions(VkCommandBuffer buffer) {
        VkMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        cmd_pipeline_barrier(
            buffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,
            1,
            &barrier,
            0,
            nullptr,
            0,
            nullptr);
    }

    bool acquire_command_ring_slot() {
        if (!command_ring_ready) {
            return false;
        }
        // PS5GPU_NATIVE_COMMAND_RING: how many of the slots to use.
        static const std::size_t ring_slots = [] {
            const auto* value = std::getenv("PS5GPU_NATIVE_COMMAND_RING");
            const auto parsed = value == nullptr
                ? kCommandRing
                : static_cast<std::size_t>(std::strtoul(value, nullptr, 0));
            return parsed == 0 || parsed > kCommandRing ? kCommandRing
                                                        : parsed;
        }();
        command_ring_index = (command_ring_index + 1) % ring_slots;
        const auto slot = command_ring_index;
        if (command_ring_pending[slot]) {
            const auto waited = wait_for_fences(
                device,
                1,
                &command_ring_fences[slot],
                VK_TRUE,
                UINT64_MAX);
            if (waited != VK_SUCCESS) {
                return false;
            }
            if (reset_fences(device, 1, &command_ring_fences[slot]) !=
                VK_SUCCESS) {
                return false;
            }
            command_ring_pending[slot] = false;
            g_command_ring_waits.fetch_add(1, std::memory_order_relaxed);
        }
        command_buffer = command_ring[slot];
        g_command_ring_uses.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool trace_compute_storage_output(
        std::uint32_t state,
        std::uint32_t binding,
        RenderSurface& surface) {
        if (!environment_flag_enabled(
                "PS5GPU_NATIVE_COMPUTE_READBACK")) {
            return true;
        }
        if (surface.guest_address != 0x000000055D700000ULL &&
            surface.guest_address != 0x0000000525B10000ULL) {
            return true;
        }
        if (surface.width >
                std::numeric_limits<std::uint64_t>::max() /
                    surface.height /
                    surface.bytes_per_pixel) {
            return false;
        }
        const auto byte_size = static_cast<VkDeviceSize>(
            static_cast<std::uint64_t>(surface.width) *
            surface.height *
            surface.bytes_per_pixel);
        if (!ensure_readback_resources(byte_size)) {
            return false;
        }

        auto result = finish_pending_gpu_work();
        if (result == VK_SUCCESS) {
            result = reset_command_buffer(command_buffer, 0);
        }
        if (result != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags =
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = begin_command_buffer(command_buffer, &begin_info);
        if (result != VK_SUCCESS) {
            return false;
        }
        order_after_earlier_submissions(command_buffer);

        transition_surface(
            command_buffer,
            surface,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {};
        copy.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {
            surface.width,
            surface.height,
            1,
        };
        cmd_copy_image_to_buffer(
            command_buffer,
            surface.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readback_buffer,
            1,
            &copy);
        VkBufferMemoryBarrier readback_barrier = {};
        readback_barrier.sType =
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        readback_barrier.srcAccessMask =
            VK_ACCESS_TRANSFER_WRITE_BIT;
        readback_barrier.dstAccessMask =
            VK_ACCESS_HOST_READ_BIT;
        readback_barrier.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        readback_barrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        readback_barrier.buffer = readback_buffer;
        readback_barrier.size = byte_size;
        cmd_pipeline_barrier(
            command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            0,
            0,
            nullptr,
            1,
            &readback_barrier,
            0,
            nullptr);
        transition_surface(
            command_buffer,
            surface,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        result = end_command_buffer(command_buffer);
        if (result == VK_SUCCESS) {
            VkSubmitInfo submit_info = {};
            submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &command_buffer;
            result = queue_submit(
                queue,
                1,
                &submit_info,
                VK_NULL_HANDLE);
        }
        if (result == VK_SUCCESS) {
            result = wait_queue_idle(
                gpu_wait_us_readback, gpu_wait_n_readback);
        }
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.compute_readback_failed state=%u "
                "binding=%u address=0x%016llX result=%d\n",
                state,
                binding,
                static_cast<unsigned long long>(
                    surface.guest_address),
                static_cast<int>(result));
            return false;
        }

        void* mapped = nullptr;
        result = map_memory(
            device,
            readback_memory,
            0,
            byte_size,
            0,
            &mapped);
        if (result != VK_SUCCESS || mapped == nullptr) {
            return false;
        }
        const auto* bytes =
            static_cast<const std::uint8_t*>(mapped);
        std::uint64_t nonzero_bytes = 0;
        std::uint64_t nonzero_pixels = 0;
        std::uint64_t first_nonzero_pixel =
            std::numeric_limits<std::uint64_t>::max();
        const auto pixel_count =
            static_cast<std::uint64_t>(surface.width) *
            surface.height;
        for (std::uint64_t pixel = 0;
             pixel < pixel_count;
             ++pixel) {
            const auto offset =
                pixel * surface.bytes_per_pixel;
            auto pixel_nonzero = false;
            for (std::uint32_t byte = 0;
                 byte < surface.bytes_per_pixel;
                 ++byte) {
                const auto nonzero = bytes[offset + byte] != 0;
                nonzero_bytes += nonzero ? 1 : 0;
                pixel_nonzero = pixel_nonzero || nonzero;
            }
            if (pixel_nonzero) {
                if (first_nonzero_pixel ==
                    std::numeric_limits<std::uint64_t>::max()) {
                    first_nonzero_pixel = pixel;
                }
                ++nonzero_pixels;
            }
        }
        const auto hash =
            hash_bytes(bytes, static_cast<std::size_t>(byte_size));
        unmap_memory(device, readback_memory);
        runtime_trace(
            "native_gpu.compute_readback state=%u binding=%u "
            "address=0x%016llX size=%ux%u unified=%u format=%s "
            "bpp=%u bytes=%llu hash=0x%016llX "
            "nonzero_bytes=%llu nonzero_pixels=%llu "
            "first_nonzero=%lld\n",
            state,
            binding,
            static_cast<unsigned long long>(
                surface.guest_address),
            surface.width,
            surface.height,
            surface.unified_format,
            render_surface_format_name(surface.format),
            surface.bytes_per_pixel,
            static_cast<unsigned long long>(byte_size),
            static_cast<unsigned long long>(hash),
            static_cast<unsigned long long>(nonzero_bytes),
            static_cast<unsigned long long>(nonzero_pixels),
            first_nonzero_pixel ==
                    std::numeric_limits<std::uint64_t>::max()
                ? -1LL
                : static_cast<long long>(first_nonzero_pixel));
        return true;
    }

    bool submit_compute(
        const RegisteredComputeState& state,
        const Ps5GpuNativeComputeDispatch& dispatch) {
        if (!ready) {
            return false;
        }
        static const auto trace_submits =
            environment_flag_enabled("PS5GPU_NATIVE_TRACE_SUBMITS");
        if (trace_submits) {
            runtime_trace(
                "native_gpu.compute_submitting state=%u shader=0x%016llX "
                "groups=%ux%ux%u\n",
                state.state_id,
                static_cast<unsigned long long>(state.shader_address),
                dispatch.group_count_x,
                dispatch.group_count_y,
                dispatch.group_count_z);
            runtime_trace_flush();
        }
        const auto pipeline_started = worker_phase_counter();
        const auto pipeline_ok = ensure_guest_compute_pipeline(state);
        g_compute_pipeline_ticks +=
            worker_phase_counter() - pipeline_started;
        ++g_compute_pipeline_count;
        if (!pipeline_ok) {
            return false;
        }
        // Before the upload when it is the same state, because the
        // upload writes the staging buffers the write-back reads.
        if (pending_writeback_state == state.state_id &&
            !flush_pending_writeback()) {
            return false;
        }
        const auto pipeline =
            guest_compute_pipelines.find(state.state_id);
        if (pipeline != guest_compute_pipelines.end()) {
            pipeline->second.last_used_dispatch = ++guest_dispatch_counter;
        }
        // Its last dispatch has to be done with the buffers before they
        // are filled again.
        if (pipeline != guest_compute_pipelines.end()) {
            auto& previous = pipeline->second;
            const auto slot = previous.in_flight_slot;
            if (slot < kCommandRing && command_ring_pending[slot] &&
                command_ring_serial[slot] == previous.in_flight_serial) {
                const auto wait_started = worker_phase_counter();
                wait_for_fences(
                    device, 1, &command_ring_fences[slot], VK_TRUE,
                    UINT64_MAX);
                reset_fences(device, 1, &command_ring_fences[slot]);
                command_ring_pending[slot] = false;
                g_record_wait_ticks += worker_phase_counter() - wait_started;
            }
        }
        const auto resource_started = worker_phase_counter();
        const auto resources_ok =
            pipeline != guest_compute_pipelines.end() &&
            update_guest_descriptor_resources(
                state.state_id,
                pipeline->second);
        g_compute_resource_ticks +=
            worker_phase_counter() - resource_started;
        // Anything still outstanding has had the whole of the preparation
        // above to finish in.
        if (!flush_pending_writeback()) {
            return false;
        }
        if (!resources_ok) {
            runtime_trace(
                "native_gpu.compute_resources_failed state=%u "
                "submission=%llu dispatch=%llu\n",
                state.state_id,
                static_cast<unsigned long long>(
                    dispatch.submission_id),
                static_cast<unsigned long long>(
                    dispatch.dispatch_id));
            return false;
        }
        auto& guest = pipeline->second;
        struct RecordTimer {
            std::int64_t started;
            ~RecordTimer() {
                g_compute_record_ticks +=
                    worker_phase_counter() - started;
            }
        } record_timer{worker_phase_counter()};

        const auto wait_started = worker_phase_counter();
        auto result = VK_SUCCESS;
        const auto on_ring = acquire_command_ring_slot();
        if (!on_ring) {
            result = finish_pending_gpu_work();
        }
        if (result == VK_SUCCESS) {
            result = reset_command_buffer(command_buffer, 0);
        }
        g_record_wait_ticks += worker_phase_counter() - wait_started;
        if (result != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags =
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = begin_command_buffer(command_buffer, &begin_info);
        if (result != VK_SUCCESS) {
            return false;
        }
        order_after_earlier_submissions(command_buffer);
        const auto buffer_copy_started = worker_phase_counter();

        // The guest's bytes were written into the staging buffers by
        // update_guest_descriptor_resources before recording started. Carry
        // them across to the device-local buffers the shader binds.
        std::uint32_t staged_uploads = 0;
        for (const auto& buffer : guest.buffers) {
            if (buffer.copy_from_shared != VK_NULL_HANDLE &&
                buffer.buffer != VK_NULL_HANDLE &&
                buffer.size != 0) {
                VkBufferCopy copy = {};
                copy.size = buffer.size;
                cmd_copy_buffer(
                    command_buffer,
                    buffer.copy_from_shared,
                    buffer.buffer,
                    1,
                    &copy);
                ++staged_uploads;
                continue;
            }
            if (buffer.staging_buffer == VK_NULL_HANDLE ||
                buffer.buffer == VK_NULL_HANDLE ||
                buffer.size == 0 ||
                buffer.upload_skipped) {
                continue;
            }
            VkBufferCopy copy = {};
            copy.size = buffer.size;
            cmd_copy_buffer(
                command_buffer,
                buffer.staging_buffer,
                buffer.buffer,
                1,
                &copy);
            if (buffer.fill_shared != VK_NULL_HANDLE) {
                cmd_copy_buffer(
                    command_buffer,
                    buffer.staging_buffer,
                    buffer.fill_shared,
                    1,
                    &copy);
            }
            ++staged_uploads;
        }
        if (staged_uploads != 0) {
            VkMemoryBarrier upload_barrier = {};
            upload_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            upload_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            upload_barrier.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            cmd_pipeline_barrier(
                command_buffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                1,
                &upload_barrier,
                0,
                nullptr,
                0,
                nullptr);
        }

        g_record_buffer_copy_ticks +=
            worker_phase_counter() - buffer_copy_started;

        const auto image_started = worker_phase_counter();
        std::uint32_t uploads = 0;
        for (auto& image : guest.images) {
            if (image.surface == nullptr) {
                return false;
            }
            const auto storage =
                (image.flags &
                    PS5GPU_RESOURCE_IMAGE_STORAGE) != 0;
            // Storage images too: one the dispatch reads before anything
            // on the device wrote it takes its bytes from guest memory.
            if (image.upload_ready &&
                image.upload_pending &&
                image.upload_buffer != VK_NULL_HANDLE) {
                transition_surface(
                    command_buffer,
                    *image.surface,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy copy = {};
                copy.imageSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                copy.imageSubresource.layerCount = 1;
                copy.imageExtent = {
                    image.width,
                    image.height,
                    image.depth,
                };
                cmd_copy_buffer_to_image(
                    command_buffer,
                    image.upload_buffer,
                    image.surface->image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1,
                    &copy);
                image.upload_pending = false;
                image.surface->initialized = true;
                ++uploads;
            }
            transition_surface(
                command_buffer,
                *image.surface,
                storage
                    ? VK_IMAGE_LAYOUT_GENERAL
                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                storage
                    ? VK_ACCESS_SHADER_READ_BIT |
                        VK_ACCESS_SHADER_WRITE_BIT
                    : VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }

        cmd_bind_pipeline(
            command_buffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            guest.pipeline);
        if (guest.descriptor_set != VK_NULL_HANDLE) {
            cmd_bind_descriptor_sets(
                command_buffer,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                guest.pipeline_layout,
                0,
                1,
                &guest.descriptor_set,
                0,
                nullptr);
        }
        g_record_image_ticks += worker_phase_counter() - image_started;
        const auto submit_started = worker_phase_counter();
        struct SubmitTimer {
            std::int64_t started;
            ~SubmitTimer() {
                g_record_submit_ticks +=
                    worker_phase_counter() - started;
            }
        } submit_timer{submit_started};
        const std::uint32_t thread_limits[3] = {
            dispatch.thread_count_x,
            dispatch.thread_count_y,
            dispatch.thread_count_z,
        };
        cmd_push_constants(
            command_buffer,
            guest.pipeline_layout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(thread_limits),
            thread_limits);
        checkpoint(command_buffer, 2, state.state_id,
                   static_cast<std::uint32_t>(dispatch.dispatch_id));
        cmd_dispatch(
            command_buffer,
            dispatch.group_count_x,
            dispatch.group_count_y,
            dispatch.group_count_z);
        checkpoint(command_buffer, 3, state.state_id,
                   static_cast<std::uint32_t>(dispatch.dispatch_id));

        // Only the buffers that are written back need to come home. The
        // shadow is sized during the upload for exactly that set.
        const auto readback_started = worker_phase_counter();
        std::uint32_t staged_readbacks = 0;
        for (std::size_t index = 0; index < guest.buffers.size(); ++index) {
            const auto& buffer = guest.buffers[index];
            const auto is_scalar_block =
                static_cast<int>(index) == guest.scalar_block_index;
            if (buffer.staging_buffer == VK_NULL_HANDLE ||
                buffer.buffer == VK_NULL_HANDLE ||
                (!is_scalar_block &&
                 buffer.writeback_shadow.size() != buffer.size)) {
                continue;
            }
            if (staged_readbacks == 0) {
                VkMemoryBarrier readback_barrier = {};
                readback_barrier.sType =
                    VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                readback_barrier.srcAccessMask =
                    VK_ACCESS_SHADER_WRITE_BIT;
                readback_barrier.dstAccessMask =
                    VK_ACCESS_TRANSFER_READ_BIT;
                cmd_pipeline_barrier(
                    command_buffer,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0,
                    1,
                    &readback_barrier,
                    0,
                    nullptr,
                    0,
                    nullptr);
            }
            VkBufferCopy copy = {};
            copy.size = buffer.size;
            cmd_copy_buffer(
                command_buffer,
                buffer.buffer,
                buffer.staging_buffer,
                1,
                &copy);
            ++staged_readbacks;
        }
        if (staged_readbacks != 0) {
            VkMemoryBarrier staging_host_barrier = {};
            staging_host_barrier.sType =
                VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            staging_host_barrier.srcAccessMask =
                VK_ACCESS_TRANSFER_WRITE_BIT;
            staging_host_barrier.dstAccessMask =
                VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
            cmd_pipeline_barrier(
                command_buffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT,
                0,
                1,
                &staging_host_barrier,
                0,
                nullptr,
                0,
                nullptr);
        }

        VkMemoryBarrier compute_barrier = {};
        compute_barrier.sType =
            VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        compute_barrier.srcAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT;
        compute_barrier.dstAccessMask =
            VK_ACCESS_HOST_READ_BIT |
            VK_ACCESS_MEMORY_READ_BIT |
            VK_ACCESS_MEMORY_WRITE_BIT;
        cmd_pipeline_barrier(
            command_buffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT |
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,
            1,
            &compute_barrier,
            0,
            nullptr,
            0,
            nullptr);

        g_record_readback_ticks +=
            worker_phase_counter() - readback_started;
        const auto end_started = worker_phase_counter();
        result = end_command_buffer(command_buffer);
        if (result == VK_SUCCESS) {
            VkSubmitInfo submit_info = {};
            submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &command_buffer;
            result = queue_submit(
                queue,
                1,
                &submit_info,
                on_ring
                    ? command_ring_fences[command_ring_index]
                    : VK_NULL_HANDLE);
            if (result == VK_SUCCESS && on_ring) {
                command_ring_pending[command_ring_index] = true;
                const auto serial = ++command_ring_serial[command_ring_index];
                guest.in_flight_slot = command_ring_index;
                guest.in_flight_serial = serial;
            }
        }
        g_record_end_ticks += worker_phase_counter() - end_started;
        // Only a dispatch whose buffers go back to the guest needs its
        // results now; the rest are waited for when the command buffer is
        // next reused, by which time the worker has prepared the following
        // dispatch alongside the GPU rather than after it. Measured, 1289
        // of 1346 dispatches in a run write nothing back.
        auto needs_result_now = false;
        for (const auto& buffer : guest.buffers) {
            if ((buffer.flags &
                    (PS5GPU_RESOURCE_BUFFER_WRITABLE |
                     PS5GPU_RESOURCE_BUFFER_WRITE_BACK)) ==
                (PS5GPU_RESOURCE_BUFFER_WRITABLE |
                 PS5GPU_RESOURCE_BUFFER_WRITE_BACK)) {
                needs_result_now = true;
                break;
            }
        }
        const auto wait_before_us = gpu_wait_us_compute;
        const auto result_started = worker_phase_counter();
        if (result == VK_SUCCESS) {
            gpu_work_pending = true;
            if (needs_result_now) {
                pending_writeback_state = state.state_id;
            }
        }
        g_record_result_ticks += worker_phase_counter() - result_started;
        // Per dispatch, so the frame total can be traced back to the
        // shader that owns it rather than shared out evenly.
        const auto dispatch_wait_us =
            gpu_wait_us_compute - wait_before_us;
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.compute_execute_failed state=%u "
                "shader=0x%016llX submission=%llu dispatch=%llu "
                "result=%d\n",
                state.state_id,
                static_cast<unsigned long long>(
                    state.shader_address),
                static_cast<unsigned long long>(
                    dispatch.submission_id),
                static_cast<unsigned long long>(
                    dispatch.dispatch_id),
                static_cast<int>(result));
            if (result == VK_ERROR_DEVICE_LOST) {
                report_checkpoints("compute");
            }
            return false;
        }

        // What the shader says it wrote. Read before the comparison so
        std::uint32_t buffer_writebacks = 0;

        std::uint32_t storage_images = 0;
        std::vector<RenderSurface*> readback_surfaces;
        for (auto& image : guest.images) {
            if ((image.flags &
                 PS5GPU_RESOURCE_IMAGE_STORAGE) == 0 ||
                image.surface == nullptr) {
                continue;
            }
            image.surface->initialized = true;
            image.surface->device_written = true;
            ++storage_images;
            if (std::find(
                    readback_surfaces.begin(),
                    readback_surfaces.end(),
                    image.surface) == readback_surfaces.end()) {
                readback_surfaces.push_back(image.surface);
                (void)trace_compute_storage_output(
                    state.state_id,
                    image.binding,
                    *image.surface);
            }
        }
        {
            static std::atomic<std::uint64_t> sampled_compute_executed{0};
            const auto seen = sampled_compute_executed.fetch_add(1, std::memory_order_relaxed);
            // Once per dispatch filled the log and cost the worker a
            // thirtieth of its time writing it: the first few, then a sample.
            if (seen < 256 || (seen % 1024) == 0) runtime_trace(
            "native_gpu.compute_executed state=%u "
            "shader=0x%016llX owner=%u submission=%llu "
            "dispatch=%llu groups=%u/%u/%u local=%u/%u/%u "
            "threads=%u/%u/%u "
            "buffers=%llu images=%llu storage_images=%u "
            "uploads=%u writebacks=%u staged=%u/%u gpu_us=%llu\n",
            state.state_id,
            static_cast<unsigned long long>(state.shader_address),
            dispatch.owner_handle,
            static_cast<unsigned long long>(
                dispatch.submission_id),
            static_cast<unsigned long long>(
                dispatch.dispatch_id),
            dispatch.group_count_x,
            dispatch.group_count_y,
            dispatch.group_count_z,
            dispatch.local_size_x,
            dispatch.local_size_y,
            dispatch.local_size_z,
            dispatch.thread_count_x,
            dispatch.thread_count_y,
            dispatch.thread_count_z,
            static_cast<unsigned long long>(guest.buffers.size()),
            static_cast<unsigned long long>(guest.images.size()),
            storage_images,
            uploads,
            buffer_writebacks,
            staged_uploads,
            staged_readbacks,
            static_cast<unsigned long long>(dispatch_wait_us));
        }
        return true;
    }

    void accept_draw(const GpuIrDraw& draw) {
        if (!ready) {
            return;
        }
        ++ir_draws;
        frame_ir.push_back(draw);
    }

    // Builds the pipeline of a state compiled during this run, the first
    // time a frame draws with it. Taken out of the live store rather than
    // copied, so a state is built at most once however often it is drawn.
    void ensure_live_guest_pipeline(std::uint32_t state) {
        if (state == 0 ||
            guest_descriptor_pipelines.count(state) != 0 ||
            resource_free_guest_pipelines.count(state) != 0) {
            return;
        }
        LiveGraphicsState live;
        if (!take_live_graphics_state(state, live)) {
            return;
        }
        if (live.es_spirv == nullptr || live.ps_spirv == nullptr) {
            return;
        }
        runtime_trace(
            "native_gpu.live_pipeline_begin state=%u es=%zu ps=%zu\n",
            state,
            live.es_spirv->size(),
            live.ps_spirv->size());
        runtime_trace_flush();
        if (!spirv_uses_descriptors(*live.es_spirv) &&
            !spirv_uses_descriptors(*live.ps_spirv)) {
            runtime_trace(
                "native_gpu.live_pipeline_skipped state=%u "
                "reason=no-descriptors\n",
                state);
            return;
        }
        const auto built = build_guest_descriptor_pipeline(
            state,
            std::move(live.es_spirv),
            std::move(live.ps_spirv),
            live.es_manifest,
            live.ps_manifest,
            false,
            false,
            0,
            0);
        runtime_trace(
            "native_gpu.live_pipeline state=%u result=%d\n",
            state,
            static_cast<int>(built));
    }

    bool submit_frame(const Ps5GpuNativeFlip& flip) {
        struct FlipPhases {
            std::uint64_t started = worker_phase_counter();
            std::uint64_t recording = 0;
            std::uint64_t submitting = 0;
            ~FlipPhases() {
                const auto ended = worker_phase_counter();
                const auto record_at = recording != 0 ? recording : ended;
                const auto submit_at = submitting != 0 ? submitting : ended;
                g_flip_pre_ticks += record_at - started;
                g_flip_record_ticks += submit_at - record_at;
                g_flip_post_ticks += ended - submit_at;
            }
        } flip_phases;
        if (!flush_pending_writeback()) {
            return false;
        }
        // Every state this frame draws with has its pipeline before anything
        // asks about it - the dump selection included.
        for (const auto& draw : frame_ir) {
            ensure_live_guest_pipeline(draw.shader_state_id);
        }
        const auto* frame_dump_path =
            std::getenv("PS5GPU_NATIVE_FRAME_DUMP_PATH");
        std::uint64_t frame_dump_address = 0;
        std::uint64_t frame_dump_flip = 0;
        std::uint32_t frame_dump_state = 0;
        std::uint32_t frame_dump_draw = 0;
        // Dump the target of the first draw that reads the image at this
        // guest address - a way to follow a picture from one pass to the
        // next when state ids and target addresses differ run to run.
        std::uint64_t frame_dump_after_image = 0;
        if (const auto* value =
                std::getenv("PS5GPU_NATIVE_FRAME_DUMP_AFTER_IMAGE");
            value != nullptr && value[0] != '\0') {
            frame_dump_after_image = std::strtoull(value, nullptr, 0);
        }
        // Dump what a self-sampling draw actually reads, rather than what the
        // attachment holds - the two answer different questions when such a
        // draw produces the wrong colour.
        const auto frame_dump_read_copy =
            environment_flag_enabled("PS5GPU_NATIVE_FRAME_DUMP_READ_COPY");
        if (const auto* value =
                std::getenv("PS5GPU_NATIVE_FRAME_DUMP_ADDRESS");
            value != nullptr && value[0] != '\0') {
            char* end = nullptr;
            frame_dump_address = std::strtoull(value, &end, 0);
            if (end == value || (end != nullptr && end[0] != '\0')) {
                frame_dump_address = 0;
            }
        }
        if (const auto* value =
                std::getenv("PS5GPU_NATIVE_FRAME_DUMP_FLIP");
            value != nullptr && value[0] != '\0') {
            char* end = nullptr;
            frame_dump_flip = std::strtoull(value, &end, 0);
            if (end == value ||
                (end != nullptr && end[0] != '\0')) {
                frame_dump_flip = 0;
            }
        }
        if (const auto* value =
                std::getenv("PS5GPU_NATIVE_FRAME_DUMP_STATE");
            value != nullptr && value[0] != '\0') {
            char* end = nullptr;
            const auto parsed = std::strtoul(value, &end, 0);
            if (end != value &&
                (end == nullptr || end[0] == '\0') &&
                parsed <=
                    std::numeric_limits<std::uint32_t>::max()) {
                frame_dump_state =
                    static_cast<std::uint32_t>(parsed);
            }
        }
        if (const auto* value =
                std::getenv("PS5GPU_NATIVE_FRAME_DUMP_DRAW");
            value != nullptr && value[0] != '\0') {
            char* end = nullptr;
            const auto parsed = std::strtoul(value, &end, 0);
            if (end != value &&
                (end == nullptr || end[0] == '\0') &&
                parsed <=
                    std::numeric_limits<std::uint32_t>::max()) {
                frame_dump_draw = static_cast<std::uint32_t>(parsed);
            }
        }
        std::uint32_t frame_dump_width = flip.width;
        std::uint32_t frame_dump_height = flip.height;
        auto parse_dump_dimension = [](const char* name) {
            const auto* value = std::getenv(name);
            if (value == nullptr || value[0] == '\0') {
                return std::uint32_t{0};
            }
            char* end = nullptr;
            const auto parsed = std::strtoul(value, &end, 0);
            if (end == value ||
                (end != nullptr && end[0] != '\0') ||
                parsed > 16384) {
                return std::uint32_t{0};
            }
            return static_cast<std::uint32_t>(parsed);
        };
        const auto requested_dump_width =
            parse_dump_dimension(
                "PS5GPU_NATIVE_FRAME_DUMP_WIDTH");
        const auto requested_dump_height =
            parse_dump_dimension(
                "PS5GPU_NATIVE_FRAME_DUMP_HEIGHT");
        if (frame_dump_address != 0) {
            // An explicit extent wins over the one inferred from a draw: the
            // same guest address can back several views, and inferring from
            // the first matching draw makes the other views undumpable.
            if (requested_dump_width != 0 &&
                requested_dump_height != 0) {
                frame_dump_width = requested_dump_width;
                frame_dump_height = requested_dump_height;
            } else {
                const auto draw = std::find_if(
                    frame_ir.begin(),
                    frame_ir.end(),
                    [frame_dump_address](const GpuIrDraw& candidate) {
                        return candidate.render_target_address ==
                            frame_dump_address;
                    });
                if (draw != frame_ir.end()) {
                    frame_dump_width = draw->render_target_width;
                    frame_dump_height = draw->render_target_height;
                } else {
                    frame_dump_address = 0;
                }
            }
        }
        // Only a frame that draws a mesh into the dumped target, when asked:
        // the scene is drawn about once in every flip or two, and a dump of
        // a flip that only cleared the target says nothing about the mesh.
        static const auto dump_indexed_only =
            environment_flag_enabled("PS5GPU_NATIVE_FRAME_DUMP_INDEXED");
        const auto frame_has_indexed_draw =
            std::any_of(
                frame_ir.begin(),
                frame_ir.end(),
                [&](const GpuIrDraw& draw) {
                    return (draw.flags & PS5GPU_NATIVE_DRAW_INDEXED) != 0 &&
                        draw.vertex_count >= 1024 &&
                        (frame_dump_address == 0 ||
                         draw.render_target_address == frame_dump_address);
                });
        // With an image to follow, only a frame that draws with it.
        auto frame_reads_dump_image = frame_dump_after_image == 0;
        for (const auto& draw : frame_ir) {
            if (frame_reads_dump_image) {
                break;
            }
            const auto found = guest_descriptor_pipelines.find(
                resolve_pipeline_state(draw));
            if (found == guest_descriptor_pipelines.end()) {
                continue;
            }
            static const auto alternate_image = [] {
                const auto* value = std::getenv(
                    "PS5GPU_NATIVE_FRAME_DUMP_AFTER_IMAGE_ALT");
                return value == nullptr ? std::uint64_t{0}
                                        : std::strtoull(value, nullptr, 0);
            }();
            for (const auto& image : found->second.images) {
                // Either half of a double buffer; and an image whose
                // address this frame has not resolved yet - a replay is one
                // frame, and it has not - counts when the draw is into the
                // dumped target.
                if (image.guest_address == frame_dump_after_image ||
                    (alternate_image != 0 &&
                     image.guest_address == alternate_image) ||
                    (image.guest_address == 0 && frame_dump_address != 0 &&
                     draw.render_target_address == frame_dump_address)) {
                    frame_reads_dump_image = true;
                    break;
                }
            }
        }
        // And not the first few of those: a video starts black.
        if (frame_dump_after_image != 0 && frame_reads_dump_image) {
            static std::uint32_t frames_seen = 0;
            static const auto frames_to_skip = [] {
                const auto* value = std::getenv(
                    "PS5GPU_NATIVE_FRAME_DUMP_AFTER_IMAGE_SKIP");
                return value == nullptr
                    ? 0u
                    : static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0));
            }();
            ++frames_seen;
            frame_reads_dump_image = frames_seen > frames_to_skip;
        }
        const bool dump_frame =
            frame_reads_dump_image &&
            (!dump_indexed_only || frame_has_indexed_draw) &&
            !frame_dump_written &&
            flip.flip_id >= frame_dump_next_flip &&
            frame_dump_path != nullptr &&
            frame_dump_path[0] != '\0' &&
            (frame_dump_flip == 0 ||
             flip.flip_id >= frame_dump_flip) &&
            frame_dump_width != 0 &&
            frame_dump_height != 0 &&
            frame_dump_width <=
                std::numeric_limits<std::uint64_t>::max() /
                    frame_dump_height / 4;
        // Why a dump did or did not happen, once. The native producer
        // reached a flip past the one asked for and wrote nothing, and a
        // decision with five inputs is not one to guess at.
        {
            static std::atomic<std::uint32_t> shown{0};
            if (shown.fetch_add(1, std::memory_order_relaxed) < 4) {
                runtime_trace(
                    "native_gpu.frame_dump_decision flip=%llu want=%llu "
                    "path=%d written=%d size=%ux%u dump=%d\n",
                    static_cast<unsigned long long>(flip.flip_id),
                    static_cast<unsigned long long>(frame_dump_flip),
                    frame_dump_path != nullptr &&
                            frame_dump_path[0] != ' '
                        ? 1
                        : 0,
                    frame_dump_written ? 1 : 0,
                    frame_dump_width,
                    frame_dump_height,
                    dump_frame ? 1 : 0);
                runtime_trace_flush();
            }
        }
        // Sized again below once the dump surface is known: a surface that is
        // not four bytes per pixel copies more than this into the buffer.
        // Past the probe pixel, room for the surface probes below.
        auto readback_bytes = dump_frame
            ? static_cast<VkDeviceSize>(
                static_cast<std::uint64_t>(frame_dump_width) *
                frame_dump_height * 4)
            : VkDeviceSize{256};
        if (!ready || !ensure_readback_resources(readback_bytes)) {
            runtime_trace(
                "native_gpu.vulkan_target_failed flip=%llu size=%ux%u\n",
                static_cast<unsigned long long>(flip.flip_id),
                flip.width,
                flip.height);
            return false;
        }
        for (const auto& draw : frame_ir) {
            if (draw.render_target_address == 0) {
                continue;
            }
            if (ensure_render_surface(
                    draw.render_target_address,
                    draw.render_target_width,
                    draw.render_target_height,
                    draw_render_target_unified_format(draw)) == nullptr) {
                runtime_trace(
                    "native_gpu.vulkan_surface_failed "
                    "address=0x%016llX size=%ux%u\n",
                    static_cast<unsigned long long>(
                        draw.render_target_address),
                    draw.render_target_width,
                    draw.render_target_height);
                return false;
            }
        }
        auto* present_surface = ensure_render_surface(
            flip.display_address,
            flip.width,
            flip.height,
            frame_target_unified_format(
                flip.display_address,
                flip.width,
                flip.height));
        if (present_surface == nullptr) {
            runtime_trace(
                "native_gpu.vulkan_present_surface_failed flip=%llu "
                "address=0x%016llX size=%ux%u\n",
                static_cast<unsigned long long>(flip.flip_id),
                static_cast<unsigned long long>(flip.display_address),
                flip.width,
                flip.height);
            return false;
        }
        auto* frame_dump_surface = present_surface;
        if (dump_frame && frame_dump_address != 0) {
            // Match on address and extent only. Requiring the format to be
            // guessed correctly as well meant a request for, say, the
            // B10G11R11 buffer at 0x511D00000 silently missed, cleared the
            // address, and let the composition path write the display buffer
            // out under the requested filename - a different surface entirely,
            // reported as a success.
            // Address and extent do not identify a surface on their own.
            // This title aliases one guest region across render passes:
            // 0x53B9F0000 carries an R16G16B16A16_SFLOAT and an
            // R8G8B8A8_UNORM target at the same 2432x1368, plus an
            // R8G8_UNORM one at 1920x1080. Taking the first match meant
            // dumping whichever the map happened to order first and
            // reporting it as the composition source.
            //
            // PS5GPU_NATIVE_FRAME_DUMP_BPP picks between them; every
            // candidate is listed either way, so a wrong guess is
            // visible rather than silent.
            std::uint32_t wanted_bpp = 0;
            if (const auto* value =
                    std::getenv("PS5GPU_NATIVE_FRAME_DUMP_BPP");
                value != nullptr && value[0] != '\0') {
                char* end = nullptr;
                const auto parsed = std::strtoul(value, &end, 0);
                if (end != value && parsed <= 16) {
                    wanted_bpp = static_cast<std::uint32_t>(parsed);
                }
            }
            RenderSurface* found = nullptr;
            // A surface something drew into beats one only created for
            // the address: the flip's own format guess makes one of those,
            // and a dump of it is black whatever the frame showed.
            if (wanted_bpp == 0) {
                for (auto& [key, candidate] : render_surfaces) {
                    if (std::get<0>(key) == frame_dump_address &&
                        std::get<1>(key) == frame_dump_width &&
                        std::get<2>(key) == frame_dump_height &&
                        candidate.device_written) {
                        found = &candidate;
                        break;
                    }
                }
            }
            for (auto& [key, candidate] : render_surfaces) {
                if (std::get<0>(key) != frame_dump_address ||
                    std::get<1>(key) != frame_dump_width ||
                    std::get<2>(key) != frame_dump_height) {
                    continue;
                }
                runtime_trace(
                    "native_gpu.frame_dump_candidate "
                    "address=0x%016llX size=%ux%u bpp=%u "
                    "format=%d chosen=%d\n",
                    static_cast<unsigned long long>(
                        frame_dump_address),
                    frame_dump_width,
                    frame_dump_height,
                    candidate.bytes_per_pixel,
                    static_cast<int>(std::get<3>(key)),
                    (found == nullptr &&
                     (wanted_bpp == 0 ||
                      candidate.bytes_per_pixel == wanted_bpp))
                        ? 1
                        : 0);
                if (found == nullptr &&
                    (wanted_bpp == 0 ||
                     candidate.bytes_per_pixel == wanted_bpp)) {
                    found = &candidate;
                }
            }
            // A volume, for one slice of it: PS5GPU_NATIVE_FRAME_DUMP_SLICE.
            if (found == nullptr) {
                for (auto& [key, candidate] : volume_surfaces) {
                    if (std::get<0>(key) == frame_dump_address &&
                        std::get<1>(key) == frame_dump_width &&
                        std::get<2>(key) == frame_dump_height) {
                        found = &candidate;
                        break;
                    }
                }
            }
            if (found != nullptr) {
                frame_dump_surface = found;
            } else {
                runtime_trace(
                    "native_gpu.frame_dump_surface_missing "
                    "address=0x%016llX size=%ux%u\n",
                    static_cast<unsigned long long>(frame_dump_address),
                    frame_dump_width,
                    frame_dump_height);
                frame_dump_address = 0;
                frame_dump_width = flip.width;
                frame_dump_height = flip.height;
            }
        }
        // The copy below writes one whole pixel per pixel, whatever the format
        // costs, so the buffer has to be sized from the surface rather than
        // from the four bytes a display buffer happens to use.
        if (dump_frame) {
            readback_bytes = static_cast<VkDeviceSize>(
                static_cast<std::uint64_t>(frame_dump_width) *
                frame_dump_height *
                frame_dump_surface->bytes_per_pixel);
            if (!ensure_readback_resources(readback_bytes)) {
                runtime_trace(
                    "native_gpu.readback_resize_failed size=%ux%u "
                    "bytes_per_pixel=%u\n",
                    frame_dump_width,
                    frame_dump_height,
                    frame_dump_surface->bytes_per_pixel);
                return false;
            }
        }
        std::array<RenderSurface*, kAstroPostPasses.size()>
            astro_post_sources = {};
        std::array<bool, kAstroPostPasses.size()>
            astro_post_frame_resources_ready = {};
        for (std::size_t pass_index = 0;
             pass_index < kAstroPostPasses.size();
             ++pass_index) {
            const auto post_draw = std::find_if(
                frame_ir.begin(),
                frame_ir.end(),
                [pass_index](const GpuIrDraw& draw) {
                    return draw.astro_post_process &&
                        draw.astro_post_pass_index == pass_index;
                });
            if (post_draw == frame_ir.end()) {
                continue;
            }
            auto* source = ensure_render_surface(
                post_draw->sampled_address,
                post_draw->sampled_width,
                post_draw->sampled_height,
                frame_target_unified_format(
                    post_draw->sampled_address,
                    post_draw->sampled_width,
                    post_draw->sampled_height));
            if (source != nullptr) {
                astro_post_sources[pass_index] = source;
                astro_post_frame_resources_ready[pass_index] =
                    update_astro_post_descriptors(
                        *post_draw,
                        *source);
            }
        }
        std::array<RenderSurface*, 4> astro_state28_images = {};
        bool astro_state28_frame_resources_ready = false;
        const auto astro_state28_draw = std::find_if(
            frame_ir.begin(),
            frame_ir.end(),
            [](const GpuIrDraw& draw) {
                return draw.es_address == kAstroState28ExportAddress &&
                    draw.ps_address == kAstroState28PixelAddress &&
                    draw.render_target_address ==
                        kAstroState28TargetAddress;
            });
        if (astro_state28_pipeline != VK_NULL_HANDLE &&
            astro_state28_draw != frame_ir.end()) {
            for (std::size_t index = 0;
                 index < astro_state28_images.size();
                 ++index) {
                astro_state28_images[index] = ensure_render_surface(
                    kAstroState28ImageAddresses[index],
                    kAstroState28ImageWidths[index],
                    kAstroState28ImageHeights[index]);
            }
            astro_state28_frame_resources_ready =
                update_astro_state28_descriptors(
                    astro_state28_images);
        }
        RenderSurface* astro_state29_source = nullptr;
        bool astro_state29_frame_resources_ready = false;
        const auto astro_state29_draw = std::find_if(
            frame_ir.begin(),
            frame_ir.end(),
            [](const GpuIrDraw& draw) {
                return draw.astro_state29;
            });
        if (astro_state29_pipeline != VK_NULL_HANDLE &&
            astro_state29_draw != frame_ir.end()) {
            astro_state29_source = ensure_render_surface(
                astro_state29_draw->sampled_address,
                astro_state29_draw->sampled_width,
                astro_state29_draw->sampled_height,
                frame_target_unified_format(
                    astro_state29_draw->sampled_address,
                    astro_state29_draw->sampled_width,
                    astro_state29_draw->sampled_height));
            astro_state29_frame_resources_ready =
                astro_state29_source != nullptr &&
                update_astro_state29_descriptors(
                    *astro_state29_draw,
                    *astro_state29_source);
        }
        RenderSurface* real_source = nullptr;
        RenderSurface* real_texture1 = nullptr;
        RenderSurface* real_texture2 = nullptr;
        bool real_frame_resources_ready = false;
        const auto real_composition_draw = std::find_if(
            frame_ir.begin(),
            frame_ir.end(),
            [](const GpuIrDraw& draw) {
                return draw.ps_hash == kAstroCompositionPixelHash;
            });
        const auto has_real_composition_draw =
            real_composition_draw != frame_ir.end();
        if (real_composition_enabled &&
            real_composition_pipeline != VK_NULL_HANDLE &&
            has_real_composition_draw) {
            real_source = ensure_render_surface(
                kAstroCompositionSourceAddress,
                kAstroCompositionSourceWidth,
                kAstroCompositionSourceHeight,
                frame_target_unified_format(
                    kAstroCompositionSourceAddress,
                    kAstroCompositionSourceWidth,
                    kAstroCompositionSourceHeight));
            real_texture1 = ensure_render_surface(
                kAstroCompositionTexture1Address,
                1,
                1);
            real_texture2 = ensure_render_surface(
                kAstroCompositionTexture2Address,
                1,
                1);
            real_frame_resources_ready =
                real_source != nullptr &&
                real_texture1 != nullptr &&
                real_texture2 != nullptr &&
                update_real_composition_descriptors(
                    *real_composition_draw,
                    *real_source,
                    *real_texture1,
                    *real_texture2);
        }
        // real= in native_gpu.vulkan_frame runs 1 for the first four
        // frames of a run, 0 for the next six, 1 again after that, and the
        // condition that decides it is six terms wide. One line a frame
        // naming each of them costs nothing and says which one moved.
        runtime_trace(
            "native_gpu.real_composition_terms enabled=%u pipeline=%u "
            "draw=%u source=%u texture1=%u texture2=%u descriptors=%u "
            "initialized=%u layout=%d mode=%s vertex=%u\n",
            real_composition_enabled ? 1u : 0u,
            real_composition_pipeline != VK_NULL_HANDLE ? 1u : 0u,
            has_real_composition_draw ? 1u : 0u,
            real_source != nullptr ? 1u : 0u,
            real_texture1 != nullptr ? 1u : 0u,
            real_texture2 != nullptr ? 1u : 0u,
            real_frame_resources_ready ? 1u : 0u,
            real_source != nullptr && real_source->initialized ? 1u : 0u,
            real_source != nullptr
                ? static_cast<int>(real_source->layout)
                : -1,
            real_composition_mode_name(real_composition_mode),
            real_vertex_ready ? 1u : 0u);

        ++graphics_frame_counter;
        for (const auto& draw : frame_ir) {
            for (const auto id :
                 {draw.shader_state_id, resolve_pipeline_state(draw)}) {
                const auto used = guest_descriptor_pipelines.find(id);
                if (used != guest_descriptor_pipelines.end()) {
                    used->second.last_used_frame = graphics_frame_counter;
                }
            }
        }
        retire_idle_graphics_pipelines();
        for (const auto& draw : frame_ir) {
            ensure_live_guest_pipeline(draw.shader_state_id);
        }
        for (auto& [state, guest] : guest_descriptor_pipelines) {
            (void)state;
            guest.frame_resources_ready = false;
        }
        begin_frame_arena_pass();
        for (const auto& draw : frame_ir) {
            const auto guest =
                guest_descriptor_pipelines.find(
                    resolve_pipeline_state(draw));
            if (guest == guest_descriptor_pipelines.end()) {
                continue;
            }
            const auto descriptor_target =
                render_surfaces.find({
                    draw.render_target_address,
                    draw.render_target_width,
                    draw.render_target_height,
                    supported_surface_format(
                        draw_render_target_unified_format(draw)),
                });
            const RenderSurface* target =
                descriptor_target == render_surfaces.end()
                    ? nullptr
                    : &descriptor_target->second;
            // Resolve once per frame, and again whenever the target moves to a
            // surface this state also samples - that is the only case where the
            // answer changes, and re-reading every buffer for each draw would
            // cost far more than it buys.
            const auto samples_new_target =
                target != nullptr &&
                target != guest->second.frame_resources_target &&
                std::any_of(
                    guest->second.images.begin(),
                    guest->second.images.end(),
                    [target](const GuestImageResource& image) {
                        return image.surface == target;
                    });
            if (guest->second.frame_resources_ready &&
                !samples_new_target) {
                continue;
            }
            guest->second.frame_resources_target = target;
            (void)update_guest_descriptor_resources(
                resolve_pipeline_state(draw),
                guest->second,
                target,
                draw.shader_state_id);
        }
        end_frame_arena_pass();

        const auto finish_started = worker_phase_counter();
        auto result = finish_pending_gpu_work();
        g_flip_finish_ticks += worker_phase_counter() - finish_started;
        // The queue is idle here, which is what makes destroying a spent
        // state's buffers safe.
        retire_spent_compute_states();
        if (result == VK_SUCCESS) {
            result = reset_command_buffer(command_buffer, 0);
        }
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.vulkan_frame_reset_failed flip=%llu "
                "result=%d\n",
                static_cast<unsigned long long>(flip.flip_id),
                static_cast<int>(result));
            return false;
        }
        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        flip_phases.recording = worker_phase_counter();
        result = begin_command_buffer(
            command_buffer,
            &begin_info);
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.vulkan_frame_begin_failed flip=%llu "
                "result=%d\n",
                static_cast<unsigned long long>(flip.flip_id),
                static_cast<int>(result));
            return false;
        }
        order_after_earlier_submissions(command_buffer);

        VkClearValue clear_value = {};
        clear_value.color.float32[3] = 1.0f;
        RenderSurface* active_surface = nullptr;
        VkPipeline bound_pipeline = VK_NULL_HANDLE;
        std::uint32_t executed_draws = 0;
        std::uint32_t no_surface_draws = 0;
        std::uint32_t no_pipeline_draws = 0;
        std::uint32_t masked_draws = 0;
        std::uint32_t present_draws = 0;
        // The surface the frame's last draw into the display buffer used.
        // One guest address carries surfaces of several formats, and the
        // one the flip's format guess names need not be the one the
        // composition drew into: the window showed another frame's leftovers.
        RenderSurface* displayed_surface = nullptr;
        std::uint32_t guest_clear_draws = 0;
        std::uint32_t sampled_draws = 0;
        std::uint32_t astro_post_draws = 0;
        std::uint32_t real_draws = 0;
        std::uint32_t astro_state28_draws = 0;
        std::uint32_t astro_state29_draws = 0;
        std::uint32_t guest_descriptor_draws = 0;
        std::uint32_t guest_pipeline_draws = 0;
        std::uint32_t expected_pixel = 0;
        bool dumped_after_real_draw = false;
        bool dumped_after_selected_draw = false;

        const auto finish_surface_pass = [&]() {
            if (active_surface == nullptr) {
                return;
            }
            cmd_end_render_pass(command_buffer);
            active_surface->initialized = true;
            active_surface->device_written = true;
            transition_surface(
                command_buffer,
                *active_surface,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            active_surface = nullptr;
            bound_pipeline = VK_NULL_HANDLE;
        };
        // The value a fast clear left a target holding, in its own format.
        const auto apply_fast_clear = [&](RenderSurface& surface,
                                          std::uint32_t word0,
                                          std::uint32_t word1) {
            VkClearColorValue color = {};
            const auto half = [](std::uint32_t bits) {
                return half_to_float(static_cast<std::uint16_t>(bits));
            };
            switch (surface.format) {
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                color.float32[0] = half(word0 & 0xFFFFu);
                color.float32[1] = half(word0 >> 16);
                color.float32[2] = half(word1 & 0xFFFFu);
                color.float32[3] = half(word1 >> 16);
                break;
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
                for (int index = 0; index < 4; ++index) {
                    color.float32[index] =
                        static_cast<float>((word0 >> (index * 8)) & 0xFFu) /
                        255.0f;
                }
                break;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
                color.float32[0] =
                    static_cast<float>(word0 & 0x3FFu) / 1023.0f;
                color.float32[1] =
                    static_cast<float>((word0 >> 10) & 0x3FFu) / 1023.0f;
                color.float32[2] =
                    static_cast<float>((word0 >> 20) & 0x3FFu) / 1023.0f;
                color.float32[3] = static_cast<float>(word0 >> 30) / 3.0f;
                break;
            case VK_FORMAT_R32_SFLOAT:
                std::memcpy(&color.float32[0], &word0, sizeof(float));
                break;
            default:
                return;
            }
            finish_surface_pass();
            transition_surface(
                command_buffer,
                surface,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageSubresourceRange range = {};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;
            cmd_clear_color_image(
                command_buffer,
                surface.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &color,
                1,
                &range);
            surface.initialized = true;
            surface.device_written = true;
            transition_surface(
                command_buffer,
                surface,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            static std::atomic<std::uint32_t> shown{0};
            if (shown.fetch_add(1, std::memory_order_relaxed) < 32) {
                runtime_trace(
                    "native_gpu.fast_clear target=0x%016llX format=%s "
                    "clear=0x%08X_%08X\n",
                    static_cast<unsigned long long>(surface.guest_address),
                    render_surface_format_name(surface.format),
                    word1,
                    word0);
            }
        };
        const auto begin_surface_pass = [&](RenderSurface& surface) {
            const auto surface_render_pass =
                ensure_render_pass(surface.format, surface.initialized);
            if (surface_render_pass == VK_NULL_HANDLE ||
                surface.framebuffer == VK_NULL_HANDLE) {
                runtime_trace(
                    "native_gpu.surface_pass_unavailable "
                    "address=0x%016llX size=%ux%u format=%s "
                    "framebuffer=%u\n",
                    static_cast<unsigned long long>(
                        surface.guest_address),
                    surface.width,
                    surface.height,
                    render_surface_format_name(surface.format),
                    surface.framebuffer != VK_NULL_HANDLE ? 1u : 0u);
                return false;
            }
            transition_surface(
                command_buffer,
                surface,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            VkRenderPassBeginInfo render_begin = {};
            render_begin.sType =
                VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            render_begin.renderPass = surface_render_pass;
            render_begin.framebuffer = surface.framebuffer;
            render_begin.renderArea.offset = {0, 0};
            render_begin.renderArea.extent = {
                surface.width,
                surface.height,
            };
            render_begin.clearValueCount = 1;
            render_begin.pClearValues = &clear_value;
            cmd_begin_render_pass(
                command_buffer,
                &render_begin,
                VK_SUBPASS_CONTENTS_INLINE);
            active_surface = &surface;
            bound_pipeline = VK_NULL_HANDLE;
            return true;
        };
        const auto guest_texture_white =
            environment_flag_enabled(
                "PS5GPU_NATIVE_GUEST_TEXTURE_WHITE");
        std::uint32_t uploaded_guest_textures = 0;
        for (auto& [state, guest] : guest_descriptor_pipelines) {
            if (!guest.frame_resources_ready) {
                continue;
            }
            for (auto& image : guest.images) {
                if (!image.upload_ready ||
                    !image.upload_pending ||
                    image.upload_buffer == VK_NULL_HANDLE ||
                    image.surface == nullptr) {
                    continue;
                }
                transition_surface(
                    command_buffer,
                    *image.surface,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy copy = {};
                copy.bufferOffset = 0;
                copy.bufferRowLength = 0;
                copy.bufferImageHeight = 0;
                copy.imageSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                copy.imageSubresource.mipLevel = 0;
                copy.imageSubresource.baseArrayLayer = 0;
                copy.imageSubresource.layerCount = 1;
                copy.imageOffset = {0, 0, 0};
                copy.imageExtent = {
                    image.width,
                    image.height,
                    image.depth,
                };
                cmd_copy_buffer_to_image(
                    command_buffer,
                    image.upload_buffer,
                    image.surface->image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1,
                    &copy);
                transition_surface(
                    command_buffer,
                    *image.surface,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                image.surface->initialized = true;
                image.upload_pending = false;
                ++uploaded_guest_textures;
                runtime_trace(
                    "native_gpu.guest_image_upload_recorded "
                    "state=%u binding=%u address=0x%016llX "
                    "size=%ux%u hash=0x%016llX\n",
                    state,
                    image.binding,
                    static_cast<unsigned long long>(
                        image.guest_address),
                    image.width,
                    image.height,
                    static_cast<unsigned long long>(
                        image.upload_hash));
            }
        }
        if (uploaded_guest_textures != 0) {
            runtime_trace(
                "native_gpu.guest_image_uploads_recorded count=%u\n",
                uploaded_guest_textures);
        }
        std::uint32_t initialized_guest_textures = 0;
        for (auto& [state, guest] : guest_descriptor_pipelines) {
            (void)state;
            if (!guest.frame_resources_ready) {
                continue;
            }
            for (auto& image : guest.images) {
                if ((image.flags &
                        PS5GPU_RESOURCE_IMAGE_STORAGE) != 0 ||
                    image.surface == nullptr ||
                    image.surface->initialized) {
                    continue;
                }
                if (guest_texture_white) {
                    clear_value.color.float32[0] = 1.0f;
                    clear_value.color.float32[1] = 1.0f;
                    clear_value.color.float32[2] = 1.0f;
                }
                begin_surface_pass(*image.surface);
                finish_surface_pass();
                clear_value.color.float32[0] = 0.0f;
                clear_value.color.float32[1] = 0.0f;
                clear_value.color.float32[2] = 0.0f;
                ++initialized_guest_textures;
            }
        }
        if (initialized_guest_textures != 0) {
            runtime_trace(
                "native_gpu.guest_textures_initialized "
                "count=%u color=%s\n",
                initialized_guest_textures,
                guest_texture_white ? "white" : "black");
        }
        if (real_frame_resources_ready) {
            if (!real_texture1->initialized) {
                begin_surface_pass(*real_texture1);
                finish_surface_pass();
            }
            if (!real_texture2->initialized) {
                begin_surface_pass(*real_texture2);
                finish_surface_pass();
            }
        }

        // Where a large draw leaves this loop. A mesh in the scene has
        // thousands of indices and there are only a few of them a frame,
        // so each exit names them without flooding the trace.
        const auto trace_large_draw_fate =
            [](const GpuIrDraw& draw, const char* fate) {
                if (draw.vertex_count < 1024) {
                    return;
                }
                static std::atomic<std::uint32_t> shown{0};
                if (shown.fetch_add(1, std::memory_order_relaxed) < 48) {
                    runtime_trace(
                        "native_gpu.large_draw_fate fate=%s state=%u "
                        "vertices=%u indexed=%d target=0x%016llX\n",
                        fate,
                        draw.shader_state_id,
                        draw.vertex_count,
                        (draw.flags & PS5GPU_NATIVE_DRAW_INDEXED) != 0
                            ? 1 : 0,
                        static_cast<unsigned long long>(
                            draw.render_target_address));
                    runtime_trace_flush();
                }
            };
        for (const auto& draw : frame_ir) {
            // CB_TARGET_MASK says which channels the target accepts,
            // CB_SHADER_MASK which ones the pixel shader exports; with no
            // channel in common the draw writes no colour at all. On
            // hardware it would still write depth, and we have no depth
            // attachment, so the whole draw is dropped rather than run for
            // an effect it cannot have. KytyPS5 gates its pixel stage on
            // the same intersection.
            if ((draw.flags & PS5GPU_NATIVE_DRAW_MASKS_KNOWN) != 0 &&
                (draw.target_mask & draw.shader_mask) == 0 &&
                !mask_skip_disabled()) {
                ++masked_draws;
                trace_large_draw_fate(draw, "masked");
                continue;
            }
            const auto target =
                render_surfaces.find({
                    draw.render_target_address,
                    draw.render_target_width,
                    draw.render_target_height,
                    supported_surface_format(
                        draw_render_target_unified_format(draw)),
                });
            if (target == render_surfaces.end()) {
                ++no_surface_draws;
                trace_large_draw_fate(draw, "no_surface");
                continue;
            }
            auto& surface = target->second;
            if (draw.clear_first && cmd_clear_color_image != nullptr) {
                apply_fast_clear(surface, draw.clear_word0, draw.clear_word1);
            }
            RenderSurface* sampled_surface = nullptr;
            if (draw.sample_render_surface) {
                const auto sampled = render_surfaces.find({
                    draw.sampled_address,
                    draw.sampled_width,
                    draw.sampled_height,
                    supported_surface_format(
                        frame_target_unified_format(
                            draw.sampled_address,
                            draw.sampled_width,
                            draw.sampled_height)),
                });
                if (sampled != render_surfaces.end()) {
                    sampled_surface = &sampled->second;
                }
            }
            // A guest image can arrive in the wrong layout - compute leaves
            // its storage images in GENERAL, and a surface just rendered into
            // is still a colour attachment. That used to disqualify the whole
            // draw ("reason=layout"), which then painted the target with the
            // diagnostic shader. A layout is something to change, not a reason
            // to drop a draw, so move them now, while no pass is open.
            if (const auto guest_images =
                    guest_descriptor_pipelines.find(
                        resolve_pipeline_state(draw));
                guest_images != guest_descriptor_pipelines.end() &&
                guest_images->second.frame_resources_ready) {
                for (auto& image : guest_images->second.images) {
                    if (image.read_copy != nullptr &&
                        image.surface == &surface) {
                        // Snapshot the attachment as it stands, then let the
                        // pass reopen and draw into it.
                        finish_surface_pass();
                        record_surface_read_copy(
                            command_buffer,
                            surface,
                            *image.read_copy);
                        continue;
                    }
                    // A storage image is bound in GENERAL, which is not
                    // the layout a sampled one is moved to.
                    if (image.surface != nullptr &&
                        image.surface != &surface &&
                        (image.flags &
                            PS5GPU_RESOURCE_IMAGE_STORAGE) != 0) {
                        if (image.surface->layout != VK_IMAGE_LAYOUT_GENERAL) {
                            finish_surface_pass();
                            transition_surface(
                                command_buffer,
                                *image.surface,
                                VK_IMAGE_LAYOUT_GENERAL,
                                VK_ACCESS_SHADER_READ_BIT |
                                    VK_ACCESS_SHADER_WRITE_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                        }
                        continue;
                    }
                    if (image.surface == nullptr ||
                        image.surface == &surface ||
                        !image.surface->initialized ||
                        image.surface->layout ==
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                        continue;
                    }
                    finish_surface_pass();
                    transition_surface(
                        command_buffer,
                        *image.surface,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                }
            }
            if (active_surface != &surface) {
                finish_surface_pass();
                if (!begin_surface_pass(surface)) {
                    trace_large_draw_fate(draw, "no_pass");
                    continue;
                }
            }
            const auto can_sample =
                sampled_surface != nullptr &&
                sampled_surface != &surface &&
                sampled_surface->initialized &&
                sampled_surface->layout ==
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (draw.sample_render_surface && !can_sample) {
                // The lookup above demands an exact (address, extent, format)
                // match. A guest surface reached through several views - a
                // different render extent or format at the same address -
                // lands in a different cache entry, so report what does exist
                // for the address instead of silently sampling nothing.
                runtime_trace(
                    "native_gpu.sample_surface_unavailable state=%u "
                    "address=0x%016llX want=%ux%u found=%u "
                    "initialized=%u layout=%d\n",
                    draw.shader_state_id,
                    static_cast<unsigned long long>(
                        draw.sampled_address),
                    draw.sampled_width,
                    draw.sampled_height,
                    sampled_surface != nullptr ? 1u : 0u,
                    sampled_surface != nullptr &&
                            sampled_surface->initialized
                        ? 1u
                        : 0u,
                    sampled_surface != nullptr
                        ? static_cast<int>(sampled_surface->layout)
                        : -1);
                std::uint32_t reported = 0;
                for (const auto& [key, candidate] : render_surfaces) {
                    if (std::get<0>(key) != draw.sampled_address ||
                        reported >= 8) {
                        continue;
                    }
                    ++reported;
                    runtime_trace(
                        "native_gpu.sample_surface_candidate "
                        "address=0x%016llX size=%ux%u format=%d "
                        "initialized=%u device_written=%u layout=%d\n",
                        static_cast<unsigned long long>(
                            std::get<0>(key)),
                        std::get<1>(key),
                        std::get<2>(key),
                        static_cast<int>(std::get<3>(key)),
                        candidate.initialized ? 1u : 0u,
                        candidate.device_written ? 1u : 0u,
                        static_cast<int>(candidate.layout));
                }
            }
            const auto can_astro_post =
                draw.astro_post_process &&
                draw.astro_post_pass_index <
                    astro_post_sources.size() &&
                astro_post_frame_resources_ready[
                    draw.astro_post_pass_index] &&
                sampled_surface ==
                    astro_post_sources[draw.astro_post_pass_index] &&
                can_sample;
            const auto can_real_composition =
                real_frame_resources_ready &&
                draw.ps_hash == kAstroCompositionPixelHash &&
                real_source->initialized &&
                real_source->layout ==
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                (real_composition_mode ==
                     RealCompositionMode::FullscreenRealPs ||
                 real_composition_mode ==
                     RealCompositionMode::FullscreenCopySource ||
                 real_vertex_ready);
            const auto can_astro_state29 =
                astro_state29_frame_resources_ready &&
                draw.astro_state29 &&
                sampled_surface == astro_state29_source &&
                can_sample;
            const auto can_astro_state28 =
                astro_state28_frame_resources_ready &&
                draw.es_address == kAstroState28ExportAddress &&
                draw.ps_address == kAstroState28PixelAddress &&
                draw.render_target_address ==
                    kAstroState28TargetAddress &&
                std::all_of(
                    astro_state28_images.begin(),
                    astro_state28_images.end(),
                    [](const RenderSurface* image) {
                        return image != nullptr &&
                            image->initialized &&
                            image->layout ==
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    });
            const auto pipeline_state = resolve_pipeline_state(draw);
            const auto guest_descriptor =
                guest_descriptor_pipelines.find(pipeline_state);
            const auto force_sample_copy =
                static_cast<int>(draw.shader_state_id) ==
                    sample_copy_state &&
                can_sample;
            const auto can_use_guest_descriptor =
                guest_descriptor !=
                    guest_descriptor_pipelines.end() &&
                !force_sample_copy &&
                guest_descriptor_images_ready(
                    guest_descriptor->second,
                    surface,
                    draw.shader_state_id) &&
                (!uses_triangle_strip(draw) ||
                 guest_descriptor->second.strip_pipeline !=
                    VK_NULL_HANDLE);
            const auto guest_pipeline =
                resource_free_guest_pipelines.find(pipeline_state);
            const auto guest_strip_pipeline =
                resource_free_guest_strip_pipelines.find(pipeline_state);
            const auto use_guest_strip =
                uses_triangle_strip(draw);
            const auto can_use_guest_pipeline =
                guest_pipeline !=
                    resource_free_guest_pipelines.end() &&
                !force_sample_copy &&
                (!use_guest_strip ||
                 guest_strip_pipeline !=
                    resource_free_guest_strip_pipelines.end());
            // Every pipeline below is resolved against the format of the
            // surface actually being drawn into: a pipeline built for one
            // attachment format cannot be used inside a render pass carrying
            // another.
            const auto target_format = surface.format;
            // When no real pipeline fits the draw, leave the surface alone.
            // The fallback used to be the diagnostic shader, which fills the
            // target with solid green - in an HDR post chain that is the
            // brightest value there is, and it propagated: a state sampling
            // the surface it also renders into was rejected, painted green,
            // and every downsample level plus the final composite inherited
            // it. Painting nothing keeps whatever compute or an earlier pass
            // legitimately put there. Set PS5GPU_NATIVE_DIAGNOSTIC_FALLBACK to
            // get the old behaviour back when hunting for missing draws.
            static const auto diagnostic_fallback =
                environment_flag_enabled(
                    "PS5GPU_NATIVE_DIAGNOSTIC_FALLBACK");
            auto pipeline = diagnostic_fallback
                ? ensure_guest_pipeline(
                    diagnostic_pipeline_variants,
                    target_format)
                : VK_NULL_HANDLE;
            if (can_real_composition) {
                pipeline =
                    ensure_real_composition_pipeline(target_format);
            } else if (can_astro_state29) {
                pipeline = ensure_guest_pipeline(
                    astro_state29_pipeline_variants,
                    target_format);
            } else if (can_astro_post) {
                pipeline = ensure_guest_pipeline(
                    astro_post_pipelines[draw.astro_post_pass_index]
                        .pipeline_variants,
                    target_format);
            } else if (can_astro_state28) {
                pipeline = ensure_guest_pipeline(
                    astro_state28_pipeline_variants,
                    target_format);
            } else if (can_use_guest_descriptor) {
                pipeline = ensure_guest_pipeline(
                    use_guest_strip
                        ? guest_descriptor->second
                              .graphics_strip_pipeline
                        : guest_descriptor->second.graphics_pipeline,
                    target_format,
                    draw.blend_control);
            } else if (can_use_guest_pipeline) {
                pipeline = ensure_guest_pipeline(
                    use_guest_strip
                        ? guest_strip_pipeline->second
                        : guest_pipeline->second,
                    target_format,
                    draw.blend_control);
            } else if (can_sample) {
                pipeline = ensure_guest_pipeline(
                    sampled_pipeline_variants,
                    target_format);
            } else if (draw.solid_white_pixel_shader) {
                pipeline = ensure_guest_pipeline(
                    solid_white_pipeline_variants,
                    target_format);
            }
            if (pipeline == VK_NULL_HANDLE) {
                ++no_pipeline_draws;
                trace_large_draw_fate(draw, "no_pipeline");
                // Which draws these are and which of the conditions for a
                // guest pipeline they missed. Every mesh in the scene is an
                // indexed draw, and not one of them has reached the device.
                static std::atomic<std::uint32_t> shown{0};
                if (shown.fetch_add(1, std::memory_order_relaxed) < 64) {
                    runtime_trace(
                        "native_gpu.no_pipeline state=%u prim=0x%X "
                        "indexed=%d vertices=%u target=0x%016llX "
                        "descriptor=%d images_ready=%d strip=%d "
                        "resource_free=%d sample=%d\n",
                        draw.shader_state_id,
                        draw.primitive_type,
                        (draw.flags & PS5GPU_NATIVE_DRAW_INDEXED) != 0
                            ? 1 : 0,
                        draw.vertex_count,
                        static_cast<unsigned long long>(
                            draw.render_target_address),
                        guest_descriptor != guest_descriptor_pipelines.end()
                            ? 1 : 0,
                        guest_descriptor != guest_descriptor_pipelines.end() &&
                                guest_descriptor_images_ready(
                                    guest_descriptor->second,
                                    surface,
                                    draw.shader_state_id)
                            ? 1 : 0,
                        uses_triangle_strip(draw) ? 1 : 0,
                        guest_pipeline != resource_free_guest_pipelines.end()
                            ? 1 : 0,
                        draw.sample_render_surface ? 1 : 0);
                    runtime_trace_flush();
                }
                continue;
            }
            if (pipeline != bound_pipeline) {
                cmd_bind_pipeline(
                    command_buffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline);
                bound_pipeline = pipeline;
            }
            if (can_real_composition) {
                cmd_bind_descriptor_sets(
                    command_buffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    real_pipeline_layout,
                    0,
                    1,
                    &real_descriptor_set,
                    0,
                    nullptr);
                if (real_composition_mode !=
                        RealCompositionMode::FullscreenRealPs &&
                    real_composition_mode !=
                        RealCompositionMode::FullscreenCopySource &&
                    real_vertex_ready) {
                    const VkDeviceSize vertex_offset = 0;
                    cmd_bind_vertex_buffers(
                        command_buffer,
                        0,
                        1,
                        &real_vertex_buffer,
                        &vertex_offset);
                }
                ++real_draws;
                runtime_trace(
                    "native_gpu.real_composition_draw "
                    "mode=%s state=%u source=0x%016llX "
                    "target=0x%016llX size=%ux%u\n",
                    real_composition_mode_name(
                        real_composition_mode),
                    draw.shader_state_id,
                    static_cast<unsigned long long>(
                        real_source->guest_address),
                    static_cast<unsigned long long>(
                        surface.guest_address),
                    surface.width,
                    surface.height);
            } else if (can_astro_state29) {
                cmd_bind_descriptor_sets(
                    command_buffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    astro_state29_pipeline_layout,
                    0,
                    1,
                    &astro_state29_descriptor_set,
                    0,
                    nullptr);
                ++astro_state29_draws;
                runtime_trace(
                    "native_gpu.astro_state29_draw "
                    "state=%u vertices=%u "
                    "source=0x%016llX target=0x%016llX "
                    "size=%ux%u\n",
                    draw.shader_state_id,
                    draw.vertex_count,
                    static_cast<unsigned long long>(
                        sampled_surface->guest_address),
                    static_cast<unsigned long long>(
                        surface.guest_address),
                    surface.width,
                    surface.height);
            } else if (can_astro_state28) {
                cmd_bind_descriptor_sets(
                    command_buffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    astro_state28_pipeline_layout,
                    0,
                    1,
                    &astro_state28_descriptor_set,
                    0,
                    nullptr);
                ++astro_state28_draws;
                runtime_trace(
                    "native_gpu.astro_state28_draw "
                    "state=%u vertices=%u target=0x%016llX "
                    "size=%ux%u images=4\n",
                    draw.shader_state_id,
                    draw.vertex_count,
                    static_cast<unsigned long long>(
                        surface.guest_address),
                    surface.width,
                    surface.height);
            } else if (can_astro_post) {
                const auto& post =
                    astro_post_pipelines[
                        draw.astro_post_pass_index];
                cmd_bind_descriptor_sets(
                    command_buffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    post.pipeline_layout,
                    0,
                    1,
                    &post.descriptor_set,
                    0,
                    nullptr);
                ++astro_post_draws;
                runtime_trace(
                    "native_gpu.astro_post_draw state=%u pass=%u "
                    "source=0x%016llX target=0x%016llX "
                    "size=%ux%u images=%u\n",
                    draw.shader_state_id,
                    kAstroPostPasses[
                        draw.astro_post_pass_index].shader_state_file,
                    static_cast<unsigned long long>(
                        sampled_surface->guest_address),
                    static_cast<unsigned long long>(
                        surface.guest_address),
                    surface.width,
                    surface.height,
                    kAstroPostPasses[
                        draw.astro_post_pass_index].image_count);
            } else if (can_use_guest_descriptor) {
                cmd_bind_descriptor_sets(
                    command_buffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    guest_descriptor->second.pipeline_layout,
                    0,
                    1,
                    &guest_descriptor->second.descriptor_set,
                    0,
                    nullptr);
                ++guest_descriptor_draws;
                runtime_trace(
                    "native_gpu.guest_descriptor_draw "
                    "state=%u vertices=%u submitted=%u prim=0x%X "
                    "topology=%s target=0x%016llX "
                    "size=%ux%u buffers=%llu images=%llu\n",
                    draw.shader_state_id,
                    draw.vertex_count,
                    guest_vertex_count(draw),
                    draw.primitive_type,
                    use_guest_strip ? "strip" : "list",
                    static_cast<unsigned long long>(
                        surface.guest_address),
                    surface.width,
                    surface.height,
                    static_cast<unsigned long long>(
                        guest_descriptor->second.buffers.size()),
                    static_cast<unsigned long long>(
                        guest_descriptor->second.images.size()));
            } else if (can_use_guest_pipeline) {
                ++guest_pipeline_draws;
                runtime_trace(
                    "native_gpu.guest_pipeline_draw "
                    "state=%u vertices=%u submitted=%u prim=0x%X "
                    "topology=%s target=0x%016llX "
                    "size=%ux%u\n",
                    draw.shader_state_id,
                    draw.vertex_count,
                    guest_vertex_count(draw),
                    draw.primitive_type,
                    use_guest_strip ? "strip" : "list",
                    static_cast<unsigned long long>(
                        surface.guest_address),
                    surface.width,
                    surface.height);
            } else if (can_sample) {
                cmd_bind_descriptor_sets(
                    command_buffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    sampled_pipeline_layout,
                    0,
                    1,
                    &sampled_surface->sampled_descriptor_set,
                    0,
                    nullptr);
                ++sampled_draws;
                runtime_trace(
                    "native_gpu.vulkan_surface_sample "
                    "state=%u source=0x%016llX size=%ux%u "
                    "target=0x%016llX size=%ux%u\n",
                    draw.shader_state_id,
                    static_cast<unsigned long long>(
                        sampled_surface->guest_address),
                    sampled_surface->width,
                    sampled_surface->height,
                    static_cast<unsigned long long>(
                        surface.guest_address),
                    surface.width,
                    surface.height);
            }
            if (draw.render_target_address == flip.display_address) {
                displayed_surface = &surface;
                ++present_draws;
                if (draw.solid_white_pixel_shader) {
                    ++guest_clear_draws;
                    expected_pixel = 0xFFFFFFFFu;
                } else {
                    expected_pixel = 0xFF00FF00u;
                }
            }
            const auto fullscreen_override =
                (can_real_composition &&
                 (real_composition_mode ==
                      RealCompositionMode::FullscreenRealPs ||
                  real_composition_mode ==
                      RealCompositionMode::FullscreenCopySource)) ||
                (can_astro_state29 &&
                 (astro_state29_mode ==
                      RealCompositionMode::FullscreenRealPs ||
                  astro_state29_mode ==
                      RealCompositionMode::FullscreenCopySource));
            const auto force_fullscreen_viewport =
                fullscreen_override ||
                (can_astro_state28 &&
                 astro_state28_copy_image >= 0) ||
                (can_astro_post &&
                 static_cast<int>(
                     kAstroPostPasses[
                         draw.astro_post_pass_index]
                         .shader_state_file) ==
                     astro_post_copy_state) ||
                force_sample_copy;
            VkViewport viewport = {};
            if (force_fullscreen_viewport) {
                viewport = {
                    0.0f,
                    0.0f,
                    static_cast<float>(surface.width),
                    static_cast<float>(surface.height),
                    0.0f,
                    1.0f,
                };
            } else {
                viewport.x = draw.viewport[0];
                viewport.y = draw.viewport[1];
                viewport.width = draw.viewport[2];
                viewport.height = draw.viewport[3];
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;
                if (viewport.width == 0.0f ||
                    viewport.height == 0.0f) {
                    viewport = {
                        0.0f,
                        0.0f,
                        static_cast<float>(surface.width),
                        static_cast<float>(surface.height),
                        0.0f,
                        1.0f,
                    };
                }
            }
            cmd_set_viewport(command_buffer, 0, 1, &viewport);

            VkRect2D scissor = {};
            if (force_fullscreen_viewport) {
                scissor.offset = {0, 0};
                scissor.extent = {surface.width, surface.height};
            } else {
                const auto left = std::clamp(
                    draw.scissor[0],
                    0,
                    static_cast<int>(surface.width));
                const auto top = std::clamp(
                    draw.scissor[1],
                    0,
                    static_cast<int>(surface.height));
                const auto right = std::clamp(
                    left + static_cast<int>(
                        draw.scissor_extent[0]),
                    left,
                    static_cast<int>(surface.width));
                const auto bottom = std::clamp(
                    top + static_cast<int>(
                        draw.scissor_extent[1]),
                    top,
                    static_cast<int>(surface.height));
                scissor.offset = {left, top};
                scissor.extent = {
                    static_cast<std::uint32_t>(right - left),
                    static_cast<std::uint32_t>(bottom - top),
                };
                if (scissor.extent.width == 0 ||
                    scissor.extent.height == 0) {
                    scissor.offset = {0, 0};
                    scissor.extent = {
                        surface.width,
                        surface.height,
                    };
                }
            }
            cmd_set_scissor(command_buffer, 0, 1, &scissor);
            const auto native_vertex_count =
                can_use_guest_descriptor ||
                    can_use_guest_pipeline
                ? guest_vertex_count(draw)
                : can_astro_post ||
                    can_astro_state28 ||
                    can_astro_state29
                ? std::max(draw.vertex_count, 1u)
                : 3u;
            // Only the recompiled guest shaders index a vertex buffer by
            // gl_VertexIndex; the substitute pipelines generate their own
            // geometry and must still start at zero.
            const auto native_first_vertex =
                can_use_guest_descriptor || can_use_guest_pipeline
                    ? draw.first_vertex
                    : 0u;
            // Only a draw on a guest pipeline has indices worth binding.
            // The substitute pipelines generate their own geometry and have
            // no index buffer behind them at all.
            VkBuffer index_buffer = VK_NULL_HANDLE;
            if (draw.vertex_count >= 1024) {
                static std::atomic<std::uint64_t> seen{0};
                if (seen.fetch_add(1, std::memory_order_relaxed) < 8) {
                    runtime_trace(
                        "native_gpu.index_decision vertices=%u idx=0x%016llX "
                        "descriptor=%d pipeline=%d bind=%d drawidx=%d\n",
                        draw.vertex_count,
                        static_cast<unsigned long long>(draw.index_address),
                        can_use_guest_descriptor ? 1 : 0,
                        can_use_guest_pipeline ? 1 : 0,
                        cmd_bind_index_buffer != nullptr ? 1 : 0,
                        cmd_draw_indexed != nullptr ? 1 : 0);
                    runtime_trace_flush();
                }
            }
            if ((can_use_guest_descriptor || can_use_guest_pipeline) &&
                draw.index_address != 0 &&
                cmd_bind_index_buffer != nullptr &&
                cmd_draw_indexed != nullptr) {
                index_buffer = ensure_guest_index_buffer(
                    draw.shader_state_id,
                    draw.index_address,
                    draw.index_bytes,
                    native_vertex_count);
            }
            if (index_buffer != VK_NULL_HANDLE) {
                cmd_bind_index_buffer(
                    command_buffer,
                    index_buffer,
                    0,
                    draw.index_bytes == 4
                        ? VK_INDEX_TYPE_UINT32
                        : VK_INDEX_TYPE_UINT16);
                // The indices are already the vertices this draw wants, so
                // the offset the non-indexed path applies would apply it
                // twice.
                checkpoint(command_buffer, 1, draw.shader_state_id,
                           static_cast<std::uint32_t>(executed_draws));
                cmd_draw_indexed(
                    command_buffer,
                    native_vertex_count,
                    1,
                    0,
                    0,
                    0);
                g_indexed_draws.fetch_add(1, std::memory_order_relaxed);
            } else {
                checkpoint(command_buffer, 1, draw.shader_state_id,
                           static_cast<std::uint32_t>(executed_draws));
                cmd_draw(
                    command_buffer,
                    native_vertex_count,
                    1,
                    native_first_vertex,
                    0);
            }
            ++executed_draws;
            trace_large_draw_fate(draw, "executed");
            // What actually reached the device, by primitive. Half the
            // draws this title submits are rect lists, which are drawn
            // here as a strip with a fourth vertex rather than as the
            // rectangle they are - and the scene targets hold one value
            // each. Whether the two are connected needs the count of what
            // got this far, not what was submitted.
            {
                static std::atomic<std::uint32_t> shown{0};
                if (shown.fetch_add(1, std::memory_order_relaxed) < 24) {
                    runtime_trace(
                        "native_gpu.draw_executed prim=0x%X vertices=%u "
                        "first=%u indexed=%d target=0x%016llX\n",
                        draw.primitive_type,
                        native_vertex_count,
                        native_first_vertex,
                        (draw.flags & PS5GPU_NATIVE_DRAW_INDEXED) != 0
                            ? 1
                            : 0,
                        static_cast<unsigned long long>(
                            draw.render_target_address));
                    runtime_trace_flush();
                }
            }
            auto reads_dump_image = false;
            if (frame_dump_after_image != 0 && !dumped_after_selected_draw) {
                if (const auto found = guest_descriptor_pipelines.find(
                        resolve_pipeline_state(draw));
                    found != guest_descriptor_pipelines.end()) {
                    // Within the chosen frame, the first draw into the
                    // dumped target that reads any image: the followed
                    // image is double buffered, and by the time the draw
                    // runs its resources may name the other half.
                    // PS5GPU_NATIVE_FRAME_DUMP_AFTER_IMAGE_ALT names the
                    // other half, and with it the match is exact.
                    static const auto alternate = [] {
                        const auto* value = std::getenv(
                            "PS5GPU_NATIVE_FRAME_DUMP_AFTER_IMAGE_ALT");
                        return value == nullptr
                            ? std::uint64_t{0}
                            : std::strtoull(value, nullptr, 0);
                    }();
                    if (alternate != 0) {
                        for (const auto& image : found->second.images) {
                            if (image.guest_address == frame_dump_after_image ||
                                image.guest_address == alternate) {
                                reads_dump_image = true;
                            }
                        }
                    } else {
                        reads_dump_image =
                            !found->second.images.empty() &&
                            (frame_dump_address == 0 ||
                             draw.render_target_address == frame_dump_address);
                    }
                }
                if (reads_dump_image) {
                    runtime_trace(
                        "native_gpu.frame_dump_after_image state=%u "
                        "target=0x%016llX size=%ux%u\n",
                        draw.shader_state_id,
                        static_cast<unsigned long long>(
                            draw.render_target_address),
                        surface.width,
                        surface.height);
                }
            }
            const auto dump_after_this_draw =
                reads_dump_image ||
                (frame_dump_state != 0 &&
                 draw.shader_state_id == frame_dump_state &&
                 !dumped_after_selected_draw) ||
                (frame_dump_vertices() != 0 &&
                 draw.vertex_count == frame_dump_vertices() &&
                 !dumped_after_selected_draw) ||
                (frame_dump_draw != 0 &&
                 executed_draws == frame_dump_draw);
            if (dump_frame &&
                dump_after_this_draw &&
                surface.width == frame_dump_width &&
                surface.height == frame_dump_height) {
                finish_surface_pass();
                RenderSurface* dump_source = &surface;
                if (frame_dump_read_copy) {
                    if (const auto dumped =
                            guest_descriptor_pipelines.find(
                                resolve_pipeline_state(draw));
                        dumped != guest_descriptor_pipelines.end()) {
                        for (auto& image : dumped->second.images) {
                            if (image.read_copy != nullptr) {
                                dump_source = image.read_copy;
                                break;
                            }
                        }
                    }
                }
                transition_surface(
                    command_buffer,
                    *dump_source,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy selected_copy = {};
                selected_copy.bufferOffset = 0;
                selected_copy.imageSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                selected_copy.imageSubresource.mipLevel = 0;
                selected_copy.imageSubresource.baseArrayLayer = 0;
                selected_copy.imageSubresource.layerCount = 1;
                selected_copy.imageExtent = {
                    surface.width,
                    surface.height,
                    1,
                };
                cmd_copy_image_to_buffer(
                    command_buffer,
                    dump_source->image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    readback_buffer,
                    1,
                    &selected_copy);
                transition_surface(
                    command_buffer,
                    *dump_source,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                dumped_after_selected_draw = true;
            }
            if (can_real_composition &&
                dump_frame &&
                frame_dump_address == 0 &&
                !dumped_after_real_draw) {
                finish_surface_pass();
                transition_surface(
                    command_buffer,
                    surface,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy real_copy = {};
                real_copy.bufferOffset = 0;
                real_copy.imageSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                real_copy.imageSubresource.mipLevel = 0;
                real_copy.imageSubresource.baseArrayLayer = 0;
                real_copy.imageSubresource.layerCount = 1;
                real_copy.imageExtent = {
                    surface.width,
                    surface.height,
                    1,
                };
                cmd_copy_image_to_buffer(
                    command_buffer,
                    surface.image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    readback_buffer,
                    1,
                    &real_copy);
                transition_surface(
                    command_buffer,
                    surface,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                dumped_after_real_draw = true;
            }
        }
        finish_surface_pass();

        if (present_draws == 0 && begin_surface_pass(*present_surface)) {
            VkViewport viewport = {
                0.0f,
                0.0f,
                static_cast<float>(flip.width),
                static_cast<float>(flip.height),
                0.0f,
                1.0f,
            };
            VkRect2D scissor = {{0, 0}, {flip.width, flip.height}};
            const auto present_pipeline = ensure_guest_pipeline(
                diagnostic_pipeline_variants,
                present_surface->format);
            if (present_pipeline != VK_NULL_HANDLE) {
                cmd_set_viewport(command_buffer, 0, 1, &viewport);
                cmd_set_scissor(command_buffer, 0, 1, &scissor);
                cmd_bind_pipeline(
                    command_buffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    present_pipeline);
                cmd_draw(command_buffer, 3, 1, 0, 0);
                ++executed_draws;
                present_draws = 1;
                expected_pixel = 0xFF00FF00u;
            }
            finish_surface_pass();
        }

        if (!dumped_after_real_draw &&
            !dumped_after_selected_draw) {
            transition_surface(
                command_buffer,
                *frame_dump_surface,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy = {};
            copy.bufferOffset = 0;
            copy.bufferRowLength = 0;
            copy.bufferImageHeight = 0;
            copy.imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.mipLevel = 0;
            copy.imageSubresource.baseArrayLayer = 0;
            copy.imageSubresource.layerCount = 1;
            static const auto dump_slice = [] {
                const auto* value =
                    std::getenv("PS5GPU_NATIVE_FRAME_DUMP_SLICE");
                return value == nullptr
                    ? 0
                    : static_cast<std::int32_t>(
                          std::strtol(value, nullptr, 0));
            }();
            copy.imageOffset = {
                0, 0,
                frame_dump_surface->depth > 1
                    ? std::min<std::int32_t>(
                          dump_slice,
                          static_cast<std::int32_t>(
                              frame_dump_surface->depth) - 1)
                    : 0};
            copy.imageExtent = dump_frame
                ? VkExtent3D{
                    frame_dump_width,
                    frame_dump_height,
                    1}
                : VkExtent3D{1, 1, 1};
            cmd_copy_image_to_buffer(
                command_buffer,
                frame_dump_surface->image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                readback_buffer,
                1,
                &copy);

            transition_surface(
                command_buffer,
                *frame_dump_surface,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        // PS5GPU_NATIVE_PROBE_SURFACES: up to eight surface addresses whose
        // centre pixel is read back at the end of every frame and traced -
        // a frame's intermediate results, without a dump's cost or its
        // effect on timing.
        static const std::vector<std::uint64_t> probe_addresses = [] {
            std::vector<std::uint64_t> addresses;
            const auto* value = std::getenv("PS5GPU_NATIVE_PROBE_SURFACES");
            while (value != nullptr && *value != '\0' &&
                   addresses.size() < 8) {
                char* end = nullptr;
                const auto parsed = std::strtoull(value, &end, 0);
                if (end == value) {
                    break;
                }
                addresses.push_back(parsed);
                value = *end == ',' ? end + 1 : end;
            }
            return addresses;
        }();
        std::vector<std::pair<std::uint64_t, std::uint32_t>> probed;
        if (!dump_frame) {
            for (std::size_t index = 0; index < probe_addresses.size();
                 ++index) {
                RenderSurface* found = nullptr;
                for (auto& [key, candidate] : render_surfaces) {
                    // A 1x1 image counts - an exposure is one - and so does
                    // one only ever uploaded from guest memory.
                    if (std::get<0>(key) == probe_addresses[index] &&
                        candidate.width >= 1 &&
                        (candidate.device_written || candidate.initialized)) {
                        found = &candidate;
                        break;
                    }
                }
                if (found == nullptr) {
                    continue;
                }
                transition_surface(
                    command_buffer,
                    *found,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy probe = {};
                probe.bufferOffset = 64 + index * 16;
                probe.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                probe.imageSubresource.layerCount = 1;
                probe.imageOffset = {
                    static_cast<std::int32_t>(found->width / 2),
                    static_cast<std::int32_t>(found->height / 2),
                    0};
                probe.imageExtent = {1, 1, 1};
                cmd_copy_image_to_buffer(
                    command_buffer,
                    found->image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    readback_buffer,
                    1,
                    &probe);
                transition_surface(
                    command_buffer,
                    *found,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                probed.push_back(
                    {probe_addresses[index], found->bytes_per_pixel});
            }
        }
        VkBufferMemoryBarrier readback_barrier = {};
        readback_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        readback_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        readback_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        readback_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readback_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readback_barrier.buffer = readback_buffer;
        readback_barrier.offset = 0;
        readback_barrier.size = readback_bytes;
        cmd_pipeline_barrier(
            command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            0,
            0,
            nullptr,
            1,
            &readback_barrier,
            0,
            nullptr);

        // The frame into the window: the buffer this flip names, scaled.
        auto* window_source = displayed_surface != nullptr
            ? displayed_surface
            : present_surface;
        if (window_presenter.started() && window_source != nullptr) {
            transition_surface(
                command_buffer,
                *window_source,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
            window_presenter.record(
                command_buffer,
                window_source->image,
                window_source->width,
                window_source->height);
            transition_surface(
                command_buffer,
                *window_source,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        result = end_command_buffer(command_buffer);
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.vulkan_frame_end_failed flip=%llu result=%d\n",
                static_cast<unsigned long long>(flip.flip_id),
                static_cast<int>(result));
            return false;
        }
        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
        // With a window frame in it, the submission waits for the image
        // and tells the present when the frame is done.
        const VkSemaphore window_wait = window_presenter.image_available();
        const VkSemaphore window_signal = window_presenter.frame_finished();
        const VkPipelineStageFlags window_stage =
            VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (window_presenter.pending()) {
            submit_info.waitSemaphoreCount = 1;
            submit_info.pWaitSemaphores = &window_wait;
            submit_info.pWaitDstStageMask = &window_stage;
            submit_info.signalSemaphoreCount = 1;
            submit_info.pSignalSemaphores = &window_signal;
        }
        flip_phases.submitting = worker_phase_counter();
        result = queue_submit(queue, 1, &submit_info, VK_NULL_HANDLE);
        if (result == VK_SUCCESS) {
            result = wait_queue_idle(
                gpu_wait_us_frame, gpu_wait_n_frame);
        }
        if (result == VK_SUCCESS) {
            window_presenter.present();
            if (window_presenter.started() &&
                (flip.flip_id < 4 || flip.flip_id % 10 == 0)) {
                const auto extent = window_presenter.extent();
                runtime_trace(
                    "native_gpu.native_window_frame tick=%llu flip=%llu hwnd=%p "
                    "recorded=%llu presented=%llu result=%d extent=%ux%u "
                    "source=0x%016llX format=%s from_draw=%d\n",
                    static_cast<unsigned long long>(GetTickCount64()),
                    static_cast<unsigned long long>(flip.flip_id),
                    static_cast<void*>(window_presenter.window()),
                    static_cast<unsigned long long>(
                        window_presenter.recorded()),
                    static_cast<unsigned long long>(
                        window_presenter.presented()),
                    window_presenter.last_result(),
                    extent.width,
                    extent.height,
                    static_cast<unsigned long long>(
                        present_surface != nullptr ? flip.display_address : 0),
                    window_source != nullptr
                        ? render_surface_format_name(window_source->format)
                        : "none",
                    displayed_surface != nullptr ? 1 : 0);
            }
        }
        if (result != VK_SUCCESS) {
            runtime_trace(
                "native_gpu.vulkan_frame_failed flip=%llu result=%d\n",
                static_cast<unsigned long long>(flip.flip_id),
                static_cast<int>(result));
            if (result == VK_ERROR_DEVICE_LOST) {
                report_checkpoints("frame");
            }
            return false;
        }
        std::uint32_t pixel = 0;
        void* mapped = nullptr;
        if (map_memory(
                device,
                readback_memory,
                0,
                readback_bytes,
                0,
                &mapped) == VK_SUCCESS &&
            mapped != nullptr) {
            std::memcpy(&pixel, mapped, sizeof(pixel));
            if (!probed.empty()) {
                std::string text;
                char item[160] = {};
                for (std::size_t index = 0; index < probed.size(); ++index) {
                    const auto* slot =
                        static_cast<const std::uint8_t*>(mapped) + 64 +
                        index * 16;
                    if (probed[index].second == 16) {
                        // Four floats, as floats: an exposure texel.
                        float texel[4] = {};
                        std::memcpy(texel, slot, sizeof(texel));
                        std::snprintf(
                            item, sizeof(item), " %llX=(%g,%g,%g,%g)",
                            static_cast<unsigned long long>(
                                probed[index].first),
                            texel[0], texel[1], texel[2], texel[3]);
                        text += item;
                        continue;
                    }
                    std::uint64_t value = 0;
                    std::memcpy(
                        &value, slot, probed[index].second >= 8 ? 8 : 4);
                    std::snprintf(
                        item, sizeof(item), " %llX=%016llX",
                        static_cast<unsigned long long>(probed[index].first),
                        static_cast<unsigned long long>(value));
                    text += item;
                }
                runtime_trace(
                    "native_gpu.surface_probe flip=%llu%s\n",
                    static_cast<unsigned long long>(flip.flip_id),
                    text.c_str());
            }
            // Asked for the target after one draw or one state's draw, a
            // frame without it is not the answer: wait for one that has
            // it rather than write the end of this frame under its name.
            const auto write_dump = dump_frame &&
                !((frame_dump_state != 0 || frame_dump_draw != 0 ||
                   frame_dump_vertices() != 0) &&
                  !dumped_after_selected_draw);
            if (write_dump) {
                const auto pixel_count =
                    static_cast<std::size_t>(frame_dump_width) *
                    frame_dump_height;
                const auto dump_bytes_per_pixel = static_cast<std::size_t>(
                    frame_dump_surface->bytes_per_pixel);
                // Alpha sits in the trailing quarter of a four-channel pixel;
                // for anything else "not black" can only mean "not zero".
                const auto colour_bytes =
                    dump_bytes_per_pixel == 4 || dump_bytes_per_pixel == 8 ||
                        dump_bytes_per_pixel == 16
                    ? dump_bytes_per_pixel - dump_bytes_per_pixel / 4
                    : dump_bytes_per_pixel;
                std::uint64_t nonzero_pixels = 0;
                std::uint64_t nonblack_pixels = 0;
                auto hash = 1469598103934665603ULL;
                const auto* bytes =
                    static_cast<const std::uint8_t*>(mapped);
                for (std::size_t index = 0;
                     index < static_cast<std::size_t>(readback_bytes);
                     ++index) {
                    hash ^= bytes[index];
                    hash *= 1099511628211ULL;
                }
                for (std::size_t index = 0;
                     index < pixel_count;
                     ++index) {
                    const auto* pixel_bytes =
                        bytes + index * dump_bytes_per_pixel;
                    bool any_set = false;
                    bool colour_set = false;
                    for (std::size_t byte = 0;
                         byte < dump_bytes_per_pixel;
                         ++byte) {
                        if (pixel_bytes[byte] == 0) {
                            continue;
                        }
                        any_set = true;
                        if (byte < colour_bytes) {
                            colour_set = true;
                            break;
                        }
                    }
                    nonzero_pixels += any_set ? 1 : 0;
                    nonblack_pixels += colour_set ? 1 : 0;
                }
                static const auto series = [] {
                    const auto* value =
                        std::getenv("PS5GPU_NATIVE_FRAME_DUMP_SERIES");
                    return value == nullptr
                        ? 0u
                        : static_cast<std::uint32_t>(
                              std::strtoul(value, nullptr, 0));
                }();
                static const auto every = [] {
                    const auto* value =
                        std::getenv("PS5GPU_NATIVE_FRAME_DUMP_EVERY");
                    const auto parsed = value == nullptr
                        ? 1ull
                        : std::strtoull(value, nullptr, 0);
                    return parsed == 0 ? 1ull : parsed;
                }();
                std::string series_path = frame_dump_path;
                if (series != 0) {
                    series_path += "." + std::to_string(flip.flip_id);
                }
                const std::filesystem::path output_path(series_path);
                std::error_code directory_error;
                if (!output_path.parent_path().empty()) {
                    std::filesystem::create_directories(
                        output_path.parent_path(),
                        directory_error);
                }
                std::ofstream output(output_path, std::ios::binary);
                output.write(
                    reinterpret_cast<const char*>(mapped),
                    static_cast<std::streamsize>(readback_bytes));
                frame_dump_written = static_cast<bool>(output);
                if (series != 0 && ++frame_dump_series_done < series) {
                    frame_dump_written = false;
                    frame_dump_next_flip = flip.flip_id + every;
                }
                runtime_trace(
                    "native_gpu.frame_dump path=%s wrote=%u "
                    "stage=%s address=0x%016llX size=%ux%u "
                    "format=%s bytes_per_pixel=%llu "
                    "bytes=%llu hash=0x%016llX "
                    "nonzero=%llu nonblack=%llu\n",
                    output_path.string().c_str(),
                    frame_dump_written ? 1u : 0u,
                    dumped_after_real_draw
                        ? "real-draw"
                        : dumped_after_selected_draw
                        ? "selected-draw"
                        : frame_dump_address != 0
                        ? "surface"
                        : "final",
                    static_cast<unsigned long long>(
                        frame_dump_surface->guest_address),
                    frame_dump_width,
                    frame_dump_height,
                    render_surface_format_name(
                        frame_dump_surface->format),
                    static_cast<unsigned long long>(
                        dump_bytes_per_pixel),
                    static_cast<unsigned long long>(readback_bytes),
                    static_cast<unsigned long long>(hash),
                    static_cast<unsigned long long>(nonzero_pixels),
                    static_cast<unsigned long long>(nonblack_pixels));
            }
            unmap_memory(device, readback_memory);
        }
        ++submitted_frames;
        runtime_trace(
            "native_gpu.gpu_wait flip=%llu compute=%llums/%u "
            "readback=%llums/%u frame=%llums/%u\n",
            static_cast<unsigned long long>(flip.flip_id),
            static_cast<unsigned long long>(gpu_wait_us_compute / 1000),
            gpu_wait_n_compute,
            static_cast<unsigned long long>(gpu_wait_us_readback / 1000),
            gpu_wait_n_readback,
            static_cast<unsigned long long>(gpu_wait_us_frame / 1000),
            gpu_wait_n_frame);
        gpu_wait_us_compute = 0;
        gpu_wait_us_readback = 0;
        gpu_wait_us_frame = 0;
        gpu_wait_n_compute = 0;
        gpu_wait_n_readback = 0;
        gpu_wait_n_frame = 0;
        {
            // How long after the title handed the frame over it went out:
            // what the picture trails the title's clock - and the sound -
            // by.
            LARGE_INTEGER now = {};
            LARGE_INTEGER frequency = {};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            const auto submitted =
                g_flip_submitted_at[flip.flip_id % kFlipSubmittedSlots].load(
                    std::memory_order_relaxed);
            if (submitted != 0 && frequency.QuadPart != 0) {
                runtime_trace(
                    "native_gpu.flip_latency flip=%llu ms=%.1f\n",
                    static_cast<unsigned long long>(flip.flip_id),
                    static_cast<double>(now.QuadPart - submitted) * 1000.0 /
                        static_cast<double>(frequency.QuadPart));
            }
        }
        runtime_trace(
            "native_gpu.vulkan_frame flip=%llu draws=%llu total=%llu "
            "target=0x%016llX "
            "size=%ux%u surfaces=%llu executed=%u masked=%u present=%u "
            "guest_clear=%u sampled=%u real=%u state28=%u state29=%u "
            "post=%u guest_descriptor=%u guest_pipeline=%u "
            // Both zero over a run: the draw loop lets every draw it is
            // given through to a pipeline. Kept because "the draws are
            // being dropped" is the first guess every time the picture is
            // wrong, and it costs a run to answer without them.
            "no_surface=%u no_pipeline=%u "
            "pixel=0x%08X expected=0x%08X\n",
            static_cast<unsigned long long>(flip.flip_id),
            static_cast<unsigned long long>(frame_ir.size()),
            static_cast<unsigned long long>(ir_draws),
            static_cast<unsigned long long>(flip.display_address),
            flip.width,
            flip.height,
            static_cast<unsigned long long>(render_surfaces.size()),
            executed_draws,
            masked_draws,
            present_draws,
            guest_clear_draws,
            sampled_draws,
            real_draws,
            astro_state28_draws,
            astro_state29_draws,
            astro_post_draws,
            guest_descriptor_draws,
            guest_pipeline_draws,
            no_surface_draws,
            no_pipeline_draws,
            pixel,
            expected_pixel);
        frame_ir.clear();
        return
            real_draws != 0 ||
            (dump_frame && frame_dump_address != 0) ||
            pixel == expected_pixel;
    }

    void destroy() {
        ready = false;
        if (device != VK_NULL_HANDLE) {
            if (queue != VK_NULL_HANDLE && queue_wait_idle != nullptr) {
                queue_wait_idle(queue);
            }
            (void)save_pipeline_cache();
            for (auto& [state, guest] :
                 guest_descriptor_pipelines) {
                (void)state;
                destroy_guest_descriptor_pipeline(guest);
            }
            guest_descriptor_pipelines.clear();
            for (auto& [state, guest] :
                 guest_compute_pipelines) {
                (void)state;
                destroy_guest_descriptor_pipeline(guest);
            }
            guest_compute_pipelines.clear();
            destroy_compute_buffer_pool();
            destroy_guest_imports();
            destroy_frame_arena();
            destroy_shared_graphics_pipelines();
        destroy_guest_index_buffers();
            // The states borrowed these and left them alone; they are owned
            // here and go last, once nothing refers to them.
            for (auto& [key, program] : shared_compute_programs) {
                (void)key;
                if (program.pipeline != VK_NULL_HANDLE &&
                    destroy_pipeline != nullptr) {
                    destroy_pipeline(device, program.pipeline, nullptr);
                }
                if (program.pipeline_layout != VK_NULL_HANDLE &&
                    destroy_pipeline_layout != nullptr) {
                    destroy_pipeline_layout(
                        device,
                        program.pipeline_layout,
                        nullptr);
                }
                if (program.descriptor_set_layout != VK_NULL_HANDLE &&
                    destroy_descriptor_set_layout != nullptr) {
                    destroy_descriptor_set_layout(
                        device,
                        program.descriptor_set_layout,
                        nullptr);
                }
            }
            shared_compute_programs.clear();
            for (auto& [key, layouts] : shared_graphics_layouts) {
                (void)key;
                if (layouts.second != VK_NULL_HANDLE &&
                    destroy_pipeline_layout != nullptr) {
                    destroy_pipeline_layout(device, layouts.second, nullptr);
                }
                if (layouts.first != VK_NULL_HANDLE &&
                    destroy_descriptor_set_layout != nullptr) {
                    destroy_descriptor_set_layout(
                        device, layouts.first, nullptr);
                }
            }
            shared_graphics_layouts.clear();
            destroy_output_resources();
            // The scalar handles are mirrors of the RGBA8 variants; the
            // variant maps own every pipeline and destroy them all.
            destroy_guest_pipeline_variants(diagnostic_pipeline_variants);
            destroy_guest_pipeline_variants(solid_white_pipeline_variants);
            destroy_guest_pipeline_variants(sampled_pipeline_variants);
            destroy_guest_pipeline_variants(
                astro_state29_pipeline_variants);
            destroy_guest_pipeline_variants(
                astro_state28_pipeline_variants);
            if (destroy_pipeline != nullptr) {
                for (const auto& entry : real_composition_pipelines) {
                    if (entry.second != VK_NULL_HANDLE) {
                        destroy_pipeline(device, entry.second, nullptr);
                    }
                }
            }
            real_composition_pipelines.clear();
            for (auto& pass : astro_post_pipelines) {
                destroy_guest_pipeline_variants(pass.pipeline_variants);
                for (std::size_t index = 0;
                     index < pass.storage_buffers.size();
                     ++index) {
                    if (pass.storage_buffers[index] !=
                            VK_NULL_HANDLE &&
                        destroy_buffer != nullptr) {
                        destroy_buffer(
                            device,
                            pass.storage_buffers[index],
                            nullptr);
                    }
                    if (pass.storage_memory[index] !=
                            VK_NULL_HANDLE &&
                        free_memory != nullptr) {
                        free_memory(
                            device,
                            pass.storage_memory[index],
                            nullptr);
                    }
                }
            }
            for (auto& [state, pipeline] :
                 resource_free_guest_pipelines) {
                (void)state;
                destroy_guest_pipeline_variants(pipeline);
            }
            for (auto& [state, pipeline] :
                 resource_free_guest_strip_pipelines) {
                (void)state;
                destroy_guest_pipeline_variants(pipeline);
            }
            resource_free_guest_pipelines.clear();
            resource_free_guest_strip_pipelines.clear();
            if (real_vertex_buffer != VK_NULL_HANDLE &&
                destroy_buffer != nullptr) {
                destroy_buffer(device, real_vertex_buffer, nullptr);
            }
            if (real_vertex_memory != VK_NULL_HANDLE &&
                free_memory != nullptr) {
                free_memory(device, real_vertex_memory, nullptr);
            }
            for (std::size_t index = 0;
                 index < astro_state29_storage_buffers.size();
                 ++index) {
                if (astro_state29_storage_buffers[index] !=
                        VK_NULL_HANDLE &&
                    destroy_buffer != nullptr) {
                    destroy_buffer(
                        device,
                        astro_state29_storage_buffers[index],
                        nullptr);
                }
                if (astro_state29_storage_memory[index] !=
                        VK_NULL_HANDLE &&
                    free_memory != nullptr) {
                    free_memory(
                        device,
                        astro_state29_storage_memory[index],
                        nullptr);
                }
            }
            for (std::size_t index = 0;
                 index < astro_state28_storage_buffers.size();
                 ++index) {
                if (astro_state28_storage_buffers[index] !=
                        VK_NULL_HANDLE &&
                    destroy_buffer != nullptr) {
                    destroy_buffer(
                        device,
                        astro_state28_storage_buffers[index],
                        nullptr);
                }
                if (astro_state28_storage_memory[index] !=
                        VK_NULL_HANDLE &&
                    free_memory != nullptr) {
                    free_memory(
                        device,
                        astro_state28_storage_memory[index],
                        nullptr);
                }
            }
            if (pipeline_layout != VK_NULL_HANDLE &&
                destroy_pipeline_layout != nullptr) {
                destroy_pipeline_layout(device, pipeline_layout, nullptr);
            }
            if (sampled_pipeline_layout != VK_NULL_HANDLE &&
                destroy_pipeline_layout != nullptr) {
                destroy_pipeline_layout(
                    device,
                    sampled_pipeline_layout,
                    nullptr);
            }
            if (real_pipeline_layout != VK_NULL_HANDLE &&
                destroy_pipeline_layout != nullptr) {
                destroy_pipeline_layout(
                    device,
                    real_pipeline_layout,
                    nullptr);
            }
            if (astro_state29_pipeline_layout != VK_NULL_HANDLE &&
                destroy_pipeline_layout != nullptr) {
                destroy_pipeline_layout(
                    device,
                    astro_state29_pipeline_layout,
                    nullptr);
            }
            if (astro_state28_pipeline_layout != VK_NULL_HANDLE &&
                destroy_pipeline_layout != nullptr) {
                destroy_pipeline_layout(
                    device,
                    astro_state28_pipeline_layout,
                    nullptr);
            }
            for (auto& pass : astro_post_pipelines) {
                if (pass.pipeline_layout != VK_NULL_HANDLE &&
                    destroy_pipeline_layout != nullptr) {
                    destroy_pipeline_layout(
                        device,
                        pass.pipeline_layout,
                        nullptr);
                }
            }
            if (sampled_descriptor_pool != VK_NULL_HANDLE &&
                destroy_descriptor_pool != nullptr) {
                destroy_descriptor_pool(
                    device,
                    sampled_descriptor_pool,
                    nullptr);
            }
            if (real_descriptor_pool != VK_NULL_HANDLE &&
                destroy_descriptor_pool != nullptr) {
                destroy_descriptor_pool(
                    device,
                    real_descriptor_pool,
                    nullptr);
            }
            if (astro_state29_descriptor_pool != VK_NULL_HANDLE &&
                destroy_descriptor_pool != nullptr) {
                destroy_descriptor_pool(
                    device,
                    astro_state29_descriptor_pool,
                    nullptr);
            }
            if (astro_state28_descriptor_pool != VK_NULL_HANDLE &&
                destroy_descriptor_pool != nullptr) {
                destroy_descriptor_pool(
                    device,
                    astro_state28_descriptor_pool,
                    nullptr);
            }
            for (auto& pass : astro_post_pipelines) {
                if (pass.descriptor_pool != VK_NULL_HANDLE &&
                    destroy_descriptor_pool != nullptr) {
                    destroy_descriptor_pool(
                        device,
                        pass.descriptor_pool,
                        nullptr);
                }
            }
            for (std::size_t index = 0;
                 index < real_storage_buffers.size();
                 ++index) {
                if (real_storage_buffers[index] != VK_NULL_HANDLE &&
                    destroy_buffer != nullptr) {
                    destroy_buffer(
                        device,
                        real_storage_buffers[index],
                        nullptr);
                }
                if (real_storage_memory[index] != VK_NULL_HANDLE &&
                    free_memory != nullptr) {
                    free_memory(
                        device,
                        real_storage_memory[index],
                        nullptr);
                }
            }
            if (sampled_sampler != VK_NULL_HANDLE &&
                destroy_sampler != nullptr) {
                destroy_sampler(device, sampled_sampler, nullptr);
            }
            if (sampled_descriptor_set_layout != VK_NULL_HANDLE &&
                destroy_descriptor_set_layout != nullptr) {
                destroy_descriptor_set_layout(
                    device,
                    sampled_descriptor_set_layout,
                    nullptr);
            }
            if (real_descriptor_set_layout != VK_NULL_HANDLE &&
                destroy_descriptor_set_layout != nullptr) {
                destroy_descriptor_set_layout(
                    device,
                    real_descriptor_set_layout,
                    nullptr);
            }
            if (astro_state29_descriptor_set_layout != VK_NULL_HANDLE &&
                destroy_descriptor_set_layout != nullptr) {
                destroy_descriptor_set_layout(
                    device,
                    astro_state29_descriptor_set_layout,
                    nullptr);
            }
            if (astro_state28_descriptor_set_layout != VK_NULL_HANDLE &&
                destroy_descriptor_set_layout != nullptr) {
                destroy_descriptor_set_layout(
                    device,
                    astro_state28_descriptor_set_layout,
                    nullptr);
            }
            for (auto& pass : astro_post_pipelines) {
                if (pass.descriptor_set_layout != VK_NULL_HANDLE &&
                    destroy_descriptor_set_layout != nullptr) {
                    destroy_descriptor_set_layout(
                        device,
                        pass.descriptor_set_layout,
                        nullptr);
                }
            }
            if (destroy_render_pass != nullptr) {
                for (const auto& entry : clear_render_passes) {
                    destroy_render_pass(device, entry.second, nullptr);
                }
                for (const auto& entry : load_render_passes) {
                    destroy_render_pass(device, entry.second, nullptr);
                }
            }
            clear_render_passes.clear();
            load_render_passes.clear();
            surface_usage_cache.clear();
            if (command_buffer != VK_NULL_HANDLE &&
                free_command_buffers != nullptr &&
                command_pool != VK_NULL_HANDLE) {
                free_command_buffers(
                    device,
                    command_pool,
                    1,
                    &command_buffer);
            }
            if (command_pool != VK_NULL_HANDLE &&
                destroy_command_pool != nullptr) {
                destroy_command_pool(device, command_pool, nullptr);
            }
            if (pipeline_cache != VK_NULL_HANDLE &&
                destroy_pipeline_cache != nullptr) {
                destroy_pipeline_cache(
                    device,
                    pipeline_cache,
                    nullptr);
            }
            if (destroy_device != nullptr) {
                destroy_device(device, nullptr);
            }
        }
        command_buffer = VK_NULL_HANDLE;
        command_pool = VK_NULL_HANDLE;
        pipeline_cache = VK_NULL_HANDLE;
        pipeline_cache_path.clear();
        saved_pipeline_cache_hash = 0;
        diagnostic_pipeline = VK_NULL_HANDLE;
        solid_white_pipeline = VK_NULL_HANDLE;
        sampled_pipeline = VK_NULL_HANDLE;
        real_composition_pipeline = VK_NULL_HANDLE;
        astro_state28_pipeline = VK_NULL_HANDLE;
        astro_state29_pipeline = VK_NULL_HANDLE;
        real_vertex_buffer = VK_NULL_HANDLE;
        real_vertex_memory = VK_NULL_HANDLE;
        real_vertex_address = 0;
        real_vertex_stride = 0;
        real_vertex_count = 0;
        real_vertex_ready = false;
        pipeline_layout = VK_NULL_HANDLE;
        sampled_pipeline_layout = VK_NULL_HANDLE;
        real_pipeline_layout = VK_NULL_HANDLE;
        astro_state28_pipeline_layout = VK_NULL_HANDLE;
        astro_state29_pipeline_layout = VK_NULL_HANDLE;
        sampled_descriptor_pool = VK_NULL_HANDLE;
        real_descriptor_pool = VK_NULL_HANDLE;
        real_descriptor_set = VK_NULL_HANDLE;
        astro_state28_descriptor_pool = VK_NULL_HANDLE;
        astro_state28_descriptor_set = VK_NULL_HANDLE;
        astro_state28_storage_buffers.fill(VK_NULL_HANDLE);
        astro_state28_storage_memory.fill(VK_NULL_HANDLE);
        astro_state28_resources_ready = false;
        for (auto& pass : astro_post_pipelines) {
            pass.descriptor_set_layout = VK_NULL_HANDLE;
            pass.descriptor_pool = VK_NULL_HANDLE;
            pass.descriptor_set = VK_NULL_HANDLE;
            pass.pipeline_layout = VK_NULL_HANDLE;
            pass.pipeline = VK_NULL_HANDLE;
            pass.storage_buffers.fill(VK_NULL_HANDLE);
            pass.storage_memory.fill(VK_NULL_HANDLE);
            pass.resources_ready = false;
        }
        astro_state29_descriptor_pool = VK_NULL_HANDLE;
        astro_state29_descriptor_set = VK_NULL_HANDLE;
        astro_state29_storage_buffers.fill(VK_NULL_HANDLE);
        astro_state29_storage_memory.fill(VK_NULL_HANDLE);
        astro_state29_resources_ready = false;
        real_storage_buffers.fill(VK_NULL_HANDLE);
        real_storage_memory.fill(VK_NULL_HANDLE);
        real_resources_ready = false;
        sampled_sampler = VK_NULL_HANDLE;
        sampled_descriptor_set_layout = VK_NULL_HANDLE;
        real_descriptor_set_layout = VK_NULL_HANDLE;
        astro_state28_descriptor_set_layout = VK_NULL_HANDLE;
        astro_state29_descriptor_set_layout = VK_NULL_HANDLE;
        render_pass = VK_NULL_HANDLE;
        load_render_pass = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        physical_device = VK_NULL_HANDLE;
        if (instance != VK_NULL_HANDLE && destroy_instance != nullptr) {
            destroy_instance(instance, nullptr);
        }
        instance = VK_NULL_HANDLE;
        if (loader != nullptr) {
            FreeLibrary(loader);
            loader = nullptr;
        }
    }
};

struct NativeGpuRuntime {
    std::mutex mutex;
    std::condition_variable work_available;
    std::condition_variable idle;
    // Signalled when the worker has finished a flip, for the submitting
    // side to wait on when it has run too far ahead.
    std::condition_variable flip_finished;
    std::uint64_t flips_finished = 0;
    std::uint64_t flips_enqueued = 0;
    std::deque<QueuedCommand> queue;
    std::thread worker;
    std::uint32_t queue_capacity = kDefaultQueueCapacity;
    bool stopping = false;
    bool worker_running = false;
    std::uint64_t commands_in_flight = 0;
    std::uint64_t draws_submitted = 0;
    std::uint64_t draws_processed = 0;
    std::uint64_t draws_dropped = 0;
    std::uint64_t compute_dispatches_submitted = 0;
    std::uint64_t compute_dispatches_processed = 0;
    std::uint64_t compute_dispatches_dropped = 0;
    std::uint64_t flips_submitted = 0;
    std::uint64_t flips_processed = 0;
    std::uint64_t flips_dropped = 0;
    std::uint64_t queue_high_watermark = 0;
    std::uint64_t frames_discarded = 0;
    std::uint64_t commands_discarded = 0;
    VulkanBackend vulkan;
    std::mutex compute_state_mutex;
    std::map<
        std::uint32_t,
        std::shared_ptr<const RegisteredComputeState>>
        compute_states;
    std::map<std::uint64_t, std::vector<std::uint32_t>>
        compute_state_ids_by_hash;
    std::uint32_t next_compute_state_id = 1;
    std::mutex capture_mutex;
    std::filesystem::path capture_path;
    std::vector<Ps5GpuNativeDraw> frame_draws;
    std::vector<Ps5GpuCaptureCommand> frame_commands;
    struct CapturedShader {
        Ps5GpuCaptureShader record = {};
        std::vector<std::uint32_t> words;
        std::array<
            std::uint32_t,
            static_cast<std::size_t>(ps5gpu::gen5::Encoding::Count)>
            encoding_counts = {};
    };
    std::map<
        std::pair<std::uint32_t, std::uint64_t>,
        CapturedShader> captured_shaders;
    struct CapturedState {
        Ps5GpuCaptureShaderState record = {};
        std::vector<Ps5GpuNativeRegisterValue> sh_registers;
        std::vector<Ps5GpuNativeRegisterValue> cx_registers;
    };
    std::map<std::uint32_t, CapturedState> captured_states;
    std::map<std::uint64_t, std::vector<std::uint32_t>> state_ids_by_hash;
    std::uint32_t next_state_id = 1;
    struct CapturedComputeState {
        Ps5GpuCaptureComputeState record = {};
        std::vector<std::uint8_t> spirv;
        std::vector<std::uint8_t> resource_manifest;
    };
    std::map<std::uint32_t, CapturedComputeState> captured_compute_states;
    struct CapturedMemory {
        Ps5GpuCaptureMemory record = {};
        std::vector<std::uint8_t> bytes;
    };
    std::map<std::uint64_t, CapturedMemory> captured_memory;
    bool capture_written = false;
    std::size_t captured_flips = 0;
};

// The same block writing the AGC timeline needed, for the same reason: one
// unbuffered WriteFile per line measured at 1.1ms there, and the worker
// emits its own eighteen thousand lines a run. Lines stay whole and stay
// ordered within a thread; every line carries its own timestamp, so what
// ordering is lost between threads was never being relied on.
constexpr std::size_t kRuntimeTraceBufferBytes = 16384;
thread_local char g_runtime_trace_buffer[kRuntimeTraceBufferBytes] = {};
thread_local std::size_t g_runtime_trace_used = 0;
thread_local std::uint64_t g_runtime_trace_ticks = 0;
thread_local std::uint64_t g_runtime_trace_count = 0;

void runtime_trace_flush() {
    if (g_runtime_trace_used == 0) {
        return;
    }
    DWORD written = 0;
    WriteFile(
        GetStdHandle(STD_ERROR_HANDLE),
        g_runtime_trace_buffer,
        static_cast<DWORD>(g_runtime_trace_used),
        &written,
        nullptr);
    g_runtime_trace_used = 0;
}

// No thread_local destructor to flush a departing thread, the way the HLE
// side does it: this is a DLL, and a build where that hook is missing loses
// the whole stream rather than a tail. The worker flushes on every flip and
// again when it stops, which covers the thread that traces.
namespace {

// What the worker spends its time on. The queue now saturates - four
// thousand draws and fifty-seven flips were refused in one run - so the
// worker is what the frame rate is made of, and the three command kinds
// want telling apart before anything is done about any of them. Written
// only by the worker thread.
std::uint64_t g_worker_draw_ticks = 0;
std::uint64_t g_worker_draw_count = 0;
std::uint64_t g_worker_compute_ticks = 0;
std::uint64_t g_worker_compute_count = 0;
std::uint64_t g_worker_flip_ticks = 0;
std::uint64_t g_worker_flip_count = 0;
std::uint64_t g_worker_idle_ticks = 0;
std::uint64_t g_worker_commands = 0;
// The worker stops taking commands part way through a run and the
// ticks simply end. Whatever it is stuck in is one command, so carry
// the worst one of each batch: a stall shows up as a batch whose
// slowest command accounts for the whole batch.
std::uint64_t g_worker_slowest_ticks = 0;
const char* g_worker_slowest_type = "none";

std::int64_t runtime_trace_counter() {
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

std::int64_t runtime_trace_frequency() {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value = {};
        QueryPerformanceFrequency(&value);
        return value.QuadPart != 0 ? value.QuadPart : 1;
    }();
    return frequency;
}

}  // namespace

#define SITE_PAIR(site)                                              \
    static_cast<unsigned long long>(                                     \
        g_vk_site_count[static_cast<std::size_t>(site)].load(            \
            std::memory_order_relaxed)),                                 \
        static_cast<unsigned long long>(                                 \
            g_vk_site_bytes[static_cast<std::size_t>(site)].load(        \
                std::memory_order_relaxed) >>                            \
            20)

std::uint64_t worker_commit_free_megabytes() {
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return 0;
    }
    return status.ullAvailPageFile >> 20;
}

struct WorkerProcessMemoryCounters {
    DWORD cb = 0;
    DWORD page_fault_count = 0;
    SIZE_T peak_working_set_size = 0;
    SIZE_T working_set_size = 0;
    SIZE_T quota_peak_paged_pool_usage = 0;
    SIZE_T quota_paged_pool_usage = 0;
    SIZE_T quota_peak_non_paged_pool_usage = 0;
    SIZE_T quota_non_paged_pool_usage = 0;
    SIZE_T pagefile_usage = 0;
    SIZE_T peak_pagefile_usage = 0;
    SIZE_T private_usage = 0;
};

extern "C" BOOL WINAPI K32GetProcessMemoryInfo(
    HANDLE, WorkerProcessMemoryCounters*, DWORD);

// Where the committed memory actually is. Every account of this leak so
// far has been inferred from a counter that turned out to measure churn
// rather than residency - manifests at 16.7GB that are trimmed on
// registration, snapshots at a claimed gigabytes that measure 67MB. This
// asks the address space instead: it walks every region, sums what is
// committed and private, and names the largest blocks. Printed once per
// call, and the caller only makes it when the total has grown by a
// gigabyte since the last one, so a run carries a handful of these.
void trace_committed_memory_map() {
    struct Block {
        std::uintptr_t base = 0;
        std::uint64_t bytes = 0;
        DWORD protect = 0;
        DWORD type = 0;
    };
    std::vector<Block> blocks;
    std::uint64_t private_bytes = 0;
    std::uint64_t mapped_bytes = 0;
    std::uint64_t image_bytes = 0;
    std::uint64_t reserved_bytes = 0;
    std::uintptr_t cursor = 0;
    MEMORY_BASIC_INFORMATION region = {};
    while (VirtualQuery(
               reinterpret_cast<const void*>(cursor),
               &region,
               sizeof(region)) == sizeof(region)) {
        const auto base = reinterpret_cast<std::uintptr_t>(
            region.BaseAddress);
        const auto size = static_cast<std::uint64_t>(region.RegionSize);
        if (size == 0) {
            break;
        }
        if (region.State == MEM_COMMIT) {
            if (region.Type == MEM_PRIVATE) {
                private_bytes += size;
                blocks.push_back(
                    Block{base, size, region.Protect, region.Type});
            } else if (region.Type == MEM_IMAGE) {
                image_bytes += size;
            } else {
                mapped_bytes += size;
            }
        } else if (region.State == MEM_RESERVE) {
            reserved_bytes += size;
        }
        const auto next = base + size;
        if (next <= cursor) {
            break;
        }
        cursor = next;
    }
    std::sort(
        blocks.begin(),
        blocks.end(),
        [](const Block& left, const Block& right) {
            return left.bytes > right.bytes;
        });
    runtime_trace(
        "native_gpu.memory_map private=%lluMB mapped=%lluMB image=%lluMB "
        "reserved=%lluMB blocks=%llu\n",
        static_cast<unsigned long long>(private_bytes / (1024 * 1024)),
        static_cast<unsigned long long>(mapped_bytes / (1024 * 1024)),
        static_cast<unsigned long long>(image_bytes / (1024 * 1024)),
        static_cast<unsigned long long>(reserved_bytes / (1024 * 1024)),
        static_cast<unsigned long long>(blocks.size()));
    const auto shown = (std::min)(blocks.size(), std::size_t{12});
    for (std::size_t index = 0; index < shown; ++index) {
        runtime_trace(
            "native_gpu.memory_block base=0x%016llX bytes=%lluMB "
            "protect=0x%08lX\n",
            static_cast<unsigned long long>(blocks[index].base),
            static_cast<unsigned long long>(
                blocks[index].bytes / (1024 * 1024)),
            static_cast<unsigned long>(blocks[index].protect));
    }
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>>
        buckets;
    for (const auto& block : blocks) {
        auto& bucket = buckets[block.base >> 32];
        ++bucket.first;
        bucket.second += block.bytes;
    }
    for (const auto& [prefix, totals] : buckets) {
        if (totals.second < 64ULL * 1024ULL * 1024ULL) {
            continue;
        }
        runtime_trace(
            "native_gpu.memory_bucket prefix=0x%08llX blocks=%llu "
            "bytes=%lluMB\n",
            static_cast<unsigned long long>(prefix),
            static_cast<unsigned long long>(totals.first),
            static_cast<unsigned long long>(
                totals.second / (1024 * 1024)));
    }
    // Many small private regions are a different shape of leak from one
    // large one, so say how much is in blocks too small to list.
    std::uint64_t tail_bytes = 0;
    for (std::size_t index = shown; index < blocks.size(); ++index) {
        tail_bytes += blocks[index].bytes;
    }
    runtime_trace(
        "native_gpu.memory_tail blocks=%llu bytes=%lluMB\n",
        static_cast<unsigned long long>(blocks.size() - shown),
        static_cast<unsigned long long>(tail_bytes / (1024 * 1024)));
}

std::uint64_t worker_private_megabytes() {
    WorkerProcessMemoryCounters counters = {};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(
            GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return static_cast<std::uint64_t>(counters.private_usage) >> 20;
}

std::uint64_t worker_physical_free_megabytes() {
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return 0;
    }
    return status.ullAvailPhys >> 20;
}

void runtime_trace(const char* format, ...) {
    // worker_phases has outgrown 1152 characters and has been cut off mid
    // word for a while, which quietly took watch_arms, watch_faults and
    // watch_refused out of every run - counters that exist and are never
    // seen are worse than counters that do not.
    char buffer[4096] = {};
    // PS5RT_TRACE_TIME stamps each line with system uptime, the one clock all
    // modules in this process share, so runner, HLE and GPU traces land on a
    // single comparable timeline. Off by default: astro-cycle.ps1 parses this
    // stream and should not see a format change unless timing is asked for.
    static const bool trace_time =
        environment_flag_enabled("PS5RT_TRACE_TIME") ||
        environment_flag_enabled("PS5GPU_NATIVE_TRACE_TIME");
    int offset = 0;
    if (trace_time) {
        const auto now = GetTickCount64();
        const int written_prefix = std::snprintf(
            buffer,
            sizeof(buffer),
            "[t=%llu.%03llu] ",
            static_cast<unsigned long long>(now / 1000),
            static_cast<unsigned long long>(now % 1000));
        if (written_prefix > 0) {
            offset = written_prefix;
        }
    }
    std::va_list arguments;
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
    const auto total = static_cast<std::size_t>(
        std::min<int>(
            offset + length,
            static_cast<int>(sizeof(buffer) - 1)));
    const auto started = runtime_trace_counter();
    if (g_runtime_trace_used + total > kRuntimeTraceBufferBytes) {
        runtime_trace_flush();
    }
    if (total > kRuntimeTraceBufferBytes) {
        DWORD written = 0;
        WriteFile(
            GetStdHandle(STD_ERROR_HANDLE),
            buffer,
            static_cast<DWORD>(total),
            &written,
            nullptr);
    } else {
        std::memcpy(g_runtime_trace_buffer + g_runtime_trace_used, buffer, total);
        g_runtime_trace_used += total;
    }
    const auto elapsed = runtime_trace_counter() - started;
    g_runtime_trace_ticks += elapsed > 0
        ? static_cast<std::uint64_t>(elapsed)
        : 0;
    ++g_runtime_trace_count;
}

bool valid_header(std::uint32_t struct_size, std::uint32_t expected_size,
                  std::uint32_t abi_version) {
    return struct_size >= expected_size &&
        abi_version == PS5GPU_NATIVE_ABI_VERSION;
}

// The guest is this process, so reading and writing its memory is a
// memcpy. It was ReadProcessMemory and WriteProcessMemory, and the
// write-back path makes two calls per changed page - 8656 pages in a run,
// 3.7 seconds, four hundred microseconds a page, against 150ms for the
// comparison that finds them.
//
// What those calls gave was a false return rather than a fault on a range
// that is not there, and that is kept by asking VirtualQuery. But the
// expense is not the kernel call: it is the range check itself. Replacing
// the calls with a memcpy behind a VirtualQuery per page changed nothing -
// every copy took the fast path, 6101 writes with no fallback, and the
// splice still cost 0.58ms a page. This address space has enough regions
// that one query is about 0.29ms, and there are two per page.
//
// So the answer is cached, and the two ways it could go stale are closed.
// This file changes protections itself - the image write watch arms pages
// read-only on the thread that later writes them back - so every
// VirtualProtect here bumps a generation the cache carries. Anything the
// guest changes behind us is not covered by that, so the cache does not
// outlive a frame either. Without both, one run in three ended in an
// access violation on a page the cache still believed was writable.
//
// 3.7 seconds to 45 milliseconds, over six runs, with the same page counts.
struct GuestRegionEntry {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    DWORD protect = 0;
    std::uint64_t generation = 0;
};
// One entry was one region: the worker reads a few hundred buffers a
// frame from different regions, and every one of them asked VirtualQuery
// again - a seventh of the worker during the intro video.
struct GuestRegionCache {
    // A frame reads a couple of thousand buffers out of a few hundred
    // regions; with 32 the cache turned over every frame and each read
    // went to VirtualQuery.
    static constexpr int kEntries = 128;
    GuestRegionEntry entries[kEntries];
    int next = 0;
};

thread_local GuestRegionCache g_guest_region_cache;

std::atomic<std::uint64_t> g_guest_copy_fast{0};
std::atomic<std::uint64_t> g_guest_copy_slow{0};
// Reads that only succeeded because the reserved part of the range was
// taken to be zero. They are the reads that used to fail outright and
// fall back to the manifest's snapshot of the buffer.
std::atomic<std::uint64_t> g_guest_copy_reserved{0};
std::atomic<std::uint64_t> g_guest_write_fast{0};
std::atomic<std::uint64_t> g_guest_write_slow{0};
std::atomic<std::uint64_t> g_guest_write_disarmed{0};
std::atomic<std::uint64_t> g_guest_commit_on_use{0};

bool guest_range_writable(DWORD protect) {
    return (protect &
            (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
             PAGE_EXECUTE_WRITECOPY)) != 0;
}

// The runner reserves the large guest maps and commits them as they are
// used, so a range the GPU worker wants can legitimately be MEM_RESERVE.
// A render target only this side ever writes would otherwise never be
// committed by anyone: the guest never touches it, so no fault brings it
// in. Bounded to the guest address window so a stray address cannot make
// this commit host memory.
constexpr std::uint64_t kGuestWindowBegin = 0x0000000400000000ULL;
constexpr std::uint64_t kGuestWindowEnd = 0x0000010000000000ULL;

bool commit_reserved_guest_region(
    std::uint64_t begin,
    std::uint64_t end,
    MEMORY_BASIC_INFORMATION& region) {
    if (region.State != MEM_RESERVE ||
        begin < kGuestWindowBegin ||
        end > kGuestWindowEnd) {
        return false;
    }
    const auto region_begin =
        reinterpret_cast<std::uint64_t>(region.BaseAddress);
    const auto region_end = region_begin + region.RegionSize;
    const auto span = (region_end < end ? region_end : end) - begin;
    if (region_end <= begin ||
        VirtualAlloc(
            reinterpret_cast<void*>(begin),
            static_cast<SIZE_T>(span),
            MEM_COMMIT,
            PAGE_READWRITE) == nullptr) {
        return false;
    }
    g_guest_commit_on_use.fetch_add(span, std::memory_order_relaxed);
    return VirtualQuery(
               reinterpret_cast<const void*>(begin),
               &region,
               sizeof(region)) == sizeof(region) &&
        region.State == MEM_COMMIT;
}

bool guest_range_usable(
    std::uint64_t address,
    std::size_t size,
    bool for_write) {
    const auto begin = static_cast<std::uintptr_t>(address);
    const auto end = begin + size;
    if (end < begin) {
        return false;
    }

    const auto generation =
        g_guest_protect_generation.load(std::memory_order_acquire);
    auto& caches = g_guest_region_cache;
    GuestRegionEntry* hit = nullptr;
    for (auto& entry : caches.entries) {
        if (entry.end != 0 && begin >= entry.begin && end <= entry.end) {
            // A read is answered whatever protection changed since: this
            // file's watches only move pages between read-write and
            // read-only. Only a write needs the current protection.
            if (!for_write) {
                hit = &entry;
            } else if (entry.generation == generation ||
                protect_unchanged_since(
                    entry.begin, entry.end, entry.generation, generation)) {
                entry.generation = generation;
                hit = &entry;
            }
            break;
        }
    }
    auto& cache = hit != nullptr
        ? *hit
        : caches.entries[caches.next++ % GuestRegionCache::kEntries];
    MEMORY_BASIC_INFORMATION region = {};
    if (hit != nullptr) {
        region.State = MEM_COMMIT;
        region.BaseAddress = reinterpret_cast<void*>(cache.begin);
        region.RegionSize = cache.end - cache.begin;
        region.Protect = cache.protect;
    } else if (
        VirtualQuery(
            reinterpret_cast<const void*>(begin),
            &region,
            sizeof(region)) != sizeof(region) ||
        (region.State != MEM_COMMIT &&
         !commit_reserved_guest_region(begin, end, region))) {
        cache = {};
        return false;
    } else {
        cache.begin =
            reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        cache.end = cache.begin + region.RegionSize;
        cache.protect = region.Protect;
        cache.generation = generation;
    }

    const auto region_begin =
        reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (begin < region_begin ||
        end > region_begin + region.RegionSize) {
        return false;
    }

    if ((region.Protect & PAGE_NOACCESS) != 0 ||
        (region.Protect & PAGE_GUARD) != 0 ||
        (for_write && !guest_range_writable(region.Protect))) {
        static std::atomic<std::uint64_t> refusals{0};
        if (refusals.fetch_add(1, std::memory_order_relaxed) < 8) {
            runtime_trace(
                "native_gpu.guest_copy_refused address=0x%016llX "
                "size=%llu write=%u state=0x%08lX protect=0x%08lX "
                "base=0x%016llX region=%llu\n",
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(size),
                for_write ? 1u : 0u,
                static_cast<unsigned long>(region.State),
                static_cast<unsigned long>(region.Protect),
                static_cast<unsigned long long>(region_begin),
                static_cast<unsigned long long>(region.RegionSize));
        }
        return false;
    }

    return true;
}

// ReadProcessMemory refuses a whole range that contains one uncommitted
// page, and guest_range_usable only answers for a range lying inside a
// single region, so a buffer straddling the boundary between memory the
// title has written and memory it has only reserved could not be read at
// all. The reserved part is not data that went missing: nothing has ever
// written it, so it reads as zero. Walk the range a region at a time,
// copying what is committed and zeroing the rest - which is what the
// managed bridge was already taught to do, for this same reason.
bool read_guest_range_filling_reserved(
    std::uint64_t address,
    void* destination,
    std::size_t size) {
    auto* out = static_cast<std::uint8_t*>(destination);
    auto cursor = static_cast<std::uintptr_t>(address);
    const auto end = cursor + size;
    if (end < cursor) {
        return false;
    }
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE |
        PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
        PAGE_EXECUTE_WRITECOPY;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION region = {};
        if (VirtualQuery(
                reinterpret_cast<const void*>(cursor),
                &region,
                sizeof(region)) != sizeof(region)) {
            return false;
        }
        const auto region_begin =
            reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto region_end = region_begin + region.RegionSize;
        if (region_end <= cursor) {
            return false;
        }
        const auto chunk = static_cast<std::size_t>(
            (std::min)(region_end, end) - cursor);
        if (region.State == MEM_COMMIT &&
            (region.Protect & kReadable) != 0 &&
            (region.Protect & PAGE_GUARD) == 0) {
            std::memcpy(
                out, reinterpret_cast<const void*>(cursor), chunk);
        } else if (region.State == MEM_RESERVE) {
            std::memset(out, 0, chunk);
        } else {
            // Free, or committed and not readable. Neither is a page the
            // title merely has not touched yet, so do not invent content
            // for it.
            return false;
        }
        out += chunk;
        cursor += chunk;
    }
    return true;
}

bool read_current_process_memory(
    std::uint64_t address,
    void* destination,
    std::size_t size,
    void*) {
    if (address < 0x10000 || destination == nullptr || size == 0) {
        return false;
    }
    if (guest_range_usable(address, size, false)) {
        std::memcpy(
            destination,
            reinterpret_cast<const void*>(address),
            size);
        g_guest_copy_fast.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_guest_copy_slow.fetch_add(1, std::memory_order_relaxed);
    SIZE_T bytes_read = 0;
    if (ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(address),
            destination,
            size,
            &bytes_read) &&
        bytes_read == size) {
        return true;
    }
    if (read_guest_range_filling_reserved(address, destination, size)) {
        g_guest_copy_reserved.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool write_current_process_memory(
    std::uint64_t address,
    const void* source,
    std::size_t size) {
    if (address < 0x10000 || source == nullptr || size == 0) {
        return false;
    }
    if (guest_range_usable(address, size, true)) {
        std::memcpy(reinterpret_cast<void*>(address), source, size);
        g_guest_write_fast.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // Before deciding the page cannot be written, ask whether the thing
    // refusing is this file's own texture watch.
    if (guest_watch_disarm_covering(address, size) &&
        guest_range_usable(address, size, true)) {
        std::memcpy(reinterpret_cast<void*>(address), source, size);
        g_guest_write_disarmed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_guest_write_slow.fetch_add(1, std::memory_order_relaxed);
    SIZE_T bytes_written = 0;
    if (WriteProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<void*>(address),
            source,
            size,
            &bytes_written) &&
        bytes_written == size) {
        return true;
    }
    const auto error = GetLastError();
    static std::atomic<std::uint64_t> write_failures{0};
    if (write_failures.fetch_add(1, std::memory_order_relaxed) < 8) {
        MEMORY_BASIC_INFORMATION region = {};
        const auto queried =
            VirtualQuery(
                reinterpret_cast<const void*>(address),
                &region,
                sizeof(region)) == sizeof(region);
        runtime_trace(
            "native_gpu.guest_write_failed address=0x%016llX size=%llu "
            "written=%llu error=%lu queried=%d base=0x%016llX "
            "alloc=0x%016llX state=0x%08lX protect=0x%08lX "
            "alloc_protect=0x%08lX type=0x%08lX region=%llu\n",
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(bytes_written),
            static_cast<unsigned long>(error),
            queried ? 1 : 0,
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(region.BaseAddress)),
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(region.AllocationBase)),
            static_cast<unsigned long>(region.State),
            static_cast<unsigned long>(region.Protect),
            static_cast<unsigned long>(region.AllocationProtect),
            static_cast<unsigned long>(region.Type),
            static_cast<unsigned long long>(region.RegionSize));
    }
    return false;
}

std::uint64_t hash_bytes(
    const void* data,
    std::size_t size,
    std::uint64_t hash) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t hash_shader_state(
    const Ps5GpuCaptureShaderState& record,
    const std::vector<Ps5GpuNativeRegisterValue>& sh_registers,
    const std::vector<Ps5GpuNativeRegisterValue>& cx_registers) {
    auto hash = hash_bytes(
        &record.flags,
        offsetof(Ps5GpuCaptureShaderState, hash) -
            offsetof(Ps5GpuCaptureShaderState, flags));
    hash = hash_bytes(
        &record.es_address,
        offsetof(Ps5GpuCaptureShaderState, reserved0) -
            offsetof(Ps5GpuCaptureShaderState, es_address),
        hash);
    if (!sh_registers.empty()) {
        hash = hash_bytes(
            sh_registers.data(),
            sh_registers.size() * sizeof(sh_registers[0]),
            hash);
    }
    if (!cx_registers.empty()) {
        hash = hash_bytes(
            cx_registers.data(),
            cx_registers.size() * sizeof(cx_registers[0]),
            hash);
    }
    return hash;
}

bool equal_shader_state(
    const NativeGpuRuntime::CapturedState& left,
    const NativeGpuRuntime::CapturedState& right) {
    const auto registers_equal = [](
        const std::vector<Ps5GpuNativeRegisterValue>& lhs,
        const std::vector<Ps5GpuNativeRegisterValue>& rhs) {
        return lhs.size() == rhs.size() &&
            std::equal(
                lhs.begin(),
                lhs.end(),
                rhs.begin(),
                [](const Ps5GpuNativeRegisterValue& a,
                   const Ps5GpuNativeRegisterValue& b) {
                    return a.address == b.address &&
                        a.value == b.value;
                });
    };
    return left.record.flags == right.record.flags &&
        left.record.es_address == right.record.es_address &&
        left.record.ps_address == right.record.ps_address &&
        left.record.es_header_address == right.record.es_header_address &&
        left.record.ps_header_address == right.record.ps_header_address &&
        left.record.export_user_data_base_register ==
            right.record.export_user_data_base_register &&
        left.record.pixel_user_data_base_register ==
            right.record.pixel_user_data_base_register &&
        left.record.pixel_input_enable == right.record.pixel_input_enable &&
        left.record.pixel_input_address == right.record.pixel_input_address &&
        registers_equal(left.sh_registers, right.sh_registers) &&
        registers_equal(left.cx_registers, right.cx_registers);
}

bool read_u16(std::uint64_t address, std::uint16_t& value) {
    return read_current_process_memory(
        address,
        &value,
        sizeof(value),
        nullptr);
}

bool read_u64(std::uint64_t address, std::uint64_t& value) {
    return read_current_process_memory(
        address,
        &value,
        sizeof(value),
        nullptr);
}

void capture_memory_range_locked(
    NativeGpuRuntime& runtime,
    std::uint64_t address,
    std::uint64_t size,
    std::uint32_t flags) {
    if (address < 0x10000 ||
        address > 0x0000FFFFFFFFFFFFULL ||
        size == 0) {
        return;
    }
    const auto first = address & ~(kCapturePageSize - 1);
    const auto requested_end =
        size > std::numeric_limits<std::uint64_t>::max() - address
            ? std::numeric_limits<std::uint64_t>::max()
            : address + size;
    const auto end = requested_end >
            std::numeric_limits<std::uint64_t>::max() -
                (kCapturePageSize - 1)
        ? std::numeric_limits<std::uint64_t>::max() &
            ~(kCapturePageSize - 1)
        : (requested_end + kCapturePageSize - 1) &
            ~(kCapturePageSize - 1);
    const auto page_limit = maximum_captured_memory_pages();
    for (auto page = first; page < end; page += kCapturePageSize) {
        if (runtime.captured_memory.size() >= page_limit) {
            // Truncating here is what makes a capture lie about texture
            // contents, so it must never be silent.
            static bool reported = false;
            if (!reported) {
                reported = true;
                runtime_trace(
                    "native_gpu.capture_memory_limit_reached pages=%zu "
                    "megabytes=%llu address=0x%016llX "
                    "hint=PS5GPU_NATIVE_CAPTURE_MEMORY_MB\n",
                    page_limit,
                    static_cast<unsigned long long>(
                        static_cast<std::uint64_t>(page_limit) *
                        kCapturePageSize / (1024ULL * 1024ULL)),
                    static_cast<unsigned long long>(page));
            }
            break;
        }
        if (auto existing = runtime.captured_memory.find(page);
            existing != runtime.captured_memory.end()) {
            existing->second.record.flags |= flags;
            continue;
        }
        NativeGpuRuntime::CapturedMemory captured;
        captured.record.address = page;
        captured.record.byte_size =
            static_cast<std::uint32_t>(kCapturePageSize);
        captured.record.flags = flags;
        captured.bytes.resize(kCapturePageSize);
        if (!read_current_process_memory(
                page,
                captured.bytes.data(),
                captured.bytes.size(),
                nullptr)) {
            break;
        }
        captured.record.hash = hash_bytes(
            captured.bytes.data(),
            captured.bytes.size());
        runtime.captured_memory.emplace(page, std::move(captured));
    }
}

// The runtime a capture is being written for, so the worker can put into
// it what it reads. A capture used to take a buffer's pages at registration
// or when the file was written, and per-frame constants were something else
// at both moments: replay drew the intro's vertex stage with a zero matrix.
NativeGpuRuntime* g_capture_runtime = nullptr;

void capture_worker_read(std::uint64_t address, std::uint64_t size) {
    auto* runtime = g_capture_runtime;
    if (runtime == nullptr || runtime->capture_path.empty() ||
        runtime->capture_written || size == 0 ||
        size > kDrawSnapshotMaximumBytes) {
        // Constants only: re-taking every large buffer on every read slowed
        // the worker to a tenth of its pace and the capture never came.
        return;
    }
    std::lock_guard capture_lock(runtime->capture_mutex);
    // The worker's read wins over whatever an earlier moment took.
    const auto first = address & ~(kCapturePageSize - 1);
    for (auto page = first; page < address + size; page += kCapturePageSize) {
        runtime->captured_memory.erase(page);
    }
    capture_memory_range_locked(
        *runtime, address, size, PS5GPU_CAPTURE_MEMORY_BUFFER_RESOURCE);
}

// A submit-time snapshot goes into the capture over whatever an earlier
// moment took for the same pages, so replay draws with what the draw saw.
void capture_snapshot_bytes(
    std::uint64_t address, const std::vector<std::uint8_t>& bytes) {
    auto* runtime = g_capture_runtime;
    if (runtime == nullptr || runtime->capture_path.empty() ||
        runtime->capture_written || bytes.empty()) {
        return;
    }
    std::lock_guard capture_lock(runtime->capture_mutex);
    capture_memory_range_locked(
        *runtime, address, bytes.size(),
        PS5GPU_CAPTURE_MEMORY_BUFFER_RESOURCE);
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto at = address + done;
        const auto page = at & ~(kCapturePageSize - 1);
        const auto offset = static_cast<std::size_t>(at - page);
        const auto take = std::min<std::size_t>(
            bytes.size() - done, kCapturePageSize - offset);
        const auto found = runtime->captured_memory.find(page);
        if (found != runtime->captured_memory.end() &&
            found->second.bytes.size() >= offset + take) {
            std::memcpy(
                found->second.bytes.data() + offset,
                bytes.data() + done,
                take);
            found->second.record.hash = hash_bytes(
                found->second.bytes.data(), found->second.bytes.size());
        }
        done += take;
    }
}

// The third copy of this fact, and the third to know a handful of formats where
// the format map knows forty-four. A capture skipped every image it could not
// size, so the textures most worth inspecting - a 1536x1536 atlas among them -
// were never in the file, and offline analysis could not reach them.
std::uint32_t captured_image_bytes_per_element(
    std::uint32_t unified_format) {
    return VulkanBackend::linear_guest_bytes_per_pixel(unified_format);
}

bool calculate_captured_image_bytes(
    const VulkanBackend::GuestImageResource& image,
    std::uint64_t& byte_size) {
    byte_size = 0;
    // A volume is its slices one after another: the fog's 240x135x64 was
    // captured as one slice and replay found the rest unreadable.
    if (image.type == 10 && image.depth > 1) {
        auto slice = image;
        slice.type = 9;
        slice.depth = 1;
        if (!calculate_captured_image_bytes(slice, byte_size)) {
            return false;
        }
        byte_size *= image.depth;
        return true;
    }
    // Block compressed: a 4x4 block is the element, eight bytes for BC1 and
    // BC4 and sixteen for the rest. Left out, every material texture in the
    // scene was missing from captures and replay drew nothing with them.
    if (image.unified_format >= 169 && image.unified_format <= 182) {
        auto blocks = image;
        blocks.width = (image.width + 3) / 4;
        blocks.height = (image.height + 3) / 4;
        blocks.pitch = (std::max(image.pitch, image.width) + 3) / 4;
        const auto eight = image.unified_format == 169 ||
            image.unified_format == 170 || image.unified_format == 175 ||
            image.unified_format == 176;
        // As a format of the block's size, which the code below knows.
        blocks.unified_format = eight ? 71u : 77u;
        if (captured_image_bytes_per_element(blocks.unified_format) !=
                (eight ? 8u : 16u) ||
            !calculate_captured_image_bytes(blocks, byte_size)) {
            return false;
        }
        // Mips and alignment ride along in the slack.
        byte_size = byte_size + byte_size / 2 + 65536;
        return true;
    }
    const auto bytes_per_element =
        captured_image_bytes_per_element(image.unified_format);
    if (bytes_per_element == 0 ||
        image.width == 0 ||
        image.height == 0) {
        return false;
    }

    if (image.tile_mode == 0) {
        const auto pitch = std::max(image.pitch, image.width);
        if (pitch >
                std::numeric_limits<std::uint64_t>::max() /
                    image.height ||
            static_cast<std::uint64_t>(pitch) * image.height >
                std::numeric_limits<std::uint64_t>::max() /
                    bytes_per_element) {
            return false;
        }
        byte_size =
            static_cast<std::uint64_t>(pitch) *
            image.height *
            bytes_per_element;
        return byte_size != 0;
    }

    std::uint32_t block_bytes = 0;
    if (image.tile_mode >= 1 && image.tile_mode <= 3) {
        block_bytes = 256;
    } else if ((image.tile_mode >= 4 && image.tile_mode <= 7) ||
               (image.tile_mode >= 20 && image.tile_mode <= 23)) {
        block_bytes = 4096;
    } else if ((image.tile_mode >= 8 && image.tile_mode <= 11) ||
               (image.tile_mode >= 16 && image.tile_mode <= 19) ||
               (image.tile_mode >= 24 && image.tile_mode <= 27)) {
        block_bytes = 65536;
    } else {
        return false;
    }
    if ((bytes_per_element & (bytes_per_element - 1)) != 0 ||
        block_bytes < bytes_per_element) {
        return false;
    }

    const auto block_elements = block_bytes / bytes_per_element;
    const auto total_bits =
        std::countr_zero(static_cast<std::uint32_t>(block_elements));
    const auto width_bits = (total_bits + 1) / 2;
    const auto height_bits = total_bits - width_bits;
    const auto block_width = 1ULL << width_bits;
    const auto block_height = 1ULL << height_bits;
    const auto blocks_wide =
        (static_cast<std::uint64_t>(image.width) +
         block_width - 1) /
        block_width;
    const auto blocks_high =
        (static_cast<std::uint64_t>(image.height) +
         block_height - 1) /
        block_height;
    if (blocks_wide >
            std::numeric_limits<std::uint64_t>::max() / blocks_high ||
        blocks_wide * blocks_high >
            std::numeric_limits<std::uint64_t>::max() / block_bytes) {
        return false;
    }
    byte_size = blocks_wide * blocks_high * block_bytes;
    return byte_size != 0;
}

// Record the buffers and images a state samples, so a replay of the capture
// sees the same texture contents the live run did. This used to bail out for
// every state but 11, which left the rest of the frame's textures out of the
// capture entirely: replay read them back as zeroes and drew from blank
// images, and the resulting black frame looked like a rendering bug rather
// than a missing recording. Capture every state; the page ceiling in
// capture_memory_range_locked is what bounds the file, and it now says so
// when it truncates.
void capture_state_resources_locked(
    NativeGpuRuntime& runtime,
    std::uint32_t state_id) {
    const auto pipeline =
        runtime.vulkan.guest_descriptor_pipelines.find(state_id);
    if (pipeline ==
        runtime.vulkan.guest_descriptor_pipelines.end()) {
        runtime_trace(
            "native_gpu.capture_image_manifest_missing state=%u\n",
            state_id);
        return;
    }

    for (const auto& buffer : pipeline->second.buffers) {
        if (buffer.guest_address < 0x10000 ||
            buffer.size == 0) {
            continue;
        }
        const auto pages_before = runtime.captured_memory.size();
        capture_memory_range_locked(
            runtime,
            buffer.guest_address,
            buffer.size,
            PS5GPU_CAPTURE_MEMORY_BUFFER_RESOURCE);
        const auto pages_after = runtime.captured_memory.size();
        runtime_trace(
            "native_gpu.capture_buffer_resource "
            "state=%u descriptor=%u address=0x%016llX "
            "bytes=%llu pages=%llu total_pages=%llu\n",
            state_id,
            buffer.descriptor_index,
            static_cast<unsigned long long>(
                buffer.guest_address),
            static_cast<unsigned long long>(buffer.size),
            static_cast<unsigned long long>(
                pages_after - pages_before),
            static_cast<unsigned long long>(pages_after));
    }

    for (const auto& image : pipeline->second.images) {
        std::uint64_t byte_size = 0;
        if (!calculate_captured_image_bytes(image, byte_size)) {
            runtime_trace(
                "native_gpu.capture_image_unsupported "
                "state=%u binding=%u address=0x%016llX "
                "size=%ux%u unified=%u tile=%u\n",
                state_id,
                image.binding,
                static_cast<unsigned long long>(image.guest_address),
                image.width,
                image.height,
                image.unified_format,
                image.tile_mode);
            continue;
        }
        const auto pages_before = runtime.captured_memory.size();
        capture_memory_range_locked(
            runtime,
            image.guest_address,
            byte_size,
            PS5GPU_CAPTURE_MEMORY_IMAGE_RESOURCE);
        const auto pages_after = runtime.captured_memory.size();
        runtime_trace(
            "native_gpu.capture_image_resource "
            "state=%u binding=%u address=0x%016llX "
            "size=%ux%u pitch=%u unified=%u tile=%u "
            "bytes=%llu pages=%llu total_pages=%llu\n",
            state_id,
            image.binding,
            static_cast<unsigned long long>(image.guest_address),
            image.width,
            image.height,
            image.pitch,
            image.unified_format,
            image.tile_mode,
            static_cast<unsigned long long>(byte_size),
            static_cast<unsigned long long>(
                pages_after - pages_before),
            static_cast<unsigned long long>(pages_after));
    }
}

void capture_compute_resources_locked(
    NativeGpuRuntime& runtime,
    std::uint32_t state_id,
    const LoadedResourceManifest& manifest) {
    for (const auto& buffer : manifest.globals) {
        if (buffer.base_address < 0x10000) {
            continue;
        }
        const auto byte_size = buffer.data_size != 0
            ? std::max<std::uint64_t>(buffer.data_size, 4)
            : kCapturePageSize;
        const auto pages_before = runtime.captured_memory.size();
        capture_memory_range_locked(
            runtime,
            buffer.base_address,
            byte_size,
            PS5GPU_CAPTURE_MEMORY_BUFFER_RESOURCE);
        const auto pages_after = runtime.captured_memory.size();
        runtime_trace(
            "native_gpu.capture_compute_buffer "
            "state=%u descriptor=%u address=0x%016llX "
            "bytes=%llu pages=%llu total_pages=%llu\n",
            state_id,
            buffer.descriptor_index,
            static_cast<unsigned long long>(buffer.base_address),
            static_cast<unsigned long long>(byte_size),
            static_cast<unsigned long long>(
                pages_after - pages_before),
            static_cast<unsigned long long>(pages_after));
    }

    for (const auto& record : manifest.images) {
        VulkanBackend::GuestImageResource image;
        image.binding = record.binding;
        image.flags = record.flags;
        image.guest_address = record.base_address;
        image.width = record.width;
        image.height = record.height;
        image.unified_format =
            (record.resource_descriptor[1] >> 20) & 0x1FFu;
        image.tile_mode =
            (record.resource_descriptor[3] >> 20) & 0x1Fu;
        image.type =
            (record.resource_descriptor[3] >> 28) & 0xFu;
        image.pitch =
            image.type == 8 || image.type == 9 || image.type == 14
            ? (record.resource_descriptor[4] != 0
                ? (record.resource_descriptor[4] & 0x3FFFu) + 1
                : image.width)
            : image.width;
        std::uint64_t byte_size = 0;
        if (image.guest_address < 0x10000 ||
            !calculate_captured_image_bytes(image, byte_size)) {
            runtime_trace(
                "native_gpu.capture_compute_image_unsupported "
                "state=%u binding=%u address=0x%016llX "
                "size=%ux%u unified=%u tile=%u\n",
                state_id,
                image.binding,
                static_cast<unsigned long long>(
                    image.guest_address),
                image.width,
                image.height,
                image.unified_format,
                image.tile_mode);
            continue;
        }
        const auto pages_before = runtime.captured_memory.size();
        capture_memory_range_locked(
            runtime,
            image.guest_address,
            byte_size,
            PS5GPU_CAPTURE_MEMORY_IMAGE_RESOURCE);
        const auto pages_after = runtime.captured_memory.size();
        runtime_trace(
            "native_gpu.capture_compute_image "
            "state=%u binding=%u address=0x%016llX "
            "size=%ux%u pitch=%u unified=%u tile=%u "
            "storage=%u bytes=%llu pages=%llu total_pages=%llu\n",
            state_id,
            image.binding,
            static_cast<unsigned long long>(image.guest_address),
            image.width,
            image.height,
            image.pitch,
            image.unified_format,
            image.tile_mode,
            (image.flags & PS5GPU_RESOURCE_IMAGE_STORAGE) != 0,
            static_cast<unsigned long long>(byte_size),
            static_cast<unsigned long long>(
                pages_after - pages_before),
            static_cast<unsigned long long>(pages_after));
    }
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
    const NativeGpuRuntime::CapturedState& state,
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

bool decode_first_pixel_image(
    NativeGpuRuntime& runtime,
    std::uint32_t state_id,
    std::uint64_t& address,
    std::uint32_t& width,
    std::uint32_t& height) {
    address = 0;
    width = 0;
    height = 0;
    if (state_id == 0) {
        return false;
    }
    std::lock_guard capture_lock(runtime.capture_mutex);
    const auto state = runtime.captured_states.find(state_id);
    if (state == runtime.captured_states.end()) {
        return false;
    }
    const auto base =
        state->second.record.pixel_user_data_base_register;
    if (shader_user_data_count(state->second, base) < 4) {
        return false;
    }
    const auto word0 = find_register_value(
        state->second.sh_registers,
        base);
    const auto word1 = find_register_value(
        state->second.sh_registers,
        base + 1);
    const auto word2 = find_register_value(
        state->second.sh_registers,
        base + 2);
    const auto word3 = find_register_value(
        state->second.sh_registers,
        base + 3);

    address =
        ((static_cast<std::uint64_t>(word1 & 0xFFu) << 32) |
         word0) << 8;
    width =
        (((word1 >> 30) & 0x3u) |
         ((word2 & 0x3FFFu) << 2)) + 1;
    height = ((word2 >> 14) & 0xFFFFu) + 1;
    const auto unified_format = (word1 >> 20) & 0x1FFu;
    const auto type = (word3 >> 28) & 0xFu;
    return address >= 0x10000 &&
        width != 0 &&
        height != 0 &&
        unified_format != 0 &&
        !(type >= 1 && type <= 7);
}

bool decode_astro_post_buffers(
    NativeGpuRuntime& runtime,
    std::uint32_t state_id,
    std::array<
        std::uint64_t,
        kAstroPostStorageBufferCount>& addresses) {
    addresses.fill(0);
    if (state_id == 0) {
        return false;
    }
    std::lock_guard capture_lock(runtime.capture_mutex);
    const auto state = runtime.captured_states.find(state_id);
    if (state == runtime.captured_states.end()) {
        return false;
    }

    const auto es_base =
        state->second.record.export_user_data_base_register;
    const auto ps_base =
        state->second.record.pixel_user_data_base_register;
    if (shader_user_data_count(state->second, es_base) < 4 ||
        shader_user_data_count(state->second, ps_base) < 16) {
        return false;
    }
    const auto es_table =
        static_cast<std::uint64_t>(find_register_value(
            state->second.sh_registers,
            es_base)) |
        (static_cast<std::uint64_t>(find_register_value(
             state->second.sh_registers,
             es_base + 1))
         << 32);
    const auto control =
        static_cast<std::uint64_t>(find_register_value(
            state->second.sh_registers,
            es_base + 2)) |
        (static_cast<std::uint64_t>(find_register_value(
             state->second.sh_registers,
             es_base + 3))
         << 32);

    std::array<std::uint32_t, 4> vertex_descriptor = {};
    if (es_table < 0x10000 ||
        control < 0x10000 ||
        !read_current_process_memory(
            es_table,
            vertex_descriptor.data(),
            sizeof(vertex_descriptor),
            nullptr)) {
        return false;
    }
    const auto vertex_address =
        static_cast<std::uint64_t>(vertex_descriptor[0]) |
        (static_cast<std::uint64_t>(
             vertex_descriptor[1] & 0xFFFFu)
         << 32);

    const auto ps_word0 = find_register_value(
        state->second.sh_registers,
        ps_base + 12);
    const auto ps_word1 = find_register_value(
        state->second.sh_registers,
        ps_base + 13);
    const auto ps_buffer =
        static_cast<std::uint64_t>(ps_word0) |
        (static_cast<std::uint64_t>(ps_word1 & 0xFFFFu) << 32);
    if (vertex_address < 0x10000 || ps_buffer < 0x10000) {
        return false;
    }
    addresses = {
        control,
        es_table,
        vertex_address,
        ps_buffer,
    };
    return true;
}

bool decode_astro_state29_buffer(
    NativeGpuRuntime& runtime,
    std::uint32_t state_id,
    std::uint64_t& control_address,
    std::uint64_t& table_address,
    std::uint64_t& address,
    std::uint32_t& byte_size) {
    control_address = 0;
    table_address = 0;
    address = 0;
    byte_size = 0;
    if (state_id == 0) {
        return false;
    }
    std::lock_guard capture_lock(runtime.capture_mutex);
    const auto state = runtime.captured_states.find(state_id);
    if (state == runtime.captured_states.end()) {
        return false;
    }
    const auto base =
        state->second.record.export_user_data_base_register;
    if (shader_user_data_count(state->second, base) < 4) {
        return false;
    }
    table_address =
        static_cast<std::uint64_t>(find_register_value(
            state->second.sh_registers,
            base)) |
        (static_cast<std::uint64_t>(find_register_value(
             state->second.sh_registers,
             base + 1))
         << 32);
    control_address =
        static_cast<std::uint64_t>(find_register_value(
            state->second.sh_registers,
            base + 2)) |
        (static_cast<std::uint64_t>(find_register_value(
             state->second.sh_registers,
             base + 3))
         << 32);
    if (table_address < 0x10000 ||
        control_address < 0x10000) {
        table_address = 0;
        control_address = 0;
        return false;
    }

    std::array<std::uint32_t, 4> descriptor = {};
    if (!read_current_process_memory(
            table_address,
            descriptor.data(),
            sizeof(descriptor),
            nullptr)) {
        return false;
    }
    address =
        static_cast<std::uint64_t>(descriptor[0]) |
        (static_cast<std::uint64_t>(descriptor[1] & 0xFFFFu)
         << 32);
    const auto stride = (descriptor[1] >> 16) & 0x3FFFu;
    const auto records = descriptor[2];
    const auto size =
        stride != 0
        ? static_cast<std::uint64_t>(stride) * records
        : static_cast<std::uint64_t>(records);
    if (address < 0x10000 ||
        size == 0 ||
        size > kAstroState29BufferBytes) {
        table_address = 0;
        control_address = 0;
        address = 0;
        return false;
    }
    byte_size = static_cast<std::uint32_t>(size);
    return true;
}

bool decode_astro_export_resources(
    NativeGpuRuntime& runtime,
    std::uint32_t state_id,
    std::uint64_t& constant_address,
    std::uint64_t& resource_table_address) {
    constant_address = 0;
    resource_table_address = 0;
    if (state_id == 0) {
        return false;
    }
    std::lock_guard capture_lock(runtime.capture_mutex);
    const auto state = runtime.captured_states.find(state_id);
    if (state == runtime.captured_states.end()) {
        return false;
    }
    const auto base =
        state->second.record.export_user_data_base_register;
    if (shader_user_data_count(state->second, base) < 6) {
        return false;
    }
    const auto descriptor_word0 = find_register_value(
        state->second.sh_registers,
        base);
    const auto descriptor_word1 = find_register_value(
        state->second.sh_registers,
        base + 1);
    constant_address =
        static_cast<std::uint64_t>(descriptor_word0) |
        (static_cast<std::uint64_t>(descriptor_word1 & 0xFFFFu) << 32);
    resource_table_address =
        static_cast<std::uint64_t>(find_register_value(
            state->second.sh_registers,
            base + 4)) |
        (static_cast<std::uint64_t>(find_register_value(
             state->second.sh_registers,
             base + 5))
         << 32);
    return constant_address >= 0x10000 &&
        resource_table_address >= 0x10000;
}

void capture_shader_metadata_locked(
    NativeGpuRuntime& runtime,
    const NativeGpuRuntime::CapturedState& state,
    std::uint64_t header_address,
    std::uint32_t user_data_base_register) {
    std::uint16_t extended_user_data_size = 0;
    std::uint16_t shader_resource_table_size = 0;
    if (header_address != 0) {
        capture_memory_range_locked(
            runtime,
            header_address,
            0x60,
            PS5GPU_CAPTURE_MEMORY_SHADER_METADATA);

        std::uint64_t user_data_address = 0;
        if (read_u64(header_address + 0x08, user_data_address) &&
            user_data_address != 0) {
            capture_memory_range_locked(
                runtime,
                user_data_address,
                0x38,
                PS5GPU_CAPTURE_MEMORY_SHADER_METADATA);
            std::uint16_t direct_resource_count = 0;
            (void)read_u16(
                user_data_address + 0x28,
                extended_user_data_size);
            (void)read_u16(
                user_data_address + 0x2A,
                shader_resource_table_size);
            (void)read_u16(
                user_data_address + 0x2C,
                direct_resource_count);

            std::uint64_t direct_resource_offsets = 0;
            if (read_u64(
                    user_data_address,
                    direct_resource_offsets) &&
                direct_resource_count != 0) {
                capture_memory_range_locked(
                    runtime,
                    direct_resource_offsets,
                    static_cast<std::uint64_t>(
                        direct_resource_count) *
                        sizeof(std::uint16_t),
                    PS5GPU_CAPTURE_MEMORY_SHADER_METADATA);
            }
            for (std::uint32_t resource_class = 0;
                 resource_class < 4;
                 ++resource_class) {
                std::uint64_t offsets_address = 0;
                std::uint16_t resource_count = 0;
                if (read_u64(
                        user_data_address + 0x08 +
                            resource_class *
                                sizeof(std::uint64_t),
                        offsets_address) &&
                    read_u16(
                        user_data_address + 0x2E +
                            resource_class *
                                sizeof(std::uint16_t),
                        resource_count) &&
                    offsets_address != 0 &&
                    resource_count != 0) {
                    capture_memory_range_locked(
                        runtime,
                        offsets_address,
                        static_cast<std::uint64_t>(
                            resource_count) *
                            sizeof(std::uint16_t),
                        PS5GPU_CAPTURE_MEMORY_SHADER_METADATA);
                }
            }
        }
    }

    const auto user_data_count =
        shader_user_data_count(state, user_data_base_register);
    std::vector<std::uint32_t> user_data(user_data_count);
    for (std::uint32_t index = 0; index < user_data_count; ++index) {
        user_data[index] = find_register_value(
            state.sh_registers,
            user_data_base_register + index);
    }
    const auto resource_bytes = std::clamp<std::uint64_t>(
        static_cast<std::uint64_t>(
            shader_resource_table_size + extended_user_data_size) *
            sizeof(std::uint32_t),
        kCapturePageSize,
        kMaximumShaderResourceBytes);
    for (std::size_t index = 0;
         index + 1 < user_data.size();
         ++index) {
        const auto pointer =
            static_cast<std::uint64_t>(user_data[index]) |
            (static_cast<std::uint64_t>(user_data[index + 1]) << 32);
        capture_memory_range_locked(
            runtime,
            pointer,
            resource_bytes,
            PS5GPU_CAPTURE_MEMORY_RESOURCE_TABLE);
    }
}

const char* shader_stage_name(std::uint32_t stage) {
    return stage == PS5GPU_CAPTURE_SHADER_STAGE_EXPORT ? "es" : "ps";
}

NativeGpuRuntime::CapturedShader* capture_guest_shader(
    NativeGpuRuntime& runtime,
    std::uint32_t stage,
    std::uint64_t address) {
    if (address == 0) {
        return nullptr;
    }
    std::lock_guard capture_lock(runtime.capture_mutex);
    const auto key = std::make_pair(stage, address);
    if (const auto existing = runtime.captured_shaders.find(key);
        existing != runtime.captured_shaders.end()) {
        return &existing->second;
    }

    auto decoded = ps5gpu::gen5::decode_program(
        address,
        read_current_process_memory,
        nullptr);
    NativeGpuRuntime::CapturedShader captured;
    captured.record.address = address;
    captured.record.hash = ps5gpu::gen5::hash_words(decoded.words);
    captured.record.stage = stage;
    captured.record.flags =
        (decoded.terminated ? PS5GPU_CAPTURE_SHADER_TERMINATED : 0u) |
        (decoded.read_failed ? PS5GPU_CAPTURE_SHADER_READ_FAILED : 0u) |
        (decoded.decode_failed
            ? PS5GPU_CAPTURE_SHADER_DECODE_FAILED
            : 0u);
    captured.record.byte_size = static_cast<std::uint32_t>(
        decoded.words.size() * sizeof(std::uint32_t));
    // A shader's data can live just past its last instruction - a vertex
    // table it finds with s_getpc_b64 - and the record ends at s_endpgm.
    // Without the pages around it a replayed draw reads zeroes for its
    // vertices and draws nothing.
    capture_memory_range_locked(
        runtime,
        address,
        captured.record.byte_size + 4096,
        PS5GPU_CAPTURE_MEMORY_BUFFER_RESOURCE);
    captured.record.instruction_count = decoded.instruction_count;
    captured.record.failure_pc = decoded.failure_pc;
    captured.record.failure_word = decoded.failure_word;
    captured.words = std::move(decoded.words);
    captured.encoding_counts = decoded.encoding_counts;
    for (std::size_t index = 0;
         index < captured.encoding_counts.size();
         ++index) {
        if (captured.encoding_counts[index] != 0) {
            captured.record.encoding_mask |=
                1u << static_cast<std::uint32_t>(index);
        }
    }

    char encodings[512] = {};
    std::size_t used = 0;
    for (std::size_t index = 0;
         index < captured.encoding_counts.size();
         ++index) {
        const auto count = captured.encoding_counts[index];
        if (count == 0 || used >= sizeof(encodings) - 1) {
            continue;
        }
        const auto written = std::snprintf(
            encodings + used,
            sizeof(encodings) - used,
            "%s%s:%u",
            used == 0 ? "" : ",",
            ps5gpu::gen5::kEncodingNames[index],
            count);
        if (written <= 0) {
            break;
        }
        used += std::min<std::size_t>(
            static_cast<std::size_t>(written),
            sizeof(encodings) - used - 1);
    }

    runtime_trace(
        "native_gpu.shader_preflight stage=%s addr=0x%016llX "
        "bytes=%u instructions=%u hash=0x%016llX terminated=%u "
        "read_failed=%u decode_failed=%u failure_pc=0x%X "
        "failure_word=0x%08X encodings=%s\n",
        shader_stage_name(stage),
        static_cast<unsigned long long>(address),
        captured.record.byte_size,
        captured.record.instruction_count,
        static_cast<unsigned long long>(captured.record.hash),
        (captured.record.flags &
            PS5GPU_CAPTURE_SHADER_TERMINATED) != 0,
        (captured.record.flags &
            PS5GPU_CAPTURE_SHADER_READ_FAILED) != 0,
        (captured.record.flags &
            PS5GPU_CAPTURE_SHADER_DECODE_FAILED) != 0,
        captured.record.failure_pc,
        captured.record.failure_word,
        encodings[0] == '\0' ? "none" : encodings);
    const auto [inserted, success] =
        runtime.captured_shaders.emplace(key, std::move(captured));
    return success ? &inserted->second : nullptr;
}

bool is_astro_title_clear_pixel_shader(
    const NativeGpuRuntime::CapturedShader* shader) {
    static constexpr std::array<std::uint32_t, 4> words = {
        0x7E000280u,
        0xF8001803u,
        0x00000000u,
        0xBF810000u,
    };
    return shader != nullptr &&
        shader->record.hash == 0x8FC39BF649AAC262ULL &&
        shader->words.size() == words.size() &&
        std::equal(
            shader->words.begin(),
            shader->words.end(),
            words.begin());
}

void write_gpu_capture(
    NativeGpuRuntime& runtime,
    const Ps5GpuNativeFlip& flip) {
    std::lock_guard capture_lock(runtime.capture_mutex);
    if (runtime.capture_written || runtime.capture_path.empty()) {
        return;
    }
    static const auto target_flip = []() {
        const auto* value =
            std::getenv("PS5GPU_NATIVE_CAPTURE_FLIP");
        if (value == nullptr || value[0] == '\0') {
            return std::uint64_t{0};
        }
        char* end = nullptr;
        const auto parsed = std::strtoull(value, &end, 0);
        return end != value &&
                (end == nullptr || end[0] == '\0')
            ? static_cast<std::uint64_t>(parsed)
            : std::uint64_t{0};
    }();
    if (target_flip != 0 && flip.flip_id < target_flip) {
        return;
    }
    // Or the frame that draws with a given pixel shader - after skipping
    // the first few that do: PS5GPU_NATIVE_CAPTURE_PS and _PS_SKIP. Flip
    // numbers wander from run to run, and the intro video is on screen for
    // a few hundred of them somewhere in the first few thousand.
    static const auto capture_ps = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_CAPTURE_PS");
        return value == nullptr ? std::uint64_t{0}
                                : std::strtoull(value, nullptr, 0);
    }();
    if (capture_ps != 0) {
        static const auto skip = [] {
            const auto* value = std::getenv("PS5GPU_NATIVE_CAPTURE_PS_SKIP");
            return value == nullptr ? 0u
                : static_cast<std::uint32_t>(std::strtoul(value, nullptr, 0));
        }();
        static std::uint32_t seen = 0;
        const auto has = std::any_of(
            runtime.frame_draws.begin(),
            runtime.frame_draws.end(),
            [](const Ps5GpuNativeDraw& draw) {
                return draw.ps_address == capture_ps;
            });
        if (!has || ++seen <= skip) {
            return;
        }
    }
    // The resources of every state this frame drew with, taken now. They
    // are taken at registration too, but a state compiled during the run
    // has no pipeline until its first draw, so registration found nothing
    // to take - and replay drew those states from zeroes: no matrices, no
    // video texture, no geometry.
    {
        std::set<std::uint32_t> states;
        for (const auto& draw : runtime.frame_draws) {
            if (draw.reserved0 != 0) {
                states.insert(draw.reserved0);
            }
        }
        for (const auto state : states) {
            capture_state_resources_locked(runtime, state);
        }
    }
    std::error_code directory_error;
    const auto parent = runtime.capture_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, directory_error);
    }
    // Write beside the real path and rename only once the whole file is on
    // disk. A capture is a few hundred megabytes and the probe is killed the
    // moment the flip it was waiting for arrives, so the write loses that race
    // often enough to matter. Truncated captures used to keep the real name,
    // and replay met them with "Invalid GPU memory capture record" - an error
    // about the wrong thing entirely, several steps away from the cause.
    const auto pending_path =
        std::filesystem::path(runtime.capture_path).concat(".partial");
    std::ofstream output(pending_path, std::ios::binary);
    if (!output) {
        runtime_trace(
            "native_gpu.capture_open_failed path=%s\n",
            pending_path.string().c_str());
        return;
    }
    Ps5GpuCaptureHeaderV4 header = {};
    header.base.magic = PS5GPU_CAPTURE_MAGIC;
    header.base.version = PS5GPU_CAPTURE_VERSION;
    header.base.header_size = sizeof(header);
    header.base.draw_size = sizeof(Ps5GpuNativeDraw);
    header.base.flip_size = sizeof(Ps5GpuNativeFlip);
    header.base.draw_count = runtime.frame_draws.size();
    header.base.shader_record_size = sizeof(Ps5GpuCaptureShader);
    header.base.shader_count = static_cast<std::uint32_t>(
        runtime.captured_shaders.size());
    header.base.state_record_size = sizeof(Ps5GpuCaptureShaderState);
    header.base.state_count = static_cast<std::uint32_t>(
        runtime.captured_states.size());
    header.base.memory_record_size = sizeof(Ps5GpuCaptureMemory);
    header.base.memory_count = static_cast<std::uint32_t>(
        runtime.captured_memory.size());
    header.compute_state_record_size =
        sizeof(Ps5GpuCaptureComputeState);
    header.compute_state_count = static_cast<std::uint32_t>(
        runtime.captured_compute_states.size());
    header.command_record_size = sizeof(Ps5GpuCaptureCommand);
    header.command_count = static_cast<std::uint32_t>(
        runtime.frame_commands.size());
    header.command_bytes =
        runtime.frame_commands.size() * sizeof(Ps5GpuCaptureCommand);
    for (const auto& [key, shader] : runtime.captured_shaders) {
        (void)key;
        header.base.shader_bytes += shader.record.byte_size;
    }
    for (const auto& [state_id, state] : runtime.captured_states) {
        (void)state_id;
        header.base.state_register_bytes +=
            (state.sh_registers.size() + state.cx_registers.size()) *
            sizeof(Ps5GpuNativeRegisterValue);
    }
    for (const auto& [address, memory] : runtime.captured_memory) {
        (void)address;
        header.base.memory_bytes += memory.bytes.size();
    }
    for (const auto& [state_id, state] :
         runtime.captured_compute_states) {
        (void)state_id;
        header.compute_state_payload_bytes +=
            state.spirv.size() + state.resource_manifest.size();
    }
    output.write(
        reinterpret_cast<const char*>(&header),
        sizeof(header));
    for (const auto& [key, shader] : runtime.captured_shaders) {
        (void)key;
        output.write(
            reinterpret_cast<const char*>(&shader.record),
            sizeof(shader.record));
        if (!shader.words.empty()) {
            output.write(
                reinterpret_cast<const char*>(shader.words.data()),
                static_cast<std::streamsize>(shader.record.byte_size));
        }
    }
    for (const auto& [state_id, state] : runtime.captured_states) {
        (void)state_id;
        output.write(
            reinterpret_cast<const char*>(&state.record),
            sizeof(state.record));
        if (!state.sh_registers.empty()) {
            output.write(
                reinterpret_cast<const char*>(state.sh_registers.data()),
                static_cast<std::streamsize>(
                    state.sh_registers.size() *
                    sizeof(Ps5GpuNativeRegisterValue)));
        }
        if (!state.cx_registers.empty()) {
            output.write(
                reinterpret_cast<const char*>(state.cx_registers.data()),
                static_cast<std::streamsize>(
                    state.cx_registers.size() *
                    sizeof(Ps5GpuNativeRegisterValue)));
        }
    }
    for (const auto& [address, memory] : runtime.captured_memory) {
        (void)address;
        output.write(
            reinterpret_cast<const char*>(&memory.record),
            sizeof(memory.record));
        if (!memory.bytes.empty()) {
            output.write(
                reinterpret_cast<const char*>(memory.bytes.data()),
                static_cast<std::streamsize>(memory.bytes.size()));
        }
    }
    if (!runtime.frame_draws.empty()) {
        output.write(
            reinterpret_cast<const char*>(runtime.frame_draws.data()),
            static_cast<std::streamsize>(
                runtime.frame_draws.size() *
                sizeof(Ps5GpuNativeDraw)));
    }
    for (const auto& [state_id, state] :
         runtime.captured_compute_states) {
        (void)state_id;
        output.write(
            reinterpret_cast<const char*>(&state.record),
            sizeof(state.record));
        if (!state.spirv.empty()) {
            output.write(
                reinterpret_cast<const char*>(state.spirv.data()),
                static_cast<std::streamsize>(state.spirv.size()));
        }
        if (!state.resource_manifest.empty()) {
            output.write(
                reinterpret_cast<const char*>(
                    state.resource_manifest.data()),
                static_cast<std::streamsize>(
                    state.resource_manifest.size()));
        }
    }
    if (!runtime.frame_commands.empty()) {
        output.write(
            reinterpret_cast<const char*>(
                runtime.frame_commands.data()),
            static_cast<std::streamsize>(header.command_bytes));
    }
    output.write(reinterpret_cast<const char*>(&flip), sizeof(flip));
    // The modules of the captured graphics states, as a trailer.
    {
        std::vector<std::pair<std::uint32_t, CaptureGraphicsPayload>> kept;
        {
            std::lock_guard payload_guard(g_capture_graphics_payloads_mutex);
            for (const auto& [state_id, state] : runtime.captured_states) {
                (void)state;
                const auto found = g_capture_graphics_payloads.find(state_id);
                if (found != g_capture_graphics_payloads.end() &&
                    found->second.es_spirv != nullptr &&
                    found->second.ps_spirv != nullptr) {
                    kept.emplace_back(state_id, found->second);
                }
            }
        }
        const std::uint32_t magic = PS5GPU_CAPTURE_GRAPHICS_PAYLOAD_MAGIC;
        const auto count = static_cast<std::uint32_t>(kept.size());
        output.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        output.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const auto& [state_id, payload] : kept) {
            Ps5GpuCaptureGraphicsPayload record = {};
            record.state_id = state_id;
            record.es_spirv_size = static_cast<std::uint32_t>(
                payload.es_spirv->size() * sizeof(std::uint32_t));
            record.es_manifest_size =
                static_cast<std::uint32_t>(payload.es_manifest.size());
            record.ps_spirv_size = static_cast<std::uint32_t>(
                payload.ps_spirv->size() * sizeof(std::uint32_t));
            record.ps_manifest_size =
                static_cast<std::uint32_t>(payload.ps_manifest.size());
            output.write(
                reinterpret_cast<const char*>(&record), sizeof(record));
            output.write(
                reinterpret_cast<const char*>(payload.es_spirv->data()),
                record.es_spirv_size);
            output.write(
                reinterpret_cast<const char*>(payload.es_manifest.data()),
                record.es_manifest_size);
            output.write(
                reinterpret_cast<const char*>(payload.ps_spirv->data()),
                record.ps_spirv_size);
            output.write(
                reinterpret_cast<const char*>(payload.ps_manifest.data()),
                record.ps_manifest_size);
        }
        runtime_trace(
            "native_gpu.capture_graphics_payloads states=%u\n", count);
    }
    output.flush();
    if (!output) {
        runtime_trace(
            "native_gpu.capture_write_failed path=%s\n",
            pending_path.string().c_str());
        return;
    }
    output.close();
    std::error_code rename_error;
    std::filesystem::rename(
        pending_path,
        runtime.capture_path,
        rename_error);
    if (rename_error) {
        runtime_trace(
            "native_gpu.capture_rename_failed path=%s error=%d\n",
            pending_path.string().c_str(),
            rename_error.value());
        return;
    }
    runtime.capture_written = true;
    // The capture records a graphics state's registers but not its modules,
    // and replay used to find those by state id in the live shader cache -
    // where, by the time it ran, a later run had put different shaders
    // under the same numbers. The cache as this run left it goes beside
    // the capture, for replay to point at.
    if (const auto* live = std::getenv("PS5GPU_NATIVE_LIVE_SHADER_CACHE_DIR");
        live != nullptr && live[0] != '\0') {
        auto snapshot = runtime.capture_path;
        snapshot += ".shaders";
        std::error_code copy_error;
        std::filesystem::remove_all(snapshot, copy_error);
        std::filesystem::create_directories(snapshot, copy_error);
        std::filesystem::copy(
            live,
            snapshot,
            std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::overwrite_existing,
            copy_error);
        runtime_trace(
            "native_gpu.capture_shaders path=%s error=%d\n",
            snapshot.string().c_str(),
            copy_error.value());
    }
    runtime_trace(
        "native_gpu.capture_written path=%s draws=%llu shaders=%u "
        "states=%u memory_pages=%u compute_states=%u commands=%u "
        "shader_bytes=%llu state_register_bytes=%llu "
        "memory_bytes=%llu compute_state_bytes=%llu bytes=%llu\n",
        runtime.capture_path.string().c_str(),
        static_cast<unsigned long long>(runtime.frame_draws.size()),
        header.base.shader_count,
        header.base.state_count,
        header.base.memory_count,
        header.compute_state_count,
        header.command_count,
        static_cast<unsigned long long>(header.base.shader_bytes),
        static_cast<unsigned long long>(
            header.base.state_register_bytes),
        static_cast<unsigned long long>(header.base.memory_bytes),
        static_cast<unsigned long long>(
            header.compute_state_payload_bytes),
        static_cast<unsigned long long>(
            sizeof(header) +
            runtime.captured_shaders.size() *
                sizeof(Ps5GpuCaptureShader) +
            header.base.shader_bytes +
            runtime.captured_states.size() *
                sizeof(Ps5GpuCaptureShaderState) +
            header.base.state_register_bytes +
            runtime.captured_memory.size() *
                sizeof(Ps5GpuCaptureMemory) +
            header.base.memory_bytes +
            runtime.frame_draws.size() * sizeof(Ps5GpuNativeDraw) +
            runtime.captured_compute_states.size() *
                sizeof(Ps5GpuCaptureComputeState) +
            header.compute_state_payload_bytes +
            header.command_bytes +
            sizeof(Ps5GpuNativeFlip)));
}

Ps5GpuNativeFlip make_capture_flip_from_draw(
    const Ps5GpuNativeDraw& draw) {
    Ps5GpuNativeFlip flip = {};
    flip.struct_size = sizeof(flip);
    flip.abi_version = PS5GPU_NATIVE_ABI_VERSION;
    flip.display_address = draw.render_target_address;
    flip.width = draw.render_target_width;
    flip.height = draw.render_target_height;
    flip.pitch_in_pixels = draw.render_target_width;
    flip.tiling_mode = draw.render_target_tile_mode;
    return flip;
}

void process_draw(NativeGpuRuntime& runtime, const Ps5GpuNativeDraw& draw) {
    (void)capture_guest_shader(
        runtime,
        PS5GPU_CAPTURE_SHADER_STAGE_EXPORT,
        draw.es_address);
    auto* pixel_shader = capture_guest_shader(
        runtime,
        PS5GPU_CAPTURE_SHADER_STAGE_PIXEL,
        draw.ps_address);
    auto ir = make_gpu_ir_draw(draw);
    if (pixel_shader != nullptr) {
        ir.ps_hash = pixel_shader->record.hash;
        ir.solid_white_pixel_shader =
            is_astro_title_clear_pixel_shader(pixel_shader);
        for (std::size_t copy_index = 0;
             copy_index < kAstroCopyPasses.size();
             ++copy_index) {
            const auto& config = kAstroCopyPasses[copy_index];
            if (ir.es_address != config.es_address ||
                ir.ps_address != config.ps_address ||
                ir.render_target_address != config.target_address) {
                continue;
            }
            ir.astro_copy_pass_index =
                static_cast<std::uint32_t>(copy_index);
            ir.astro_copy_process =
                decode_first_pixel_image(
                    runtime,
                    ir.shader_state_id,
                    ir.sampled_address,
                    ir.sampled_width,
                    ir.sampled_height);
            ir.sample_render_surface = ir.astro_copy_process;
            runtime_trace(
                "native_gpu.astro_copy_source state=%u pass=%u "
                "source=0x%016llX/%ux%u "
                "target=0x%016llX ready=%u\n",
                ir.shader_state_id,
                config.shader_state_file,
                static_cast<unsigned long long>(
                    ir.sampled_address),
                ir.sampled_width,
                ir.sampled_height,
                static_cast<unsigned long long>(
                    ir.render_target_address),
                ir.astro_copy_process ? 1u : 0u);
            break;
        }
        for (std::size_t pass_index = 0;
             pass_index < kAstroPostPasses.size();
             ++pass_index) {
            const auto& config = kAstroPostPasses[pass_index];
            if (ir.es_address != config.es_address ||
                ir.ps_address != config.ps_address ||
                ir.render_target_address != config.target_address) {
                continue;
            }
            ir.astro_post_pass_index =
                static_cast<std::uint32_t>(pass_index);
            ir.astro_post_process =
                decode_astro_post_buffers(
                    runtime,
                    ir.shader_state_id,
                    ir.astro_post_buffer_addresses) &&
                decode_first_pixel_image(
                    runtime,
                    ir.shader_state_id,
                    ir.sampled_address,
                    ir.sampled_width,
                    ir.sampled_height);
            ir.sample_render_surface = ir.astro_post_process;
            runtime_trace(
                "native_gpu.astro_post_source state=%u pass=%u "
                "source=0x%016llX/%ux%u "
                "target=0x%016llX/%ux%u "
                "buffers=0x%016llX,0x%016llX,"
                "0x%016llX,0x%016llX ready=%u\n",
                ir.shader_state_id,
                config.shader_state_file,
                static_cast<unsigned long long>(
                    ir.sampled_address),
                ir.sampled_width,
                ir.sampled_height,
                static_cast<unsigned long long>(
                    ir.render_target_address),
                ir.render_target_width,
                ir.render_target_height,
                static_cast<unsigned long long>(
                    ir.astro_post_buffer_addresses[0]),
                static_cast<unsigned long long>(
                    ir.astro_post_buffer_addresses[1]),
                static_cast<unsigned long long>(
                    ir.astro_post_buffer_addresses[2]),
                static_cast<unsigned long long>(
                    ir.astro_post_buffer_addresses[3]),
                ir.astro_post_process ? 1u : 0u);
            break;
        }
        if (ir.es_address == kAstroState29ExportAddress &&
            ir.ps_address == kAstroState29PixelAddress) {
            ir.astro_state29 =
                decode_astro_state29_buffer(
                    runtime,
                    ir.shader_state_id,
                    ir.astro_state29_control_address,
                    ir.astro_state29_table_address,
                    ir.guest_buffer_address,
                    ir.guest_buffer_size) &&
                decode_first_pixel_image(
                    runtime,
                    ir.shader_state_id,
                    ir.sampled_address,
                    ir.sampled_width,
                    ir.sampled_height);
            ir.sample_render_surface = ir.astro_state29;
            runtime_trace(
                "native_gpu.astro_state29_source state=%u "
                "control=0x%016llX table=0x%016llX "
                "buffer=0x%016llX/%u "
                "source=0x%016llX size=%ux%u ready=%u\n",
                ir.shader_state_id,
                static_cast<unsigned long long>(
                    ir.astro_state29_control_address),
                static_cast<unsigned long long>(
                    ir.astro_state29_table_address),
                static_cast<unsigned long long>(
                    ir.guest_buffer_address),
                ir.guest_buffer_size,
                static_cast<unsigned long long>(
                    ir.sampled_address),
                ir.sampled_width,
                ir.sampled_height,
                ir.astro_state29 ? 1u : 0u);
        }
        if (ir.ps_hash == kAstroCompositionPixelHash) {
            if (decode_first_pixel_image(
                    runtime,
                    ir.shader_state_id,
                    ir.sampled_address,
                    ir.sampled_width,
                    ir.sampled_height)) {
                ir.sample_render_surface = true;
            }
            (void)decode_astro_export_resources(
                runtime,
                ir.shader_state_id,
                ir.export_constant_address,
                ir.export_resource_table_address);
            runtime_trace(
                "native_gpu.composition_source state=%u "
                "ps=0x%016llX source=0x%016llX size=%ux%u "
                "es_constant=0x%016llX es_table=0x%016llX\n",
                ir.shader_state_id,
                static_cast<unsigned long long>(ir.ps_address),
                static_cast<unsigned long long>(ir.sampled_address),
                ir.sampled_width,
                ir.sampled_height,
                static_cast<unsigned long long>(
                    ir.export_constant_address),
                static_cast<unsigned long long>(
                    ir.export_resource_table_address));
        }
    }
    runtime.vulkan.accept_draw(ir);
    runtime.frame_draws.push_back(draw);
    Ps5GpuCaptureCommand captured_command = {};
    captured_command.type = PS5GPU_CAPTURE_COMMAND_DRAW;
    captured_command.payload_size = sizeof(Ps5GpuNativeDraw);
    captured_command.payload.draw = draw;
    runtime.frame_commands.push_back(captured_command);
    // Writing the capture partway through a frame truncates it: the run
    // used to dump after 64 draws into the display buffer, which cut the last
    // seven draws of an Astro Bot frame - including the ones that put the UI
    // on screen - out of every capture, so replay could never reproduce them.
    // A flip is the honest boundary, and the normal path writes there. This
    // early dump only exists for runs that never reach a flip, so it is now
    // opt-in through PS5GPU_NATIVE_CAPTURE_DRAW_THRESHOLD and no longer cares
    // which target the draw names.
    const auto early_capture_draws = capture_draw_threshold();
    if (!runtime.capture_written &&
        !runtime.capture_path.empty() &&
        early_capture_draws != 0 &&
        runtime.frame_draws.size() >= early_capture_draws) {
        const auto capture_flip = make_capture_flip_from_draw(draw);
        runtime_trace(
            "native_gpu.capture_draw_threshold draws=%llu "
            "target=0x%016llX size=%ux%u\n",
            static_cast<unsigned long long>(runtime.frame_draws.size()),
            static_cast<unsigned long long>(draw.render_target_address),
            draw.render_target_width,
            draw.render_target_height);
        write_gpu_capture(runtime, capture_flip);
    }
    std::uint64_t processed = 0;
    {
        std::lock_guard lock(runtime.mutex);
        processed = ++runtime.draws_processed;
    }
    // Tracing every draw to find the one carrying the scene costs 144k lines
    // a run, and the cost is not just the file: it takes the wall clock the
    // title needs to finish streaming, so the run reaches more flips and less
    // of the game, and the geometry never appears at all. A draw with
    // thousands of vertices is the scene in any title, and there are a few
    // thousand of those against tens of thousands of fullscreen passes.
    constexpr std::uint32_t kLargeDrawVertices = 1024;
    if (environment_flag_enabled("PS5GPU_NATIVE_TRACE_ALL_DRAWS") ||
        draw.vertex_count >= kLargeDrawVertices ||
        processed == 1 || (processed % 8) == 0 ||
        draw.render_target_address == kAstroDisplayAddress ||
        draw.render_target_address == kAstroCompositionSourceAddress ||
        (draw.vertex_count == 4 && draw.primitive_type == 6)) {
        runtime_trace(
            "native_gpu.draw_processed count=%llu submission=%llu draw=%llu "
            "total=%llu state=%u vertices=%u first=%u indexed=%u prim=0x%X "
            "idx=0x%016llX idx_bytes=%u "
            "rt=0x%016llX size=%ux%u fmt=%u tile=%u "
            "viewport=%.1f,%.1f,%.1fx%.1f "
            "scissor=%d,%d,%ux%u\n",
            static_cast<unsigned long long>(processed),
            static_cast<unsigned long long>(draw.submission_id),
            static_cast<unsigned long long>(draw.draw_id),
            static_cast<unsigned long long>(draw.total_draw_id),
            ir.shader_state_id,
            draw.vertex_count,
            draw.first_vertex,
            (draw.flags & PS5GPU_NATIVE_DRAW_INDEXED) != 0,
            draw.primitive_type,
            static_cast<unsigned long long>(draw.index_address),
            draw.index_bytes,
            static_cast<unsigned long long>(draw.render_target_address),
            draw.render_target_width,
            draw.render_target_height,
            draw.render_target_format,
            draw.render_target_tile_mode,
            static_cast<double>(draw.viewport_x),
            static_cast<double>(draw.viewport_y),
            static_cast<double>(draw.viewport_width),
            static_cast<double>(draw.viewport_height),
            draw.scissor_x,
            draw.scissor_y,
            draw.scissor_width,
            draw.scissor_height);
    }
}

void process_compute(
    NativeGpuRuntime& runtime,
    const QueuedCommand& command) {
    Ps5GpuCaptureCommand captured_command = {};
    captured_command.type = PS5GPU_CAPTURE_COMMAND_COMPUTE;
    captured_command.payload_size = sizeof(Ps5GpuNativeComputeDispatch);
    captured_command.payload.compute = command.compute;
    runtime.frame_commands.push_back(captured_command);
    const auto backend_ok =
        command.compute_state != nullptr &&
        runtime.vulkan.submit_compute(
            *command.compute_state,
            command.compute);
    std::uint64_t processed = 0;
    {
        std::lock_guard lock(runtime.mutex);
        processed = ++runtime.compute_dispatches_processed;
        if (!backend_ok) {
            ++runtime.compute_dispatches_dropped;
        }
    }
    {
        static std::atomic<std::uint64_t> sampled_compute_processed{0};
        const auto seen = sampled_compute_processed.fetch_add(1, std::memory_order_relaxed);
        // Once per dispatch filled the log and cost the worker a
        // thirtieth of its time writing it: the first few, then a sample.
        if (seen < 256 || (seen % 1024) == 0) runtime_trace(
        "native_gpu.compute_processed count=%llu state=%u "
        "owner=%u submission=%llu dispatch=%llu groups=%u/%u/%u "
        "ok=%u\n",
        static_cast<unsigned long long>(processed),
        command.compute.compute_state_id,
        command.compute.owner_handle,
        static_cast<unsigned long long>(
            command.compute.submission_id),
        static_cast<unsigned long long>(
            command.compute.dispatch_id),
        command.compute.group_count_x,
        command.compute.group_count_y,
        command.compute.group_count_z,
        backend_ok ? 1u : 0u);
    }
}

void process_flip(NativeGpuRuntime& runtime, const Ps5GpuNativeFlip& flip) {
    std::uint64_t processed = 0;
    std::uint64_t draws_processed = 0;
    {
        std::lock_guard lock(runtime.mutex);
        processed = ++runtime.flips_processed;
        draws_processed = runtime.draws_processed;
    }
    // Not a present failure. submit_frame records, submits, waits and
    // reads the frame back whatever this says; what it returns is the
    // verdict at the end of it - no real composition draw ran and the
    // pixel read back is not the magenta sentinel. A run reports 21 of
    // these against 31 frames and every one of those frames went to the
    // GPU. The real= field of native_gpu.vulkan_frame is the same fact
    // under a name that means what it says. Reading this one as a
    // Vulkan error cost an afternoon.
    const bool backend_ok = runtime.vulkan.submit_frame(flip);
    {
        std::lock_guard lock(runtime.mutex);
        ++runtime.flips_finished;
    }
    runtime.flip_finished.notify_all();
    if (!backend_ok) {
        std::lock_guard lock(runtime.mutex);
        ++runtime.flips_dropped;
        runtime_trace(
            "native_gpu.vulkan_flip_failed flip=%llu\n",
            static_cast<unsigned long long>(flip.flip_id));
    }
    if (!runtime.capture_path.empty() && !runtime.capture_written) {
        Ps5GpuCaptureCommand captured_flip = {};
        captured_flip.type = PS5GPU_CAPTURE_COMMAND_FLIP;
        captured_flip.payload_size = sizeof(Ps5GpuNativeFlip);
        captured_flip.payload.flip = flip;
        runtime.frame_commands.push_back(captured_flip);
        runtime.captured_flips += 1;
    }
    // Hold the commands until the capture spans as many flips as asked for.
    // Replay drives everything from this stream, so a multi-frame capture is
    // simply a longer one.
    const auto wanted_flips = capture_flip_count();
    const auto capturing =
        !runtime.capture_path.empty() &&
        !runtime.capture_written &&
        runtime.captured_flips < wanted_flips;
    if (!capturing) {
        write_gpu_capture(runtime, flip);
        runtime.frame_draws.clear();
        runtime.frame_commands.clear();
    }
    {
        std::size_t retained_states = 0;
        std::uint64_t retained_manifest_bytes = 0;
        {
            std::lock_guard lock(runtime.compute_state_mutex);
            retained_states = runtime.compute_states.size();
            for (const auto& [id, held] : runtime.compute_states) {
                (void)id;
                if (held != nullptr) {
                    retained_manifest_bytes +=
                        held->resource_manifest.size() +
                        held->spirv.size() * sizeof(std::uint32_t);
                }
            }
        }
        const auto frequency = runtime_trace_frequency();
        // Take the picture of the address space while the leak is running,
        // once per gigabyte of growth, rather than after the process has
        // died of it.
        {
            static std::uint64_t last_mapped_at = 0;
            const auto private_now = worker_private_megabytes();
            if (private_now >= last_mapped_at + 1024) {
                last_mapped_at = private_now;
                trace_committed_memory_map();
            }
        }
        if (runtime.flips_processed % 50 == 0) {
            runtime_trace(
                "native_gpu.buffer_cache flip=%llu hits=%llu MB=%llu "
                "shared_hits=%llu shared_MB=%llu\n",
                static_cast<unsigned long long>(runtime.flips_processed),
                static_cast<unsigned long long>(g_buffer_cache_hits),
                static_cast<unsigned long long>(g_buffer_cache_bytes >> 20),
                static_cast<unsigned long long>(g_shared_buffer_hits),
                static_cast<unsigned long long>(g_shared_buffer_bytes >> 20));
        }
        if (!g_buffer_read_tally.empty() && runtime.flips_processed % 10 == 0) {
            std::vector<std::pair<std::uint64_t, BufferReadTally>> top(
                g_buffer_read_tally.begin(), g_buffer_read_tally.end());
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
                return a.second.bytes > b.second.bytes;
            });
            for (std::size_t index = 0; index < top.size() && index < 12; ++index) {
                runtime_trace(
                    "native_gpu.buffer_read_top flip=%llu address=0x%016llX "
                    "MB=%llu reads=%llu flags=0x%X pages=%llu/%llu\n",
                    static_cast<unsigned long long>(runtime.flips_processed),
                    static_cast<unsigned long long>(top[index].first),
                    static_cast<unsigned long long>(top[index].second.bytes >> 20),
                    static_cast<unsigned long long>(top[index].second.reads),
                    top[index].second.flags,
                    static_cast<unsigned long long>(
                        top[index].second.pages_changed),
                    static_cast<unsigned long long>(
                        top[index].second.pages_compared));
            }
        }
        if (!g_image_read_tally.empty() && runtime.flips_processed % 10 == 0) {
            std::vector<std::pair<std::uint64_t, ImageReadTally>> top(
                g_image_read_tally.begin(), g_image_read_tally.end());
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
                return a.second.bytes > b.second.bytes;
            });
            for (std::size_t index = 0; index < top.size() && index < 12; ++index) {
                const auto& [address, tally] = top[index];
                runtime_trace(
                    "native_gpu.image_read_top flip=%llu address=0x%016llX "
                    "MB=%llu reads=%llu unarmed=%llu dirty=%llu uninit=%llu "
                    "size=%ux%u unified=%u tile=%u\n",
                    static_cast<unsigned long long>(runtime.flips_processed),
                    static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(tally.bytes >> 20),
                    static_cast<unsigned long long>(tally.reads),
                    static_cast<unsigned long long>(tally.unarmed),
                    static_cast<unsigned long long>(tally.dirty),
                    static_cast<unsigned long long>(tally.uninitialized),
                    tally.width,
                    tally.height,
                    tally.unified_format,
                    tally.tile_mode);
            }
        }
        runtime_trace(
            "native_gpu.worker_phases flip=%llu draw=%llums/%llu "
            "compute=%llums/%llu flip_work=%llums/%llu idle=%llums "
            "pipeline=%llums/%llu resources=%llums record=%llums "
            "programs=%zu pl_cached=%llu pl_built=%llu pl_shared=%llu "
            "pl_created=%llu pl_decode=%llums pl_create=%llums "
            "pl_desc=%llums pl_buf=%llums/%llu pl_shapes=%llu "
            "buffers=%llums images=%llums "
            "states=%zu retained=%lluMB commit_free=%lluMB "
            "phys_free=%lluMB private=%lluMB vk_live=%lluMB "
            "vk_alloc=%llu vk_free=%llu vk_evicted=%llu "
            "vk_revived=%llu manifests_retired=%llu "
            "pool_hit=%llu pool_miss=%llu pool_back=%llu "
            "watch_races=%llu "
            "vk_hostbuf=%llu/%lluMB "
            "vk_devbuf=%llu/%lluMB vk_readback=%llu/%lluMB "
            "vk_image=%llu/%lluMB vk_other=%llu/%lluMB "
            "read_failed=%llu snapshot=%llu "
            "snap_kept=%lluMB snap_dropped=%lluMB "
            "img_read=%llums img_zero=%llums img_detile=%llums "
            "img_convert=%llums img_upload=%llums img_n=%llu "
            "flip_pre=%llums flip_finish=%llums flip_record=%llums "
            "flip_post=%llums "
            "rec_wait=%llums rec_bufcopy=%llums rec_img=%llums "
            "rec_submit=%llums rec_readback=%llums rec_end=%llums "
            "rec_result=%llums rec_writeback=%llums "
            "wb_n=%llu wb_MB=%llu wb_pages=%llu wb_map=%llums "
            "wb_compare=%llums wb_clean=%llu "
            "wb_unmarked=%llu/%lluMB wb_missed=%llu "
            "wb_scan=%llums wb_splice=%llums/%llu "
            "copy_fast=%llu/%llu/%llu write_fast=%llu/%llu/%llu "
            "wb_small=%llu/%lluMB "
            "wb_large=%llu/%lluMB wb_largest=%lluKB "
            "buf_n=%llu buf_MB=%llu buf_repeat=%llu/%lluMB "
            "buf_repeat_ro=%llu/%lluMB "
            "buf_same=%llu/%lluMB buf_differ=%llu/%lluMB "
            "buf_differ_ro=%llu/%lluMB "
            "buf_map=%llums buf_read=%llums/%llu/%lluMB "
            "buf_fill_MB=%llu "
            "buf_fill=%llums buf_probe=%llums "
            "img_MB=%llu img_zero_n=%llu img_zero_cached=%llu "
            "img_watch_skipped=%llu img_loop=%llu "
            "watch_arms=%llu "
            "watch_faults=%llu watch_refused=%llu "
            "watch_committed=%lluKB watch_yielded=%llu "
            "ring_uses=%llu ring_waits=%llu "
            "indexed_draws=%llu idx_buffers=%llu idx_uploads=%llu "
            "idx_MB=%llu idx_failed=%llu idx_read_failed=%llu\n",
            static_cast<unsigned long long>(flip.flip_id),
            static_cast<unsigned long long>(
                g_worker_draw_ticks * 1000 / frequency),
            static_cast<unsigned long long>(g_worker_draw_count),
            static_cast<unsigned long long>(
                g_worker_compute_ticks * 1000 / frequency),
            static_cast<unsigned long long>(g_worker_compute_count),
            static_cast<unsigned long long>(
                g_worker_flip_ticks * 1000 / frequency),
            static_cast<unsigned long long>(g_worker_flip_count),
            static_cast<unsigned long long>(
                g_worker_idle_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_compute_pipeline_ticks * 1000 / frequency),
            static_cast<unsigned long long>(g_compute_pipeline_count),
            static_cast<unsigned long long>(
                g_compute_resource_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_compute_record_ticks * 1000 / frequency),
            runtime.vulkan.shared_compute_programs.size(),
            static_cast<unsigned long long>(g_pipeline_cached),
            static_cast<unsigned long long>(g_pipeline_built),
            static_cast<unsigned long long>(g_pipeline_shared),
            static_cast<unsigned long long>(g_pipeline_created),
            static_cast<unsigned long long>(
                g_pipeline_decode_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_pipeline_create_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_pipeline_descriptor_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_pipeline_buffer_ticks * 1000 / frequency),
            static_cast<unsigned long long>(g_pipeline_buffer_n),
            static_cast<unsigned long long>(g_pipeline_shapes.size()),
            static_cast<unsigned long long>(
                g_resource_buffer_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_resource_image_ticks * 1000 / frequency),
            retained_states,
            static_cast<unsigned long long>(
                retained_manifest_bytes / (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(
                worker_commit_free_megabytes()),
            static_cast<unsigned long long>(
                worker_physical_free_megabytes()),
            static_cast<unsigned long long>(worker_private_megabytes()),
            static_cast<unsigned long long>(
                (g_vk_alloc_bytes.load(std::memory_order_relaxed) -
                 g_vk_free_bytes.load(std::memory_order_relaxed)) >> 20),
            static_cast<unsigned long long>(
                g_vk_alloc_count.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_vk_free_count.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_evicted_compute_states.load(
                    std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_revived_compute_states.load(
                    std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_retired_compute_manifests.load(
                    std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_pooled_buffer_hits.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_pooled_buffer_misses.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_pooled_buffer_returns.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_guest_watch_races.load(std::memory_order_relaxed)),
            SITE_PAIR(VkAllocSite::HostStorageBuffer),
            SITE_PAIR(VkAllocSite::DeviceLocalBuffer),
            SITE_PAIR(VkAllocSite::Readback),
            SITE_PAIR(VkAllocSite::SurfaceImage),
            SITE_PAIR(VkAllocSite::Other),
            static_cast<unsigned long long>(g_buffer_guest_read_failed),
            static_cast<unsigned long long>(g_buffer_snapshot_used),
            static_cast<unsigned long long>(
                g_snapshot_kept_bytes.load(std::memory_order_relaxed) /
                (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(
                g_snapshot_dropped_bytes.load(std::memory_order_relaxed) /
                (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(
                g_image_read_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_image_zero_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_image_detile_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_image_convert_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_image_upload_ticks * 1000 / frequency),
            static_cast<unsigned long long>(g_image_prepared),
            static_cast<unsigned long long>(
                g_flip_pre_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_flip_finish_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_flip_record_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_flip_post_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_record_wait_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_record_buffer_copy_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_record_image_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_record_submit_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_record_readback_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_record_end_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_record_result_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_record_writeback_ticks * 1000 / frequency),
            static_cast<unsigned long long>(g_writeback_n),
            static_cast<unsigned long long>(
                g_writeback_bytes / (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(g_writeback_changed_pages),
            static_cast<unsigned long long>(
                g_writeback_map_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_writeback_compare_ticks * 1000 / frequency),
            static_cast<unsigned long long>(g_writeback_clean_n),
            static_cast<unsigned long long>(g_writeback_unmarked_n),
            static_cast<unsigned long long>(
                g_writeback_unmarked_bytes / (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(g_writeback_false_clean),
            static_cast<unsigned long long>(
                g_writeback_scan_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_writeback_splice_ticks * 1000 / frequency),
            static_cast<unsigned long long>(g_writeback_splice_n),
            static_cast<unsigned long long>(
                g_guest_copy_fast.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_guest_copy_slow.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_guest_copy_reserved.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_guest_write_fast.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_guest_write_slow.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_guest_write_disarmed.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_writeback_small_n),
            static_cast<unsigned long long>(
                g_writeback_small_bytes / (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(g_writeback_large_n),
            static_cast<unsigned long long>(
                g_writeback_large_bytes / (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(
                g_writeback_largest / 1024ULL),
            static_cast<unsigned long long>(g_buffer_n),
            static_cast<unsigned long long>(
                g_buffer_bytes / (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(g_buffer_repeat_n),
            static_cast<unsigned long long>(
                g_buffer_repeat_bytes / (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(g_buffer_repeat_readonly_n),
            static_cast<unsigned long long>(
                g_buffer_repeat_readonly_bytes / (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(
                g_buffer_repeat_same_n.load(
                    std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_buffer_repeat_same_bytes.load(
                    std::memory_order_relaxed) /
                    (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(
                g_buffer_repeat_differ_n.load(
                    std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_buffer_repeat_differ_bytes.load(
                    std::memory_order_relaxed) /
                    (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(
                g_buffer_repeat_differ_ro_n.load(
                    std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_buffer_repeat_differ_ro_bytes.load(
                    std::memory_order_relaxed) /
                    (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(
                g_buffer_map_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_buffer_read_ticks * 1000 / frequency),
            static_cast<unsigned long long>(g_buffer_read_n),
            static_cast<unsigned long long>(
                g_buffer_read_bytes / (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(
                g_buffer_fill_bytes / (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(
                g_buffer_fill_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_buffer_probe_ticks * 1000 / frequency),
            static_cast<unsigned long long>(
                g_image_bytes_read / (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(g_image_zero_exits),
            static_cast<unsigned long long>(g_image_zero_cached),
            static_cast<unsigned long long>(g_image_watch_skipped),
            static_cast<unsigned long long>(g_image_loop_n),
            static_cast<unsigned long long>(
                g_guest_watch_arms.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_guest_watch_faults.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_guest_watch_refused.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_guest_watch_committed_bytes.load(
                    std::memory_order_relaxed) / 1024ULL),
            static_cast<unsigned long long>(
                g_guest_watch_yielded.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_command_ring_uses.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_command_ring_waits.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_indexed_draws.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_index_buffers_created.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_index_buffer_uploads.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_index_buffer_bytes.load(std::memory_order_relaxed) /
                (1024ULL * 1024ULL)),
            static_cast<unsigned long long>(
                g_index_buffer_failures.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_index_buffer_read_failures.load(
                    std::memory_order_relaxed)));
    }
    runtime_trace(
        "native_gpu.trace_cost flip=%llu write=%llums/%llu\n",
        static_cast<unsigned long long>(flip.flip_id),
        static_cast<unsigned long long>(
            g_runtime_trace_ticks * 1000 / runtime_trace_frequency()),
        static_cast<unsigned long long>(g_runtime_trace_count));
    runtime_trace(
        "native_gpu.flip_processed count=%llu flip=%llu handle=%u index=%d "
        "address=0x%016llX size=%ux%u pitch=%u fmt=0x%016llX tile=%u "
        "draws_processed=%llu\n",
        static_cast<unsigned long long>(processed),
        static_cast<unsigned long long>(flip.flip_id),
        flip.video_handle,
        flip.buffer_index,
        static_cast<unsigned long long>(flip.display_address),
        flip.width,
        flip.height,
        flip.pitch_in_pixels,
        static_cast<unsigned long long>(flip.pixel_format),
        flip.tiling_mode,
        static_cast<unsigned long long>(draws_processed));
    // Once a frame, so the tail reaches disk while the probe is watching.
    runtime_trace_flush();
}

void worker_main(NativeGpuRuntime* runtime) {
    runtime_trace("native_gpu.worker_thread tid=%lu\n",
        static_cast<unsigned long>(GetCurrentThreadId()));
    {
        std::lock_guard lock(runtime->mutex);
        runtime->worker_running = true;
    }
    runtime_trace(
        "native_gpu.worker_started abi=0x%08X queue_capacity=%u\n",
        PS5GPU_NATIVE_ABI_VERSION,
        runtime->queue_capacity);

    for (;;) {
        QueuedCommand command;
        {
            const auto idle_started = runtime_trace_counter();
            std::unique_lock lock(runtime->mutex);
            runtime->work_available.wait(
                lock,
                [&]() {
                    return runtime->stopping || !runtime->queue.empty();
                });
            if (runtime->queue.empty() && runtime->stopping) {
                break;
            }
            command = runtime->queue.front();
            runtime->queue.pop_front();
            ++runtime->commands_in_flight;
            g_worker_idle_ticks +=
                runtime_trace_counter() - idle_started;
        }

        const auto command_started = runtime_trace_counter();
        if (command.type == CommandType::Draw) {
            process_draw(*runtime, command.draw);
            g_worker_draw_ticks +=
                runtime_trace_counter() - command_started;
            ++g_worker_draw_count;
        } else if (command.type == CommandType::Compute) {
            process_compute(*runtime, command);
            g_worker_compute_ticks +=
                runtime_trace_counter() - command_started;
            ++g_worker_compute_count;
        } else {
            process_flip(*runtime, command.flip);
            g_worker_flip_ticks +=
                runtime_trace_counter() - command_started;
            ++g_worker_flip_count;
            g_guest_region_cache = {};
            // Everything the last frame concluded about a surface being
            // empty stops counting here.
            g_image_zero_epoch.fetch_add(1, std::memory_order_relaxed);
            g_buffer_frame_reads.clear();
            {
                std::lock_guard<std::mutex> hash_guard(
                    g_buffer_frame_hash_lock);
                g_buffer_frame_hashes.clear();
            }
        }

        std::size_t queue_size = 0;
        {
            std::lock_guard lock(runtime->mutex);
            --runtime->commands_in_flight;
            queue_size = runtime->queue.size();
            if (runtime->queue.empty() &&
                runtime->commands_in_flight == 0) {
                runtime->idle.notify_all();
            }
        }

        {
            const auto elapsed = runtime_trace_counter() - command_started;
            if (elapsed > g_worker_slowest_ticks) {
                g_worker_slowest_ticks = elapsed;
                g_worker_slowest_type =
                    command.type == CommandType::Draw ? "draw"
                    : command.type == CommandType::Compute ? "compute"
                    : "flip";
            }
        }

        if (++g_worker_commands % 128 == 0) {
            const auto frequency = runtime_trace_frequency();
            runtime_trace(
                "native_gpu.worker_tick commands=%llu draw=%llums/%llu "
                "compute=%llums/%llu flip=%llums/%llu idle=%llums "
                "queue=%zu slowest=%llums/%s\n",
                static_cast<unsigned long long>(g_worker_commands),
                static_cast<unsigned long long>(
                    g_worker_draw_ticks * 1000 / frequency),
                static_cast<unsigned long long>(g_worker_draw_count),
                static_cast<unsigned long long>(
                    g_worker_compute_ticks * 1000 / frequency),
                static_cast<unsigned long long>(g_worker_compute_count),
                static_cast<unsigned long long>(
                    g_worker_flip_ticks * 1000 / frequency),
                static_cast<unsigned long long>(g_worker_flip_count),
                static_cast<unsigned long long>(
                    g_worker_idle_ticks * 1000 / frequency),
                queue_size,
                static_cast<unsigned long long>(
                    g_worker_slowest_ticks * 1000 / frequency),
                g_worker_slowest_type);
            g_worker_slowest_ticks = 0;
            g_worker_slowest_type = "none";
            runtime_trace_flush();
        }
    }

    {
        std::lock_guard lock(runtime->mutex);
        runtime->worker_running = false;
        runtime->idle.notify_all();
    }
    runtime_trace("native_gpu.worker_stopped\n");
    runtime_trace_flush();
}

Ps5GpuNativeResult enqueue(
    NativeGpuRuntime* runtime,
    const QueuedCommand& command) {
    std::lock_guard lock(runtime->mutex);
    if (runtime->stopping) {
        return PS5GPU_NATIVE_ERROR_STOPPED;
    }
    if (runtime->queue.size() >= runtime->queue_capacity) {
        // The guest outruns this queue, and it used to be the newest work
        // that got refused: the backlog was rendered to the end while the
        // present was thrown away, so the picture fell further behind the
        // sound with every frame, and the refusal landed in the middle of a
        // frame and left it half drawn.
        //
        // Discard the oldest whole frame instead - everything up to and
        // including the flip that ends it. Skipping a frame under overload
        // is a frame that never appears; refusing part of one is a frame
        // that appears wrong. If there is no flip queued there is no whole
        // frame to drop, and the old behaviour is the only thing left.
        const auto flip = std::find_if(
            runtime->queue.begin(),
            runtime->queue.end(),
            [](const QueuedCommand& queued) {
                return queued.type == CommandType::Flip;
            });
        if (flip == runtime->queue.end()) {
            if (command.type == CommandType::Draw) {
                ++runtime->draws_dropped;
            } else if (command.type == CommandType::Compute) {
                ++runtime->compute_dispatches_dropped;
            } else {
                ++runtime->flips_dropped;
            }
            return PS5GPU_NATIVE_ERROR_QUEUE_FULL;
        }
        const auto discarded =
            static_cast<std::size_t>(
                std::distance(runtime->queue.begin(), flip)) + 1;
        for (auto entry = runtime->queue.begin();
             entry != std::next(flip);
             ++entry) {
            if (entry->type == CommandType::Draw) {
                ++runtime->draws_dropped;
            } else if (entry->type == CommandType::Compute) {
                ++runtime->compute_dispatches_dropped;
            } else {
                ++runtime->flips_dropped;
            }
        }
        runtime->queue.erase(runtime->queue.begin(), std::next(flip));
        ++runtime->frames_discarded;
        runtime->commands_discarded += discarded;
        if ((runtime->frames_discarded % 16) == 1) {
            runtime_trace(
                "native_gpu.frame_discarded frames=%llu commands=%llu "
                "queue=%zu\n",
                static_cast<unsigned long long>(
                    runtime->frames_discarded),
                static_cast<unsigned long long>(
                    runtime->commands_discarded),
                runtime->queue.size());
        }
    }
    runtime->queue.push_back(command);
    if (command.type == CommandType::Draw) {
        ++runtime->draws_submitted;
    } else if (command.type == CommandType::Compute) {
        ++runtime->compute_dispatches_submitted;
    } else {
        ++runtime->flips_submitted;
    }
    runtime->queue_high_watermark = std::max<std::uint64_t>(
        runtime->queue_high_watermark,
        runtime->queue.size());
    runtime->work_available.notify_one();
    return PS5GPU_NATIVE_OK;
}

} // namespace

extern "C" {

std::uint32_t PS5GPU_NATIVE_CALL ps5gpu_native_get_abi_version() {
    return PS5GPU_NATIVE_ABI_VERSION;
}

Ps5GpuNativeResult PS5GPU_NATIVE_CALL ps5gpu_native_create(
    const Ps5GpuNativeCreateInfo* create_info,
    Ps5GpuNativeHandle* handle) {
    if (create_info == nullptr || handle == nullptr) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    *handle = nullptr;
    if (!valid_header(
            create_info->struct_size,
            sizeof(Ps5GpuNativeCreateInfo),
            create_info->abi_version)) {
        return PS5GPU_NATIVE_ERROR_ABI_MISMATCH;
    }

    auto* runtime = new (std::nothrow) NativeGpuRuntime();
    g_capture_runtime = runtime;
    if (runtime == nullptr) {
        return PS5GPU_NATIVE_ERROR_OUT_OF_MEMORY;
    }
    // The queue fills and whole frames are discarded when it does - 49
    // frames and 4704 commands in a run, which is a quarter of the work
    // the title submits. Whether that is why the scene targets hold one
    // value each is a question the capacity answers directly, so it is
    // settable rather than fixed.
    auto requested = create_info->queue_capacity == 0
        ? kDefaultQueueCapacity
        : create_info->queue_capacity;
    if (const auto* value = std::getenv("PS5GPU_NATIVE_QUEUE_CAPACITY")) {
        char* end = nullptr;
        const auto parsed = std::strtoul(value, &end, 0);
        if (end != value && parsed != 0) {
            requested = static_cast<std::uint32_t>(parsed);
        }
    }
    runtime->queue_capacity = std::clamp(
        requested, 64u, kMaximumQueueCapacity);
    std::vector<wchar_t> capture_path(32768);
    const auto capture_length = GetEnvironmentVariableW(
        L"PS5GPU_NATIVE_CAPTURE_PATH",
        capture_path.data(),
        static_cast<DWORD>(capture_path.size()));
    if (capture_length != 0 &&
        capture_length < capture_path.size()) {
        runtime->capture_path = std::filesystem::path(
            capture_path.data(),
            capture_path.data() + capture_length);
        runtime_trace(
            "native_gpu.capture_armed path=%s\n",
            runtime->capture_path.string().c_str());
    }
    (void)runtime->vulkan.initialize();
    try {
        runtime->worker = std::thread(worker_main, runtime);
    } catch (...) {
        delete runtime;
        return PS5GPU_NATIVE_ERROR_INTERNAL;
    }
    *handle = runtime;
    return PS5GPU_NATIVE_OK;
}

void PS5GPU_NATIVE_CALL ps5gpu_native_destroy(Ps5GpuNativeHandle handle) {
    auto* runtime = static_cast<NativeGpuRuntime*>(handle);
    if (runtime == nullptr) {
        return;
    }
    {
        std::lock_guard lock(runtime->mutex);
        runtime->stopping = true;
        runtime->work_available.notify_all();
    }
    if (runtime->worker.joinable()) {
        runtime->worker.join();
    }
    runtime->vulkan.destroy();
    runtime_trace(
        "native_gpu.destroy draws=%llu/%llu dropped=%llu "
        "compute=%llu/%llu dropped=%llu flips=%llu/%llu "
        "dropped=%llu high_watermark=%llu\n",
        static_cast<unsigned long long>(runtime->draws_processed),
        static_cast<unsigned long long>(runtime->draws_submitted),
        static_cast<unsigned long long>(runtime->draws_dropped),
        static_cast<unsigned long long>(
            runtime->compute_dispatches_processed),
        static_cast<unsigned long long>(
            runtime->compute_dispatches_submitted),
        static_cast<unsigned long long>(
            runtime->compute_dispatches_dropped),
        static_cast<unsigned long long>(runtime->flips_processed),
        static_cast<unsigned long long>(runtime->flips_submitted),
        static_cast<unsigned long long>(runtime->flips_dropped),
        static_cast<unsigned long long>(runtime->queue_high_watermark));
    delete runtime;
}

Ps5GpuNativeResult PS5GPU_NATIVE_CALL
ps5gpu_native_register_shader_state(
    Ps5GpuNativeHandle handle,
    const Ps5GpuNativeShaderState* state,
    std::uint32_t* state_id) {
    auto* runtime = static_cast<NativeGpuRuntime*>(handle);
    if (runtime == nullptr || state == nullptr || state_id == nullptr) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    *state_id = 0;
    if (!valid_header(
            state->struct_size,
            sizeof(Ps5GpuNativeShaderState),
            state->abi_version)) {
        return PS5GPU_NATIVE_ERROR_ABI_MISMATCH;
    }
    if (state->sh_register_count > 4096 ||
        state->cx_register_count > 8192 ||
        (state->sh_register_count != 0 &&
         state->sh_registers == nullptr) ||
        (state->cx_register_count != 0 &&
         state->cx_registers == nullptr)) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }

    NativeGpuRuntime::CapturedState captured;
    captured.record.flags = state->flags;
    captured.record.es_address = state->es_address;
    captured.record.ps_address = state->ps_address;
    captured.record.es_header_address = state->es_header_address;
    captured.record.ps_header_address = state->ps_header_address;
    captured.record.export_user_data_base_register =
        state->export_user_data_base_register;
    captured.record.pixel_user_data_base_register =
        state->pixel_user_data_base_register;
    captured.record.pixel_input_enable = state->pixel_input_enable;
    captured.record.pixel_input_address = state->pixel_input_address;
    if (state->sh_register_count != 0) {
        captured.sh_registers.assign(
            state->sh_registers,
            state->sh_registers + state->sh_register_count);
    }
    if (state->cx_register_count != 0) {
        captured.cx_registers.assign(
            state->cx_registers,
            state->cx_registers + state->cx_register_count);
    }
    const auto register_order = [](
        const Ps5GpuNativeRegisterValue& left,
        const Ps5GpuNativeRegisterValue& right) {
        return left.address < right.address;
    };
    std::sort(
        captured.sh_registers.begin(),
        captured.sh_registers.end(),
        register_order);
    std::sort(
        captured.cx_registers.begin(),
        captured.cx_registers.end(),
        register_order);
    captured.record.sh_register_count =
        static_cast<std::uint32_t>(captured.sh_registers.size());
    captured.record.cx_register_count =
        static_cast<std::uint32_t>(captured.cx_registers.size());
    captured.record.hash = hash_shader_state(
        captured.record,
        captured.sh_registers,
        captured.cx_registers);

    std::lock_guard capture_lock(runtime->capture_mutex);
    if (const auto bucket =
            runtime->state_ids_by_hash.find(captured.record.hash);
        bucket != runtime->state_ids_by_hash.end()) {
        for (const auto candidate_id : bucket->second) {
            const auto existing =
                runtime->captured_states.find(candidate_id);
            if (existing != runtime->captured_states.end() &&
                equal_shader_state(existing->second, captured)) {
                if (!cache_live_graphics_state(
                        candidate_id,
                        *state)) {
                    return PS5GPU_NATIVE_ERROR_INTERNAL;
                }
                *state_id = candidate_id;
                return PS5GPU_NATIVE_OK;
            }
        }
    }
    if (runtime->next_state_id == 0) {
        return PS5GPU_NATIVE_ERROR_OUT_OF_MEMORY;
    }
    captured.record.state_id = runtime->next_state_id++;
    const auto inserted_id = captured.record.state_id;
    const auto [inserted, success] = runtime->captured_states.emplace(
        inserted_id,
        std::move(captured));
    if (!success) {
        return PS5GPU_NATIVE_ERROR_INTERNAL;
    }
    runtime->state_ids_by_hash[inserted->second.record.hash].push_back(
        inserted_id);
    // Registered graphics states were kept for the life of the process.
    // Registration deduplicates on content, and after the intro a few
    // hundred genuinely new ones arrive every frame - the user data
    // addresses differ - so they grow without end. The oldest are let go,
    // with what they left elsewhere; one that comes back registers again.
    // PS5GPU_NATIVE_GRAPHICS_STATE_CAP sets how many are kept.
    static const std::size_t graphics_state_cap = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_GRAPHICS_STATE_CAP");
        const auto parsed =
            value == nullptr ? 0ull : std::strtoull(value, nullptr, 0);
        return parsed == 0 ? std::size_t{16384}
                           : static_cast<std::size_t>(parsed);
    }();
    while (runtime->captured_states.size() > graphics_state_cap) {
        const auto oldest = runtime->captured_states.begin();
        if (oldest->first == inserted_id) {
            break;
        }
        const auto oldest_id = oldest->first;
        const auto bucket =
            runtime->state_ids_by_hash.find(oldest->second.record.hash);
        if (bucket != runtime->state_ids_by_hash.end()) {
            auto& ids = bucket->second;
            ids.erase(std::remove(ids.begin(), ids.end(), oldest_id),
                      ids.end());
            if (ids.empty()) {
                runtime->state_ids_by_hash.erase(bucket);
            }
        }
        runtime->captured_states.erase(oldest);
        forget_graphics_state(oldest_id);
        g_retired_graphics_captures.fetch_add(1, std::memory_order_relaxed);
    }
    if (!cache_live_graphics_state(inserted_id, *state)) {
        return PS5GPU_NATIVE_ERROR_INTERNAL;
    }
    if (!runtime->capture_path.empty()) {
        capture_shader_metadata_locked(
            *runtime,
            inserted->second,
            inserted->second.record.es_header_address,
            inserted->second.record.export_user_data_base_register);
        capture_shader_metadata_locked(
            *runtime,
            inserted->second,
            inserted->second.record.ps_header_address,
            inserted->second.record.pixel_user_data_base_register);
        capture_state_resources_locked(
            *runtime,
            inserted_id);
    }
    // Copy the export stage's user data across for the backend: buffer
    // descriptors live in it and the guest moves them every frame, so the
    // addresses baked into the resource manifest go stale after frame one.
    for (const auto pixel_stage : {false, true}) {
        const auto base = pixel_stage
            ? inserted->second.record.pixel_user_data_base_register
            : inserted->second.record.export_user_data_base_register;
        const auto count = shader_user_data_count(inserted->second, base);
        if (count == 0) {
            continue;
        }
        std::vector<std::uint32_t> user_data(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            user_data[index] = find_register_value(
                inserted->second.sh_registers,
                base + index);
        }
        runtime->vulkan.set_guest_user_data(
            inserted_id,
            pixel_stage,
            user_data.data(),
            user_data.size());
    }
    *state_id = inserted_id;
    runtime_trace(
        "native_gpu.shader_state_registered id=%u hash=0x%016llX "
        "es=0x%016llX ps=0x%016llX sh=%u cx=%u memory_pages=%llu\n",
        inserted_id,
        static_cast<unsigned long long>(inserted->second.record.hash),
        static_cast<unsigned long long>(
            inserted->second.record.es_address),
        static_cast<unsigned long long>(
            inserted->second.record.ps_address),
        inserted->second.record.sh_register_count,
        inserted->second.record.cx_register_count,
        static_cast<unsigned long long>(
            runtime->captured_memory.size()));
    return PS5GPU_NATIVE_OK;
}

Ps5GpuNativeResult PS5GPU_NATIVE_CALL
ps5gpu_native_register_compute_state(
    Ps5GpuNativeHandle handle,
    const Ps5GpuNativeComputeState* state,
    std::uint32_t* state_id) {
    auto* runtime = static_cast<NativeGpuRuntime*>(handle);
    if (runtime == nullptr || state == nullptr || state_id == nullptr) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    *state_id = 0;
    if (!valid_header(
            state->struct_size,
            sizeof(Ps5GpuNativeComputeState),
            state->abi_version)) {
        return PS5GPU_NATIVE_ERROR_ABI_MISMATCH;
    }
    constexpr std::uint32_t kMaximumSpirvBytes =
        64u * 1024u * 1024u;
    constexpr std::uint32_t kMaximumManifestBytes =
        256u * 1024u * 1024u;
    if (state->shader_address == 0 ||
        state->spirv == nullptr ||
        state->spirv_size < 5 * sizeof(std::uint32_t) ||
        state->spirv_size > kMaximumSpirvBytes ||
        (state->spirv_size % sizeof(std::uint32_t)) != 0 ||
        state->resource_manifest == nullptr ||
        state->resource_manifest_size <
            sizeof(Ps5GpuResourceManifestHeader) ||
        state->resource_manifest_size > kMaximumManifestBytes) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }

    auto registered =
        std::make_shared<RegisteredComputeState>();
    registered->flags = state->flags;
    registered->state_hash = state->state_hash;
    registered->shader_address = state->shader_address;
    registered->shader_header_address =
        state->shader_header_address;
    registered->spirv.resize(
        state->spirv_size / sizeof(std::uint32_t));
    std::memcpy(
        registered->spirv.data(),
        state->spirv,
        state->spirv_size);
    registered->resource_manifest.assign(
        state->resource_manifest,
        state->resource_manifest +
            state->resource_manifest_size);
    trim_manifest_data_section(registered->resource_manifest);
    if (registered->spirv[0] != spv::MagicNumber) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    LoadedResourceManifest manifest;
    if (!decode_resource_manifest(
            registered->resource_manifest.data(),
            registered->resource_manifest.size(),
            manifest) ||
        manifest.header.stage != PS5GPU_STAGE_COMPUTE) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    if (registered->state_hash == 0) {
        registered->state_hash = hash_bytes(
            registered->spirv.data(),
            registered->spirv.size() * sizeof(std::uint32_t));
        registered->state_hash = hash_bytes(
            registered->resource_manifest.data(),
            registered->resource_manifest.size(),
            registered->state_hash);
    }

    const auto capture_registered_state =
        [&](std::uint32_t captured_state_id) {
            if (runtime->capture_path.empty()) {
                return;
            }
            NativeGpuRuntime::CapturedComputeState captured;
            captured.record.state_id = captured_state_id;
            captured.record.flags = registered->flags;
            captured.record.state_hash = registered->state_hash;
            captured.record.shader_address =
                registered->shader_address;
            captured.record.shader_header_address =
                registered->shader_header_address;
            captured.record.spirv_size = static_cast<std::uint32_t>(
                registered->spirv.size() * sizeof(std::uint32_t));
            captured.record.resource_manifest_size =
                static_cast<std::uint32_t>(
                    registered->resource_manifest.size());
            captured.spirv.resize(captured.record.spirv_size);
            std::memcpy(
                captured.spirv.data(),
                registered->spirv.data(),
                captured.spirv.size());
            captured.resource_manifest =
                registered->resource_manifest;
            std::lock_guard capture_lock(runtime->capture_mutex);
            runtime->captured_compute_states.try_emplace(
                captured_state_id,
                std::move(captured));
            capture_compute_resources_locked(
                *runtime,
                captured_state_id,
                manifest);
        };

    std::lock_guard lock(runtime->compute_state_mutex);
    if (const auto bucket =
            runtime->compute_state_ids_by_hash.find(
                registered->state_hash);
        bucket != runtime->compute_state_ids_by_hash.end()) {
        for (const auto candidate_id : bucket->second) {
            const auto candidate =
                runtime->compute_states.find(candidate_id);
            if (candidate == runtime->compute_states.end()) {
                continue;
            }
            const auto& existing = *candidate->second;
            if (existing.flags == registered->flags &&
                existing.shader_address ==
                    registered->shader_address &&
                existing.shader_header_address ==
                    registered->shader_header_address &&
                existing.spirv == registered->spirv &&
                existing.resource_manifest ==
                    registered->resource_manifest) {
                capture_registered_state(candidate_id);
                *state_id = candidate_id;
                return PS5GPU_NATIVE_OK;
            }
        }
    }
    if (runtime->next_compute_state_id == 0) {
        return PS5GPU_NATIVE_ERROR_OUT_OF_MEMORY;
    }
    registered->state_id = runtime->next_compute_state_id++;
    const auto inserted_id = registered->state_id;
    runtime->compute_states.emplace(inserted_id, registered);
    // Registration deduplicates on content, so 7619 states in a run means
    // 7619 genuinely different manifests - the manifest carries the guest
    // buffer data the dispatch was given, which differs every time. Kept
    // forever they reach 2.3GB and are what still grows once the device
    // allocations are retired. Ids rise with use, so the oldest are the
    // coldest; a retired manifest is not a lost dispatch either, because
    // an identical one registers again as a new id.
    static const std::size_t state_cap = [] {
        char value[16] = {};
        const auto length = GetEnvironmentVariableA(
            "PS5GPU_NATIVE_COMPUTE_STATE_CAP",
            value,
            static_cast<DWORD>(sizeof(value)));
        if (length == 0 || length >= sizeof(value)) {
            return std::size_t{2048};
        }
        const auto parsed = std::strtoull(value, nullptr, 0);
        return parsed == 0 ? std::size_t{2048}
                           : static_cast<std::size_t>(parsed);
    }();
    while (runtime->compute_states.size() > state_cap) {
        const auto oldest = runtime->compute_states.begin();
        if (oldest->first == inserted_id) {
            break;
        }
        runtime->compute_states.erase(oldest);
        g_retired_compute_manifests.fetch_add(
            1, std::memory_order_relaxed);
    }
    runtime->compute_state_ids_by_hash[
        registered->state_hash].push_back(inserted_id);
    capture_registered_state(inserted_id);
    *state_id = inserted_id;
    runtime_trace(
        "native_gpu.compute_state_registered id=%u "
        "hash=0x%016llX shader=0x%016llX spirv=%llu "
        "manifest=%llu buffers=%u images=%u\n",
        inserted_id,
        static_cast<unsigned long long>(
            registered->state_hash),
        static_cast<unsigned long long>(
            registered->shader_address),
        static_cast<unsigned long long>(
            registered->spirv.size() * sizeof(std::uint32_t)),
        static_cast<unsigned long long>(
            registered->resource_manifest.size()),
        manifest.header.global_count,
        manifest.header.image_count);
    return PS5GPU_NATIVE_OK;
}

Ps5GpuNativeResult PS5GPU_NATIVE_CALL ps5gpu_native_submit_draw(
    Ps5GpuNativeHandle handle,
    const Ps5GpuNativeDraw* draw) {
    auto* runtime = static_cast<NativeGpuRuntime*>(handle);
    if (runtime == nullptr || draw == nullptr) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    if (!valid_header(
            draw->struct_size,
            sizeof(Ps5GpuNativeDraw),
            draw->abi_version)) {
        return PS5GPU_NATIVE_ERROR_ABI_MISMATCH;
    }
    // A draw's constants are taken here, when the title hands the draw
    // over. The worker runs frames behind, and the ring they live in is
    // reused every third frame: the intro's matrix slot held a later
    // frame's texture descriptors by the time the worker read it.
    static const auto no_snapshot_at_submit =
        environment_flag_enabled("PS5GPU_NATIVE_NO_SNAPSHOT_AT_SUBMIT");
    if (!no_snapshot_at_submit) {
        take_draw_snapshots(draw->reserved0);
        if (draw->index_address != 0) {
            take_index_snapshot(
                draw->reserved0,
                draw->index_address,
                static_cast<std::uint64_t>(draw->vertex_count) *
                    (draw->index_bytes == 4 ? 4u : 2u));
        }
    }
    QueuedCommand command;
    command.type = CommandType::Draw;
    command.draw = *draw;
    return enqueue(runtime, command);
}

Ps5GpuNativeResult PS5GPU_NATIVE_CALL ps5gpu_native_submit_compute(
    Ps5GpuNativeHandle handle,
    const Ps5GpuNativeComputeDispatch* dispatch) {
    auto* runtime = static_cast<NativeGpuRuntime*>(handle);
    if (runtime == nullptr || dispatch == nullptr) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    if (!valid_header(
            dispatch->struct_size,
            sizeof(Ps5GpuNativeComputeDispatch),
            dispatch->abi_version)) {
        return PS5GPU_NATIVE_ERROR_ABI_MISMATCH;
    }
    if (dispatch->compute_state_id == 0 ||
        dispatch->group_count_x == 0 ||
        dispatch->group_count_y == 0 ||
        dispatch->group_count_z == 0) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }

    std::shared_ptr<const RegisteredComputeState> compute_state;
    {
        std::lock_guard lock(runtime->compute_state_mutex);
        const auto state =
            runtime->compute_states.find(
                dispatch->compute_state_id);
        if (state == runtime->compute_states.end()) {
            return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
        }
        compute_state = state->second;
    }
    QueuedCommand command;
    command.type = CommandType::Compute;
    command.compute = *dispatch;
    command.compute_state = std::move(compute_state);
    return enqueue(runtime, command);
}

Ps5GpuNativeResult PS5GPU_NATIVE_CALL ps5gpu_native_submit_flip(
    Ps5GpuNativeHandle handle,
    const Ps5GpuNativeFlip* flip) {
    auto* runtime = static_cast<NativeGpuRuntime*>(handle);
    if (runtime == nullptr || flip == nullptr) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    if (!valid_header(
            flip->struct_size,
            sizeof(Ps5GpuNativeFlip),
            flip->abi_version)) {
        return PS5GPU_NATIVE_ERROR_ABI_MISMATCH;
    }
    {
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        g_flip_submitted_at[flip->flip_id % kFlipSubmittedSlots].store(
            now.QuadPart, std::memory_order_relaxed);
    }
    QueuedCommand command;
    command.type = CommandType::Flip;
    command.flip = *flip;
    const auto result = enqueue(runtime, command);
    if (result != PS5GPU_NATIVE_OK) {
        return result;
    }
    // The title's side executes its command buffers when it submits them -
    // DMA copies included - and the worker draws them later. Left to run
    // free it got seconds ahead: the intro's twelve-megabyte frame copies
    // had all landed, the last of them black, before the worker drew the
    // first frame that samples them. So the submitting side waits here
    // until the worker is at most a few flips behind, which keeps what a
    // copy wrote in memory while the frames that read it are drawn.
    static const std::uint64_t frames_ahead = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_MAX_FRAMES_AHEAD");
        return value == nullptr ? std::uint64_t{2}
                                : std::strtoull(value, nullptr, 0);
    }();
    if (frames_ahead != 0) {
        std::unique_lock lock(runtime->mutex);
        const auto enqueued = ++runtime->flips_enqueued;
        runtime->flip_finished.wait_for(
            lock,
            std::chrono::milliseconds(2000),
            [&] {
                return !runtime->worker_running ||
                    runtime->flips_finished + frames_ahead >= enqueued;
            });
    }
    return result;
}

Ps5GpuNativeResult PS5GPU_NATIVE_CALL ps5gpu_native_flush(
    Ps5GpuNativeHandle handle,
    std::uint32_t timeout_ms) {
    auto* runtime = static_cast<NativeGpuRuntime*>(handle);
    if (runtime == nullptr) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    std::unique_lock lock(runtime->mutex);
    const auto idle = [&]() {
        return runtime->queue.empty() &&
            runtime->commands_in_flight == 0;
    };
    if (idle()) {
        return PS5GPU_NATIVE_OK;
    }
    if (!runtime->idle.wait_for(
            lock,
            std::chrono::milliseconds(timeout_ms),
            idle)) {
        return PS5GPU_NATIVE_ERROR_TIMEOUT;
    }
    return PS5GPU_NATIVE_OK;
}

Ps5GpuNativeResult PS5GPU_NATIVE_CALL ps5gpu_native_get_stats(
    Ps5GpuNativeHandle handle,
    Ps5GpuNativeStats* stats) {
    auto* runtime = static_cast<NativeGpuRuntime*>(handle);
    if (runtime == nullptr || stats == nullptr) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    if (stats->struct_size < sizeof(Ps5GpuNativeStats)) {
        return PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(runtime->mutex);
    stats->abi_version = PS5GPU_NATIVE_ABI_VERSION;
    stats->draws_submitted = runtime->draws_submitted;
    stats->draws_processed = runtime->draws_processed;
    stats->draws_dropped = runtime->draws_dropped;
    stats->compute_dispatches_submitted =
        runtime->compute_dispatches_submitted;
    stats->compute_dispatches_processed =
        runtime->compute_dispatches_processed;
    stats->compute_dispatches_dropped =
        runtime->compute_dispatches_dropped;
    stats->flips_submitted = runtime->flips_submitted;
    stats->flips_processed = runtime->flips_processed;
    stats->flips_dropped = runtime->flips_dropped;
    stats->queue_depth = runtime->queue.size();
    stats->queue_high_watermark = runtime->queue_high_watermark;
    stats->commands_in_flight = runtime->commands_in_flight;
    stats->worker_running = runtime->worker_running ? 1u : 0u;
    stats->reserved0 = 0;
    return PS5GPU_NATIVE_OK;
}

} // extern "C"
