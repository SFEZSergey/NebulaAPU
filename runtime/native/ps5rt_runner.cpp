// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdarg>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "runtime_guest_image.h"
#include "ps5gpu_bridge_api.h"
#include "ps5gpu_native_api.h"
#include "ps5rt_api.h"
#include "ps5rt_hle_impl.h"

#if defined(PS5RT_HAS_GENERATED_HLE)
#include "hle_registry.generated.h"
#endif

namespace generated = sharprecomp::generated;

namespace {

#if defined(__GNUC__) && defined(_WIN32)
#define PS5_GUEST_ABI __attribute__((sysv_abi))
#else
#define PS5_GUEST_ABI
#endif

constexpr std::uint64_t kPageSize = 0x1000;
constexpr std::uint64_t kAllocationGranularity = 0x10000;
// Keep relocated mappings above the low GPU aperture and loaded guest images.
// Exact PS5 address hints below this boundary are still attempted first.
constexpr std::uint64_t kGuestVirtualSearchStart = 0x0000001000000000ULL;
constexpr std::uint64_t kGuestVirtualAddressEnd = 0x0000010000000000ULL;
constexpr std::uint64_t kRenderTargetFallbackStart = 0x0000002000000000ULL;
constexpr std::uint64_t kLazyCommitGranule =
    4ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kGuestLowApertureStart = 0x0000000400000000ULL;
constexpr std::uint64_t kGuestLowApertureEnd = 0x0000000800000000ULL;
constexpr std::uint64_t kBootstrapHleAddress = 0x00000007FFF00000;
constexpr std::uint64_t kUnresolvedThunkAddress = 0x0000700010000000;
constexpr std::uint64_t kUnresolvedImportResult = 0x80020001;
constexpr wchar_t kHleBridgeFileName[] = L"Ps5HleBridge.dll";
constexpr wchar_t kGpuBridgeFileName[] = L"Ps5GpuBridge.dll";
constexpr wchar_t kNativeGpuRuntimeFileName[] = L"Ps5GpuRuntime.dll";
constexpr std::uint64_t kDirectMemorySize = 16ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kFlexibleMemorySize = 448ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kStaticTlsReservation = 0x10000;
constexpr std::uint64_t kTlsControlSize = 0x1000;
constexpr std::uint64_t kUnresolvedThunkSize = 32;
constexpr std::size_t kPthreadKeyCapacity = 1024;
constexpr std::uint64_t kKernelErrorNoEnt = 0x80020002ULL;
constexpr std::uint64_t kKernelErrorSrch = 0x80020003ULL;
constexpr std::uint64_t kKernelErrorFault = 0x8002000EULL;
constexpr std::uint64_t kKernelErrorInvalidArgument = 0x80020016ULL;
constexpr std::uint32_t kHleControlContextTransfer = 1u << 0;
constexpr std::uint32_t kHleControlRestoreFullFpuState = 1u << 1;

struct Options {
    std::filesystem::path package_directory;
    std::filesystem::path app0_directory;
    std::optional<std::uint64_t> expected_return;
    Ps5RtGpuBackend gpu_backend = PS5RT_GPU_AUTO;
    bool validate_only = false;
    bool allow_unresolved_imports = false;
    bool headless = false;
    bool run_initializers = false;
};

struct AddressRange {
    std::uint64_t base;
    std::uint64_t end;
};

struct Reservation {
    void* base;
    std::uint64_t size;
};

struct ImportSetupResult {
    std::size_t resolved = 0;
    std::size_t unresolved = 0;
    std::string first_unresolved;
};

struct RelocationSetupResult {
    std::size_t resolved = 0;
    std::size_t unresolved = 0;
    std::string first_unresolved;
};

struct GpuPoolFallbackState {
    std::uint64_t pool = 0;
    std::uint64_t base = 0;
    std::uint64_t capacity = 0;
    std::uint64_t used = 0;
};

struct Ps5HleCallFrame {
    std::uint64_t gpr[16];
    std::uint64_t rip;
    std::uint64_t rflags;
    std::uint64_t fs_base;
    std::uint64_t gs_base;
    std::uint16_t fpu_control_word;
    std::uint16_t reserved0;
    std::uint32_t mxcsr;
    std::uint64_t xmm[32];
    std::uint32_t control_flags;
    std::uint32_t reserved1;
};

struct alignas(16) Ps5GuestContextTransferFrame {
    std::uint64_t rip;
    std::uint64_t rsp;
    std::uint64_t rax;
    std::uint64_t rcx;
    std::uint64_t rdx;
    std::uint64_t rbx;
    std::uint64_t rbp;
    std::uint64_t rsi;
    std::uint64_t rdi;
    std::uint64_t r8;
    std::uint64_t r9;
    std::uint64_t r10;
    std::uint64_t r11;
    std::uint64_t r12;
    std::uint64_t r13;
    std::uint64_t r14;
    std::uint64_t r15;
    std::uint64_t mxcsr;
    std::uint64_t fpu_control_word;
    std::uint64_t restore_full_fpu_state;
};

static_assert(offsetof(Ps5GuestContextTransferFrame, mxcsr) == 136);
static_assert(offsetof(Ps5GuestContextTransferFrame, fpu_control_word) == 144);
static_assert(
    offsetof(Ps5GuestContextTransferFrame, restore_full_fpu_state) == 152);

using HleBridgeInitialize = int (__cdecl*)(const char*);
using HleBridgeHasNid = int (__cdecl*)(const char*);
using HleBridgeDispatch = int (__cdecl*)(
    const char*,
    Ps5HleCallFrame*);

struct alignas(16) ProcessEntryParameters {
    std::uint32_t argument_count;
    std::uint32_t reserved;
    std::uint64_t argument_0;
    std::uint64_t argument_1;
    std::uint64_t argument_2;
};

alignas(16) std::uint64_t g_stack_check_guard[2] = {
    0xC0DEC0DECAFEBA00ULL,
    0xC0DEC0DECAFEBA00ULL,
};
char g_process_name[] = "eboot.bin";
// Sony's loader starts a title with the command line the package ships in
// args.txt, and the entry block is the standard argc / argv[] / NULL /
// envp[] / NULL layout rather than the three fixed slots that were here.
// Astro Bot reads it: given no arguments it decides it is a development
// build and looks for every asset under /host/%ASOBI_ROOT%/target/...,
// a prefix no package can satisfy - 30 of its 36 opens in a run fail with
// ENOENT on exactly that. Reading a file the package ships is not a
// title-specific rule.
//
// Opt-in for now under PS5RT_PACKAGE_ARGS=1: with the arguments the
// title switches to its packaged asset path and drives APR/AMPR for
// real - resolve, ReadFile, submit, wait - and every one of those is
// still a permissive stub returning zero, so it reads garbage where a
// file should be and jumps through it after about nineteen seconds.
// The default stays off until that path is served.
constexpr std::size_t kGuestEntryBlockSlots = 128;
alignas(16) std::uint64_t g_guest_entry_block[kGuestEntryBlockSlots] = {};
std::vector<std::string> g_guest_arguments;
char* g_process_name_pointer = g_process_name;
std::uint32_t g_libc_need_flag = 1;
std::uint32_t g_libc_internal_need_flag = 1;
DWORD g_guest_tls_base_tls_index = TLS_OUT_OF_INDEXES;
std::atomic<std::uint64_t> g_guest_fs_recovery_count = 0;
thread_local int g_guest_errno = 0;
thread_local std::array<std::uint64_t, kPthreadKeyCapacity>
    g_pthread_specific_values = {};
SRWLOCK g_pthread_key_lock = SRWLOCK_INIT;
std::array<bool, kPthreadKeyCapacity> g_pthread_key_active = {};
std::array<std::uint64_t, kPthreadKeyCapacity> g_pthread_key_destructors = {};
std::uint32_t g_next_pthread_key = 1;
std::uint64_t g_thread_dtors_callback = 0;
std::uint64_t g_thread_atexit_count_callback = 0;
std::uint64_t g_thread_atexit_report_callback = 0;
std::uint64_t g_application_heap_api = 0;
std::uint64_t g_next_direct_memory_offset = 0;
std::atomic<std::uint32_t> g_astro_arena_zero_index = 0;
SRWLOCK g_guest_virtual_mapping_lock = SRWLOCK_INIT;
alignas(16) std::uint8_t g_heap_trace_storage[
    sizeof(std::uint64_t) + (64 * sizeof(std::uint64_t))] = {};
struct alignas(16) FontSelectionStub {
    std::uint32_t magic;
    std::uint32_t object_size;
    std::array<std::uint64_t, 3> reserved;
};
FontSelectionStub g_font_library_selection = {0, 0x38, {}};
FontSelectionStub g_font_renderer_selection = {0, 0x100, {}};
volatile LONG g_crash_trace_written = 0;
volatile LONG g_ignored_guest_int41_count = 0;
volatile LONG g_exception_trace_count = 0;
bool g_ignore_guest_int41 = false;
SRWLOCK g_gpu_pool_fallback_lock = SRWLOCK_INIT;
std::array<GpuPoolFallbackState, 8> g_gpu_pool_fallback_states = {};
std::uint64_t g_active_initializer = 0;
const char* g_active_initializer_name = nullptr;
HMODULE g_hle_bridge = nullptr;
HleBridgeHasNid g_hle_bridge_has_nid = nullptr;
HleBridgeDispatch g_hle_bridge_dispatch = nullptr;
HMODULE g_gpu_bridge = nullptr;
Ps5GpuCompileSpirv g_gpu_compile_spirv = nullptr;
Ps5GpuFree g_gpu_free = nullptr;
HMODULE g_native_gpu_runtime_module = nullptr;
Ps5GpuNativeHandle g_native_gpu_runtime = nullptr;
Ps5GpuNativeCreate g_native_gpu_create = nullptr;
Ps5GpuNativeDestroy g_native_gpu_destroy = nullptr;
Ps5GpuNativeRegisterShaderState g_native_gpu_register_shader_state = nullptr;
Ps5GpuNativeRegisterComputeState g_native_gpu_register_compute_state =
    nullptr;
Ps5GpuNativeSubmitDraw g_native_gpu_submit_draw = nullptr;
Ps5GpuNativeSubmitCompute g_native_gpu_submit_compute = nullptr;
Ps5GpuNativeSubmitFlip g_native_gpu_submit_flip = nullptr;
Ps5GpuNativeFlush g_native_gpu_flush = nullptr;
Ps5GpuNativeGetStats g_native_gpu_get_stats = nullptr;
SRWLOCK g_guest_context_transfer_stub_lock = SRWLOCK_INIT;
std::atomic<void*> g_guest_context_transfer_stub = nullptr;
void* g_guest_tls_load_handler = nullptr;
alignas(16) thread_local Ps5GuestContextTransferFrame
    g_guest_context_transfer_frame = {};

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment);
std::uint64_t align_down(std::uint64_t value, std::uint64_t alignment);
void* allocate_guest_virtual_range(
    std::uint64_t requested_address,
    std::uint64_t direct_memory_start,
    std::uint64_t length,
    std::uint64_t alignment,
    bool fixed);
extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_return_zero(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t);
bool try_write_process_bytes(
    std::uint64_t address,
    const void* source,
    std::size_t size);
extern "C" void ps5rt_native_gpu_guest_written(
    std::uint64_t address, std::uint64_t size);

std::uint64_t allocate_gpu_pool_fallback(
    std::uint64_t pool,
    std::uint64_t base,
    std::uint64_t capacity,
    std::uint64_t size) {
    if (pool == 0 || base == 0 || capacity == 0 || size == 0 ||
        size > capacity) {
        return 0;
    }

    const auto alignment = size >= 0x200000 ? 0x200000ULL : 0x100ULL;
    AcquireSRWLockExclusive(&g_gpu_pool_fallback_lock);
    auto* selected = static_cast<GpuPoolFallbackState*>(nullptr);
    for (auto& state : g_gpu_pool_fallback_states) {
        if (state.pool == pool) {
            selected = &state;
            break;
        }
        if (selected == nullptr && state.pool == 0) {
            selected = &state;
        }
    }

    std::uint64_t address = 0;
    if (selected != nullptr) {
        if (selected->pool == 0) {
            selected->pool = pool;
            selected->base = base;
            selected->capacity = capacity;
        }
        if (selected->base == base && selected->capacity == capacity) {
            const auto offset = align_up(selected->used, alignment);
            if (offset <= capacity && size <= capacity - offset) {
                address = base + offset;
                selected->used = offset + size;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_gpu_pool_fallback_lock);
    return address;
}

std::uint64_t read_fs_base_raw() {
    std::uint64_t value = 0;
#if defined(__GNUC__) && defined(_WIN32)
    __asm__ volatile("rdfsbase %0" : "=r"(value));
#endif
    return value;
}

void write_fs_base_raw(std::uint64_t value) {
#if defined(__GNUC__) && defined(_WIN32)
    __asm__ volatile("wrfsbase %0" : : "r"(value));
#else
    (void)value;
#endif
}

std::uint64_t get_guest_thread_pointer() {
    if (g_guest_tls_base_tls_index == TLS_OUT_OF_INDEXES) {
        return 0;
    }
    return reinterpret_cast<std::uint64_t>(
        TlsGetValue(g_guest_tls_base_tls_index));
}

bool set_guest_thread_pointer(std::uint64_t value) {
    return g_guest_tls_base_tls_index != TLS_OUT_OF_INDEXES &&
        TlsSetValue(
            g_guest_tls_base_tls_index,
            reinterpret_cast<void*>(value)) != FALSE;
}

void trace_stderr(const char* format, ...);

// Where the seventy-odd seconds between frames go. That stretch emits no
// GPU events and almost no bridge calls, so nothing existing observes it;
// the only thing left is to ask the threads directly. A sampler suspends
// each other thread briefly, reads its instruction pointer, and buckets
// the address - the standard way to find a hot loop that calls nothing.
//
// Off unless PS5RECOMP_SAMPLE_MS is set, because suspending threads is not
// free and not something to leave running by default.
std::atomic<bool> g_sampler_stop{false};
std::atomic<std::uint32_t> g_sample_thread_count{0};
SRWLOCK g_sample_lock = SRWLOCK_INIT;
// Keyed by thread as well as page. Sampling every thread equally means
// idle ones dominate by sheer count - the process keeps many parked guest
// threads - and a per-page total says nothing about the one thread that
// owes the next frame.
std::map<std::pair<DWORD, std::uint64_t>, std::uint64_t> g_sample_counts;
std::map<std::uint64_t, std::uint64_t> g_guest_sites;
std::map<std::string, std::uint64_t> g_guest_stacks;

// A bare instruction pointer named a module and stopped there: every
// thread sat in ntdll, which is equally true of one waiting on a handle,
// one parked on a managed lock, and one sleeping - and those have
// different causes. Two things separate them. The nearest preceding ntdll
// export says which kind of wait it is: NtWaitForSingleObject is a handle,
// NtWaitForAlertByThreadId is a managed lock or a task continuation,
// NtDelayExecution is a sleep, NtWaitForWorkViaWorkerFactory is an idle
// pool thread. And the first return address on the stack belonging to a
// module that is not Windows' own says who asked for the wait.
//
// Neither is exact. The export match takes the nearest export below the
// address and only within a syscall stub's span, and the stack scan reads
// raw words, so a stale value left by an earlier call can be read as a
// live frame. Both are good enough to tell a sleep from a lock.
struct SampleModule {
    std::uintptr_t base;
    std::uintptr_t end;
    bool system_module;
    char name[64];
    // Only code counts as a frame. The first pass accepted any stack word
    // pointing anywhere into a module, and the answer it produced -
    // every thread called from the same address - was a pointer into our
    // own .bss that happens to sit on the stack, not a return address.
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> code;
};

struct NtdllStub {
    std::uintptr_t address;
    const char* name;
};

std::vector<SampleModule> g_sample_modules;
std::vector<NtdllStub> g_ntdll_stubs;
std::vector<NtdllStub> g_ntdll_wait_functions;
std::map<std::string, std::uint64_t> g_sample_sites;
// Ranking by (thread, page) answers "where is one thread" and hides "where
// is the process": the parked threads all sit on one ntdll page and win
// every bucket, while a busy thread scatters across a whole code region and
// wins none. The process was measured at seven cores busy while every top
// bucket was a wait, which is that flaw exactly. This bucket is per module,
// so a region that is hot in total shows up whether or not any single page
// of it is.
std::map<std::string, std::uint64_t> g_sample_regions;

// Naming the export an instruction pointer landed in. win32u.dll needed
// this first - it is the syscall stub layer for user32 and gdi32 alike,
// and for D3DKMT, so "busy in win32u" could mean polling the message
// queue or waiting on the GPU. ntdll needs it for the same reason: a
// tenth of every sample sits there outside a wait stub, and that bucket
// says nothing until the function has a name.
struct ModuleExports {
    std::vector<std::string> names;
    std::vector<NtdllStub> entries;
};

ModuleExports g_win32u_exports;
ModuleExports g_ntdll_exports;

void resolve_module_exports(const char* name, ModuleExports& out) {
    const auto module = GetModuleHandleA(name);
    if (module == nullptr) {
        return;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    const auto& directory =
        headers->OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0) {
        return;
    }
    const auto* exports =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
            base + directory.VirtualAddress);
    const auto* functions = reinterpret_cast<const DWORD*>(
        base + exports->AddressOfFunctions);
    const auto* names = reinterpret_cast<const DWORD*>(
        base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(
        base + exports->AddressOfNameOrdinals);
    out.names.reserve(exports->NumberOfNames);
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        out.names.emplace_back(
            reinterpret_cast<const char*>(base + names[index]));
    }
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(
            base + functions[ordinals[index]]);
        out.entries.push_back({address, out.names[index].c_str()});
    }
    std::sort(
        out.entries.begin(),
        out.entries.end(),
        [](const NtdllStub& left, const NtdllStub& right) {
            return left.address < right.address;
        });
}

// The nearest export at or below the address, with how far past it the
// sample landed. A syscall stub is a handful of instructions, so a near
// hit names the function outright. A far hit is reported too, because
// ntdll's interesting work - the heap, the loader, lock contention -
// lives in internal routines that are not exported at all, and the
// export in front of one still says roughly where the time went. The
// caller marks those approximate rather than pretending to certainty.
const char* classify_export(
    const ModuleExports& exports,
    std::uintptr_t address,
    std::uintptr_t* distance) {
    if (exports.entries.empty()) {
        return nullptr;
    }
    auto upper = std::upper_bound(
        exports.entries.begin(),
        exports.entries.end(),
        address,
        [](std::uintptr_t value, const NtdllStub& entry) {
            return value < entry.address;
        });
    if (upper == exports.entries.begin()) {
        return nullptr;
    }
    --upper;
    const auto offset = address - upper->address;
    if (offset >= 0x10000) {
        return nullptr;
    }
    if (distance != nullptr) {
        *distance = offset;
    }
    return upper->name;
}

void resolve_ntdll_stubs() {
    static const char* const kNames[] = {
        "NtWaitForSingleObject",
        "NtWaitForMultipleObjects",
        "NtWaitForAlertByThreadId",
        "NtDelayExecution",
        "NtSignalAndWaitForSingleObject",
        "NtWaitForKeyedEvent",
        "NtReleaseKeyedEvent",
        "NtRemoveIoCompletion",
        "NtRemoveIoCompletionEx",
        "NtWaitForWorkViaWorkerFactory",
        "NtAlpcSendWaitReceivePort",
        "NtDeviceIoControlFile",
        "NtReadFile",
        "NtWriteFile",
        "NtYieldExecution",
        "NtQueryPerformanceCounter",
        "NtGetContextThread",
        "NtSuspendThread",
    };
    // Not syscall stubs but ordinary functions, so they need a wider
    // window than the twenty-odd bytes a stub occupies. Both park a
    // thread as thoroughly as any Nt*Wait*, and both were found only
    // once ntdll's whole export table was classified - until then they
    // counted as busy and put two sleeping pools near the top of the
    // ranking.
    static const char* const kWaitFunctions[] = {
        "RtlSleepConditionVariableSRW",
        "RtlSleepConditionVariableCS",
        "AlpcGetMessageFromCompletionList",
    };
    const auto ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll == nullptr) {
        return;
    }
    for (const auto* name : kNames) {
        const auto address = reinterpret_cast<std::uintptr_t>(
            GetProcAddress(ntdll, name));
        if (address != 0) {
            g_ntdll_stubs.push_back({address, name});
        }
    }
    for (const auto* name : kWaitFunctions) {
        const auto address = reinterpret_cast<std::uintptr_t>(
            GetProcAddress(ntdll, name));
        if (address != 0) {
            g_ntdll_wait_functions.push_back({address, name});
        }
    }
    const auto by_address =
        [](const NtdllStub& left, const NtdllStub& right) {
            return left.address < right.address;
        };
    std::sort(g_ntdll_stubs.begin(), g_ntdll_stubs.end(), by_address);
    std::sort(
        g_ntdll_wait_functions.begin(),
        g_ntdll_wait_functions.end(),
        by_address);
}

const char* classify_ntdll(std::uintptr_t address) {
    // A syscall stub is around twenty bytes and the sampled address is
    // the instruction after the syscall, so anything further out than
    // this belongs to a function with no export of its own.
    constexpr std::uintptr_t kStubSpan = 0x40;
    constexpr std::uintptr_t kFunctionSpan = 0x800;
    const char* best = nullptr;
    for (const auto& stub : g_ntdll_stubs) {
        if (stub.address > address) {
            break;
        }
        if (address - stub.address < kStubSpan) {
            best = stub.name;
        }
    }
    if (best != nullptr) {
        return best;
    }
    for (const auto& entry : g_ntdll_wait_functions) {
        if (entry.address > address) {
            break;
        }
        if (address - entry.address < kFunctionSpan) {
            best = entry.name;
        }
    }
    return best;
}

// Reads the module's own section table to learn which of its pages hold
// code. Walking the headers in memory is exact, where asking the memory
// manager page by page would cost a system call per candidate word.
void collect_module_code_ranges(SampleModule& module) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    if (IsBadReadPtr(dos, sizeof(*dos)) != 0 ||
        dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        module.base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (IsBadReadPtr(headers, sizeof(*headers)) != 0 ||
        headers->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    const auto* section = IMAGE_FIRST_SECTION(headers);
    const auto count = headers->FileHeader.NumberOfSections;
    if (IsBadReadPtr(section, sizeof(*section) * count) != 0) {
        return;
    }
    for (WORD index = 0; index < count; ++index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        const auto start = module.base + section->VirtualAddress;
        const auto size = section->Misc.VirtualSize != 0
            ? section->Misc.VirtualSize
            : section->SizeOfRawData;
        module.code.emplace_back(start, start + size);
    }
}

void refresh_sample_modules() {
    std::vector<SampleModule> modules;
    const auto snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    MODULEENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32First(snapshot, &entry)) {
        do {
            SampleModule module = {};
            module.base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
            module.end = module.base + entry.modBaseSize;
            std::string path = entry.szExePath;
            for (auto& character : path) {
                character = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(character)));
            }
            module.system_module =
                path.find("\\windows\\") != std::string::npos;
            std::snprintf(
                module.name, sizeof(module.name), "%s", entry.szModule);
            collect_module_code_ranges(module);
            modules.push_back(module);
        } while (Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    std::sort(
        modules.begin(),
        modules.end(),
        [](const SampleModule& left, const SampleModule& right) {
            return left.base < right.base;
        });
    AcquireSRWLockExclusive(&g_sample_lock);
    g_sample_modules.swap(modules);
    ReleaseSRWLockExclusive(&g_sample_lock);
}

const SampleModule* find_sample_module(
    const std::vector<SampleModule>& modules, std::uintptr_t address) {
    for (const auto& module : modules) {
        if (address >= module.base && address < module.end) {
            return &module;
        }
    }
    return nullptr;
}

// Walks the raw stack rather than unwinding it. Unwinding across managed
// frames needs the runtime's own reader, and all that is wanted here is
// the nearest frame belonging to code this project built.
void describe_sample_caller(
    std::uintptr_t stack_pointer, char* text, std::size_t size) {
    std::snprintf(text, size, "?");
    if (stack_pointer == 0) {
        return;
    }
    std::uint64_t words[256] = {};
    SIZE_T read = 0;
    if (ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(stack_pointer),
            words,
            sizeof(words),
            &read) == 0) {
        return;
    }
    std::vector<SampleModule> modules;
    AcquireSRWLockShared(&g_sample_lock);
    modules = g_sample_modules;
    ReleaseSRWLockShared(&g_sample_lock);
    const auto count = read / sizeof(words[0]);
    // A frame in somebody else's code is still an answer - "none" told us
    // only that four busy threads were not ours, which is the least useful
    // thing it could have said. The first system frame is kept as a
    // fallback and reported when nothing of ours appears above it.
    char fallback[96] = {};
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = static_cast<std::uintptr_t>(words[index]);
        const auto* module = find_sample_module(modules, value);
        if (module == nullptr) {
            continue;
        }
        if (module->system_module) {
            if (fallback[0] == '\0') {
                bool code = false;
                for (const auto& range : module->code) {
                    if (value >= range.first && value < range.second) {
                        code = true;
                        break;
                    }
                }
                if (code) {
                    std::snprintf(
                        fallback,
                        sizeof(fallback),
                        "sys:%s+0x%llX",
                        module->name,
                        static_cast<unsigned long long>(
                            value - module->base));
                }
            }
            continue;
        }
        bool executable = false;
        for (const auto& range : module->code) {
            if (value >= range.first && value < range.second) {
                executable = true;
                break;
            }
        }
        if (!executable) {
            continue;
        }
        std::snprintf(
            text,
            size,
            "%s+0x%llX",
            module->name,
            static_cast<unsigned long long>(value - module->base));
        return;
    }
    if (fallback[0] != '\0') {
        std::snprintf(text, size, "%s", fallback);
        return;
    }
    // Finding nothing at all - not even a system frame - means the stack
    // held no return address this pass could recognise.
    std::snprintf(text, size, "none");
}

// Which module the instruction pointer itself is in, with waiting split out
// so a parked thread cannot be mistaken for a busy one.
void describe_sample_region(
    std::uintptr_t address,
    char* text,
    std::size_t size) {
    std::vector<SampleModule> modules;
    AcquireSRWLockShared(&g_sample_lock);
    modules = g_sample_modules;
    ReleaseSRWLockShared(&g_sample_lock);
    const auto* module = find_sample_module(modules, address);
    if (module == nullptr) {
        // Guest code and anything else mapped without a module - which is
        // where the recompiled title runs. The page is kept because the
        // bucket alone cannot tell forty seconds of loading from forty
        // seconds spinning on a fence the GPU never writes: work walks
        // over many pages, a spin sits on one.
        std::snprintf(
            text,
            size,
            "unmapped:0x%012llX",
            static_cast<unsigned long long>(address & ~0xFFFULL));
        return;
    }
    const auto* wait = classify_ntdll(address);
    if (wait != nullptr) {
        std::snprintf(text, size, "%s:waiting", module->name);
        return;
    }
    // Each table only describes its own module, so pick by name rather
    // than letting a distant export claim an address in another image.
    std::uintptr_t distance = 0;
    const char* stub = nullptr;
    if (_stricmp(module->name, "win32u.dll") == 0) {
        stub = classify_export(g_win32u_exports, address, &distance);
    } else if (_stricmp(module->name, "ntdll.dll") == 0) {
        stub = classify_export(g_ntdll_exports, address, &distance);
    }
    if (stub == nullptr) {
        std::snprintf(text, size, "%s", module->name);
        return;
    }
    // A tilde means the sample is far enough past the export that it is
    // most likely an unexported routine following it, not the export.
    std::snprintf(
        text,
        size,
        "%s:%s%s",
        module->name,
        distance < 0x800 ? "" : "~",
        stub);
}

// The site ranking is owned by the parked threads - eighty-seven of them,
// all on the same page - so the few that are actually running never appear.
// This one lists only the samples that were not in a wait.
void report_busy_sites() {
    std::vector<std::pair<std::uint64_t, std::string>> ranked;
    AcquireSRWLockShared(&g_sample_lock);
    for (const auto& entry : g_sample_sites) {
        if (entry.first.find("wait=- ") == std::string::npos) {
            continue;
        }
        ranked.emplace_back(entry.second, entry.first);
    }
    ReleaseSRWLockShared(&g_sample_lock);
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    std::string text;
    for (std::size_t index = 0;
         index < ranked.size() && index < 10 && text.size() < 900;
         ++index) {
        char item[224] = {};
        std::snprintf(
            item,
            sizeof(item),
            " [%llu %s]",
            static_cast<unsigned long long>(ranked[index].first),
            ranked[index].second.c_str());
        text += item;
    }
    trace_stderr(
        "sampler.busy sites=%zu%s\n",
        ranked.size(),
        text.c_str());
}

void report_sample_regions() {
    std::vector<std::pair<std::uint64_t, std::string>> ranked;
    AcquireSRWLockShared(&g_sample_lock);
    for (const auto& entry : g_sample_regions) {
        ranked.emplace_back(entry.second, entry.first);
    }
    ReleaseSRWLockShared(&g_sample_lock);
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    std::string text;
    std::uint64_t total = 0;
    for (const auto& entry : ranked) {
        total += entry.first;
    }
    for (std::size_t index = 0;
         index < ranked.size() && index < 10;
         ++index) {
        char item[112] = {};
        std::snprintf(
            item,
            sizeof(item),
            " %s=%llu",
            ranked[index].second.c_str(),
            static_cast<unsigned long long>(ranked[index].first));
        text += item;
    }
    trace_stderr(
        "sampler.region total=%llu threads=%u%s\n",
        static_cast<unsigned long long>(total),
        g_sample_thread_count.load(std::memory_order_relaxed),
        text.c_str());
}

// Guest code, by page. The count of distinct pages is the answer on its
// own: a title loading a level walks over hundreds of them, a title
// spinning on a fence sits on one or two, and the region bucket cannot
// tell those apart because both read as "unmapped".
// Watching a handful of guest addresses. Four guest threads sit on the
// page holding a test-and-set lock and the page holding the function that
// lock guards never appears in the profile at all, which says they are
// not queueing for it - they never get it. That is a claim about one byte
// in guest memory, so read the byte. The addresses come from
// PS5RECOMP_WATCH_GUEST as a comma separated hex list rather than being
// written into the runtime: which address matters is a property of the
// title, and nothing title-specific belongs here.
std::vector<std::uint64_t> g_watch_addresses;

void parse_watch_addresses() {
    char value[512] = {};
    const auto length = GetEnvironmentVariableA(
        "PS5RECOMP_WATCH_GUEST",
        value,
        static_cast<DWORD>(sizeof(value)));
    if (length == 0 || length >= sizeof(value)) {
        return;
    }
    const char* cursor = value;
    while (*cursor != '\0') {
        char* end = nullptr;
        const auto parsed = std::strtoull(cursor, &end, 16);
        if (end == cursor) {
            break;
        }
        g_watch_addresses.push_back(parsed);
        cursor = end;
        while (*cursor == ',' || *cursor == ' ') {
            ++cursor;
        }
    }
}

void report_watch_addresses() {
    if (g_watch_addresses.empty()) {
        return;
    }
    std::string text;
    for (const auto address : g_watch_addresses) {
        std::uint8_t byte = 0;
        std::uint64_t qword = 0;
        SIZE_T read_bytes = 0;
        const auto readable = ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(address),
            &qword,
            sizeof(qword),
            &read_bytes) != 0 && read_bytes == sizeof(qword);
        byte = static_cast<std::uint8_t>(qword & 0xFF);
        char item[64] = {};
        std::snprintf(
            item,
            sizeof(item),
            " 0x%llX=%u/0x%016llX",
            static_cast<unsigned long long>(address),
            readable ? byte : 255u,
            readable ? static_cast<unsigned long long>(qword) : 0ULL);
        text += item;
    }
    trace_stderr("sampler.watch%s\n", text.c_str());
}

// Guest code broken out per thread. The aggregate says two thirds of the
// guest's CPU sits on one page, which is the title's heap lock, but it
// cannot say whether that is every guest thread waiting on a livelock or
// a few waiting while one does real work. Those are opposite diagnoses
// and only the per-thread split separates them.
void report_guest_threads() {
    std::vector<SampleModule> modules;
    std::map<DWORD, std::pair<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>>>
        per_thread;
    AcquireSRWLockShared(&g_sample_lock);
    modules = g_sample_modules;
    for (const auto& entry : g_sample_counts) {
        const auto page = entry.first.second;
        if (find_sample_module(modules, page) != nullptr) {
            continue;
        }
        auto& slot = per_thread[entry.first.first];
        slot.first += entry.second;
        if (entry.second > slot.second.second) {
            slot.second = {page, entry.second};
        }
    }
    ReleaseSRWLockShared(&g_sample_lock);
    if (per_thread.empty()) {
        return;
    }
    std::vector<std::pair<std::uint64_t, std::pair<DWORD, std::pair<std::uint64_t, std::uint64_t>>>>
        ranked;
    for (const auto& entry : per_thread) {
        ranked.emplace_back(
            entry.second.first,
            std::make_pair(entry.first, entry.second.second));
    }
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    std::string text;
    for (std::size_t index = 0;
         index < ranked.size() && index < 10;
         ++index) {
        char item[96] = {};
        std::snprintf(
            item,
            sizeof(item),
            " tid=%lu:%llu@0x%llX:%llu",
            static_cast<unsigned long>(ranked[index].second.first),
            static_cast<unsigned long long>(ranked[index].first),
            static_cast<unsigned long long>(
                ranked[index].second.second.first),
            static_cast<unsigned long long>(
                ranked[index].second.second.second));
        text += item;
    }
    trace_stderr(
        "sampler.guest_threads threads=%zu%s\n",
        ranked.size(),
        text.c_str());
}

// The guest image is mapped by hand, so its code belongs to no module and
// every host-side unwinder stops at it. The executable segments are known
// from the manifest, which is enough to tell a return address apart from
// the stack noise around it.
bool is_guest_code(std::uint64_t address) {
    for (const auto& segment : generated::kSegments) {
        if ((segment.protection & 0x1U) == 0 || segment.memory_size == 0) {
            continue;
        }
        if (address >= segment.virtual_address &&
            address < segment.virtual_address + segment.memory_size) {
            return true;
        }
    }
    return false;
}

// A stack scan finds return addresses but not their order, and order is
// the whole question here: the same lock is taken from several callers.
// The title is built with frame pointers - every guest function seen so
// far opens with push rbp / mov rbp, rsp - so the chain can be walked
// exactly, and a frame that does not look like one ends the walk rather
// than guessing.
void describe_guest_stack(
    std::uint64_t frame_pointer,
    std::uint64_t instruction_pointer,
    char* text,
    std::size_t size) {
    std::string chain;
    char head[24] = {};
    std::snprintf(
        head,
        sizeof(head),
        "0x%llX",
        static_cast<unsigned long long>(instruction_pointer));
    chain = head;
    std::uint64_t frame = frame_pointer;
    for (int depth = 0; depth < 12; ++depth) {
        if (frame == 0 || (frame & 0x7ULL) != 0) {
            break;
        }
        std::uint64_t slots[2] = {};
        SIZE_T read = 0;
        if (ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<LPCVOID>(frame),
                slots,
                sizeof(slots),
                &read) == 0 ||
            read != sizeof(slots)) {
            break;
        }
        if (!is_guest_code(slots[1])) {
            break;
        }
        char item[24] = {};
        std::snprintf(
            item,
            sizeof(item),
            "<0x%llX",
            static_cast<unsigned long long>(slots[1]));
        chain += item;
        if (slots[0] <= frame) {
            break;
        }
        frame = slots[0];
    }
    std::snprintf(text, size, "%s", chain.c_str());
}

// Which guest call chains the samples land in. Six threads spin on one
// page; the page says where they are and this says how they got there.
void report_guest_stacks() {
    std::vector<std::pair<std::uint64_t, std::string>> ranked;
    AcquireSRWLockShared(&g_sample_lock);
    for (const auto& entry : g_guest_stacks) {
        ranked.emplace_back(entry.second, entry.first);
    }
    ReleaseSRWLockShared(&g_sample_lock);
    if (ranked.empty()) {
        return;
    }
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    for (std::size_t index = 0;
         index < ranked.size() && index < 8;
         ++index) {
        trace_stderr(
            "sampler.guest_stack rank=%zu count=%llu chain=%s\n",
            index,
            static_cast<unsigned long long>(ranked[index].first),
            ranked[index].second.c_str());
    }
}

// One page can hold both the spin of a lock and its acquire, so the page
// histogram cannot tell a thread that is waiting for the lock from one
// that takes it, finds nothing to do and releases it again. Those are
// different bugs. The exact instruction separates them.
void report_guest_sites() {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranked;
    std::uint64_t total = 0;
    AcquireSRWLockShared(&g_sample_lock);
    for (const auto& entry : g_guest_sites) {
        ranked.emplace_back(entry.second, entry.first);
        total += entry.second;
    }
    ReleaseSRWLockShared(&g_sample_lock);
    if (ranked.empty()) {
        return;
    }
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    std::string text;
    for (std::size_t index = 0;
         index < ranked.size() && index < 16;
         ++index) {
        char item[48] = {};
        std::snprintf(
            item,
            sizeof(item),
            " 0x%llX=%llu",
            static_cast<unsigned long long>(ranked[index].second),
            static_cast<unsigned long long>(ranked[index].first));
        text += item;
    }
    trace_stderr(
        "sampler.guest_sites sites=%zu samples=%llu%s\n",
        ranked.size(),
        static_cast<unsigned long long>(total),
        text.c_str());
}

void report_guest_pages() {
    std::vector<std::pair<std::uint64_t, std::string>> ranked;
    std::uint64_t total = 0;
    AcquireSRWLockShared(&g_sample_lock);
    for (const auto& entry : g_sample_regions) {
        if (entry.first.rfind("unmapped:", 0) != 0) {
            continue;
        }
        ranked.emplace_back(entry.second, entry.first);
        total += entry.second;
    }
    ReleaseSRWLockShared(&g_sample_lock);
    if (ranked.empty()) {
        return;
    }
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    std::string text;
    for (std::size_t index = 0;
         index < ranked.size() && index < 12;
         ++index) {
        char item[64] = {};
        std::snprintf(
            item,
            sizeof(item),
            " %s=%llu",
            ranked[index].second.c_str() + sizeof("unmapped:") - 1,
            static_cast<unsigned long long>(ranked[index].first));
        text += item;
    }
    trace_stderr(
        "sampler.guest pages=%zu samples=%llu%s\n",
        ranked.size(),
        static_cast<unsigned long long>(total),
        text.c_str());
}

void report_sample_sites() {
    std::vector<std::pair<std::uint64_t, std::string>> ranked;
    AcquireSRWLockExclusive(&g_sample_lock);
    for (const auto& entry : g_sample_sites) {
        ranked.emplace_back(entry.second, entry.first);
    }
    ReleaseSRWLockExclusive(&g_sample_lock);
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    for (std::size_t index = 0; index < ranked.size() && index < 8;
         ++index) {
        trace_stderr(
            "sampler.site rank=%zu count=%llu %s\n",
            index + 1,
            static_cast<unsigned long long>(ranked[index].first),
            ranked[index].second.c_str());
    }

    // Ranking by raw count answers the wrong question. An idle thread
    // parks in one place for the whole run and holds a slot with a large
    // count, while a thread doing work spreads its samples over many
    // sites and never appears at all. This second list keeps only sites
    // that resolved to one of our own modules, which is where work is.
    std::size_t shown = 0;
    for (const auto& entry : ranked) {
        if (entry.second.find("caller=none") != std::string::npos ||
            entry.second.find("caller=?") != std::string::npos) {
            continue;
        }
        trace_stderr(
            "sampler.ours rank=%zu count=%llu %s\n",
            ++shown,
            static_cast<unsigned long long>(entry.first),
            entry.second.c_str());
        if (shown >= 12) {
            break;
        }
    }
    if (shown == 0) {
        trace_stderr("sampler.ours none buckets=%zu\n", ranked.size());
    }
}

void report_samples(std::uint64_t total) {
    std::vector<std::pair<std::uint64_t,
                          std::pair<DWORD, std::uint64_t>>> ranked;
    AcquireSRWLockExclusive(&g_sample_lock);
    for (const auto& entry : g_sample_counts) {
        ranked.emplace_back(entry.second, entry.first);
    }
    ReleaseSRWLockExclusive(&g_sample_lock);
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    std::string text;
    for (std::size_t index = 0;
         index < ranked.size() && index < 8;
         ++index) {
        // A bare address says nothing: most samples land in whatever
        // module the thread is parked in, and telling a busy loop from
        // a wait needs the module's name.
        const auto thread_id = ranked[index].second.first;
        const auto address = ranked[index].second.second;
        char module_name[64] = "guest-or-unknown";
        HMODULE module = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(address),
                &module) != 0 &&
            module != nullptr) {
            char full[MAX_PATH] = {};
            if (GetModuleFileNameA(module, full, sizeof(full)) != 0) {
                const auto* leaf = std::strrchr(full, '\\');
                std::snprintf(
                    module_name,
                    sizeof(module_name),
                    "%s+0x%llX",
                    leaf != nullptr ? leaf + 1 : full,
                    static_cast<unsigned long long>(
                        address -
                        reinterpret_cast<std::uintptr_t>(module)));
            }
        }
        char entry[112] = {};
        std::snprintf(
            entry,
            sizeof(entry),
            " tid=%lu:%s=%llu",
            static_cast<unsigned long>(thread_id),
            module_name,
            static_cast<unsigned long long>(ranked[index].first));
        text += entry;
    }
    trace_stderr(
        "sampler.top samples=%llu buckets=%zu%s\n",
        static_cast<unsigned long long>(total),
        ranked.size(),
        text.c_str());
    report_sample_sites();
    report_sample_regions();
    report_guest_pages();
    report_guest_sites();
    report_guest_stacks();
    report_guest_threads();
    report_watch_addresses();
    report_busy_sites();
}

DWORD WINAPI sampler_thread(LPVOID parameter) {
    const auto period =
        static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(parameter));
    const auto own = GetCurrentThreadId();
    const auto process = GetCurrentProcessId();
    resolve_ntdll_stubs();
    resolve_module_exports("win32u.dll", g_win32u_exports);
    resolve_module_exports("ntdll.dll", g_ntdll_exports);
    parse_watch_addresses();
    refresh_sample_modules();
    std::uint64_t total = 0;
    std::uint64_t since_report = 0;
    while (!g_sampler_stop.load(std::memory_order_relaxed)) {
        const auto snapshot =
            CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        // The frame interval grows by several seconds every frame while
        // the traced work per frame does not move at all, so something
        // outside the trace accumulates. A thread count is the cheapest
        // candidate to rule in or out: more threads contending for the
        // title's heap lock would look exactly like this.
        std::uint32_t threads_seen = 0;
        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry = {};
            entry.dwSize = sizeof(entry);
            if (Thread32First(snapshot, &entry)) {
                do {
                    if (entry.th32OwnerProcessID != process ||
                        entry.th32ThreadID == own) {
                        continue;
                    }
                    ++threads_seen;
                    const auto thread = OpenThread(
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                        FALSE,
                        entry.th32ThreadID);
                    if (thread == nullptr) {
                        continue;
                    }
                    if (SuspendThread(thread) != (DWORD)-1) {
                        CONTEXT context = {};
                        context.ContextFlags =
                            CONTEXT_CONTROL | CONTEXT_INTEGER;
                        if (GetThreadContext(thread, &context)) {
                            // Bucket by 4 KB page: individual addresses
                            // scatter across a loop body and hide it.
                            const auto page = context.Rip & ~0xFFFULL;
                            const auto* wait = classify_ntdll(
                                static_cast<std::uintptr_t>(context.Rip));
                            char caller[96] = {};
                            describe_sample_caller(
                                static_cast<std::uintptr_t>(context.Rsp),
                                caller,
                                sizeof(caller));
                            char site[192] = {};
                            char region[80] = {};
                            char guest_stack[192] = {};
                            describe_guest_stack(
                                context.Rbp,
                                context.Rip,
                                guest_stack,
                                sizeof(guest_stack));
                            describe_sample_region(
                                static_cast<std::uintptr_t>(context.Rip),
                                region,
                                sizeof(region));
                            std::snprintf(
                                site,
                                sizeof(site),
                                "tid=%lu wait=%s region=%s caller=%s",
                                static_cast<unsigned long>(
                                    entry.th32ThreadID),
                                wait != nullptr ? wait : "-",
                                region,
                                caller);
                            AcquireSRWLockExclusive(&g_sample_lock);
                            ++g_sample_counts[{entry.th32ThreadID, page}];
                            if (std::strncmp(region, "unmapped:", 9) == 0) {
                                ++g_guest_sites[context.Rip];
                                if (is_guest_code(context.Rip)) {
                                    ++g_guest_stacks[guest_stack];
                                }
                            }
                            ++g_sample_sites[site];
                            ++g_sample_regions[region];
                            ReleaseSRWLockExclusive(&g_sample_lock);
                            ++total;
                            ++since_report;
                        }
                        ResumeThread(thread);
                    }
                    CloseHandle(thread);
                } while (Thread32Next(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
        g_sample_thread_count.store(
            threads_seen, std::memory_order_relaxed);
        if (since_report >= 2000) {
            report_samples(total);
            since_report = 0;
            // Modules keep arriving after start - managed assemblies load
            // lazily - so a table read once would miss the ones that
            // matter most.
            refresh_sample_modules();
        }
        Sleep(period);
    }
    report_samples(total);
    return 0;
}

void start_sampler() {
    char value[8] = {};
    const auto length = GetEnvironmentVariableA(
        "PS5RECOMP_SAMPLE_MS", value, static_cast<DWORD>(sizeof(value)));
    if (length == 0 || length >= sizeof(value)) {
        return;
    }
    char* end = nullptr;
    const auto period = std::strtoul(value, &end, 0);
    if (end == value || period == 0 || period > 10000) {
        return;
    }
    const auto thread = CreateThread(
        nullptr,
        0,
        sampler_thread,
        reinterpret_cast<LPVOID>(
            static_cast<std::uintptr_t>(period)),
        0,
        nullptr);
    if (thread != nullptr) {
        CloseHandle(thread);
        trace_stderr("sampler.started period_ms=%lu\n", period);
    }
}


// Which threads are inside a managed bridge call, and since when. The
// managed side has a watchdog for exactly this, but it lives in
// HleCallTrace, and enabling HleCallTrace is what makes the run fast - so
// it can never observe the slow case it was built to explain. This one
// costs two locked map operations per call at ~1500 calls a second and is
// always on, so both cases are visible.
struct OutstandingCall {
    std::string nid;
    std::uint64_t started_ms;
};

SRWLOCK g_outstanding_call_lock = SRWLOCK_INIT;
std::map<DWORD, OutstandingCall> g_outstanding_calls;

void begin_bridge_call(const char* nid) {
    AcquireSRWLockExclusive(&g_outstanding_call_lock);
    g_outstanding_calls[GetCurrentThreadId()] =
        OutstandingCall{nid, GetTickCount64()};
    ReleaseSRWLockExclusive(&g_outstanding_call_lock);
}

void end_bridge_call() {
    AcquireSRWLockExclusive(&g_outstanding_call_lock);
    g_outstanding_calls.erase(GetCurrentThreadId());
    ReleaseSRWLockExclusive(&g_outstanding_call_lock);
}

void report_outstanding_bridge_calls(std::uint64_t minimum_ms) {
    const auto now = GetTickCount64();
    std::string text;
    AcquireSRWLockExclusive(&g_outstanding_call_lock);
    for (const auto& entry : g_outstanding_calls) {
        const auto age = now - entry.second.started_ms;
        if (age < minimum_ms || text.size() > 600) {
            continue;
        }
        char line[96] = {};
        std::snprintf(
            line,
            sizeof(line),
            " tid=%lu:%s:%llums",
            static_cast<unsigned long>(entry.first),
            entry.second.nid.c_str(),
            static_cast<unsigned long long>(age));
        text += line;
    }
    ReleaseSRWLockExclusive(&g_outstanding_call_lock);
    if (!text.empty()) {
        trace_stderr("sharpemu_hle_blocked%s\n", text.c_str());
    }
}

// Per-NID bridge call counts, sampled periodically so a stall can be read
// as "the guest is spinning on X" rather than as an absence of evidence.
SRWLOCK g_bridge_call_lock = SRWLOCK_INIT;
std::map<std::string, std::uint64_t> g_bridge_calls;

// Also per thread. The aggregate mix is drowned by the audio loop, which
// calls seven NIDs at audio rate on one thread, and what is wanted is
// the opposite cut: which thread is the renderer, which are workers, and
// what each of them is asking for while the title draws nothing.
std::map<DWORD, std::map<std::string, std::uint64_t>> g_bridge_calls_by_thread;

void record_bridge_call(const char* nid) {
    AcquireSRWLockExclusive(&g_bridge_call_lock);
    ++g_bridge_calls[nid];
    ++g_bridge_calls_by_thread[GetCurrentThreadId()][nid];
    ReleaseSRWLockExclusive(&g_bridge_call_lock);
}

void report_bridge_calls_by_thread() {
    std::vector<std::pair<std::uint64_t, std::string>> ranked;
    AcquireSRWLockExclusive(&g_bridge_call_lock);
    for (const auto& thread : g_bridge_calls_by_thread) {
        std::uint64_t total = 0;
        std::vector<std::pair<std::uint64_t, const std::string*>> nids;
        for (const auto& call : thread.second) {
            total += call.second;
            nids.emplace_back(call.second, &call.first);
        }
        std::sort(nids.begin(), nids.end(), std::greater<>());
        char line[160] = {};
        int offset = std::snprintf(
            line,
            sizeof(line),
            " tid=%lu:%llu[",
            static_cast<unsigned long>(thread.first),
            static_cast<unsigned long long>(total));
        for (std::size_t index = 0;
             index < nids.size() && index < 3 &&
                 offset < static_cast<int>(sizeof(line)) - 20;
             ++index) {
            offset += std::snprintf(
                line + offset,
                sizeof(line) - static_cast<std::size_t>(offset),
                "%s%s=%llu",
                index == 0 ? "" : ",",
                nids[index].second->c_str(),
                static_cast<unsigned long long>(nids[index].first));
        }
        std::snprintf(
            line + offset,
            sizeof(line) - static_cast<std::size_t>(offset),
            "]");
        ranked.emplace_back(total, line);
    }
    ReleaseSRWLockExclusive(&g_bridge_call_lock);
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    std::string text;
    for (std::size_t index = 0;
         index < ranked.size() && index < 8 && text.size() < 700;
         ++index) {
        text += ranked[index].second;
    }
    trace_stderr(
        "sharpemu_hle_threads threads=%zu%s\n",
        ranked.size(),
        text.c_str());
}

void report_bridge_call_mix(std::uint32_t dispatch_index) {
    std::vector<std::pair<std::uint64_t, std::string>> ranked;
    AcquireSRWLockExclusive(&g_bridge_call_lock);
    ranked.reserve(g_bridge_calls.size());
    for (const auto& entry : g_bridge_calls) {
        ranked.emplace_back(entry.second, entry.first);
    }
    ReleaseSRWLockExclusive(&g_bridge_call_lock);
    std::sort(ranked.begin(), ranked.end(), std::greater<>());

    // Eight was not enough to see anything but the audio loop, which
    // calls seven NIDs at audio rate and buries everything else. The
    // interesting question is which of the rest grows from frame to
    // frame, and that needs the tail. Twenty-four was not enough either:
    // this title calls 162 distinct imports and the loading ones sit
    // below the audio rate, so the whole ranking goes out, in chunks that
    // fit trace_stderr's line.
    constexpr std::size_t kPerLine = 24;
    const auto parts = (ranked.size() + kPerLine - 1) / kPerLine;
    for (std::size_t part = 0; part < parts; ++part) {
        std::string text;
        const auto begin = part * kPerLine;
        const auto end = std::min(begin + kPerLine, ranked.size());
        for (auto index = begin; index < end; ++index) {
            char entry[64] = {};
            std::snprintf(
                entry,
                sizeof(entry),
                " %s=%llu",
                ranked[index].second.c_str(),
                static_cast<unsigned long long>(ranked[index].first));
            text += entry;
        }
        trace_stderr(
            "sharpemu_hle_mix calls=%u uptime_ms=%llu nids=%zu "
            "part=%zu/%zu%s\n",
            dispatch_index,
            static_cast<unsigned long long>(GetTickCount64()),
            ranked.size(),
            part,
            parts,
            text.c_str());
    }
}

bool environment_flag_enabled(const char* name) {
    char value[8] = {};
    const auto length = GetEnvironmentVariableA(
        name,
        value,
        static_cast<DWORD>(sizeof(value)));
    return length == 1 && value[0] == '1';
}

// Any functions asked for by name, as a comma separated list of NIDs in
// PS5RT_TRACE_HLE_NIDS: every call and its result, however many there are.
// The per-call trace otherwise stops after the first 128, which is long
// before a title reaches its intro video. Read once into a plain buffer:
// the dispatcher that asks has its own unwind data, and a guarded static
// string there is something the assembler refuses.
bool hle_nid_traced_by_name(const char* nid) {
    static char list[1024] = {};
    static LONG state = 0;
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        list[0] = ',';
        const auto length = GetEnvironmentVariableA(
            "PS5RT_TRACE_HLE_NIDS", list + 1, sizeof(list) - 3);
        if (length == 0 || length >= sizeof(list) - 3) {
            list[0] = '\0';
        } else {
            list[length + 1] = ',';
            list[length + 2] = '\0';
        }
        InterlockedExchange(&state, 2);
    }
    while (InterlockedCompareExchange(&state, 2, 2) != 2) {
        YieldProcessor();
    }
    if (list[0] == '\0' || nid == nullptr) {
        return false;
    }
    char needle[32] = {};
    const auto written = std::snprintf(needle, sizeof(needle), ",%s,", nid);
    return written > 0 && written < static_cast<int>(sizeof(needle)) &&
        std::strstr(list, needle) != nullptr;
}

void trace_stderr(const char* format, ...) {
    static const bool trace_hle_calls =
        environment_flag_enabled("PS5RECOMP_TRACE_HLE") ||
        environment_flag_enabled("PS5RT_TRACE_HLE_CALLS");
    if (!trace_hle_calls && std::strncmp(format, "hle=", 4) == 0) {
        return;
    }

    char buffer[896] = {};
    // PS5RT_TRACE_TIME stamps each line with system uptime, the one clock all
    // modules in this process share, so runner, HLE and GPU traces land on a
    // single comparable timeline. Off by default: astro-cycle.ps1 parses this
    // stream.
    static const bool trace_time =
        environment_flag_enabled("PS5RT_TRACE_TIME");
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

    const auto bytes = static_cast<DWORD>(
        std::min<int>(
            offset + length,
            static_cast<int>(sizeof(buffer) - 1)));
    DWORD written = 0;
    WriteFile(
        GetStdHandle(STD_ERROR_HANDLE),
        buffer,
        bytes,
        &written,
        nullptr);
    const auto guest_thread_pointer = get_guest_thread_pointer();
    if (guest_thread_pointer != 0 &&
        InterlockedCompareExchange(&g_crash_trace_written, 0, 0) == 0) {
        write_fs_base_raw(guest_thread_pointer);
    }
}

bool try_recover_guest_fs(EXCEPTION_POINTERS* exception) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto* record = exception->ExceptionRecord;
    auto* context = exception->ContextRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->NumberParameters < 2 ||
        record->ExceptionInformation[1] != 0 ||
        read_fs_base_raw() != 0) {
        return false;
    }

    const auto guest_thread_pointer = get_guest_thread_pointer();
    if (guest_thread_pointer == 0) {
        return false;
    }

    std::array<std::uint8_t, 16> instruction = {};
    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(context->Rip),
            instruction.data(),
            instruction.size(),
            &bytes_read) ||
        bytes_read == 0) {
        return false;
    }

    bool has_fs_prefix = false;
    SIZE_T opcode_index = 0;
    for (; opcode_index < bytes_read; ++opcode_index) {
        const auto byte = instruction[opcode_index];
        if (byte == 0x64) {
            has_fs_prefix = true;
            continue;
        }
        const bool is_legacy_prefix =
            byte == 0xF0 || byte == 0xF2 || byte == 0xF3 ||
            byte == 0x2E || byte == 0x36 || byte == 0x3E ||
            byte == 0x26 || byte == 0x65 || byte == 0x66 ||
            byte == 0x67;
        const bool is_rex_prefix = byte >= 0x40 && byte <= 0x4F;
        if (!is_legacy_prefix && !is_rex_prefix) {
            break;
        }
    }
    constexpr SIZE_T kMovFsZeroOpcodeSize = 7;
    if (!has_fs_prefix ||
        opcode_index + kMovFsZeroOpcodeSize > bytes_read ||
        instruction[opcode_index] != 0x8B ||
        instruction[opcode_index + 1] != 0x04 ||
        instruction[opcode_index + 2] != 0x25 ||
        instruction[opcode_index + 3] != 0x00 ||
        instruction[opcode_index + 4] != 0x00 ||
        instruction[opcode_index + 5] != 0x00 ||
        instruction[opcode_index + 6] != 0x00) {
        return false;
    }

    context->Rax = guest_thread_pointer;
    context->Rip += opcode_index + kMovFsZeroOpcodeSize;
    const auto recovery_count =
        g_guest_fs_recovery_count.fetch_add(
            1,
            std::memory_order_relaxed) + 1;
    const auto trace_all =
        environment_flag_enabled("PS5RT_TRACE_FS_RECOVERY");
    const auto trace_sample =
        recovery_count <= 4 ||
        (recovery_count & (recovery_count - 1)) == 0;
    if (trace_all || trace_sample) {
        trace_stderr(
            "emulated_guest_fs_load count=%llu "
            "rip=0x%016llX fs=0x%016llX\n",
            static_cast<unsigned long long>(recovery_count),
            static_cast<unsigned long long>(
                context->Rip - opcode_index - kMovFsZeroOpcodeSize),
            static_cast<unsigned long long>(guest_thread_pointer));
    } else {
        write_fs_base_raw(guest_thread_pointer);
    }
    return true;
#else
    (void)exception;
    return false;
#endif
}

void* allocate_guest_near_code(SIZE_T size) {
    std::uint64_t lowest = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t highest = 0;
    for (const auto& segment : generated::kSegments) {
        if (segment.memory_size == 0 ||
            segment.virtual_address < 0x0000000700000000ULL ||
            segment.virtual_address >= 0x0000001000000000ULL) {
            continue;
        }
        lowest = std::min(lowest, segment.virtual_address);
        highest = std::max(
            highest,
            segment.virtual_address + segment.memory_size);
    }
    if (lowest == std::numeric_limits<std::uint64_t>::max()) {
        return nullptr;
    }

    const auto aligned_size = static_cast<SIZE_T>(
        align_up(size, kAllocationGranularity));
    const auto first_candidate =
        align_up(highest, kAllocationGranularity);
    constexpr std::uint64_t kRel32Reach = 0x70000000ULL;
    for (std::uint64_t delta = 0;
         delta <= kRel32Reach;
         delta += 0x00100000ULL) {
        const auto candidate = first_candidate + delta;
        if (candidate >= lowest &&
            candidate - lowest <=
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int32_t>::max())) {
            if (auto* allocation = VirtualAlloc(
                    reinterpret_cast<void*>(candidate),
                    aligned_size,
                    MEM_RESERVE | MEM_COMMIT,
                    PAGE_EXECUTE_READWRITE)) {
                return allocation;
            }
        }
        if (delta != 0 && lowest > delta) {
            const auto below = align_down(
                lowest - delta,
                kAllocationGranularity);
            if (highest >= below &&
                highest - below <=
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int32_t>::max())) {
                if (auto* allocation = VirtualAlloc(
                        reinterpret_cast<void*>(below),
                        aligned_size,
                        MEM_RESERVE | MEM_COMMIT,
                        PAGE_EXECUTE_READWRITE)) {
                    return allocation;
                }
            }
        }
    }
    return nullptr;
}

void* create_guest_tls_load_handler() {
    constexpr SIZE_T kHandlerRegionSize = kAllocationGranularity;
    auto* code = static_cast<std::uint8_t*>(
        allocate_guest_near_code(kHandlerRegionSize));
    if (code == nullptr) {
        return nullptr;
    }

    std::size_t offset = 0;
    auto emit = [&](std::uint8_t value) {
        code[offset++] = value;
    };
    emit(0x9C);                         // pushfq
    emit(0x51);                         // push rcx
    emit(0x52);                         // push rdx
    emit(0x41); emit(0x50);             // push r8
    emit(0x41); emit(0x51);             // push r9
    emit(0x41); emit(0x52);             // push r10
    emit(0x41); emit(0x53);             // push r11
    emit(0x48); emit(0x83); emit(0xEC); emit(0x20);
    emit(0xB9);                         // mov ecx, TLS index
    std::memcpy(
        code + offset,
        &g_guest_tls_base_tls_index,
        sizeof(g_guest_tls_base_tls_index));
    offset += sizeof(g_guest_tls_base_tls_index);
    emit(0x48); emit(0xB8);             // mov rax, TlsGetValue
    const auto tls_get_value_address =
        reinterpret_cast<std::uint64_t>(&TlsGetValue);
    std::memcpy(
        code + offset,
        &tls_get_value_address,
        sizeof(tls_get_value_address));
    offset += sizeof(tls_get_value_address);
    emit(0xFF); emit(0xD0);             // call rax
    emit(0x48); emit(0x83); emit(0xC4); emit(0x20);
    emit(0x41); emit(0x5B);             // pop r11
    emit(0x41); emit(0x5A);             // pop r10
    emit(0x41); emit(0x59);             // pop r9
    emit(0x41); emit(0x58);             // pop r8
    emit(0x5A);                         // pop rdx
    emit(0x59);                         // pop rcx
    emit(0x9D);                         // popfq
    emit(0xC3);                         // ret

    DWORD old_protection = 0;
    if (!VirtualProtect(
            code,
            kHandlerRegionSize,
            PAGE_EXECUTE_READ,
            &old_protection)) {
        VirtualFree(code, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), code, offset);
    trace_stderr(
        "guest_tls_load_handler=0x%016llX bytes=%zu tls_index=%lu\n",
        static_cast<unsigned long long>(
            reinterpret_cast<std::uint64_t>(code)),
        offset,
        static_cast<unsigned long>(g_guest_tls_base_tls_index));
    return code;
}

bool patch_guest_tls_load(
    std::uint8_t* instruction,
    std::size_t available,
    std::size_t& instruction_length) {
    instruction_length = 0;
    if (available < 8 || g_guest_tls_load_handler == nullptr) {
        return false;
    }

    std::size_t offset = 0;
    while (offset < available && instruction[offset] == 0x66) {
        ++offset;
    }
    if (offset >= available || instruction[offset++] != 0x64) {
        return false;
    }

    std::uint8_t rex = 0;
    if (offset < available &&
        instruction[offset] >= 0x40 &&
        instruction[offset] <= 0x4F) {
        rex = instruction[offset++];
    }
    if (offset + 7 > available || instruction[offset] != 0x8B) {
        return false;
    }

    const auto mod_rm = instruction[offset + 1];
    if ((mod_rm >> 6) != 0 ||
        (mod_rm & 7) != 4 ||
        instruction[offset + 2] != 0x25) {
        return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(
        &displacement,
        instruction + offset + 3,
        sizeof(displacement));
    if (displacement != 0) {
        return false;
    }

    instruction_length = offset + 7;
    const auto destination_register =
        ((mod_rm >> 3) & 7) | ((rex & 4) != 0 ? 8 : 0);
    if (destination_register != 0 && instruction_length < 8) {
        return false;
    }

    const auto source =
        reinterpret_cast<std::int64_t>(instruction + 5);
    const auto target =
        reinterpret_cast<std::int64_t>(g_guest_tls_load_handler);
    const auto relative = target - source;
    if (relative < std::numeric_limits<std::int32_t>::min() ||
        relative > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(
            instruction,
            instruction_length,
            PAGE_EXECUTE_READWRITE,
            &old_protection)) {
        return false;
    }

    instruction[0] = 0xE8;
    const auto relative32 = static_cast<std::int32_t>(relative);
    std::memcpy(instruction + 1, &relative32, sizeof(relative32));
    std::size_t output = 5;
    if (destination_register != 0) {
        instruction[output++] = static_cast<std::uint8_t>(
            0x48 | (destination_register >= 8 ? 1 : 0));
        instruction[output++] = 0x89;
        instruction[output++] = static_cast<std::uint8_t>(
            0xC0 | (destination_register & 7));
    }
    while (output < instruction_length) {
        instruction[output++] = 0x90;
    }

    DWORD ignored = 0;
    VirtualProtect(
        instruction,
        instruction_length,
        old_protection,
        &ignored);
    FlushInstructionCache(
        GetCurrentProcess(),
        instruction,
        instruction_length);
    return true;
}

std::size_t patch_guest_tls_loads() {
    g_guest_tls_load_handler = create_guest_tls_load_handler();
    if (g_guest_tls_load_handler == nullptr) {
        trace_stderr("guest_tls_load_patch=handler_unavailable\n");
        return 0;
    }

    std::size_t patch_count = 0;
    for (const auto& segment : generated::kSegments) {
        if ((segment.protection & 1U) == 0 || segment.memory_size < 8) {
            continue;
        }
        auto* bytes =
            reinterpret_cast<std::uint8_t*>(segment.virtual_address);
        for (std::uint64_t index = 0;
             index + 8 <= segment.memory_size;
             ++index) {
            std::size_t instruction_length = 0;
            if (!patch_guest_tls_load(
                    bytes + index,
                    static_cast<std::size_t>(
                        std::min<std::uint64_t>(
                            16,
                            segment.memory_size - index)),
                    instruction_length)) {
                continue;
            }
            ++patch_count;
            index += instruction_length - 1;
        }
    }
    trace_stderr("guest_tls_load_patch=count=%zu\n", patch_count);
    return patch_count;
}

bool patch_runtime_export(
    std::string_view nid,
    void* handler) {
    if (handler == nullptr) {
        return false;
    }
    for (const auto& symbol : generated::kRuntimeSymbols) {
        if (symbol.nid == nullptr || nid != symbol.nid) {
            continue;
        }

        auto* address =
            reinterpret_cast<std::uint8_t*>(symbol.address);
        DWORD old_protection = 0;
        if (!VirtualProtect(
                address,
                16,
                PAGE_EXECUTE_READWRITE,
                &old_protection)) {
            return false;
        }

        const std::array<std::uint8_t, 16> stub = {
            0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
            0xFF, 0xE0, 0x90, 0x90, 0x90, 0x90,
        };
        std::memcpy(address, stub.data(), stub.size());
        const auto target =
            reinterpret_cast<std::uint64_t>(handler);
        std::memcpy(address + 2, &target, sizeof(target));

        DWORD ignored = 0;
        VirtualProtect(
            address,
            16,
            old_protection,
            &ignored);
        FlushInstructionCache(GetCurrentProcess(), address, 16);
        trace_stderr(
            "runtime_export_patch nid=%.*s address=0x%016llX "
            "handler=0x%016llX\n",
            static_cast<int>(nid.size()),
            nid.data(),
            static_cast<unsigned long long>(symbol.address),
            static_cast<unsigned long long>(target));
        return true;
    }
    trace_stderr(
        "runtime_export_patch_missing nid=%.*s\n",
        static_cast<int>(nid.size()),
        nid.data());
    return false;
}

void patch_agc_runtime_exports() {
    patch_runtime_export(
        "1-gUn1PI4Sw",
        reinterpret_cast<void*>(&ps5rt_return_zero));
    patch_runtime_export(
        "3KDcnM3lrcU",
        reinterpret_cast<void*>(
            &ps5rt_agc_wait_reg_mem_patch_address_real));
}

bool try_recover_guest_zero_fill(EXCEPTION_POINTERS* exception) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto* record = exception->ExceptionRecord;
    auto* context = exception->ContextRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->NumberParameters < 2 ||
        record->ExceptionInformation[0] != 1) {
        return false;
    }

    const auto fault = static_cast<std::uint64_t>(
        record->ExceptionInformation[1]);
    const auto length = static_cast<std::uint64_t>(context->Rdx);
    constexpr std::uint64_t kMaxZeroFillRecovery = 0x0000000100000000ULL;
    if (fault < kGuestLowApertureStart ||
        fault >= kGuestLowApertureEnd ||
        context->Rdi != fault ||
        context->Rsi != 0 ||
        length < kPageSize ||
        length > kMaxZeroFillRecovery ||
        fault > std::numeric_limits<std::uint64_t>::max() - length) {
        return false;
    }

    MEMORY_BASIC_INFORMATION information = {};
    if (VirtualQuery(
            reinterpret_cast<const void*>(fault),
            &information,
            sizeof(information)) == 0 ||
        information.State != MEM_FREE) {
        return false;
    }

    const auto base =
        fault & ~(kAllocationGranularity - 1);
    const auto required = (fault - base) + length;
    const auto size =
        (required + kPageSize - 1) & ~(kPageSize - 1);
    if (size > information.RegionSize) {
        return false;
    }

    void* mapped = VirtualAlloc(
        reinterpret_cast<void*>(base),
        static_cast<SIZE_T>(size),
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);
    if (mapped != reinterpret_cast<void*>(base)) {
        if (mapped != nullptr) {
            VirtualFree(mapped, 0, MEM_RELEASE);
        }
        return false;
    }

    trace_stderr(
        "recovered_guest_zero_fill base=0x%016llX size=0x%016llX "
        "rip=0x%016llX\n",
        static_cast<unsigned long long>(base),
        static_cast<unsigned long long>(size),
        static_cast<unsigned long long>(context->Rip));
    return true;
#else
    (void)exception;
    return false;
#endif
}

bool try_recover_guest_aperture_access(EXCEPTION_POINTERS* exception) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->NumberParameters < 2 ||
        record->ExceptionInformation[0] > 1) {
        return false;
    }

    const auto fault = static_cast<std::uint64_t>(
        record->ExceptionInformation[1]);
    // The low aperture was the only range that faulted its way into being
    // committed. The large direct-memory maps are reserved now too, and
    // they sit above 0x1000000000, so they need the same recovery.
    if ((fault < kGuestLowApertureStart ||
         fault >= kGuestLowApertureEnd) &&
        !ps5rt_guest_reservation_contains(fault)) {
        return false;
    }

    MEMORY_BASIC_INFORMATION information = {};
    if (VirtualQuery(
            reinterpret_cast<const void*>(fault),
            &information,
            sizeof(information)) == 0) {
        return false;
    }
    // Two threads faulting on the same reserved block is ordinary: the
    // first commits it and the second arrives here to find the page
    // already usable. Retrying the instruction is the right answer, but
    // only a couple of times - if it keeps faulting on a committed,
    // writable page the cause is something else and looping would hide it.
    if (information.State == MEM_COMMIT) {
        const DWORD protection = information.Protect & 0xFFU;
        const bool usable = record->ExceptionInformation[0] == 0
            ? protection != PAGE_NOACCESS
            : protection == PAGE_READWRITE ||
                protection == PAGE_WRITECOPY ||
                protection == PAGE_EXECUTE_READWRITE ||
                protection == PAGE_EXECUTE_WRITECOPY;
        static thread_local std::uint64_t last_retry_fault = 0;
        static thread_local int retry_count = 0;
        if (fault == last_retry_fault) {
            ++retry_count;
        } else {
            last_retry_fault = fault;
            retry_count = 1;
        }
        if (!usable || retry_count > 2) {
            return false;
        }
        static std::atomic<std::uint64_t> raced{0};
        const auto count = raced.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 8 || (count % 4096) == 0) {
            trace_stderr(
                "guest_commit_race fault=0x%016llX access=%llu "
                "protect=0x%08lX count=%llu\n",
                static_cast<unsigned long long>(fault),
                static_cast<unsigned long long>(
                    record->ExceptionInformation[0]),
                static_cast<unsigned long>(information.Protect),
                static_cast<unsigned long long>(count));
        }
        return true;
    }
    if (information.State != MEM_RESERVE &&
        information.State != MEM_FREE) {
        return false;
    }

    const auto region_begin = reinterpret_cast<std::uint64_t>(
        information.BaseAddress);
    const auto region_end =
        region_begin + static_cast<std::uint64_t>(information.RegionSize);
    // Committing 64KB at a time split the reservation into thousands of
    // fragments, and every VirtualQuery walk over guest memory then had to
    // step through them - frames per run fell from about sixty to three.
    // Four megabytes a time keeps a gigabyte of working set at a few
    // hundred regions. A range that is still free is grown by the
    // allocation granularity, because that reserves address space rather
    // than only committing inside it.
    const auto granule = information.State == MEM_RESERVE
        ? kLazyCommitGranule
        : kAllocationGranularity;
    const auto commit_begin = std::max(
        region_begin,
        fault & ~(granule - 1));
    const auto commit_end = std::min(
        region_end,
        commit_begin + granule);
    if (commit_end <= commit_begin) {
        return false;
    }

    const auto allocation_type = information.State == MEM_RESERVE
        ? MEM_COMMIT
        : MEM_RESERVE | MEM_COMMIT;
    const auto* mapped = VirtualAlloc(
        reinterpret_cast<void*>(commit_begin),
        static_cast<SIZE_T>(commit_end - commit_begin),
        allocation_type,
        PAGE_READWRITE);
    if (mapped != reinterpret_cast<void*>(commit_begin)) {
        return false;
    }

    static std::atomic<std::uint32_t> recovery_count = 0;
    const auto recovery_index =
        recovery_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (recovery_index <= 32 ||
        (recovery_index & (recovery_index - 1)) == 0) {
        trace_stderr(
            "guest_aperture_lazy_commit recovery=%u "
            "base=0x%016llX size=0x%016llX "
            "fault=0x%016llX access=%llu state=0x%08lX\n",
            recovery_index,
            static_cast<unsigned long long>(commit_begin),
            static_cast<unsigned long long>(commit_end - commit_begin),
            static_cast<unsigned long long>(fault),
            static_cast<unsigned long long>(
                record->ExceptionInformation[0]),
            static_cast<unsigned long>(information.State));
    }
    return true;
#else
    (void)exception;
    return false;
#endif
}

bool try_recover_agc_null_defaults(EXCEPTION_POINTERS* exception) {
#if defined(__x86_64__) || defined(_M_X64)
    auto* context = exception->ContextRecord;
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        context->Rip != 0x0000000810416F87ULL ||
        context->Rax != 0 ||
        record->NumberParameters < 2 ||
        record->ExceptionInformation[1] != 0) {
        return false;
    }

    context->R15 = 0;
    context->R12 = 0;
    context->Rip = 0x0000000810416F8EULL;
    static volatile LONG recovery_count = 0;
    const auto count = InterlockedIncrement(&recovery_count);
    if (count <= 16) {
        trace_stderr(
            "agc_null_defaults_recovered count=%ld "
            "resume=0x%016llX\n",
            static_cast<long>(count),
            static_cast<unsigned long long>(context->Rip));
    }
    return true;
#else
    (void)exception;
    return false;
#endif
}

M128A* get_context_xmm(CONTEXT* context, std::uint8_t index) {
    if (context == nullptr || index >= 16) {
        return nullptr;
    }
    return &context->Xmm0 + index;
}

std::uint64_t extract_sse4a_bit_field(
    std::uint64_t value,
    std::uint32_t length,
    std::uint32_t index) {
    length &= 0x3FU;
    index &= 0x3FU;
    if (length == 0) {
        length = 64;
    }
    if (index >= 64) {
        return 0;
    }
    length = std::min(length, 64U - index);
    const auto mask = length == 64
        ? std::numeric_limits<std::uint64_t>::max()
        : (std::uint64_t{1} << length) - 1;
    return (value >> index) & mask;
}

std::uint64_t insert_sse4a_bit_field(
    std::uint64_t destination,
    std::uint64_t source,
    std::uint32_t length,
    std::uint32_t index) {
    length &= 0x3FU;
    index &= 0x3FU;
    if (length == 0) {
        length = 64;
    }
    if (index >= 64) {
        return destination;
    }
    length = std::min(length, 64U - index);
    const auto mask = length == 64
        ? std::numeric_limits<std::uint64_t>::max()
        : (std::uint64_t{1} << length) - 1;
    const auto shifted_mask = index == 0 ? mask : mask << index;
    return (destination & ~shifted_mask) |
        ((source & mask) << index);
}

bool try_recover_amd_compat_instruction(EXCEPTION_POINTERS* exception) {
#if defined(__x86_64__) || defined(_M_X64)
    if (exception->ExceptionRecord->ExceptionCode !=
        EXCEPTION_ILLEGAL_INSTRUCTION) {
        return false;
    }

    auto* context = exception->ContextRecord;
    const auto rip = static_cast<std::uint64_t>(context->Rip);
    if (rip < 0x0000000800000000ULL ||
        rip >= 0x0000000900000000ULL) {
        return false;
    }

    std::array<std::uint8_t, 8> opcode = {};
    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(rip),
            opcode.data(),
            opcode.size(),
            &bytes_read) ||
        bytes_read < 3) {
        return false;
    }

    if (opcode[0] == 0x0F && opcode[1] == 0x01 &&
        (opcode[2] == 0xFA || opcode[2] == 0xFB)) {
        if (opcode[2] == 0xFB) {
            SwitchToThread();
        }
        context->Rip += 3;
        static volatile LONG monitor_count = 0;
        const auto count = InterlockedIncrement(&monitor_count);
        if (count <= 16) {
            trace_stderr(
                "amd_wait_instruction_recovered count=%ld "
                "opcode=0x%02X rip=0x%016llX\n",
                static_cast<long>(count),
                static_cast<unsigned>(opcode[2]),
                static_cast<unsigned long long>(rip));
        }
        return true;
    }

    const auto prefix = opcode[0];
    if (prefix != 0x66 && prefix != 0xF2) {
        return false;
    }

    std::size_t offset = 1;
    std::uint8_t rex = 0;
    if ((opcode[offset] & 0xF0U) == 0x40U) {
        rex = opcode[offset++];
    }
    // The register forms, which take the field from a register instead of
    // two immediates: EXTRQ xmm1, xmm2 has length and index in the low
    // bytes of xmm2; INSERTQ xmm1, xmm2 has them in the high quadword of
    // xmm2, above the bits it inserts. The scene after the intro runs a
    // SIMD routine full of them, and an Intel core has neither.
    if (bytes_read >= offset + 3 &&
        opcode[offset] == 0x0F &&
        opcode[offset + 1] == 0x79 &&
        (opcode[offset + 2] & 0xC0U) == 0xC0U) {
        const auto modrm = opcode[offset + 2];
        const auto reg = static_cast<std::uint8_t>(
            ((modrm >> 3U) & 0x07U) | ((rex & 0x04U) << 1U));
        const auto rm = static_cast<std::uint8_t>(
            (modrm & 0x07U) | ((rex & 0x01U) << 3U));
        auto* destination = get_context_xmm(context, reg);
        const auto* source = get_context_xmm(context, rm);
        if (destination == nullptr || source == nullptr) {
            return false;
        }
        if (prefix == 0x66) {
            const auto control = source->Low;
            destination->Low = extract_sse4a_bit_field(
                destination->Low,
                static_cast<std::uint32_t>(control & 0x3FU),
                static_cast<std::uint32_t>((control >> 8U) & 0x3FU));
        } else {
            const auto control = static_cast<std::uint64_t>(source->High);
            destination->Low = insert_sse4a_bit_field(
                destination->Low,
                source->Low,
                static_cast<std::uint32_t>(control & 0x3FU),
                static_cast<std::uint32_t>((control >> 8U) & 0x3FU));
        }
        destination->High = 0;
        context->Rip += offset + 3;
        static volatile LONG register_form_count = 0;
        const auto count = InterlockedIncrement(&register_form_count);
        if (count <= 16) {
            trace_stderr(
                "sse4a_instruction_recovered count=%ld kind=%s_reg "
                "xmm=%u source=%u rip=0x%016llX\n",
                static_cast<long>(count),
                prefix == 0x66 ? "extrq" : "insertq",
                static_cast<unsigned>(reg),
                static_cast<unsigned>(rm),
                static_cast<unsigned long long>(rip));
        }
        return true;
    }
    if (bytes_read < offset + 5 ||
        opcode[offset] != 0x0F ||
        opcode[offset + 1] != 0x78) {
        return false;
    }

    const auto modrm = opcode[offset + 2];
    if ((modrm & 0xC0U) != 0xC0U) {
        return false;
    }

    const auto reg = static_cast<std::uint8_t>(
        ((modrm >> 3U) & 0x07U) | ((rex & 0x04U) << 1U));
    const auto rm = static_cast<std::uint8_t>(
        (modrm & 0x07U) | ((rex & 0x01U) << 3U));
    const auto length = opcode[offset + 3];
    const auto index = opcode[offset + 4];

    auto* destination = get_context_xmm(
        context,
        prefix == 0x66 ? rm : reg);
    if (destination == nullptr) {
        return false;
    }

    if (prefix == 0x66) {
        destination->Low = extract_sse4a_bit_field(
            destination->Low,
            length,
            index);
    } else {
        const auto* source = get_context_xmm(context, rm);
        if (source == nullptr) {
            return false;
        }
        destination->Low = insert_sse4a_bit_field(
            destination->Low,
            source->Low,
            length,
            index);
    }
    destination->High = 0;
    context->Rip += offset + 5;

    static volatile LONG sse4a_count = 0;
    const auto count = InterlockedIncrement(&sse4a_count);
    if (count <= 16) {
        trace_stderr(
            "sse4a_instruction_recovered count=%ld "
            "kind=%s xmm=%u length=%u index=%u rip=0x%016llX\n",
            static_cast<long>(count),
            prefix == 0x66 ? "extrq" : "insertq",
            static_cast<unsigned>(prefix == 0x66 ? rm : reg),
            static_cast<unsigned>(length),
            static_cast<unsigned>(index),
            static_cast<unsigned long long>(rip));
    }
    return true;
#else
    (void)exception;
    return false;
#endif
}

// PS5RT_FATAL_PEEK=address:bytes[,address:bytes...]: guest memory to show at
// a fatal fault or an aborted guest worker - a key a lookup was after, a
// global the faulting code read - as hex and as text.
void trace_fatal_peek() {
    const auto* peek = std::getenv("PS5RT_FATAL_PEEK");
    if (peek == nullptr) {
        return;
    }
    const auto* cursor = peek;
    while (*cursor != '\0') {
        char* end = nullptr;
        const auto address = std::strtoull(cursor, &end, 0);
        std::uint64_t length = 64;
        if (end != nullptr && *end == ':') {
            length = std::strtoull(end + 1, &end, 0);
        }
        length = std::min<std::uint64_t>(length, 256);
        std::array<unsigned char, 256> bytes = {};
        SIZE_T taken = 0;
        ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(address),
            bytes.data(),
            static_cast<SIZE_T>(length),
            &taken);
        char hex[800] = {};
        char text[260] = {};
        int used = 0;
        for (SIZE_T index = 0; index < taken; ++index) {
            used += std::snprintf(hex + used, sizeof(hex) - used, "%02X",
                                  bytes[index]);
            text[index] = bytes[index] >= 0x20 && bytes[index] < 0x7F
                ? static_cast<char>(bytes[index]) : '.';
        }
        trace_stderr(
            "fatal_peek address=0x%016llX read=%llu hex=%s text=%s\n",
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(taken), hex, text);
        if (end == nullptr || end == cursor || *end != ',') {
            break;
        }
        cursor = end + 1;
    }
}

void trace_guest_assert_context(
    std::uint64_t rip, const CONTEXT& context, bool force);

bool try_abort_guest_worker(EXCEPTION_POINTERS* exception) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto code = exception->ExceptionRecord->ExceptionCode;
    auto* context = exception->ContextRecord;
    const auto rip = static_cast<std::uint64_t>(context->Rip);
    if (!ps5rt_is_guest_worker_thread() ||
        (code != EXCEPTION_ACCESS_VIOLATION &&
         code != EXCEPTION_ILLEGAL_INSTRUCTION &&
         code != EXCEPTION_PRIV_INSTRUCTION &&
         code != EXCEPTION_STACK_OVERFLOW &&
         code != EXCEPTION_IN_PAGE_ERROR) ||
        rip < 0x0000000800000000ULL ||
        rip >= 0x0000000900000000ULL ||
        context->Rsp < 0x200) {
        return false;
    }

    auto abort_rsp =
        (static_cast<std::uint64_t>(context->Rsp) - 0x100) & ~0xFULL;
    abort_rsp -= sizeof(std::uint64_t);
    const std::uint64_t return_sentinel = 0;
    if (!try_write_process_bytes(
            abort_rsp,
            &return_sentinel,
            sizeof(return_sentinel))) {
        return false;
    }

    trace_fatal_peek();
    // The same picture an assert gets - registers, what they point at,
    // the guest frames - since a guest thread dying says little without
    // it. The first eight.
    static volatile LONG abort_reports = 0;
    if (InterlockedIncrement(&abort_reports) <= 8) {
        trace_guest_assert_context(rip, *context, true);
    }
    trace_stderr(
        "guest_worker_abort code=0x%08lX rip=0x%016llX "
        "fault=0x%016llX host_id=%lu\n",
        static_cast<unsigned long>(code),
        static_cast<unsigned long long>(rip),
        static_cast<unsigned long long>(
            exception->ExceptionRecord->NumberParameters >= 2
                ? exception->ExceptionRecord->ExceptionInformation[1]
                : 0),
        static_cast<unsigned long>(GetCurrentThreadId()));
    context->Rsp = abort_rsp;
    context->Rip = reinterpret_cast<DWORD64>(
        &ps5rt_abort_current_guest_thread);
    context->Rcx = code;
    return true;
#else
    (void)exception;
    return false;
#endif
}

// PS5RT_INT41_REPORT=1: the context of each of the title's failed asserts
// (int 0x41, which this runtime steps over) - registers, 64 bytes behind
// the callee-saved ones, and the guest return addresses on the stack. An
// assert a few instructions before a crash says what the code believed;
// this says what it was looking at. The first 32 are reported.
void trace_guest_assert_context(
    std::uint64_t rip, const CONTEXT& context, bool force) {
    static const bool enabled = [] {
        const auto* value = std::getenv("PS5RT_INT41_REPORT");
        return value != nullptr && value[0] == '1';
    }();
    static volatile LONG reported = 0;
    if (!force && (!enabled || InterlockedIncrement(&reported) > 32)) {
        return;
    }
    trace_stderr(
        "guest_assert rip=0x%016llX rax=%016llX rbx=%016llX rcx=%016llX "
        "rdx=%016llX rsi=%016llX rdi=%016llX rbp=%016llX rsp=%016llX "
        "r8=%016llX r9=%016llX r12=%016llX r13=%016llX r14=%016llX "
        "r15=%016llX\n",
        static_cast<unsigned long long>(rip),
        static_cast<unsigned long long>(context.Rax),
        static_cast<unsigned long long>(context.Rbx),
        static_cast<unsigned long long>(context.Rcx),
        static_cast<unsigned long long>(context.Rdx),
        static_cast<unsigned long long>(context.Rsi),
        static_cast<unsigned long long>(context.Rdi),
        static_cast<unsigned long long>(context.Rbp),
        static_cast<unsigned long long>(context.Rsp),
        static_cast<unsigned long long>(context.R8),
        static_cast<unsigned long long>(context.R9),
        static_cast<unsigned long long>(context.R12),
        static_cast<unsigned long long>(context.R13),
        static_cast<unsigned long long>(context.R14),
        static_cast<unsigned long long>(context.R15));
    const struct {
        const char* name;
        std::uint64_t value;
    } windows[] = {
        {"rbx", context.Rbx}, {"r12", context.R12}, {"r13", context.R13},
        {"r14", context.R14}, {"r15", context.R15},
        {"rbp-0x100", context.Rbp - 0x100},
    };
    for (const auto& window : windows) {
        std::array<std::uint64_t, 8> words = {};
        SIZE_T taken = 0;
        if (window.value < 0x10000 ||
            !ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(window.value),
                words.data(),
                sizeof(words),
                &taken) ||
            taken != sizeof(words)) {
            continue;
        }
        trace_stderr(
            "guest_assert_window %s=0x%016llX %016llX %016llX %016llX "
            "%016llX %016llX %016llX %016llX %016llX\n",
            window.name,
            static_cast<unsigned long long>(window.value),
            static_cast<unsigned long long>(words[0]),
            static_cast<unsigned long long>(words[1]),
            static_cast<unsigned long long>(words[2]),
            static_cast<unsigned long long>(words[3]),
            static_cast<unsigned long long>(words[4]),
            static_cast<unsigned long long>(words[5]),
            static_cast<unsigned long long>(words[6]),
            static_cast<unsigned long long>(words[7]));
    }
    // The object r13 points at, and the text behind the pointers in it: a
    // name the failing check was about is usually one of them.
    std::uint64_t object = 0;
    SIZE_T object_taken = 0;
    if (context.R13 >= 0x10000 &&
        ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(context.R13),
            &object,
            sizeof(object),
            &object_taken) &&
        object >= 0x10000) {
        std::array<std::uint64_t, 20> words = {};
        SIZE_T taken = 0;
        if (ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(object),
                words.data(),
                sizeof(words),
                &taken) &&
            taken == sizeof(words)) {
            for (std::size_t index = 0; index < words.size(); ++index) {
                char text[80] = {};
                SIZE_T text_taken = 0;
                const auto* source =
                    reinterpret_cast<const char*>(&words[index]);
                // Inline (short-string) text, or text behind a pointer.
                if (words[index] >= 0x10000 &&
                    words[index] < 0x0000800000000000ULL) {
                    ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<const void*>(words[index]),
                        text, sizeof(text) - 1, &text_taken);
                }
                bool printable = text_taken != 0 && text[0] >= 0x20 &&
                    text[0] < 0x7F;
                for (std::size_t c = 0; printable && c < 4 && text[c]; ++c) {
                    printable = text[c] >= 0x20 && text[c] < 0x7F;
                }
                char inline_text[9] = {};
                std::memcpy(inline_text, source, 8);
                for (auto& c : inline_text) {
                    if (c != 0 && (c < 0x20 || c >= 0x7F)) {
                        c = '.';
                    }
                }
                trace_stderr(
                    "guest_assert_object +0x%02zX=%016llX inline=%s text=%s\n",
                    index * 8,
                    static_cast<unsigned long long>(words[index]),
                    inline_text,
                    printable ? text : "");
            }
        }
    }
    char frames[400] = {};
    int used = 0;
    for (std::uint64_t index = 0; index < 256; ++index) {
        std::uint64_t candidate = 0;
        SIZE_T taken = 0;
        if (!ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(
                    context.Rsp + index * sizeof(candidate)),
                &candidate,
                sizeof(candidate),
                &taken) ||
            taken != sizeof(candidate) ||
            !is_guest_code(candidate)) {
            continue;
        }
        const auto written = std::snprintf(
            frames + used, sizeof(frames) - used, " 0x%llX",
            static_cast<unsigned long long>(candidate));
        if (written <= 0 ||
            static_cast<std::size_t>(used + written) >= sizeof(frames) - 1) {
            break;
        }
        used += written;
    }
    trace_stderr("guest_assert_frames%s\n", frames);
}

LONG CALLBACK trace_guest_exception(EXCEPTION_POINTERS* exception) {
    if (exception == nullptr ||
        exception->ExceptionRecord == nullptr ||
        exception->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto code = exception->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_IN_PAGE_ERROR) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

#if defined(__x86_64__) || defined(_M_X64)
    const auto exception_trace_index = InterlockedIncrement(
        &g_exception_trace_count);
    if (exception_trace_index <= 32) {
        std::uint64_t raw_fault_address = 0;
        if ((code == EXCEPTION_ACCESS_VIOLATION ||
             code == EXCEPTION_IN_PAGE_ERROR) &&
            exception->ExceptionRecord->NumberParameters >= 2) {
            raw_fault_address = static_cast<std::uint64_t>(
                exception->ExceptionRecord->ExceptionInformation[1]);
        }
        char buffer[256] = {};
        const auto length = std::snprintf(
            buffer,
            sizeof(buffer),
            "exception_seen=%ld code=0x%08lX rip=0x%016llX "
            "fault=0x%016llX fs=0x%016llX\n",
            static_cast<long>(exception_trace_index),
            static_cast<unsigned long>(code),
            static_cast<unsigned long long>(
                exception->ContextRecord->Rip),
            static_cast<unsigned long long>(raw_fault_address),
            static_cast<unsigned long long>(read_fs_base_raw()));
        if (length > 0) {
            const auto bytes = static_cast<DWORD>(
                std::min<int>(
                    length,
                    static_cast<int>(sizeof(buffer) - 1)));
            DWORD written = 0;
            WriteFile(
                GetStdHandle(STD_ERROR_HANDLE),
                buffer,
                bytes,
                &written,
                nullptr);
        }
    }

    if (g_ignore_guest_int41 && code == EXCEPTION_ACCESS_VIOLATION) {
        const auto rip = static_cast<std::uint64_t>(
            exception->ContextRecord->Rip);
        std::array<std::uint8_t, 2> opcode = {};
        SIZE_T bytes_read = 0;
        if (rip >= 0x10000 &&
            ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(rip),
                opcode.data(),
                opcode.size(),
                &bytes_read) &&
            bytes_read == opcode.size() &&
            opcode[0] == 0xCD &&
            opcode[1] == 0x41) {
            if (rip == 0x00000008004A7E0AULL) {
                std::array<std::uint64_t, 12> pool = {};
                SIZE_T pool_bytes = 0;
                const auto pool_address =
                    static_cast<std::uint64_t>(exception->ContextRecord->R14);
                ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(pool_address),
                    pool.data(),
                    sizeof(pool),
                    &pool_bytes);
                std::array<std::uint64_t, 8> inner = {};
                SIZE_T inner_bytes = 0;
                if (pool_bytes >= 0x18 && pool[2] != 0) {
                    ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<const void*>(pool[2]),
                        inner.data(),
                        sizeof(inner),
                        &inner_bytes);
                }
                const auto fallback_address = allocate_gpu_pool_fallback(
                    pool_address,
                    pool[3],
                    pool[5],
                    exception->ContextRecord->Rbx);
                if (fallback_address != 0) {
                    exception->ContextRecord->R15 = fallback_address;
                }
                trace_stderr(
                    "gpu_oom_pool this=0x%016llX size=0x%016llX "
                    "result=0x%016llX inner=0x%016llX "
                    "base=0x%016llX capacity=0x%016llX q6=0x%016llX "
                    "count=0x%016llX inner_vtable=0x%016llX "
                    "inner_q1=0x%016llX inner_q2=0x%016llX "
                    "inner_q3=0x%016llX inner_q4=0x%016llX "
                    "inner_q5=0x%016llX inner_q6=0x%016llX "
                    "inner_q7=0x%016llX\n",
                    static_cast<unsigned long long>(pool_address),
                    static_cast<unsigned long long>(
                        exception->ContextRecord->Rbx),
                    static_cast<unsigned long long>(
                        fallback_address),
                    static_cast<unsigned long long>(pool[2]),
                    static_cast<unsigned long long>(pool[3]),
                    static_cast<unsigned long long>(pool[5]),
                    static_cast<unsigned long long>(pool[6]),
                    static_cast<unsigned long long>(pool[7]),
                    static_cast<unsigned long long>(inner[0]),
                    static_cast<unsigned long long>(inner[1]),
                    static_cast<unsigned long long>(inner[2]),
                    static_cast<unsigned long long>(inner[3]),
                    static_cast<unsigned long long>(inner[4]),
                    static_cast<unsigned long long>(inner[5]),
                    static_cast<unsigned long long>(inner[6]),
                    static_cast<unsigned long long>(inner[7]));
            }
            if (rip == 0x00000008073DC17BULL) {
                std::array<std::uint64_t, 16> node = {};
                SIZE_T node_bytes = 0;
                const auto node_address =
                    static_cast<std::uint64_t>(exception->ContextRecord->R14);
                ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(node_address),
                    node.data(),
                    sizeof(node),
                    &node_bytes);
                std::array<std::uint64_t, 12> vtable = {};
                SIZE_T vtable_bytes = 0;
                const auto vtable_address =
                    static_cast<std::uint64_t>(exception->ContextRecord->R12);
                if (vtable_address != 0) {
                    ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<const void*>(vtable_address),
                        vtable.data(),
                        sizeof(vtable),
                        &vtable_bytes);
                }

                const auto allocation_size =
                    node_bytes >= 0x38 ? node[6] : 0;
                const auto allocation_flags =
                    node_bytes >= 0x48
                    ? static_cast<std::uint32_t>(node[8] >> 32)
                    : 0;
                std::uint64_t fallback_address = 0;
                if (allocation_size != 0 &&
                    allocation_size <= 0x40000000ULL) {
                    auto allocation_alignment = std::max(
                        kAllocationGranularity,
                        std::min(allocation_size, 0x200000ULL));
                    if ((allocation_alignment &
                         (allocation_alignment - 1)) != 0) {
                        allocation_alignment = kAllocationGranularity;
                    }
                    const auto mapped_length = align_up(
                        allocation_size,
                        kPageSize);
                    fallback_address =
                        reinterpret_cast<std::uint64_t>(
                            allocate_guest_virtual_range(
                                0,
                                kRenderTargetFallbackStart,
                                mapped_length,
                                allocation_alignment,
                                false));
                    if (fallback_address != 0) {
                        exception->ContextRecord->Rbx = fallback_address;
                    }
                }
                trace_stderr(
                    "render_target_oom node=0x%016llX "
                    "vtable=0x%016llX method=0x%016llX "
                    "resource=0x%016llX size=0x%016llX "
                    "flags=0x%08X fallback=0x%016llX "
                    "node_bytes=0x%llX vtable_bytes=0x%llX "
                    "nq0=0x%016llX nq1=0x%016llX nq2=0x%016llX "
                    "nq3=0x%016llX nq4=0x%016llX nq5=0x%016llX "
                    "nq6=0x%016llX nq7=0x%016llX "
                    "vq0=0x%016llX vq1=0x%016llX vq2=0x%016llX "
                    "vq3=0x%016llX vq4=0x%016llX\n",
                    static_cast<unsigned long long>(node_address),
                    static_cast<unsigned long long>(vtable_address),
                    static_cast<unsigned long long>(vtable[2]),
                    static_cast<unsigned long long>(node[5]),
                    static_cast<unsigned long long>(allocation_size),
                    static_cast<unsigned int>(allocation_flags),
                    static_cast<unsigned long long>(fallback_address),
                    static_cast<unsigned long long>(node_bytes),
                    static_cast<unsigned long long>(vtable_bytes),
                    static_cast<unsigned long long>(node[0]),
                    static_cast<unsigned long long>(node[1]),
                    static_cast<unsigned long long>(node[2]),
                    static_cast<unsigned long long>(node[3]),
                    static_cast<unsigned long long>(node[4]),
                    static_cast<unsigned long long>(node[5]),
                    static_cast<unsigned long long>(node[6]),
                    static_cast<unsigned long long>(node[7]),
                    static_cast<unsigned long long>(vtable[0]),
                    static_cast<unsigned long long>(vtable[1]),
                    static_cast<unsigned long long>(vtable[2]),
                    static_cast<unsigned long long>(vtable[3]),
                    static_cast<unsigned long long>(vtable[4]));
            }
            trace_guest_assert_context(
                rip, *exception->ContextRecord, false);
            exception->ContextRecord->Rip += opcode.size();
            const auto count = InterlockedIncrement(
                &g_ignored_guest_int41_count);
            if (count <= 16 || (count % 65536) == 0) {
                trace_stderr(
                    "ignored_guest_int41=%ld rip=0x%016llX "
                    "PS5RT_IGNORE_INT41=1\n",
                    static_cast<long>(count),
                    static_cast<unsigned long long>(rip));
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
#endif

    if (try_recover_guest_fs(exception) ||
        try_recover_guest_aperture_access(exception) ||
        try_recover_guest_zero_fill(exception) ||
        try_recover_agc_null_defaults(exception) ||
        try_recover_amd_compat_instruction(exception) ||
        try_abort_guest_worker(exception)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (InterlockedCompareExchange(&g_crash_trace_written, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    std::uint64_t fault_address = 0;
    if ((code == EXCEPTION_ACCESS_VIOLATION ||
         code == EXCEPTION_IN_PAGE_ERROR) &&
        exception->ExceptionRecord->NumberParameters >= 2) {
        fault_address = static_cast<std::uint64_t>(
            exception->ExceptionRecord->ExceptionInformation[1]);
    }

#if defined(__x86_64__) || defined(_M_X64)
    const auto* context = exception->ContextRecord;
    const auto rip = static_cast<std::uint64_t>(context->Rip);
    std::uint64_t return_address = 0;
    SIZE_T return_address_bytes = 0;
    ReadProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<const void*>(context->Rsp),
        &return_address,
        sizeof(return_address),
        &return_address_bytes);
    std::array<std::uint64_t, 2> r14_callback = {};
    SIZE_T r14_callback_bytes = 0;
    ReadProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<const void*>(context->R14 + 0x128),
        r14_callback.data(),
        sizeof(r14_callback),
        &r14_callback_bytes);
    if (rip == 0x00000008004532DDULL) {
        std::uint64_t caller_allocator = 0;
        SIZE_T caller_allocator_bytes = 0;
        ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(context->Rsp + 0x80),
            &caller_allocator,
            sizeof(caller_allocator),
            &caller_allocator_bytes);
        std::array<std::uint64_t, 16> allocator = {};
        SIZE_T allocator_bytes = 0;
        if (caller_allocator_bytes == sizeof(caller_allocator) &&
            caller_allocator != 0) {
            ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(caller_allocator),
                allocator.data(),
                sizeof(allocator),
                &allocator_bytes);
        }
        trace_stderr(
            "allocator_null caller=0x%016llX bytes=0x%llX "
            "q0=0x%016llX q1=0x%016llX q2=0x%016llX "
            "q3=0x%016llX q4=0x%016llX q5=0x%016llX "
            "q6=0x%016llX q7=0x%016llX q8=0x%016llX "
            "q9=0x%016llX q10=0x%016llX q11=0x%016llX "
            "q12=0x%016llX q13=0x%016llX q14=0x%016llX "
            "q15=0x%016llX\n",
            static_cast<unsigned long long>(caller_allocator),
            static_cast<unsigned long long>(allocator_bytes),
            static_cast<unsigned long long>(allocator[0]),
            static_cast<unsigned long long>(allocator[1]),
            static_cast<unsigned long long>(allocator[2]),
            static_cast<unsigned long long>(allocator[3]),
            static_cast<unsigned long long>(allocator[4]),
            static_cast<unsigned long long>(allocator[5]),
            static_cast<unsigned long long>(allocator[6]),
            static_cast<unsigned long long>(allocator[7]),
            static_cast<unsigned long long>(allocator[8]),
            static_cast<unsigned long long>(allocator[9]),
            static_cast<unsigned long long>(allocator[10]),
            static_cast<unsigned long long>(allocator[11]),
            static_cast<unsigned long long>(allocator[12]),
            static_cast<unsigned long long>(allocator[13]),
            static_cast<unsigned long long>(allocator[14]),
            static_cast<unsigned long long>(allocator[15]));
    }
#else
    const std::uint64_t rip = 0;
#endif
    // Printed before fatal_exception= on purpose: astro-cycle.ps1 stops the
    // process the moment it sees that line, so anything after it races the
    // kill. No std::string and no loops over live objects here either: this
    // runs inside a vectored handler on a guest thread that has just faulted,
    // and a second fault takes the whole report with it.
#if defined(__x86_64__) || defined(_M_X64)
    // Which module rip is in, and what the faulting page actually is.
    // "rip in a system DLL, fault inside the guest heap" and "rip in guest
    // code, fault on a null vtable" are different bugs and used to look
    // the same in this report.
    {
        char module_name[MAX_PATH] = {};
        std::uint64_t module_base = 0;
        MEMORY_BASIC_INFORMATION code = {};
        if (VirtualQuery(
                reinterpret_cast<const void*>(rip),
                &code,
                sizeof(code)) == sizeof(code) &&
            code.AllocationBase != nullptr) {
            module_base =
                reinterpret_cast<std::uint64_t>(code.AllocationBase);
            char path[MAX_PATH] = {};
            if (GetModuleFileNameA(
                    reinterpret_cast<HMODULE>(code.AllocationBase),
                    path,
                    static_cast<DWORD>(sizeof(path))) != 0) {
                const auto* leaf = std::strrchr(path, '\\');
                std::snprintf(
                    module_name,
                    sizeof(module_name),
                    "%s",
                    leaf == nullptr ? path : leaf + 1);
            }
        }
        MEMORY_BASIC_INFORMATION target = {};
        const auto have_target =
            fault_address != 0 &&
            VirtualQuery(
                reinterpret_cast<const void*>(fault_address),
                &target,
                sizeof(target)) == sizeof(target);
        const auto access =
            exception->ExceptionRecord->NumberParameters >= 1
                ? exception->ExceptionRecord->ExceptionInformation[0]
                : ~static_cast<ULONG_PTR>(0);
        trace_stderr(
            "fatal_where module=%s base=0x%016llX offset=0x%llX "
            "guest_code=%u access=%llu target_state=0x%08lX "
            "target_protect=0x%08lX target_base=0x%016llX "
            "target_size=0x%llX\n",
            module_name[0] == '\0' ? "-" : module_name,
            static_cast<unsigned long long>(module_base),
            static_cast<unsigned long long>(
                module_base == 0 ? 0 : rip - module_base),
            is_guest_code(rip) ? 1u : 0u,
            static_cast<unsigned long long>(access),
            static_cast<unsigned long>(have_target ? target.State : 0),
            static_cast<unsigned long>(have_target ? target.Protect : 0),
            static_cast<unsigned long long>(
                have_target
                    ? reinterpret_cast<std::uint64_t>(target.BaseAddress)
                    : 0),
            static_cast<unsigned long long>(
                have_target ? target.RegionSize : 0));
    }
    {
        std::array<std::uint64_t, 16> window = {};
        SIZE_T window_bytes = 0;
        ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(context->R14),
            window.data(),
            sizeof(window),
            &window_bytes);
        char text[512] = {};
        int used = 0;
        for (std::size_t index = 0;
             index < window.size() && window_bytes == sizeof(window);
             ++index) {
            const auto written = std::snprintf(
                text + used,
                sizeof(text) - static_cast<std::size_t>(used),
                " %016llX",
                static_cast<unsigned long long>(window[index]));
            if (written <= 0) {
                break;
            }
            used += written;
        }
        trace_stderr(
            "fatal_r14_window base=0x%016llX read=%llu%s\n",
            static_cast<unsigned long long>(context->R14),
            static_cast<unsigned long long>(window_bytes),
            text);
    }
    {
        std::array<std::uint64_t, 16> window = {};
        SIZE_T window_bytes = 0;
        ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(context->Rbx),
            window.data(),
            sizeof(window),
            &window_bytes);
        char text[512] = {};
        int used = 0;
        for (std::size_t index = 0;
             index < window.size() && window_bytes == sizeof(window);
             ++index) {
            const auto written = std::snprintf(
                text + used,
                sizeof(text) - static_cast<std::size_t>(used),
                " %016llX",
                static_cast<unsigned long long>(window[index]));
            if (written <= 0) {
                break;
            }
            used += written;
        }
        trace_stderr(
            "fatal_rbx_window base=0x%016llX read=%llu%s\n",
            static_cast<unsigned long long>(context->Rbx),
            static_cast<unsigned long long>(window_bytes),
            text);
    }
    // A call through a bad pointer leaves rip meaningless, and with the
    // guest image mapped by hand no host unwinder will walk out of it. The
    // stack still holds the return addresses; is_guest_code separates them
    // from the noise around them.
    {
        char frames[496] = {};
        int used = 0;
        for (std::uint64_t index = 0; index < 128; ++index) {
            std::uint64_t candidate = 0;
            SIZE_T taken = 0;
            const auto slot = context->Rsp + index * sizeof(candidate);
            if (!ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(slot),
                    &candidate,
                    sizeof(candidate),
                    &taken) ||
                taken != sizeof(candidate) ||
                !is_guest_code(candidate)) {
                continue;
            }
            const auto written = std::snprintf(
                frames + used,
                sizeof(frames) - static_cast<std::size_t>(used),
                " 0x%llX",
                static_cast<unsigned long long>(candidate));
            if (written <= 0 ||
                static_cast<std::size_t>(used + written) >=
                    sizeof(frames) - 1) {
                break;
            }
            used += written;
        }
        trace_stderr("fatal_guest_frames%s\n", frames);
    }
    trace_fatal_peek();
#endif
    trace_stderr(
        "fatal_exception=0x%08lX rip=0x%016llX fault=0x%016llX "
        "fs=0x%016llX initializer=0x%016llX image=%s"
#if defined(__x86_64__) || defined(_M_X64)
        " rsp=0x%016llX ret0=0x%016llX rax=0x%016llX "
        "rbx=0x%016llX rcx=0x%016llX rdx=0x%016llX "
        "rsi=0x%016llX rdi=0x%016llX r14=0x%016llX r15=0x%016llX "
        "r14_cb=0x%016llX r14_data=0x%016llX"
#endif
        "\n",
        static_cast<unsigned long>(code),
        static_cast<unsigned long long>(rip),
        static_cast<unsigned long long>(fault_address),
        static_cast<unsigned long long>(read_fs_base_raw()),
        static_cast<unsigned long long>(g_active_initializer),
        g_active_initializer_name == nullptr
            ? "none"
            : g_active_initializer_name
#if defined(__x86_64__) || defined(_M_X64)
        ,
        static_cast<unsigned long long>(context->Rsp),
        static_cast<unsigned long long>(
            return_address_bytes == sizeof(return_address)
                ? return_address
                : 0),
        static_cast<unsigned long long>(context->Rax),
        static_cast<unsigned long long>(context->Rbx),
        static_cast<unsigned long long>(context->Rcx),
        static_cast<unsigned long long>(context->Rdx),
        static_cast<unsigned long long>(context->Rsi),
        static_cast<unsigned long long>(context->Rdi),
        static_cast<unsigned long long>(context->R14),
        static_cast<unsigned long long>(context->R15),
        static_cast<unsigned long long>(
            r14_callback_bytes >= sizeof(std::uint64_t)
                ? r14_callback[0]
                : 0),
        static_cast<unsigned long long>(
            r14_callback_bytes == sizeof(r14_callback)
                ? r14_callback[1]
                : 0)
#endif
        );
    MEMORY_BASIC_INFORMATION fault_memory = {};
    if (fault_address != 0 &&
        VirtualQuery(
            reinterpret_cast<const void*>(fault_address),
            &fault_memory,
            sizeof(fault_memory)) != 0) {
        trace_stderr(
            "fault_memory base=0x%016llX allocation=0x%016llX "
            "size=0x%016llX state=0x%08lX protect=0x%08lX "
            "type=0x%08lX\n",
            static_cast<unsigned long long>(
                reinterpret_cast<std::uint64_t>(
                    fault_memory.BaseAddress)),
            static_cast<unsigned long long>(
                reinterpret_cast<std::uint64_t>(
                    fault_memory.AllocationBase)),
            static_cast<unsigned long long>(fault_memory.RegionSize),
            static_cast<unsigned long>(fault_memory.State),
            static_cast<unsigned long>(fault_memory.Protect),
            static_cast<unsigned long>(fault_memory.Type));
    }
#if defined(__x86_64__) || defined(_M_X64)
    ps5rt_describe_libc_address(fault_address, "fault");
    ps5rt_describe_libc_address(context->R12, "r12");
    ps5rt_describe_libc_address(context->R14, "r14");
    trace_stderr(
        "registers rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX "
        "rsi=%016llX rdi=%016llX rbp=%016llX rsp=%016llX\n",
        static_cast<unsigned long long>(context->Rax),
        static_cast<unsigned long long>(context->Rbx),
        static_cast<unsigned long long>(context->Rcx),
        static_cast<unsigned long long>(context->Rdx),
        static_cast<unsigned long long>(context->Rsi),
        static_cast<unsigned long long>(context->Rdi),
        static_cast<unsigned long long>(context->Rbp),
        static_cast<unsigned long long>(context->Rsp));
    trace_stderr(
        "registers r8=%016llX r9=%016llX r10=%016llX r11=%016llX "
        "r12=%016llX r13=%016llX r14=%016llX r15=%016llX\n",
        static_cast<unsigned long long>(context->R8),
        static_cast<unsigned long long>(context->R9),
        static_cast<unsigned long long>(context->R10),
        static_cast<unsigned long long>(context->R11),
        static_cast<unsigned long long>(context->R12),
        static_cast<unsigned long long>(context->R13),
        static_cast<unsigned long long>(context->R14),
        static_cast<unsigned long long>(context->R15));
    std::array<std::uint64_t, 32> stack_words = {};
    SIZE_T stack_bytes = 0;
    if (ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(context->Rsp),
            stack_words.data(),
            sizeof(stack_words),
            &stack_bytes)) {
        const auto word_count =
            std::min<std::size_t>(stack_words.size(), stack_bytes / sizeof(std::uint64_t));
        for (std::size_t index = 0; index < word_count; index += 4) {
            trace_stderr(
                "stack rsp+%03llX=%016llX %016llX %016llX %016llX\n",
                static_cast<unsigned long long>(index * sizeof(std::uint64_t)),
                static_cast<unsigned long long>(stack_words[index + 0]),
                static_cast<unsigned long long>(
                    index + 1 < word_count ? stack_words[index + 1] : 0),
                static_cast<unsigned long long>(
                    index + 2 < word_count ? stack_words[index + 2] : 0),
                static_cast<unsigned long long>(
                    index + 3 < word_count ? stack_words[index + 3] : 0));
        }
    }
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

class RuntimeHandle {
public:
    ~RuntimeHandle() {
        ps5rt_destroy(runtime_);
    }

    Ps5RtRuntime** address() {
        return &runtime_;
    }

    Ps5RtRuntime* get() const {
        return runtime_;
    }

private:
    Ps5RtRuntime* runtime_ = nullptr;
};

class GuestTls {
public:
    ~GuestTls() {
        if (entered_) {
            write_fs_base_raw(original_fs_base_);
            (void)set_guest_thread_pointer(0);
        }
        if (allocation_ != nullptr) {
            VirtualFree(allocation_, 0, MEM_RELEASE);
        }
    }

    bool initialize() {
        if (generated::kTlsModuleCount == 0) {
            return true;
        }
        if (!IsProcessorFeaturePresent(PF_RDWRFSGSBASE_AVAILABLE)) {
            return false;
        }

        allocation_ = VirtualAlloc(
            nullptr,
            static_cast<SIZE_T>(
                kStaticTlsReservation + kTlsControlSize),
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE);
        if (allocation_ == nullptr) {
            return false;
        }

        auto* base = static_cast<std::uint8_t*>(allocation_);
        const auto thread_pointer = reinterpret_cast<std::uint64_t>(
            base + kStaticTlsReservation);

        auto* tcb = reinterpret_cast<std::uint64_t*>(thread_pointer);
        auto* dtv = reinterpret_cast<std::uint64_t*>(thread_pointer + 0x100);
        std::uint64_t maximum_module_id = 0;
        for (const auto& module : generated::kTlsModules) {
            maximum_module_id = std::max(
                maximum_module_id,
                module.module_id);
        }
        const auto dtv_size =
            (2 + maximum_module_id) * sizeof(std::uint64_t);
        if (dtv_size > kTlsControlSize - 0x100) {
            return false;
        }

        *reinterpret_cast<std::uint64_t*>(thread_pointer - 0xF0) = 0;
        tcb[0] = thread_pointer;
        tcb[1] = reinterpret_cast<std::uint64_t>(dtv);
        tcb[2] = thread_pointer;
        tcb[5] = g_stack_check_guard[0];
        tcb[12] = thread_pointer;

        dtv[0] = 1;
        dtv[1] = maximum_module_id;
        for (const auto& module : generated::kTlsModules) {
            if (module.static_offset > kStaticTlsReservation ||
                module.memory_size > module.static_offset ||
                module.module_id == 0 ||
                module.module_id > maximum_module_id) {
                return false;
            }
            auto* destination = reinterpret_cast<void*>(
                thread_pointer - module.static_offset);
            std::memset(
                destination,
                0,
                static_cast<std::size_t>(module.memory_size));
            if (module.init_size != 0) {
                std::memcpy(
                    destination,
                    reinterpret_cast<const void*>(module.init_address),
                    static_cast<std::size_t>(module.init_size));
            }
            dtv[2 + module.module_id - 1] =
                reinterpret_cast<std::uint64_t>(destination);
        }

        if (!set_guest_thread_pointer(thread_pointer)) {
            return false;
        }
        original_fs_base_ = read_fs_base_raw();
        trace_stderr(
            "tls_enter old_fs=0x%016llX new_fs=0x%016llX "
            "dtv=0x%016llX modules=%llu\n",
            static_cast<unsigned long long>(original_fs_base_),
            static_cast<unsigned long long>(thread_pointer),
            static_cast<unsigned long long>(
                reinterpret_cast<std::uint64_t>(dtv)),
            static_cast<unsigned long long>(generated::kTlsModuleCount));
        write_fs_base_raw(thread_pointer);
        entered_ = true;
        return true;
    }

private:
    void* allocation_ = nullptr;
    std::uint64_t original_fs_base_ = 0;
    bool entered_ = false;
};

void* create_guest_tls_context() {
    auto* tls = new (std::nothrow) GuestTls();
    if (tls == nullptr || !tls->initialize()) {
        delete tls;
        return nullptr;
    }
    return tls;
}

void destroy_guest_tls_context(void* context) {
    delete static_cast<GuestTls*>(context);
}

void restore_guest_fs() {
    const auto guest_thread_pointer = get_guest_thread_pointer();
    if (guest_thread_pointer != 0) {
        write_fs_base_raw(guest_thread_pointer);
    }
}

#define PS5_HLE_GUARD() restore_guest_fs()

std::uint64_t align_down(std::uint64_t value, std::uint64_t alignment) {
    return value & ~(alignment - 1);
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

std::string windows_error(DWORD error) {
    char* message = nullptr;
    const auto length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<char*>(&message),
        0,
        nullptr);
    std::string result = length != 0 && message != nullptr
        ? std::string(message, length)
        : "unknown Windows error";
    if (message != nullptr) {
        LocalFree(message);
    }
    while (!result.empty() &&
           (result.back() == '\r' || result.back() == '\n')) {
        result.pop_back();
    }
    return result;
}

DWORD final_protection(std::uint32_t flags) {
    const bool execute = (flags & 1U) != 0;
    const bool write = (flags & 2U) != 0;
    const bool read = (flags & 4U) != 0;
    if (execute && write) {
        return PAGE_EXECUTE_READWRITE;
    }
    if (execute && read) {
        return PAGE_EXECUTE_READ;
    }
    if (execute) {
        return PAGE_EXECUTE;
    }
    if (write) {
        return PAGE_READWRITE;
    }
    if (read) {
        return PAGE_READONLY;
    }
    return PAGE_NOACCESS;
}

bool parse_u64(std::string_view text, std::uint64_t& value) {
    int base = 10;
    if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) {
        return false;
    }

    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, base);
    return result.ec == std::errc{} && result.ptr == end;
}

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    options.package_directory = std::filesystem::path(argv[0]).parent_path();
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--package" && index + 1 < argc) {
            options.package_directory = argv[++index];
            continue;
        }
        if (argument == "--app0" && index + 1 < argc) {
            options.app0_directory = argv[++index];
            continue;
        }
        if (argument == "--expect-return" && index + 1 < argc) {
            std::uint64_t value = 0;
            if (!parse_u64(argv[++index], value)) {
                return std::nullopt;
            }
            options.expected_return = value;
            continue;
        }
        if (argument == "--gpu-backend" && index + 1 < argc) {
            const std::string_view backend(argv[++index]);
            if (backend == "auto") {
                options.gpu_backend = PS5RT_GPU_AUTO;
            } else if (backend == "d3d12") {
                options.gpu_backend = PS5RT_GPU_D3D12;
            } else if (backend == "vulkan") {
                options.gpu_backend = PS5RT_GPU_VULKAN;
            } else if (backend == "none") {
                options.gpu_backend = PS5RT_GPU_NONE;
            } else {
                return std::nullopt;
            }
            continue;
        }
        if (argument == "--validate-only") {
            options.validate_only = true;
            continue;
        }
        if (argument == "--allow-unresolved-imports") {
            options.allow_unresolved_imports = true;
            continue;
        }
        if (argument == "--headless") {
            options.headless = true;
            continue;
        }
        if (argument == "--run-initializers") {
            options.run_initializers = true;
            continue;
        }
        return std::nullopt;
    }
    return options;
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(utf8.data()),
        utf8.size());
}

void initialize_hle_bridge(const Options& options) {
    std::array<wchar_t, 32768> executable_path = {};
    const auto executable_length = GetModuleFileNameW(
        nullptr,
        executable_path.data(),
        static_cast<DWORD>(executable_path.size()));
    if (executable_length == 0 ||
        executable_length >= executable_path.size()) {
        return;
    }

    const auto bridge_path =
        std::filesystem::path(
            executable_path.data(),
            executable_path.data() + executable_length)
            .parent_path() /
        kHleBridgeFileName;
    g_hle_bridge = LoadLibraryW(bridge_path.c_str());
    if (g_hle_bridge == nullptr) {
        trace_stderr(
            "hle_bridge=unavailable path=%s error=%lu\n",
            path_utf8(bridge_path).c_str(),
            static_cast<unsigned long>(GetLastError()));
        return;
    }

    const auto initialize = reinterpret_cast<HleBridgeInitialize>(
        GetProcAddress(g_hle_bridge, "ps5hle_initialize"));
    g_hle_bridge_has_nid = reinterpret_cast<HleBridgeHasNid>(
        GetProcAddress(g_hle_bridge, "ps5hle_has_nid"));
    g_hle_bridge_dispatch = reinterpret_cast<HleBridgeDispatch>(
        GetProcAddress(g_hle_bridge, "ps5hle_dispatch"));
    if (initialize == nullptr ||
        g_hle_bridge_has_nid == nullptr ||
        g_hle_bridge_dispatch == nullptr) {
        trace_stderr("hle_bridge=invalid_exports\n");
        FreeLibrary(g_hle_bridge);
        g_hle_bridge = nullptr;
        g_hle_bridge_has_nid = nullptr;
        g_hle_bridge_dispatch = nullptr;
        return;
    }

    std::string app0;
    if (!options.app0_directory.empty()) {
        app0 = path_utf8(
            std::filesystem::absolute(options.app0_directory));
    } else {
        std::array<char, 32768> configured_app0 = {};
        const auto length = GetEnvironmentVariableA(
            "PS5RECOMP_APP0",
            configured_app0.data(),
            static_cast<DWORD>(configured_app0.size()));
        if (length != 0 && length < configured_app0.size()) {
            app0.assign(configured_app0.data(), length);
        }
    }

    const auto export_count = initialize(
        app0.empty() ? nullptr : app0.c_str());
    if (export_count < 0) {
        trace_stderr("hle_bridge=initialization_failed\n");
        FreeLibrary(g_hle_bridge);
        g_hle_bridge = nullptr;
        g_hle_bridge_has_nid = nullptr;
        g_hle_bridge_dispatch = nullptr;
        return;
    }
    trace_stderr(
        "hle_bridge=ready exports=%d app0=%s\n",
        export_count,
        app0.empty() ? "?" : app0.c_str());
}

int hle_bridge_nid_support(std::string_view nid) {
    if (g_hle_bridge_has_nid == nullptr) {
        return 0;
    }
    const std::string key(nid);
    return g_hle_bridge_has_nid(key.c_str());
}

void initialize_gpu_bridge() {
    std::array<wchar_t, 32768> executable_path = {};
    const auto executable_length = GetModuleFileNameW(
        nullptr,
        executable_path.data(),
        static_cast<DWORD>(executable_path.size()));
    if (executable_length == 0 ||
        executable_length >= executable_path.size()) {
        return;
    }

    const auto bridge_path =
        std::filesystem::path(
            executable_path.data(),
            executable_path.data() + executable_length)
            .parent_path() /
        kGpuBridgeFileName;
    g_gpu_bridge = LoadLibraryW(bridge_path.c_str());
    if (g_gpu_bridge == nullptr) {
        trace_stderr(
            "gpu_bridge=unavailable path=%s error=%lu\n",
            path_utf8(bridge_path).c_str(),
            static_cast<unsigned long>(GetLastError()));
        return;
    }

    const auto get_abi_version = reinterpret_cast<Ps5GpuGetAbiVersion>(
        GetProcAddress(g_gpu_bridge, "ps5gpu_get_abi_version"));
    g_gpu_compile_spirv = reinterpret_cast<Ps5GpuCompileSpirv>(
        GetProcAddress(g_gpu_bridge, "ps5gpu_compile_spirv"));
    g_gpu_free = reinterpret_cast<Ps5GpuFree>(
        GetProcAddress(g_gpu_bridge, "ps5gpu_free"));
    if (get_abi_version == nullptr ||
        g_gpu_compile_spirv == nullptr ||
        g_gpu_free == nullptr) {
        trace_stderr("gpu_bridge=invalid_exports\n");
        FreeLibrary(g_gpu_bridge);
        g_gpu_bridge = nullptr;
        g_gpu_compile_spirv = nullptr;
        g_gpu_free = nullptr;
        return;
    }

    const auto abi_version = get_abi_version();
    if (abi_version != PS5GPU_ABI_VERSION) {
        trace_stderr(
            "gpu_bridge=abi_mismatch expected=0x%08X actual=0x%08X\n",
            PS5GPU_ABI_VERSION,
            abi_version);
        FreeLibrary(g_gpu_bridge);
        g_gpu_bridge = nullptr;
        g_gpu_compile_spirv = nullptr;
        g_gpu_free = nullptr;
        return;
    }

    trace_stderr(
        "gpu_bridge=ready abi=0x%08X backend=spirv\n",
        abi_version);
}

void shutdown_native_gpu_runtime() {
    if (g_native_gpu_runtime != nullptr) {
        const auto flush_result = g_native_gpu_flush != nullptr
            ? g_native_gpu_flush(g_native_gpu_runtime, 5000)
            : PS5GPU_NATIVE_ERROR_INTERNAL;
        Ps5GpuNativeStats stats = {};
        stats.struct_size = sizeof(stats);
        const auto stats_result = g_native_gpu_get_stats != nullptr
            ? g_native_gpu_get_stats(g_native_gpu_runtime, &stats)
            : PS5GPU_NATIVE_ERROR_INTERNAL;
        trace_stderr(
            "native_gpu.shutdown flush=%u stats=%u "
            "draws=%llu/%llu dropped=%llu compute=%llu/%llu "
            "dropped=%llu flips=%llu/%llu dropped=%llu "
            "queue=%llu high=%llu\n",
            static_cast<unsigned>(flush_result),
            static_cast<unsigned>(stats_result),
            static_cast<unsigned long long>(stats.draws_processed),
            static_cast<unsigned long long>(stats.draws_submitted),
            static_cast<unsigned long long>(stats.draws_dropped),
            static_cast<unsigned long long>(
                stats.compute_dispatches_processed),
            static_cast<unsigned long long>(
                stats.compute_dispatches_submitted),
            static_cast<unsigned long long>(
                stats.compute_dispatches_dropped),
            static_cast<unsigned long long>(stats.flips_processed),
            static_cast<unsigned long long>(stats.flips_submitted),
            static_cast<unsigned long long>(stats.flips_dropped),
            static_cast<unsigned long long>(stats.queue_depth),
            static_cast<unsigned long long>(stats.queue_high_watermark));
        if (g_native_gpu_destroy != nullptr) {
            g_native_gpu_destroy(g_native_gpu_runtime);
        }
        g_native_gpu_runtime = nullptr;
    }
    if (g_native_gpu_runtime_module != nullptr) {
        FreeLibrary(g_native_gpu_runtime_module);
        g_native_gpu_runtime_module = nullptr;
    }
    g_native_gpu_create = nullptr;
    g_native_gpu_destroy = nullptr;
    g_native_gpu_register_shader_state = nullptr;
    g_native_gpu_register_compute_state = nullptr;
    g_native_gpu_submit_draw = nullptr;
    g_native_gpu_submit_compute = nullptr;
    g_native_gpu_submit_flip = nullptr;
    g_native_gpu_flush = nullptr;
    g_native_gpu_get_stats = nullptr;
}

void initialize_native_gpu_runtime() {
    if (!environment_flag_enabled("PS5RT_NATIVE_GPU_RUNTIME")) {
        return;
    }
    std::array<wchar_t, 32768> executable_path = {};
    const auto executable_length = GetModuleFileNameW(
        nullptr,
        executable_path.data(),
        static_cast<DWORD>(executable_path.size()));
    if (executable_length == 0 ||
        executable_length >= executable_path.size()) {
        trace_stderr("native_gpu=executable_path_failed\n");
        return;
    }
    const auto runtime_path =
        std::filesystem::path(
            executable_path.data(),
            executable_path.data() + executable_length)
            .parent_path() /
        kNativeGpuRuntimeFileName;
    g_native_gpu_runtime_module = LoadLibraryW(runtime_path.c_str());
    if (g_native_gpu_runtime_module == nullptr) {
        trace_stderr(
            "native_gpu=unavailable path=%s error=%lu\n",
            path_utf8(runtime_path).c_str(),
            static_cast<unsigned long>(GetLastError()));
        return;
    }

    const auto get_abi =
        reinterpret_cast<Ps5GpuNativeGetAbiVersion>(
            GetProcAddress(
                g_native_gpu_runtime_module,
                "ps5gpu_native_get_abi_version"));
    g_native_gpu_create = reinterpret_cast<Ps5GpuNativeCreate>(
        GetProcAddress(g_native_gpu_runtime_module, "ps5gpu_native_create"));
    g_native_gpu_destroy = reinterpret_cast<Ps5GpuNativeDestroy>(
        GetProcAddress(g_native_gpu_runtime_module, "ps5gpu_native_destroy"));
    g_native_gpu_register_shader_state =
        reinterpret_cast<Ps5GpuNativeRegisterShaderState>(
            GetProcAddress(
                g_native_gpu_runtime_module,
                "ps5gpu_native_register_shader_state"));
    g_native_gpu_register_compute_state =
        reinterpret_cast<Ps5GpuNativeRegisterComputeState>(
            GetProcAddress(
                g_native_gpu_runtime_module,
                "ps5gpu_native_register_compute_state"));
    g_native_gpu_submit_draw =
        reinterpret_cast<Ps5GpuNativeSubmitDraw>(
            GetProcAddress(
                g_native_gpu_runtime_module,
                "ps5gpu_native_submit_draw"));
    g_native_gpu_submit_compute =
        reinterpret_cast<Ps5GpuNativeSubmitCompute>(
            GetProcAddress(
                g_native_gpu_runtime_module,
                "ps5gpu_native_submit_compute"));
    g_native_gpu_submit_flip =
        reinterpret_cast<Ps5GpuNativeSubmitFlip>(
            GetProcAddress(
                g_native_gpu_runtime_module,
                "ps5gpu_native_submit_flip"));
    g_native_gpu_flush = reinterpret_cast<Ps5GpuNativeFlush>(
        GetProcAddress(g_native_gpu_runtime_module, "ps5gpu_native_flush"));
    g_native_gpu_get_stats = reinterpret_cast<Ps5GpuNativeGetStats>(
        GetProcAddress(
            g_native_gpu_runtime_module,
            "ps5gpu_native_get_stats"));
    if (get_abi == nullptr ||
        g_native_gpu_create == nullptr ||
        g_native_gpu_destroy == nullptr ||
        g_native_gpu_register_shader_state == nullptr ||
        g_native_gpu_register_compute_state == nullptr ||
        g_native_gpu_submit_draw == nullptr ||
        g_native_gpu_submit_compute == nullptr ||
        g_native_gpu_submit_flip == nullptr ||
        g_native_gpu_flush == nullptr ||
        g_native_gpu_get_stats == nullptr ||
        get_abi() != PS5GPU_NATIVE_ABI_VERSION) {
        trace_stderr("native_gpu=invalid_exports_or_abi\n");
        shutdown_native_gpu_runtime();
        return;
    }

    Ps5GpuNativeCreateInfo create_info = {};
    create_info.struct_size = sizeof(create_info);
    create_info.abi_version = PS5GPU_NATIVE_ABI_VERSION;
    create_info.queue_capacity = 16384;
    const auto result =
        g_native_gpu_create(&create_info, &g_native_gpu_runtime);
    if (result != PS5GPU_NATIVE_OK ||
        g_native_gpu_runtime == nullptr) {
        trace_stderr(
            "native_gpu=create_failed result=%u\n",
            static_cast<unsigned>(result));
        shutdown_native_gpu_runtime();
        return;
    }
    trace_stderr(
        "native_gpu=ready abi=0x%08X queue_capacity=%u\n",
        PS5GPU_NATIVE_ABI_VERSION,
        create_info.queue_capacity);
}

struct NativeGpuRuntimeScope {
    ~NativeGpuRuntimeScope() {
        shutdown_native_gpu_runtime();
    }
};

bool read_segment(
    const std::filesystem::path& package_directory,
    const generated::SegmentDescriptor& segment) {
    if (segment.image_index >= generated::kImageCount) {
        return false;
    }
    const auto image_path = package_directory /
        generated::kImages[segment.image_index].relative_path;
    std::ifstream image(image_path, std::ios::binary);
    if (!image) {
        return false;
    }
    image.seekg(static_cast<std::streamoff>(segment.blob_offset), std::ios::beg);
    if (!image) {
        return false;
    }

    auto* destination = reinterpret_cast<char*>(segment.virtual_address);
    std::uint64_t remaining = segment.memory_size;
    constexpr std::uint64_t chunk_size = 16ULL * 1024ULL * 1024ULL;
    while (remaining != 0) {
        const auto chunk = static_cast<std::streamsize>(
            std::min(remaining, chunk_size));
        image.read(destination, chunk);
        if (image.gcount() != chunk) {
            return false;
        }
        destination += chunk;
        remaining -= static_cast<std::uint64_t>(chunk);
    }
    return true;
}

bool contains_executable_entry() {
    for (const auto& segment : generated::kSegments) {
        if ((segment.protection & 1U) == 0 ||
            generated::kEntryPoint < segment.virtual_address) {
            continue;
        }
        if (generated::kEntryPoint - segment.virtual_address <
            segment.memory_size) {
            return true;
        }
    }
    return false;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_test_hle() {
    PS5_HLE_GUARD();
    std::cout << "hle_test_call=1\n";
    restore_guest_fs();
    return 7;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_unresolved_import(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t) {
    PS5_HLE_GUARD();
    const char* nid = nullptr;
#if defined(__GNUC__) && defined(_WIN32)
    __asm__ volatile("mov %%r10, %0" : "=r"(nid));
#endif
    static std::atomic<std::uint32_t> hit_count = 0;
    const auto hit = hit_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (hit <= 64) {
#if defined(PS5RT_HAS_GENERATED_HLE)
        const auto* symbol = nid == nullptr
            ? nullptr
            : ps5rt::hle::HleLookup(nid);
        trace_stderr(
            "unresolved_import_hit=%u nid=%s name=%s library=%s\n",
            hit,
            nid == nullptr ? "?" : nid,
            symbol == nullptr || symbol->name[0] == '\0'
                ? "?"
                : symbol->name,
            symbol == nullptr || symbol->library[0] == '\0'
                ? "?"
                : symbol->library);
#else
        trace_stderr(
            "unresolved_import_hit=%u nid=%s\n",
            hit,
            nid == nullptr ? "?" : nid);
#endif
    }
    return kUnresolvedImportResult;
}

#if defined(__GNUC__) && defined(_WIN32)
#define PS5RT_BRIDGE_ENTRY \
    __attribute__((noinline, optimize("O0", "no-omit-frame-pointer")))
#else
#define PS5RT_BRIDGE_ENTRY
#endif

void* get_or_create_guest_context_transfer_stub() {
    if (auto* existing =
            g_guest_context_transfer_stub.load(std::memory_order_acquire)) {
        return existing;
    }

    AcquireSRWLockExclusive(&g_guest_context_transfer_stub_lock);
    auto* existing =
        g_guest_context_transfer_stub.load(std::memory_order_relaxed);
    if (existing != nullptr) {
        ReleaseSRWLockExclusive(&g_guest_context_transfer_stub_lock);
        return existing;
    }

    constexpr SIZE_T kStubSize = 256;
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr,
        kStubSize,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_EXECUTE_READWRITE));
    if (code == nullptr) {
        ReleaseSRWLockExclusive(&g_guest_context_transfer_stub_lock);
        return nullptr;
    }

    std::size_t offset = 0;
    auto emit = [&](std::uint8_t value) {
        code[offset++] = value;
    };
    auto emit_i32 = [&](std::int32_t value) {
        std::memcpy(code + offset, &value, sizeof(value));
        offset += sizeof(value);
    };
    auto emit_u32 = [&](std::uint32_t value) {
        std::memcpy(code + offset, &value, sizeof(value));
        offset += sizeof(value);
    };
    auto emit_load_from_r11 =
        [&](int target_register, std::uint8_t displacement) {
            emit(static_cast<std::uint8_t>(
                0x49 | (target_register >= 8 ? 0x04 : 0x00)));
            emit(0x8B);
            emit(static_cast<std::uint8_t>(
                0x40 | ((target_register & 7) << 3) | 0x03));
            emit(displacement);
        };
    auto emit_load_from_r11_disp32 =
        [&](int target_register, std::int32_t displacement) {
            emit(static_cast<std::uint8_t>(
                0x49 | (target_register >= 8 ? 0x04 : 0x00)));
            emit(0x8B);
            emit(static_cast<std::uint8_t>(
                0x80 | ((target_register & 7) << 3) | 0x03));
            emit_i32(displacement);
        };

    emit(0x49); emit(0x89); emit(0xC3); // mov r11, rax
    emit(0x49); emit(0x83); emit(0xBB); // cmp qword [r11+152], 0
    emit_i32(152);
    emit(0x00);
    emit(0x0F); emit(0x84);             // je merge_status
    const auto merge_branch = offset;
    emit_i32(0);
    emit(0x41); emit(0x0F); emit(0xAE); emit(0x93); // ldmxcsr [r11+136]
    emit_i32(136);
    emit(0xE9);                         // jmp mxcsr_done
    const auto done_branch = offset;
    emit_i32(0);
    const auto merge_label = offset;
    emit(0x48); emit(0x83); emit(0xEC); emit(0x08); // sub rsp, 8
    emit(0x0F); emit(0xAE); emit(0x1C); emit(0x24); // stmxcsr [rsp]
    emit(0x41); emit(0x8B); emit(0x83);             // mov eax, [r11+136]
    emit_i32(136);
    emit(0x25); emit_u32(0xFFFFFFC0u);              // and eax, ~0x3f
    emit(0x8B); emit(0x0C); emit(0x24);             // mov ecx, [rsp]
    emit(0x83); emit(0xE1); emit(0x3F);             // and ecx, 0x3f
    emit(0x09); emit(0xC8);                         // or eax, ecx
    emit(0x89); emit(0x04); emit(0x24);             // mov [rsp], eax
    emit(0x0F); emit(0xAE); emit(0x14); emit(0x24); // ldmxcsr [rsp]
    emit(0x48); emit(0x83); emit(0xC4); emit(0x08); // add rsp, 8
    const auto mxcsr_done_label = offset;
    const auto merge_displacement = static_cast<std::int32_t>(
        merge_label - (merge_branch + sizeof(std::int32_t)));
    const auto done_displacement = static_cast<std::int32_t>(
        mxcsr_done_label - (done_branch + sizeof(std::int32_t)));
    std::memcpy(
        code + merge_branch,
        &merge_displacement,
        sizeof(merge_displacement));
    std::memcpy(
        code + done_branch,
        &done_displacement,
        sizeof(done_displacement));
    emit(0x41); emit(0xD9); emit(0xAB); // fldcw [r11+144]
    emit_i32(144);

    emit_load_from_r11(4, 8);           // rsp
    emit(0x48); emit(0x83); emit(0xEC); emit(0x08);
    emit_load_from_r11(1, 24);          // rcx
    emit_load_from_r11(2, 32);          // rdx
    emit_load_from_r11(3, 40);          // rbx
    emit_load_from_r11(5, 48);          // rbp
    emit_load_from_r11(6, 56);          // rsi
    emit_load_from_r11(7, 64);          // rdi
    emit_load_from_r11(8, 72);          // r8
    emit_load_from_r11(9, 80);          // r9
    emit_load_from_r11(10, 88);         // r10
    emit_load_from_r11(12, 104);        // r12
    emit_load_from_r11(13, 112);        // r13
    emit_load_from_r11(14, 120);        // r14
    emit_load_from_r11_disp32(15, 128); // r15
    emit_load_from_r11(0, 16);          // rax
    emit_load_from_r11(11, 96);         // r11, last
    emit(0xC3);                         // ret through [target_rsp - 8]

    DWORD old_protection = 0;
    if (offset > kStubSize ||
        !VirtualProtect(
            code,
            kStubSize,
            PAGE_EXECUTE_READ,
            &old_protection)) {
        VirtualFree(code, 0, MEM_RELEASE);
        ReleaseSRWLockExclusive(&g_guest_context_transfer_stub_lock);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), code, offset);
    g_guest_context_transfer_stub.store(code, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_guest_context_transfer_stub_lock);
    return code;
}

bool is_fiber_bridge_nid(const char* nid) {
    return nid != nullptr &&
        (std::strcmp(nid, "hVYD7Ou2pCQ") == 0 ||
         std::strcmp(nid, "a0LLrZWac0M") == 0 ||
         std::strcmp(nid, "PFT2S-tJ7Uk") == 0 ||
         std::strcmp(nid, "B0ZX2hx9DMw") == 0 ||
         std::strcmp(nid, "JeNX5F-NzQU") == 0);
}

bool native_gpu_shadow_enabled() {
    static const bool enabled = []() {
        char value[8] = {};
        return GetEnvironmentVariableA(
                   "PS5RT_NATIVE_GPU_SHADOW",
                   value,
                   sizeof(value)) != 0 &&
            value[0] == '1';
    }();
    return enabled;
}

bool native_videoout_authority_enabled() {
    static const bool enabled = []() {
        char value[8] = {};
        return GetEnvironmentVariableA(
                   "PS5RT_NATIVE_VIDEOOUT",
                   value,
                   sizeof(value)) != 0 &&
            value[0] == '1';
    }();
    return enabled;
}

// With the native runtime's window on, the managed VideoOut is not asked at
// all about what the native one answers: asking it started the managed
// presenter, a second window with a swapchain on a second device, and with
// the NVIDIA overlay on the driver took the process down on the native
// window's first present.
bool native_videoout_exclusive() {
    static const bool exclusive = []() {
        char value[8] = {};
        return GetEnvironmentVariableA(
                   "PS5GPU_NATIVE_WINDOW",
                   value,
                   sizeof(value)) != 0 &&
            value[0] == '1';
    }();
    return exclusive;
}

bool native_videoout_nid_enabled(const char* nid) {
    static const std::string filter = []() {
        std::array<char, 1024> value = {};
        const auto length = GetEnvironmentVariableA(
            "PS5RT_NATIVE_VIDEOOUT_NIDS",
            value.data(),
            static_cast<DWORD>(value.size()));
        return length > 0 && length < value.size()
            ? std::string(value.data(), length)
            : std::string{};
    }();
    if (filter.empty()) {
        return true;
    }
    if (nid == nullptr) {
        return false;
    }

    auto remaining = std::string_view(filter);
    while (!remaining.empty()) {
        const auto separator = remaining.find_first_of(",;");
        auto token = remaining.substr(0, separator);
        while (!token.empty() &&
               (token.front() == ' ' || token.front() == '\t')) {
            token.remove_prefix(1);
        }
        while (!token.empty() &&
               (token.back() == ' ' || token.back() == '\t')) {
            token.remove_suffix(1);
        }
        if (token == "*" || token == nid) {
            return true;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }
    return false;
}

bool read_guest_c_string(
    std::uint64_t address,
    char* buffer,
    std::size_t capacity) noexcept;

// libKernel's APR half and libSceAmpr, plus the file calls they replace.
// Everything the title could deliver bulk data through.
// A function-local static here would need a guard variable, and the
// guard's exception path is enough to break the SEH prologue this
// trampoline is built with. Keep the lazy read outside it.
bool asset_call_trace_enabled() {
    static const bool enabled =
        environment_flag_enabled("PS5RT_TRACE_ASSET_CALLS");
    return enabled;
}

bool is_asset_bridge_nid(const char* nid) {
    static const char* const kAssetNids[] = {
        "1G3lF1Gg1k8",  // sceKernelOpen
        "Cg4srZ6TKbU",  // sceKernelRead
        "+r3rMFwItV4",  // sceKernelPread
        "oib76F-12fk",  // sceKernelLseek
        "UK2Tl2DWUns",  // sceKernelClose
        "eV9wAD2riIA",  // sceKernelStat
        "kBwCPsYX-m4",  // sceKernelFstat
        "4wSze92BhLI",  // sceKernelWrite
        "1-LFLmRFxxM",  // sceKernelMkdir
        "cQke9UuBQOk",  // sceKernelMunmap
        "WT-5NKy42fw",  // sceKernelAprResolveFilepathsToIds
        "gEpBkcwxUjw",  // sceKernelAprResolveFilepathsToIdsAndFileSizes
        "WvEu7yl3Ivg",  // sceKernelAprGetFileSize
        "ApkYaHb8Sek",  // sceKernelAprGetFileStat
        "eE4Szl8sil8",  // sceKernelAprSubmitCommandBuffer
        "qvMUCyyaCSI",  // sceKernelAprSubmitCommandBufferAndGetId
        "ASoW5WE-UPo",  // sceKernelAprSubmitCommandBufferAndGetResult
        "rqwFKI4PAiM",  // sceKernelAprWaitCommandBuffer
        "8aI7R7WaOlc",  // sceAmprCommandBufferConstructor
        "GuchCTefuZw",  // sceAmprCommandBufferDestructor
        "N-FSPA4S3nI",  // sceAmprCommandBufferSetBuffer
        "ULvXMDz56po",  // sceAmprCommandBufferClearBuffer
        "baQO9ez2gL4",  // sceAmprCommandBufferReset
        "GnxKOHEawhk",  // sceAmprCommandBufferGetCurrentOffset
        "gzndltBEzWc",  // sceAmprCommandBufferGetNumCommands
        "tZDDEo2tE5k",  // sceAmprCommandBufferGetSize
        "a8uLzYY--tM",  // sceAmprAprCommandBufferConstructor
        "Qs1xtplKo0U",  // sceAmprAprCommandBufferDestructor
        "mQ16-QdKv7k",  // sceAmprAprCommandBufferReadFile
        "vWU-odnS+fU",  // sceAmprMeasureCommandSizeReadFile
        "H896Pt-yB4I",  // sceAmprCommandBufferWriteKernelEventQueue_04_00
        "sSAUCCU1dv4",  // sceAmprMeasureCommandSizeWriteKernelEventQueue
    };
    for (const auto* candidate : kAssetNids) {
        if (std::strcmp(nid, candidate) == 0) {
            return true;
        }
    }
    return false;
}

bool asset_bridge_nid_takes_path(const char* nid) {
    return std::strcmp(nid, "1G3lF1Gg1k8") == 0 ||
        std::strcmp(nid, "eV9wAD2riIA") == 0 ||
        std::strcmp(nid, "1-LFLmRFxxM") == 0;
}

extern "C" PS5_GUEST_ABI PS5RT_BRIDGE_ENTRY std::uint64_t
ps5rt_sharpemu_import(
    std::uint64_t arg0,
    std::uint64_t arg1,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4,
    std::uint64_t arg5) {
    const char* nid = nullptr;
    std::uint64_t guest_rbx = 0;
    std::uint64_t guest_r11 = 0;
    std::uint64_t guest_r12 = 0;
    std::uint64_t guest_r13 = 0;
    std::uint64_t guest_r14 = 0;
    std::uint64_t guest_r15 = 0;
    std::uint64_t guest_rflags = 0;
    alignas(16) std::uint64_t captured_xmm[32];
#if defined(__GNUC__) && defined(_WIN32)
    __asm__ volatile("mov %%r10, %0" : "=r"(nid));
    __asm__ volatile("mov %%rbx, %0" : "=r"(guest_rbx));
    __asm__ volatile("mov %%r11, %0" : "=r"(guest_r11));
    __asm__ volatile("mov %%r12, %0" : "=r"(guest_r12));
    __asm__ volatile("mov %%r13, %0" : "=r"(guest_r13));
    __asm__ volatile("mov %%r14, %0" : "=r"(guest_r14));
    __asm__ volatile("mov %%r15, %0" : "=r"(guest_r15));
    __asm__ volatile("pushfq; pop %0" : "=r"(guest_rflags));
    __asm__ volatile("movdqu %%xmm0, %0" : "=m"(captured_xmm[0]));
    __asm__ volatile("movdqu %%xmm1, %0" : "=m"(captured_xmm[2]));
    __asm__ volatile("movdqu %%xmm2, %0" : "=m"(captured_xmm[4]));
    __asm__ volatile("movdqu %%xmm3, %0" : "=m"(captured_xmm[6]));
    __asm__ volatile("movdqu %%xmm4, %0" : "=m"(captured_xmm[8]));
    __asm__ volatile("movdqu %%xmm5, %0" : "=m"(captured_xmm[10]));
    __asm__ volatile("movdqu %%xmm6, %0" : "=m"(captured_xmm[12]));
    __asm__ volatile("movdqu %%xmm7, %0" : "=m"(captured_xmm[14]));
    __asm__ volatile("movdqu %%xmm8, %0" : "=m"(captured_xmm[16]));
    __asm__ volatile("movdqu %%xmm9, %0" : "=m"(captured_xmm[18]));
    __asm__ volatile("movdqu %%xmm10, %0" : "=m"(captured_xmm[20]));
    __asm__ volatile("movdqu %%xmm11, %0" : "=m"(captured_xmm[22]));
    __asm__ volatile("movdqu %%xmm12, %0" : "=m"(captured_xmm[24]));
    __asm__ volatile("movdqu %%xmm13, %0" : "=m"(captured_xmm[26]));
    __asm__ volatile("movdqu %%xmm14, %0" : "=m"(captured_xmm[28]));
    __asm__ volatile("movdqu %%xmm15, %0" : "=m"(captured_xmm[30]));
#else
    std::fill(std::begin(captured_xmm), std::end(captured_xmm), 0);
#endif

    auto* frame_pointer = static_cast<std::uint64_t*>(
        __builtin_frame_address(0));
    const auto guest_rbp = frame_pointer == nullptr ? 0 : frame_pointer[0];
    const auto guest_rsp = frame_pointer == nullptr
        ? 0
        : reinterpret_cast<std::uint64_t>(&frame_pointer[1]);
    const auto return_rip = frame_pointer == nullptr ? 0 : frame_pointer[1];

    Ps5HleCallFrame frame = {};
    frame.gpr[0] = 0;
    frame.gpr[1] = arg3;
    frame.gpr[2] = arg2;
    frame.gpr[3] = guest_rbx;
    frame.gpr[4] = guest_rsp;
    frame.gpr[5] = guest_rbp;
    frame.gpr[6] = arg1;
    frame.gpr[7] = arg0;
    frame.gpr[8] = arg4;
    frame.gpr[9] = arg5;
    frame.gpr[10] = reinterpret_cast<std::uint64_t>(nid);
    frame.gpr[11] = guest_r11;
    frame.gpr[12] = guest_r12;
    frame.gpr[13] = guest_r13;
    frame.gpr[14] = guest_r14;
    frame.gpr[15] = guest_r15;
    frame.rip = return_rip;
    frame.rflags = guest_rflags;
    frame.fs_base = read_fs_base_raw();
    frame.gs_base = 0;
#if defined(__GNUC__) && defined(_WIN32)
    __asm__ volatile("fnstcw %0" : "=m"(frame.fpu_control_word));
    __asm__ volatile("stmxcsr %0" : "=m"(frame.mxcsr));
#endif
    std::copy(
        std::begin(captured_xmm),
        std::end(captured_xmm),
        std::begin(frame.xmm));

    if (nid == nullptr || g_hle_bridge_dispatch == nullptr) {
        return kUnresolvedImportResult;
    }
    static std::atomic<std::uint32_t> dispatch_count = 0;
    const auto dispatch_index =
        dispatch_count.fetch_add(1, std::memory_order_relaxed) + 1;
    // Bridge call volume and mix, always on. HleCallTrace can only count
    // calls when it is enabled, and enabling it is what changes the run,
    // so what the guest is doing during a stall cannot be answered from
    // inside it. At the measured ~1500 calls a second a locked map costs
    // nothing worth avoiding.
    record_bridge_call(nid);
    if (dispatch_index % 10000 == 0) {
        report_bridge_call_mix(dispatch_index);
        report_bridge_calls_by_thread();
        ps5rt_report_outstanding_waits(2000);
        report_outstanding_bridge_calls(2000);
    }
    const bool trace_memory_dispatch =
        std::strcmp(nid, "B+vc2AO2Zrc") == 0 ||
        std::strcmp(nid, "BQQniolj9tQ") == 0;
    const bool trace_fiber_dispatch = is_fiber_bridge_nid(nid);
    const bool trace_gpu_dispatch =
        std::strcmp(nid, "UglJIZjGssM") == 0 ||
        std::strcmp(nid, "gSRnr79F8tQ") == 0;
    const bool trace_named_dispatch = hle_nid_traced_by_name(nid);
    if (dispatch_index <= 128 ||
        trace_memory_dispatch ||
        trace_fiber_dispatch ||
        trace_gpu_dispatch ||
        trace_named_dispatch) {
        trace_stderr(
            "sharpemu_hle_call=%u nid=%s rip=0x%016llX "
            "rdi=0x%016llX rsi=0x%016llX rdx=0x%016llX "
            "rcx=0x%016llX r8=0x%016llX r9=0x%016llX\n",
            dispatch_index,
            nid,
            static_cast<unsigned long long>(return_rip),
            static_cast<unsigned long long>(arg0),
            static_cast<unsigned long long>(arg1),
            static_cast<unsigned long long>(arg2),
            static_cast<unsigned long long>(arg3),
            static_cast<unsigned long long>(arg4),
            static_cast<unsigned long long>(arg5));
    }
    std::uint64_t exclusive_result = 0;
    const bool native_only =
        native_videoout_exclusive() &&
        native_videoout_authority_enabled() &&
        native_videoout_nid_enabled(nid) &&
        ps5rt_videoout_authority_dispatch(
            nid,
            arg0,
            arg1,
            arg2,
            arg3,
            arg4,
            arg5,
            guest_rsp,
            captured_xmm[0],
            &exclusive_result);
    int dispatch_result = 1;
    if (native_only) {
        frame.gpr[0] = exclusive_result;
    } else {
        begin_bridge_call(nid);
        dispatch_result = g_hle_bridge_dispatch(nid, &frame);
        end_bridge_call();
    }
    restore_guest_fs();
    const auto managed_result = frame.gpr[0];
    if (asset_call_trace_enabled() && is_asset_bridge_nid(nid)) {
        std::array<char, 256> path = {};
        if (!asset_bridge_nid_takes_path(nid) ||
            !read_guest_c_string(arg0, path.data(), path.size())) {
            path[0] = '\0';
        }
        trace_stderr(
            "asset_call nid=%s path=%s result=0x%016llX "
            "rip=0x%016llX rdi=0x%016llX rsi=0x%016llX rdx=0x%016llX "
            "rcx=0x%016llX r8=0x%016llX r9=0x%016llX dispatch=%d\n",
            nid,
            path.data(),
            static_cast<unsigned long long>(managed_result),
            static_cast<unsigned long long>(return_rip),
            static_cast<unsigned long long>(arg0),
            static_cast<unsigned long long>(arg1),
            static_cast<unsigned long long>(arg2),
            static_cast<unsigned long long>(arg3),
            static_cast<unsigned long long>(arg4),
            static_cast<unsigned long long>(arg5),
            dispatch_result);
    }
    std::uint64_t native_videoout_result = 0;
    const bool native_videoout_call =
        !native_only &&
        dispatch_result > 0 &&
        native_videoout_authority_enabled() &&
        native_videoout_nid_enabled(nid) &&
        ps5rt_videoout_authority_dispatch(
            nid,
            arg0,
            arg1,
            arg2,
            arg3,
            arg4,
            arg5,
            guest_rsp,
            captured_xmm[0],
            &native_videoout_result);
    if (native_videoout_call) {
        frame.gpr[0] = native_videoout_result;
        trace_stderr(
            "native_videoout.authority nid=%s managed=0x%016llX "
            "native=0x%016llX parity=%d\n",
            nid,
            static_cast<unsigned long long>(managed_result),
            static_cast<unsigned long long>(native_videoout_result),
            managed_result == native_videoout_result ? 1 : 0);
    }
    if (dispatch_result > 0 &&
        (native_gpu_shadow_enabled() ||
         native_videoout_authority_enabled())) {
        ps5rt_videoout_shadow_observe(
            nid,
            arg0,
            arg1,
            arg2,
            arg3,
            arg4,
            arg5,
            guest_rsp,
            frame.gpr[0]);
        if (frame.gpr[0] == 0 &&
            std::strcmp(nid, "UglJIZjGssM") == 0) {
            ps5rt_agc_shadow_submit_dcb(arg0);
        } else if (
            frame.gpr[0] == 0 &&
            std::strcmp(nid, "gSRnr79F8tQ") == 0) {
            ps5rt_agc_shadow_submit_acb(arg0, arg1);
        }
    }
    if (dispatch_result <= 0) {
        trace_stderr(
            "sharpemu_hle_failed nid=%s result=%d\n",
            nid,
            dispatch_result);
    } else if (dispatch_index <= 128 ||
               trace_memory_dispatch ||
               trace_fiber_dispatch ||
               trace_gpu_dispatch ||
               trace_named_dispatch) {
        trace_stderr(
            "sharpemu_hle_return=%u nid=%s rax=0x%016llX control=0x%08X\n",
            dispatch_index,
            nid,
            static_cast<unsigned long long>(frame.gpr[0]),
            frame.control_flags);
    }

    if ((frame.control_flags & kHleControlContextTransfer) != 0) {
        const auto target_rip = frame.rip;
        const auto target_rsp = frame.gpr[4];
        auto* transfer_stub = get_or_create_guest_context_transfer_stub();
        if (frame_pointer == nullptr ||
            target_rip < 0x10000 ||
            target_rsp < sizeof(std::uint64_t) ||
            transfer_stub == nullptr ||
            !try_write_process_bytes(
                target_rsp - sizeof(std::uint64_t),
                &target_rip,
                sizeof(target_rip))) {
            trace_stderr(
                "sharpemu_hle_context_transfer_failed nid=%s "
                "rip=0x%016llX rsp=0x%016llX stub=0x%016llX\n",
                nid,
                static_cast<unsigned long long>(target_rip),
                static_cast<unsigned long long>(target_rsp),
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uint64_t>(transfer_stub)));
            return 0x80590004ULL;
        }

        g_guest_context_transfer_frame = {
            frame.rip,
            frame.gpr[4],
            frame.gpr[0],
            frame.gpr[1],
            frame.gpr[2],
            frame.gpr[3],
            frame.gpr[5],
            frame.gpr[6],
            frame.gpr[7],
            frame.gpr[8],
            frame.gpr[9],
            frame.gpr[10],
            frame.gpr[11],
            frame.gpr[12],
            frame.gpr[13],
            frame.gpr[14],
            frame.gpr[15],
            frame.mxcsr == 0 ? 0x1F80u : frame.mxcsr,
            frame.fpu_control_word == 0 ? 0x037Fu : frame.fpu_control_word,
            (frame.control_flags & kHleControlRestoreFullFpuState) != 0
                ? 1ULL
                : 0ULL,
        };

        frame_pointer[1] =
            reinterpret_cast<std::uint64_t>(transfer_stub);
        trace_stderr(
            "sharpemu_hle_context_transfer nid=%s "
            "rip=0x%016llX rsp=0x%016llX frame=0x%016llX\n",
            nid,
            static_cast<unsigned long long>(target_rip),
            static_cast<unsigned long long>(target_rsp),
            static_cast<unsigned long long>(
                reinterpret_cast<std::uint64_t>(
                    &g_guest_context_transfer_frame)));
        return reinterpret_cast<std::uint64_t>(
            &g_guest_context_transfer_frame);
    }

    const auto result = frame.gpr[0];
#if defined(__GNUC__) && defined(_WIN32)
    __asm__ volatile("movdqu %0, %%xmm0" : : "m"(frame.xmm[0]));
#endif
    return result;
}

#undef PS5RT_BRIDGE_ENTRY

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_return_zero(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t) {
    PS5_HLE_GUARD();
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_memcpy(
    std::uint64_t destination,
    std::uint64_t source,
    std::uint64_t count) {
    if (count != 0) {
        std::memcpy(
            reinterpret_cast<void*>(destination),
            reinterpret_cast<const void*>(source),
            static_cast<std::size_t>(count));
    }
    return destination;
}

extern "C" PS5_GUEST_ABI std::int32_t ps5rt_memcpy_s(
    std::uint64_t destination,
    std::uint64_t destination_size,
    std::uint64_t source,
    std::uint64_t count) {
    if (destination == 0 || source == 0) {
        return 22;
    }
    if (count > destination_size) {
        if (destination_size != 0) {
            std::memset(
                reinterpret_cast<void*>(destination),
                0,
                static_cast<std::size_t>(destination_size));
        }
        return 34;
    }
    if (count != 0) {
        std::memcpy(
            reinterpret_cast<void*>(destination),
            reinterpret_cast<const void*>(source),
            static_cast<std::size_t>(count));
    }
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_memmove(
    std::uint64_t destination,
    std::uint64_t source,
    std::uint64_t count) {
    if (count != 0) {
        std::memmove(
            reinterpret_cast<void*>(destination),
            reinterpret_cast<const void*>(source),
            static_cast<std::size_t>(count));
    }
    return destination;
}

bool ensure_guest_low_aperture_committed(
    std::uint64_t address,
    std::uint64_t size) {
    if (size == 0 ||
        address < kGuestLowApertureStart ||
        address >= kGuestLowApertureEnd ||
        size > kGuestLowApertureEnd - address) {
        return false;
    }

    const auto range_begin = align_down(address, kPageSize);
    const auto range_end = align_up(address + size, kPageSize);
    auto cursor = range_begin;
    while (cursor < range_end) {
        MEMORY_BASIC_INFORMATION information = {};
        if (VirtualQuery(
                reinterpret_cast<const void*>(cursor),
                &information,
                sizeof(information)) == 0) {
            return false;
        }
        const auto region_begin = reinterpret_cast<std::uint64_t>(
            information.BaseAddress);
        const auto region_end = std::min(
            range_end,
            region_begin + static_cast<std::uint64_t>(information.RegionSize));
        if (region_end <= cursor) {
            return false;
        }
        if (information.State == MEM_COMMIT) {
            cursor = region_end;
            continue;
        }

        void* mapped = nullptr;
        std::uint64_t mapped_begin = cursor;
        std::uint64_t mapped_end = region_end;
        if (information.State == MEM_RESERVE) {
            mapped_begin = align_down(cursor, kPageSize);
            mapped = VirtualAlloc(
                reinterpret_cast<void*>(mapped_begin),
                static_cast<SIZE_T>(mapped_end - mapped_begin),
                MEM_COMMIT,
                PAGE_READWRITE);
        } else if (information.State == MEM_FREE) {
            mapped_begin = region_begin;
            mapped_end = align_up(region_end, kPageSize);
            mapped = VirtualAlloc(
                reinterpret_cast<void*>(mapped_begin),
                static_cast<SIZE_T>(mapped_end - mapped_begin),
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE);
        }
        if (mapped != reinterpret_cast<void*>(mapped_begin)) {
            if (mapped != nullptr && information.State == MEM_FREE) {
                VirtualFree(mapped, 0, MEM_RELEASE);
            }
            return false;
        }
        trace_stderr(
            "guest_aperture_commit base=0x%016llX size=0x%016llX\n",
            static_cast<unsigned long long>(mapped_begin),
            static_cast<unsigned long long>(mapped_end - mapped_begin));
        cursor = mapped_end;
    }
    return true;
}

bool zero_guest_low_aperture_sparse(
    std::uint64_t address,
    std::uint64_t size) {
    if (size == 0 ||
        address < kGuestLowApertureStart ||
        address >= kGuestLowApertureEnd ||
        size > kGuestLowApertureEnd - address) {
        return false;
    }

    const auto range_end = address + size;
    auto cursor = address;
    std::uint64_t committed_bytes = 0;
    std::uint64_t reserved_bytes = 0;
    while (cursor < range_end) {
        MEMORY_BASIC_INFORMATION information = {};
        if (VirtualQuery(
                reinterpret_cast<const void*>(cursor),
                &information,
                sizeof(information)) == 0) {
            return false;
        }
        const auto region_begin = reinterpret_cast<std::uint64_t>(
            information.BaseAddress);
        const auto region_end = std::min(
            range_end,
            region_begin + static_cast<std::uint64_t>(information.RegionSize));
        const auto segment_begin = std::max(cursor, region_begin);
        if (region_end <= segment_begin) {
            return false;
        }

        if (information.State == MEM_COMMIT) {
            const auto protection = information.Protect & 0xFFU;
            const bool writable =
                protection == PAGE_READWRITE ||
                protection == PAGE_WRITECOPY ||
                protection == PAGE_EXECUTE_READWRITE ||
                protection == PAGE_EXECUTE_WRITECOPY;
            if (!writable ||
                (information.Protect & PAGE_GUARD) != 0) {
                return false;
            }
            std::memset(
                reinterpret_cast<void*>(segment_begin),
                0,
                static_cast<std::size_t>(region_end - segment_begin));
            committed_bytes += region_end - segment_begin;
        } else if (information.State == MEM_RESERVE ||
                   information.State == MEM_FREE) {
            reserved_bytes += region_end - segment_begin;
        } else {
            return false;
        }
        cursor = region_end;
    }

    if (reserved_bytes != 0) {
        static std::atomic<std::uint32_t> sparse_zero_count = 0;
        const auto sparse_zero_index =
            sparse_zero_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (sparse_zero_index <= 32 ||
            (sparse_zero_index & (sparse_zero_index - 1)) == 0) {
            trace_stderr(
                "guest_aperture_sparse_zero count=%u "
                "dst=0x%016llX size=0x%016llX "
                "committed=0x%016llX reserved=0x%016llX\n",
                sparse_zero_index,
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(committed_bytes),
                static_cast<unsigned long long>(reserved_bytes));
        }
    }
    return true;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_memset(
    std::uint64_t destination,
    std::uint64_t value,
    std::uint64_t count) {
    if (count == 0) {
        return destination;
    }

    const auto original_destination = destination;
    const auto caller = reinterpret_cast<std::uint64_t>(
        __builtin_return_address(0));
    if (caller == 0x0000000800039BC2ULL ||
        caller == 0x0000000800039CE5ULL) {
        static std::atomic<std::uint32_t> ignored_logger_call_count = 0;
        const auto ignored_index =
            ignored_logger_call_count.fetch_add(
                1,
                std::memory_order_relaxed) + 1;
        if (ignored_index <= 16 ||
            (ignored_index & (ignored_index - 1)) == 0) {
            trace_stderr(
                "ignored_invalid_logger_memset count=%u "
                "caller=0x%016llX object=0x%016llX "
                "string=0x%016llX extra=0x%016llX\n",
                ignored_index,
                static_cast<unsigned long long>(caller),
                static_cast<unsigned long long>(destination),
                static_cast<unsigned long long>(value),
                static_cast<unsigned long long>(count));
        }
        return destination;
    }
    bool warm_commit_astro_arena = false;
    if (caller == 0x00000008000291FBULL &&
        (value & 0xFFU) == 0) {
        struct ArenaZero {
            std::uint64_t address;
            std::uint64_t size;
        };
        static constexpr std::array<ArenaZero, 5> kAstroArenas = {{
            {0x00000004C0000000ULL, 0x000000003F800000ULL},
            {0x0000000567580000ULL, 0x0000000044160000ULL},
            {0x00000005EF800000ULL, 0x0000000044160000ULL},
            {0x0000000677980000ULL, 0x0000000044160000ULL},
            {0x00000006FFC00000ULL, 0x0000000044160000ULL},
        }};
        const auto astro_arena_index =
            g_astro_arena_zero_index.fetch_add(
                1,
                std::memory_order_relaxed);
        if (astro_arena_index < kAstroArenas.size()) {
            const auto expected = kAstroArenas[astro_arena_index];
            if (destination == expected.address &&
                count == expected.size) {
                warm_commit_astro_arena = true;
            } else if (astro_arena_index >= 3) {
                trace_stderr(
                    "astro_arena_zero_recovered index=%llu "
                    "original_dst=0x%016llX original_size=0x%016llX "
                    "dst=0x%016llX size=0x%016llX\n",
                    static_cast<unsigned long long>(astro_arena_index),
                    static_cast<unsigned long long>(destination),
                    static_cast<unsigned long long>(count),
                    static_cast<unsigned long long>(expected.address),
                    static_cast<unsigned long long>(expected.size));
                destination = expected.address;
                count = expected.size;
                warm_commit_astro_arena = true;
            } else {
                g_astro_arena_zero_index.store(
                    0,
                    std::memory_order_relaxed);
            }
        } else {
            const auto expected = kAstroArenas.back();
            const bool destination_in_guest_aperture =
                destination >= kGuestLowApertureStart &&
                destination < kGuestLowApertureEnd &&
                count <= kGuestLowApertureEnd - destination;
            if (!destination_in_guest_aperture &&
                count == expected.size) {
                trace_stderr(
                    "astro_arena_zero_recovered index=%llu "
                    "original_dst=0x%016llX original_size=0x%016llX "
                    "dst=0x%016llX size=0x%016llX repeat_last=1\n",
                    static_cast<unsigned long long>(astro_arena_index),
                    static_cast<unsigned long long>(destination),
                    static_cast<unsigned long long>(count),
                    static_cast<unsigned long long>(expected.address),
                    static_cast<unsigned long long>(expected.size));
                destination = expected.address;
                warm_commit_astro_arena = true;
            } else {
                g_astro_arena_zero_index.store(
                    0,
                    std::memory_order_relaxed);
            }
        }
    }
    if (warm_commit_astro_arena) {
        constexpr std::uint64_t kAstroArenaWarmCommitSize = 4ULL * 1024 * 1024;
        const auto commit_size = std::min(count, kAstroArenaWarmCommitSize);
        if (!ensure_guest_low_aperture_committed(
                destination,
                commit_size)) {
            trace_stderr(
                "astro_arena_warm_commit_failed "
                "dst=0x%016llX size=0x%016llX\n",
                static_cast<unsigned long long>(destination),
                static_cast<unsigned long long>(commit_size));
        }
    }

    constexpr std::uint64_t kCanonicalUserUpper = 0x0000800000000000ULL;
    if (destination == 0 || destination >= kCanonicalUserUpper) {
        static std::atomic<std::uint32_t> recovery_count = 0;
        const auto recovery_index =
            recovery_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (recovery_index <= 8) {
            trace_stderr(
                "memset_invalid_destination recovery=%u dst=0x%016llX "
                "count=0x%016llX value=0x%02llX\n",
                recovery_index,
                static_cast<unsigned long long>(destination),
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(value & 0xFF));
        }
        return destination;
    }

    if (count > 0x10000000ULL) {
        trace_stderr(
            "guest_memset_large dst=0x%016llX size=0x%016llX "
            "value=0x%02llX caller=0x%016llX\n",
            static_cast<unsigned long long>(destination),
            static_cast<unsigned long long>(count),
            static_cast<unsigned long long>(value & 0xFFU),
            static_cast<unsigned long long>(caller));
    }

    if ((value & 0xFFU) == 0 &&
        zero_guest_low_aperture_sparse(destination, count)) {
        return original_destination;
    }
    if (destination >= kGuestLowApertureStart &&
        destination < kGuestLowApertureEnd &&
        !ensure_guest_low_aperture_committed(destination, count)) {
        trace_stderr(
            "guest_aperture_commit_failed dst=0x%016llX "
            "size=0x%016llX value=0x%02llX\n",
            static_cast<unsigned long long>(destination),
            static_cast<unsigned long long>(count),
            static_cast<unsigned long long>(value & 0xFFU));
        return original_destination;
    }
    std::memset(
        reinterpret_cast<void*>(destination),
        static_cast<int>(value),
        static_cast<std::size_t>(count));
    return original_destination;
}

extern "C" PS5_GUEST_ABI std::int64_t ps5rt_memcmp(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t count) {
    if (count == 0) {
        return 0;
    }
    return std::memcmp(
        reinterpret_cast<const void*>(left),
        reinterpret_cast<const void*>(right),
        static_cast<std::size_t>(count));
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_qsort(
    std::uint64_t base,
    std::uint64_t count,
    std::uint64_t element_size,
    std::uint64_t comparator_address) {
    if (base == 0 || count < 2 || element_size == 0 ||
        comparator_address == 0 ||
        count > std::numeric_limits<std::size_t>::max() ||
        element_size > std::numeric_limits<std::size_t>::max() ||
        count > std::numeric_limits<std::size_t>::max() / element_size) {
        return 0;
    }

    using Comparator = std::int32_t(PS5_GUEST_ABI*)(
        const void*,
        const void*);
    const auto comparator =
        reinterpret_cast<Comparator>(comparator_address);
    auto* bytes = reinterpret_cast<std::uint8_t*>(base);
    const auto length = static_cast<std::size_t>(count);
    const auto stride = static_cast<std::size_t>(element_size);

    const auto element = [&](std::size_t index) {
        return bytes + index * stride;
    };
    const auto swap_elements = [&](std::size_t left, std::size_t right) {
        if (left == right) {
            return;
        }
        auto* left_bytes = element(left);
        auto* right_bytes = element(right);
        for (std::size_t offset = 0; offset < stride; ++offset) {
            std::swap(left_bytes[offset], right_bytes[offset]);
        }
    };
    const auto sift_down = [&](std::size_t root, std::size_t end) {
        for (;;) {
            const auto left_child = root * 2 + 1;
            if (left_child >= end) {
                return;
            }
            auto selected = left_child;
            const auto right_child = left_child + 1;
            if (right_child < end &&
                comparator(element(left_child), element(right_child)) < 0) {
                selected = right_child;
            }
            if (comparator(element(root), element(selected)) >= 0) {
                return;
            }
            swap_elements(root, selected);
            root = selected;
        }
    };

    for (std::size_t start = length / 2; start > 0; --start) {
        sift_down(start - 1, length);
    }
    for (std::size_t end = length; end > 1; --end) {
        swap_elements(0, end - 1);
        sift_down(0, end - 1);
    }
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_memchr(
    std::uint64_t source,
    std::uint64_t value,
    std::uint64_t count) {
    return reinterpret_cast<std::uint64_t>(std::memchr(
        reinterpret_cast<const void*>(source),
        static_cast<int>(value),
        static_cast<std::size_t>(count)));
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_strlen(
    std::uint64_t string_address) {
    return string_address == 0
        ? 0
        : std::strlen(reinterpret_cast<const char*>(string_address));
}

extern "C" PS5_GUEST_ABI std::int64_t ps5rt_strncmp(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t count) {
    if (count == 0) {
        return 0;
    }
    return std::strncmp(
        reinterpret_cast<const char*>(left),
        reinterpret_cast<const char*>(right),
        static_cast<std::size_t>(count));
}

// The managed strcmp translates the guest address of every single byte,
// twice, which cost 19 seconds of the first 30 in a measured run. The
// guest address space is mapped flat here, so the host libc can do the
// whole comparison against the same bytes in one call.
extern "C" PS5_GUEST_ABI std::int64_t ps5rt_strcmp(
    std::uint64_t left,
    std::uint64_t right) {
    if (left == 0 || right == 0) {
        return left == right ? 0 : (left == 0 ? -1 : 1);
    }
    return std::strcmp(
        reinterpret_cast<const char*>(left),
        reinterpret_cast<const char*>(right));
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_strcpy(
    std::uint64_t destination,
    std::uint64_t source) {
    if (destination == 0 || source == 0) {
        return destination;
    }
    std::strcpy(
        reinterpret_cast<char*>(destination),
        reinterpret_cast<const char*>(source));
    return destination;
}

// strncpy pads the tail with zeroes rather than stopping at the
// terminator, and callers rely on that; std::strncpy has the same rule.
extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_strncpy(
    std::uint64_t destination,
    std::uint64_t source,
    std::uint64_t count) {
    if (destination == 0 || count == 0) {
        return destination;
    }
    if (source == 0) {
        std::memset(
            reinterpret_cast<void*>(destination),
            0,
            static_cast<std::size_t>(count));
        return destination;
    }
    std::strncpy(
        reinterpret_cast<char*>(destination),
        reinterpret_cast<const char*>(source),
        static_cast<std::size_t>(count));
    return destination;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_random_device() {
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    auto value = static_cast<std::uint64_t>(counter.QuadPart);
    value ^= GetTickCount64();
    value ^= static_cast<std::uint64_t>(GetCurrentThreadId()) << 32;
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    return static_cast<std::uint32_t>(value);
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_strtok(
    std::uint64_t string_address,
    std::uint64_t delimiter_address) {
    thread_local char* context = nullptr;
    auto* cursor = string_address == 0
        ? context
        : reinterpret_cast<char*>(string_address);
    const auto* delimiters =
        reinterpret_cast<const char*>(delimiter_address);
    if (cursor == nullptr || delimiters == nullptr) {
        return 0;
    }

    const auto is_delimiter = [delimiters](char value) {
        for (const char* current = delimiters; *current != '\0'; ++current) {
            if (*current == value) {
                return true;
            }
        }
        return false;
    };

    while (*cursor != '\0' && is_delimiter(*cursor)) {
        ++cursor;
    }
    if (*cursor == '\0') {
        context = nullptr;
        return 0;
    }

    char* token = cursor;
    while (*cursor != '\0' && !is_delimiter(*cursor)) {
        ++cursor;
    }
    if (*cursor == '\0') {
        context = nullptr;
    } else {
        *cursor = '\0';
        context = cursor + 1;
    }
    return reinterpret_cast<std::uint64_t>(token);
}

extern "C" PS5_GUEST_ABI std::int64_t ps5rt_puts(
    std::uint64_t string_address) {
    if (string_address == 0) {
        return -1;
    }
    trace_stderr(
        "guest_puts=%s\n",
        reinterpret_cast<const char*>(string_address));
    return 0;
}

extern "C" PS5_GUEST_ABI std::int64_t ps5rt_putchar(
    std::uint64_t character) {
    return static_cast<unsigned char>(character);
}

extern "C" PS5_GUEST_ABI std::int64_t ps5rt_time(
    std::uint64_t output_address) {
    const auto value = static_cast<std::int64_t>(std::time(nullptr));
    if (output_address != 0) {
        *reinterpret_cast<std::int64_t*>(output_address) = value;
    }
    return value;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_localtime(
    std::uint64_t time_address) {
    if (time_address == 0) {
        return 0;
    }
    thread_local std::tm result = {};
    const auto value =
        static_cast<std::time_t>(*reinterpret_cast<std::int64_t*>(time_address));
    if (localtime_s(&result, &value) != 0) {
        return 0;
    }
    return reinterpret_cast<std::uint64_t>(&result);
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_asctime(
    std::uint64_t time_structure_address) {
    if (time_structure_address == 0) {
        return 0;
    }
    thread_local char buffer[32] = {};
    if (asctime_s(
            buffer,
            sizeof(buffer),
            reinterpret_cast<const std::tm*>(time_structure_address)) != 0) {
        return 0;
    }
    return reinterpret_cast<std::uint64_t>(buffer);
}

extern "C" PS5_GUEST_ABI std::int64_t ps5rt_strcpy_s(
    std::uint64_t destination_address,
    std::uint64_t destination_size,
    std::uint64_t source_address) {
    if (destination_address == 0 ||
        destination_size == 0 ||
        source_address == 0) {
        return 22;
    }
    auto* destination = reinterpret_cast<char*>(destination_address);
    const auto* source = reinterpret_cast<const char*>(source_address);
    const auto length = std::strlen(source);
    if (length >= destination_size) {
        destination[0] = '\0';
        return 34;
    }
    std::memcpy(destination, source, length + 1);
    return 0;
}

extern "C" PS5_GUEST_ABI std::int64_t ps5rt_sprintf_s(
    std::uint64_t destination_address,
    std::uint64_t destination_size,
    std::uint64_t format_address,
    std::uint64_t argument0,
    std::uint64_t argument1,
    std::uint64_t argument2) {
    if (destination_address == 0 ||
        destination_size == 0 ||
        format_address == 0) {
        return -1;
    }

    auto* destination = reinterpret_cast<char*>(destination_address);
    const auto* format = reinterpret_cast<const char*>(format_address);
    const std::array<std::uint64_t, 3> arguments = {
        argument0,
        argument1,
        argument2,
    };
    std::size_t argument_index = 0;
    std::size_t output_index = 0;
    bool overflow = false;

    const auto append_character = [&](char value) {
        if (output_index + 1 >= destination_size) {
            overflow = true;
            return;
        }
        destination[output_index++] = value;
    };
    const auto append_string = [&](const char* value) {
        if (value == nullptr) {
            value = "(null)";
        }
        while (*value != '\0' && !overflow) {
            append_character(*value++);
        }
    };
    const auto next_argument = [&]() {
        return argument_index < arguments.size()
            ? arguments[argument_index++]
            : 0ULL;
    };

    for (const char* current = format; *current != '\0' && !overflow; ++current) {
        if (*current != '%') {
            append_character(*current);
            continue;
        }
        ++current;
        if (*current == '%') {
            append_character('%');
            continue;
        }

        while (*current == '-' || *current == '+' || *current == ' ' ||
               *current == '#' || *current == '0') {
            ++current;
        }
        if (*current == '*') {
            static_cast<void>(next_argument());
            ++current;
        } else {
            while (*current >= '0' && *current <= '9') {
                ++current;
            }
        }
        if (*current == '.') {
            ++current;
            if (*current == '*') {
                static_cast<void>(next_argument());
                ++current;
            } else {
                while (*current >= '0' && *current <= '9') {
                    ++current;
                }
            }
        }
        while (*current == 'h' || *current == 'l' ||
               *current == 'j' || *current == 'z' ||
               *current == 't' || *current == 'L') {
            ++current;
        }

        if (*current == 's') {
            append_string(reinterpret_cast<const char*>(next_argument()));
            continue;
        }
        if (*current == 'c') {
            append_character(static_cast<char>(next_argument()));
            continue;
        }
        if (*current == 'p') {
            append_string("0x");
        }
        if (*current == 'd' || *current == 'i' || *current == 'u' ||
            *current == 'x' || *current == 'X' || *current == 'p') {
            char number[32] = {};
            const auto value = next_argument();
            const int base =
                (*current == 'x' || *current == 'X' || *current == 'p')
                    ? 16
                    : 10;
            std::to_chars_result converted;
            if (*current == 'd' || *current == 'i') {
                converted = std::to_chars(
                    number,
                    number + sizeof(number),
                    static_cast<std::int64_t>(value),
                    base);
            } else {
                converted = std::to_chars(
                    number,
                    number + sizeof(number),
                    value,
                    base);
            }
            if (converted.ec == std::errc()) {
                *converted.ptr = '\0';
                if (*current == 'X') {
                    for (char* character = number; *character != '\0'; ++character) {
                        if (*character >= 'a' && *character <= 'f') {
                            *character =
                                static_cast<char>(*character - 'a' + 'A');
                        }
                    }
                }
                append_string(number);
            }
            continue;
        }

        append_character('%');
        append_character(*current);
    }

    if (overflow) {
        destination[0] = '\0';
        return -1;
    }
    destination[output_index] = '\0';
    return static_cast<std::int64_t>(output_index);
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_rand() {
    thread_local std::uint32_t state =
        0x9E3779B9U ^ static_cast<std::uint32_t>(GetCurrentThreadId());
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state & 0x7FFFFFFFU;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_ipmi_config_construct(
    std::uint64_t object_address) {
    return object_address;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_ipmi_return_zero(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t) {
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_ipmi_query_status(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t output_address) {
    if (output_address != 0) {
        *reinterpret_cast<std::int32_t*>(output_address) = 0;
    }
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_ipmi_client_create(
    std::uint64_t output_address,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t) {
    if (output_address == 0) {
        return 0x80020016;
    }
    static std::array<std::uint64_t, 64> vtable = []() {
        std::array<std::uint64_t, 64> table = {};
        table.fill(reinterpret_cast<std::uint64_t>(&ps5rt_ipmi_return_zero));
        table[2] = reinterpret_cast<std::uint64_t>(&ps5rt_ipmi_query_status);
        return table;
    }();
    auto* client = VirtualAlloc(
        nullptr,
        0x1000,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);
    if (client != nullptr) {
        *static_cast<std::uint64_t*>(client) =
            reinterpret_cast<std::uint64_t>(vtable.data());
    }
    *reinterpret_cast<void**>(output_address) = client;
    return client == nullptr ? 0x8002000C : 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_app_content_initialize(
    std::uint64_t init_parameter_address,
    std::uint64_t boot_parameter_address) {
    if (init_parameter_address == 0 || boot_parameter_address == 0) {
        return 0x80020016;
    }
    *reinterpret_cast<std::uint32_t*>(boot_parameter_address + 4) = 0;
    trace_stderr("hle=sceAppContentInitialize result=0\n");
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_app_content_param_get_int(
    std::uint64_t parameter_id,
    std::uint64_t output_address) {
    if (output_address == 0) {
        return 0x80020016;
    }
    *reinterpret_cast<std::int32_t*>(output_address) =
        parameter_id == 0 ? 3 : 0;
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_stack_check_guard() {
    PS5_HLE_GUARD();
    return reinterpret_cast<std::uint64_t>(g_stack_check_guard);
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_stack_check_fail() {
    PS5_HLE_GUARD();
    trace_stderr("__stack_chk_fail\n");
    return kUnresolvedImportResult;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_get_proc_param() {
    PS5_HLE_GUARD();
    return generated::kProcParamAddress;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_get_errno_address() {
    PS5_HLE_GUARD();
    return reinterpret_cast<std::uint64_t>(&g_guest_errno);
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_set_thread_dtors(
    std::uint64_t callback) {
    PS5_HLE_GUARD();
    g_thread_dtors_callback = callback;
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_set_thread_atexit_count(
    std::uint64_t callback) {
    PS5_HLE_GUARD();
    g_thread_atexit_count_callback = callback;
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_set_thread_atexit_report(
    std::uint64_t callback) {
    PS5_HLE_GUARD();
    g_thread_atexit_report_callback = callback;
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_tls_get_addr(
    std::uint64_t tls_info_address) {
    PS5_HLE_GUARD();
    const auto guest_thread_pointer = get_guest_thread_pointer();
    if (tls_info_address == 0 || guest_thread_pointer == 0) {
        return 0;
    }
    const auto* tls_info =
        reinterpret_cast<const std::uint64_t*>(tls_info_address);
    const auto module_id = tls_info[0];
    const auto offset = tls_info[1];
    for (const auto& module : generated::kTlsModules) {
        if (module.module_id == module_id &&
            offset < module.memory_size) {
            return guest_thread_pointer - module.static_offset + offset;
        }
    }
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_self() {
    PS5_HLE_GUARD();
    const auto result = static_cast<std::uint64_t>(GetCurrentThreadId());
    restore_guest_fs();
    return result;
}

// PS5RT_TRACE_THREADID=1: each thread and guest caller once - which code
// asked a thread for its id, to find where one registers. Kept out of the
// guest-ABI function: its exception tables there broke the assembler.
__attribute__((noinline)) void trace_thread_id_caller(
    std::uint64_t id, std::uint64_t caller) {
    static const bool trace_ids = [] {
        const auto* value = std::getenv("PS5RT_TRACE_THREADID");
        return value != nullptr && value[0] == '1';
    }();
    if (!trace_ids) {
        return;
    }
    static SRWLOCK seen_lock = SRWLOCK_INIT;
    static std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
    AcquireSRWLockExclusive(&seen_lock);
    const auto first = seen.insert({id, caller}).second;
    ReleaseSRWLockExclusive(&seen_lock);
    if (first) {
        trace_stderr("threadid id=%llu caller=0x%016llX\n",
                     static_cast<unsigned long long>(id),
                     static_cast<unsigned long long>(caller));
    }
}

// A thread's id as the title sees it: unique for the life of the process.
// Windows hands a finished thread's id to the next thread it creates within
// moments, which a console never does. Havok keys its per-thread contexts
// by this id and drops a thread's entry when it leaves; a new thread that
// inherited a departed one's id lost its entry to that drop, and the next
// physics operation on it read through the missing context - a loader
// thread dead on the white screen, the game there for good.
// PS5RT_WINDOWS_THREAD_IDS=1 goes back to the Windows ids.
__attribute__((noinline)) std::uint64_t unique_guest_thread_id() {
    static const bool windows_ids = [] {
        const auto* value = std::getenv("PS5RT_WINDOWS_THREAD_IDS");
        return value != nullptr && value[0] == '1';
    }();
    if (windows_ids) {
        return GetCurrentThreadId();
    }
    static const DWORD slot = TlsAlloc();
    static std::atomic<std::uint32_t> next{0x10000};
    auto id = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(TlsGetValue(slot)));
    if (id == 0) {
        id = next.fetch_add(1, std::memory_order_relaxed);
        TlsSetValue(slot, reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(id)));
    }
    return id;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_getthreadid() {
    PS5_HLE_GUARD();
    const auto result = unique_guest_thread_id();
    trace_thread_id_caller(
        result,
        reinterpret_cast<std::uint64_t>(__builtin_return_address(0)));
    restore_guest_fs();
    return result;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_equal(
    std::uint64_t left,
    std::uint64_t right) {
    PS5_HLE_GUARD();
    return left == right ? 1 : 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_yield() {
    PS5_HLE_GUARD();
    SwitchToThread();
    restore_guest_fs();
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_once(
    std::uint32_t* once,
    std::uint64_t callback) {
    PS5_HLE_GUARD();
    if (once == nullptr || callback == 0) {
        return 0x80020016;
    }
    if (*once != 2U) {
        using Callback = void (PS5_GUEST_ABI*)();
        reinterpret_cast<Callback>(callback)();
        *once = 2U;
    }
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_key_create(
    std::uint32_t* out_key,
    std::uint64_t destructor) {
    PS5_HLE_GUARD();
    if (out_key == nullptr) {
        return 0x80020016;
    }

    AcquireSRWLockExclusive(&g_pthread_key_lock);
    std::uint32_t key = 0;
    for (std::size_t attempt = 1; attempt < kPthreadKeyCapacity; ++attempt) {
        const auto candidate = static_cast<std::uint32_t>(
            (g_next_pthread_key + attempt - 1) % kPthreadKeyCapacity);
        if (candidate != 0 && !g_pthread_key_active[candidate]) {
            key = candidate;
            g_pthread_key_active[key] = true;
            g_pthread_key_destructors[key] = destructor;
            g_next_pthread_key = key + 1;
            if (g_next_pthread_key >= kPthreadKeyCapacity) {
                g_next_pthread_key = 1;
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_pthread_key_lock);

    if (key == 0) {
        restore_guest_fs();
        return 0x8002000C;
    }
    *out_key = key;
    trace_stderr(
        "hle=scePthreadKeyCreate key=%u destructor=0x%016llX\n",
        key,
        static_cast<unsigned long long>(destructor));
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_key_delete(
    std::uint32_t key) {
    PS5_HLE_GUARD();
    if (key == 0 || key >= kPthreadKeyCapacity) {
        return 0x80020016;
    }

    AcquireSRWLockExclusive(&g_pthread_key_lock);
    const bool active = g_pthread_key_active[key];
    if (active) {
        g_pthread_key_active[key] = false;
        g_pthread_key_destructors[key] = 0;
    }
    ReleaseSRWLockExclusive(&g_pthread_key_lock);
    if (!active) {
        restore_guest_fs();
        return 0x80020003;
    }
    g_pthread_specific_values[key] = 0;
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_setspecific(
    std::uint32_t key,
    std::uint64_t value) {
    PS5_HLE_GUARD();
    if (key == 0 || key >= kPthreadKeyCapacity) {
        return 0x80020016;
    }

    AcquireSRWLockShared(&g_pthread_key_lock);
    const bool active = g_pthread_key_active[key];
    ReleaseSRWLockShared(&g_pthread_key_lock);
    if (!active) {
        restore_guest_fs();
        return 0x80020003;
    }
    g_pthread_specific_values[key] = value;
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_getspecific(
    std::uint32_t key) {
    PS5_HLE_GUARD();
    if (key == 0 || key >= kPthreadKeyCapacity) {
        return 0;
    }

    AcquireSRWLockShared(&g_pthread_key_lock);
    const bool active = g_pthread_key_active[key];
    ReleaseSRWLockShared(&g_pthread_key_lock);
    const auto value = active ? g_pthread_specific_values[key] : 0;
    restore_guest_fs();
    return value;
}

void* allocate_pthread_object(std::size_t size) {
    return VirtualAlloc(
        nullptr,
        size,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);
}

bool try_read_process_bytes(
    std::uint64_t address,
    void* destination,
    std::size_t size) {
    if (address < 0x10000 || destination == nullptr || size == 0) {
        return false;
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

bool try_write_process_bytes(
    std::uint64_t address,
    const void* source,
    std::size_t size) {
    if (address < 0x10000 || source == nullptr || size == 0) {
        return false;
    }
    // Past the GPU runtime's write watch: say so, or a texture written
    // here keeps its stale upload and a watched page refuses the write.
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

bool read_guest_c_string(
    std::uint64_t address,
    char* buffer,
    std::size_t capacity) noexcept {
    if (address < 0x10000 || buffer == nullptr || capacity == 0) {
        return false;
    }
    for (std::size_t index = 0; index < capacity; ++index) {
        char value = '\0';
        if (!try_read_process_bytes(address + index, &value, sizeof(value))) {
            buffer[0] = '\0';
            return false;
        }
        buffer[index] = value;
        if (value == '\0') {
            return true;
        }
    }
    buffer[0] = '\0';
    return false;
}

void* resolve_import(std::string_view nid, bool& resolved);
extern "C" void* ps5rt_avplayer_handler(const char* nid, std::size_t length);
extern "C" void* ps5rt_json_handler(const char* nid, std::size_t length);

std::int64_t ps5rt_strtol_impl(
    std::uint64_t string_address,
    std::uint64_t end_pointer_address,
    std::int32_t base) {
    std::array<char, 4097> text = {};
    if (!read_guest_c_string(
            string_address,
            text.data(),
            text.size())) {
        g_guest_errno = EFAULT;
        return 0;
    }

    errno = 0;
    char* local_end = nullptr;
    const auto result = std::strtoll(text.data(), &local_end, base);
    g_guest_errno = errno;
    if (end_pointer_address != 0) {
        const auto consumed = static_cast<std::uint64_t>(
            local_end - text.data());
        const auto guest_end = string_address + consumed;
        if (!try_write_process_bytes(
                end_pointer_address,
                &guest_end,
                sizeof(guest_end))) {
            g_guest_errno = EFAULT;
        }
    }
    return static_cast<std::int64_t>(result);
}

extern "C" PS5_GUEST_ABI std::int64_t ps5rt_strtol(
    std::uint64_t string_address,
    std::uint64_t end_pointer_address,
    std::int32_t base) {
    PS5_HLE_GUARD();
    return ps5rt_strtol_impl(
        string_address,
        end_pointer_address,
        base);
}

extern "C" PS5_GUEST_ABI double ps5rt_sin(double value) {
    PS5_HLE_GUARD();
    return std::sin(value);
}

extern "C" PS5_GUEST_ABI float ps5rt_sinf(float value) {
    PS5_HLE_GUARD();
    return std::sin(value);
}

extern "C" PS5_GUEST_ABI double ps5rt_cos(double value) {
    PS5_HLE_GUARD();
    return std::cos(value);
}

extern "C" PS5_GUEST_ABI float ps5rt_cosf(float value) {
    PS5_HLE_GUARD();
    return std::cos(value);
}

extern "C" PS5_GUEST_ABI std::uint64_t
ps5rt_configured_flexible_memory_size(std::uint64_t output_address) {
    PS5_HLE_GUARD();
    if (output_address == 0) {
        return kKernelErrorInvalidArgument;
    }
    if (!try_write_process_bytes(
            output_address,
            &kFlexibleMemorySize,
            sizeof(kFlexibleMemorySize))) {
        return kKernelErrorFault;
    }
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_get_current_cpu() {
    PS5_HLE_GUARD();
    return 0;
}

std::uint64_t ps5rt_kernel_dlsym_impl(
    std::int32_t,
    std::uint64_t symbol_address,
    std::uint64_t output_address) {
    if (output_address == 0) {
        return kKernelErrorFault;
    }
    std::array<char, 513> symbol_name = {};
    if (!read_guest_c_string(
            symbol_address,
            symbol_name.data(),
            symbol_name.size())) {
        const std::uint64_t empty = 0;
        try_write_process_bytes(output_address, &empty, sizeof(empty));
        return kKernelErrorFault;
    }

    const auto resolve_nid = [](std::string_view nid) {
        bool resolved = false;
        void* handler = resolve_import(nid, resolved);
        if (resolved &&
            handler != reinterpret_cast<void*>(&ps5rt_sharpemu_import) &&
            handler != reinterpret_cast<void*>(&ps5rt_unresolved_import)) {
            return reinterpret_cast<std::uint64_t>(handler);
        }
        for (const auto& symbol : generated::kRuntimeSymbols) {
            if (symbol.nid != nullptr && nid == symbol.nid) {
                return symbol.address;
            }
        }
        return std::uint64_t{0};
    };

    auto resolved_address = resolve_nid(symbol_name.data());
#if defined(PS5RT_HAS_GENERATED_HLE)
    if (resolved_address == 0) {
        for (const auto& symbol : ps5rt::hle::kHleCatalog) {
            if (symbol.name != nullptr &&
                std::strcmp(symbol_name.data(), symbol.name) == 0) {
                resolved_address = resolve_nid(symbol.nid);
                if (resolved_address != 0) {
                    break;
                }
            }
        }
    }
#endif

    if (!try_write_process_bytes(
            output_address,
            &resolved_address,
            sizeof(resolved_address))) {
        return kKernelErrorFault;
    }
    if (resolved_address == 0) {
        trace_stderr(
            "hle=sceKernelDlsym symbol=%s result=not_found\n",
            symbol_name.data());
        return kKernelErrorSrch;
    }
    trace_stderr(
        "hle=sceKernelDlsym symbol=%s result=0x%016llX\n",
        symbol_name.data(),
        static_cast<unsigned long long>(resolved_address));
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_kernel_dlsym(
    std::int32_t handle,
    std::uint64_t symbol_address,
    std::uint64_t output_address) {
    PS5_HLE_GUARD();
    return ps5rt_kernel_dlsym_impl(
        handle,
        symbol_address,
        output_address);
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_mutexattr_init(
    std::uint64_t attr_address) {
    PS5_HLE_GUARD();
    if (attr_address == 0) {
        return 0x80020016;
    }
    auto* object = static_cast<std::uint8_t*>(
        allocate_pthread_object(0x40));
    if (object == nullptr) {
        return 0x8002000C;
    }
    *reinterpret_cast<std::uint32_t*>(object) = 1;
    *reinterpret_cast<std::uint32_t*>(object + 4) = 0;
    *reinterpret_cast<std::uint64_t*>(attr_address) =
        reinterpret_cast<std::uint64_t>(object);
    trace_stderr(
        "hle=scePthreadMutexattrInit attr=0x%016llX handle=0x%016llX\n",
        static_cast<unsigned long long>(attr_address),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uint64_t>(object)));
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_mutexattr_settype(
    std::uint64_t attr_address,
    std::int32_t type) {
    PS5_HLE_GUARD();
    if (attr_address == 0) {
        return 0x80020016;
    }
    auto* object = reinterpret_cast<std::uint8_t*>(
        *reinterpret_cast<std::uint64_t*>(attr_address));
    if (object == nullptr) {
        return 0x80020016;
    }
    *reinterpret_cast<std::uint32_t*>(object) =
        static_cast<std::uint32_t>(type);
    trace_stderr(
        "hle=scePthreadMutexattrSettype attr=0x%016llX "
        "handle=0x%016llX type=%d\n",
        static_cast<unsigned long long>(attr_address),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uint64_t>(object)),
        type);
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_mutexattr_setprotocol(
    std::uint64_t attr_address,
    std::int32_t protocol) {
    PS5_HLE_GUARD();
    if (attr_address == 0) {
        return 0x80020016;
    }
    auto* object = reinterpret_cast<std::uint8_t*>(
        *reinterpret_cast<std::uint64_t*>(attr_address));
    if (object == nullptr) {
        return 0x80020016;
    }
    *reinterpret_cast<std::uint32_t*>(object + 4) =
        static_cast<std::uint32_t>(protocol);
    trace_stderr(
        "hle=scePthreadMutexattrSetprotocol attr=0x%016llX "
        "handle=0x%016llX protocol=%d\n",
        static_cast<unsigned long long>(attr_address),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uint64_t>(object)),
        protocol);
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_mutexattr_destroy(
    std::uint64_t attr_address) {
    PS5_HLE_GUARD();
    if (attr_address == 0) {
        return 0x80020016;
    }
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_mutex_init(
    std::uint64_t mutex_address,
    std::uint64_t attr_address) {
    PS5_HLE_GUARD();
    if (mutex_address == 0) {
        return 0x80020016;
    }

    std::uint32_t type = 1;
    std::uint32_t protocol = 0;
    if (attr_address != 0) {
        std::uint64_t attr_handle = 0;
        std::array<std::uint32_t, 2> attr_values = {};
        if (try_read_process_bytes(
                attr_address,
                &attr_handle,
                sizeof(attr_handle)) &&
            attr_handle != 0 &&
            try_read_process_bytes(
                attr_handle,
                attr_values.data(),
                sizeof(attr_values))) {
            type = attr_values[0] >= 1 && attr_values[0] <= 4
                ? attr_values[0]
                : 1;
            protocol = attr_values[1];
        } else if (attr_handle != 0) {
            trace_stderr(
                "hle=scePthreadMutexInit attr_fallback=default "
                "attr=0x%016llX raw=0x%016llX\n",
                static_cast<unsigned long long>(attr_address),
                static_cast<unsigned long long>(attr_handle));
        }
    }

    auto* object = static_cast<std::uint8_t*>(
        allocate_pthread_object(0x100));
    if (object == nullptr) {
        return 0x8002000C;
    }
    *reinterpret_cast<std::uint32_t*>(object + 0x20) = type;
    *reinterpret_cast<std::uint32_t*>(object + 0x3C) = protocol;
    *reinterpret_cast<std::uint64_t*>(mutex_address) =
        reinterpret_cast<std::uint64_t>(object);
    trace_stderr(
        "hle=scePthreadMutexInit mutex=0x%016llX "
        "handle=0x%016llX type=%u protocol=%u\n",
        static_cast<unsigned long long>(mutex_address),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uint64_t>(object)),
        type,
        protocol);
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_mutex_destroy(
    std::uint64_t mutex_address) {
    PS5_HLE_GUARD();
    if (mutex_address == 0) {
        return 0x80020016;
    }
    *reinterpret_cast<std::uint64_t*>(mutex_address) = 0;
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_attr_init(
    std::uint64_t attr_address) {
    PS5_HLE_GUARD();
    if (attr_address == 0) {
        return 0x80020016;
    }
    auto* object = static_cast<std::uint8_t*>(
        allocate_pthread_object(0x100));
    if (object == nullptr) {
        restore_guest_fs();
        return 0x8002000C;
    }
    *reinterpret_cast<std::uint64_t*>(object + 0x08) = 0x4000;
    *reinterpret_cast<std::uint64_t*>(object + 0x10) = 0x100000;
    *reinterpret_cast<std::uint64_t*>(attr_address) =
        reinterpret_cast<std::uint64_t>(object);
    trace_stderr(
        "hle=scePthreadAttrInit attr=0x%016llX handle=0x%016llX\n",
        static_cast<unsigned long long>(attr_address),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uint64_t>(object)));
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_attr_destroy(
    std::uint64_t attr_address) {
    PS5_HLE_GUARD();
    if (attr_address == 0) {
        return 0x80020016;
    }
    *reinterpret_cast<std::uint64_t*>(attr_address) = 0;
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_pthread_attr_getaffinity(
    std::uint64_t,
    std::uint64_t out_mask_address) {
    PS5_HLE_GUARD();
    if (out_mask_address == 0) {
        return 0x80020016;
    }
    *reinterpret_cast<std::uint64_t*>(out_mask_address) = 0x7F;
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_write(
    std::int32_t file_descriptor,
    const void* buffer,
    std::uint64_t requested) {
    PS5_HLE_GUARD();
    if ((buffer == nullptr && requested != 0) ||
        (file_descriptor != 1 && file_descriptor != 2)) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    const auto handle = GetStdHandle(
        file_descriptor == 1
            ? STD_OUTPUT_HANDLE
            : STD_ERROR_HANDLE);
    const auto* source = static_cast<const std::uint8_t*>(buffer);
    std::uint64_t total = 0;
    while (total < requested) {
        const auto chunk = static_cast<DWORD>(std::min<std::uint64_t>(
            requested - total,
            std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(
                handle,
                source + total,
                chunk,
                &written,
                nullptr)) {
            restore_guest_fs();
            return std::numeric_limits<std::uint64_t>::max();
        }
        total += written;
        if (written != chunk) {
            break;
        }
    }
    restore_guest_fs();
    return total;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_set_application_heap_api(
    std::uint64_t address) {
    PS5_HLE_GUARD();
    trace_stderr(
        "hle=_sceKernelRtldSetApplicationHeapAPI address=0x%016llX\n",
        static_cast<unsigned long long>(address));
    g_application_heap_api = address;
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_libc_heap_get_trace_info(
    std::uint64_t info_address) {
    PS5_HLE_GUARD();
    trace_stderr(
        "hle=sceLibcHeapGetTraceInfo info=0x%016llX\n",
        static_cast<unsigned long long>(info_address));
    if (info_address == 0 ||
        *reinterpret_cast<const std::uint64_t*>(info_address) != 32) {
        return 0x80020016;
    }
    auto* info = reinterpret_cast<std::uint64_t*>(info_address);
    info[2] = reinterpret_cast<std::uint64_t>(g_heap_trace_storage);
    info[3] = reinterpret_cast<std::uint64_t>(
        g_heap_trace_storage + sizeof(std::uint64_t));
    return 0;
}

// --- C++ ABI stubs (critical for Astro Bot) ---

struct AtExitEntry {
    void (*func)(void*);
    void* arg;
};
std::vector<AtExitEntry> g_atexit_entries;
SRWLOCK g_atexit_lock = SRWLOCK_INIT;
SRWLOCK g_cxa_guard_lock = SRWLOCK_INIT;
struct CxaGuardState {
    DWORD owner_thread = 0;
    HANDLE completion_event = nullptr;
};
std::map<std::uint64_t, CxaGuardState> g_cxa_guard_states;

constexpr LONG64 kCxaGuardComplete = 0x0000000000000001LL;
constexpr LONG64 kCxaGuardPending = 0x0000000000000100LL;
constexpr LONG64 kCxaGuardStateMask = 0x000000000000FFFFLL;

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_cxa_atexit(
    std::uint64_t func, std::uint64_t arg, std::uint64_t) {
    PS5_HLE_GUARD();
    AcquireSRWLockExclusive(&g_atexit_lock);
    if (g_atexit_entries.capacity() == 0) {
        g_atexit_entries.reserve(8192);
    }
    g_atexit_entries.push_back({
        reinterpret_cast<void(*)(void*)>(func),
        reinterpret_cast<void*>(arg)
    });
    ReleaseSRWLockExclusive(&g_atexit_lock);
    trace_stderr("hle=__cxa_atexit func=0x%016llX arg=0x%016llX\n",
        static_cast<unsigned long long>(func),
        static_cast<unsigned long long>(arg));
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_cxa_guard_acquire(
    std::uint64_t guard_addr) {
    PS5_HLE_GUARD();
    if (guard_addr == 0) {
        return 0;
    }

    auto* guard = reinterpret_cast<volatile LONG64*>(guard_addr);
    const auto current_thread = GetCurrentThreadId();
    for (;;) {
        auto state = InterlockedCompareExchange64(guard, 0, 0);
        if ((state & kCxaGuardComplete) != 0) {
            return 0;
        }

        AcquireSRWLockExclusive(&g_cxa_guard_lock);
        state = InterlockedCompareExchange64(guard, 0, 0);
        if ((state & kCxaGuardComplete) != 0) {
            ReleaseSRWLockExclusive(&g_cxa_guard_lock);
            return 0;
        }

        auto entry = g_cxa_guard_states.find(guard_addr);
        if (entry == g_cxa_guard_states.end()) {
            CxaGuardState created;
            created.completion_event =
                CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (created.completion_event == nullptr) {
                ReleaseSRWLockExclusive(&g_cxa_guard_lock);
                return 0;
            }
            entry = g_cxa_guard_states.emplace(
                guard_addr,
                created).first;
        }

        auto& guard_state = entry->second;
        if (guard_state.owner_thread == 0) {
            ResetEvent(guard_state.completion_event);
            guard_state.owner_thread = current_thread;
            const auto pending =
                (state & ~kCxaGuardStateMask) | kCxaGuardPending;
            InterlockedExchange64(guard, pending);
            ReleaseSRWLockExclusive(&g_cxa_guard_lock);
            return 1;
        }
        if (guard_state.owner_thread == current_thread) {
            ReleaseSRWLockExclusive(&g_cxa_guard_lock);
            return 0;
        }

        const auto completion_event = guard_state.completion_event;
        ReleaseSRWLockExclusive(&g_cxa_guard_lock);
        WaitForSingleObject(completion_event, INFINITE);
    }
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_cxa_guard_release(
    std::uint64_t guard_addr) {
    PS5_HLE_GUARD();
    if (guard_addr == 0) {
        return 0;
    }

    auto* guard = reinterpret_cast<volatile LONG64*>(guard_addr);
    AcquireSRWLockExclusive(&g_cxa_guard_lock);
    const auto state = InterlockedCompareExchange64(guard, 0, 0);
    const auto complete =
        (state & ~kCxaGuardStateMask) | kCxaGuardComplete;
    InterlockedExchange64(guard, complete);
    const auto entry = g_cxa_guard_states.find(guard_addr);
    if (entry != g_cxa_guard_states.end()) {
        entry->second.owner_thread = 0;
        SetEvent(entry->second.completion_event);
    }
    ReleaseSRWLockExclusive(&g_cxa_guard_lock);
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_cxa_guard_abort(
    std::uint64_t guard_addr) {
    PS5_HLE_GUARD();
    if (guard_addr == 0) {
        return 0;
    }

    auto* guard = reinterpret_cast<volatile LONG64*>(guard_addr);
    AcquireSRWLockExclusive(&g_cxa_guard_lock);
    const auto state = InterlockedCompareExchange64(guard, 0, 0);
    const auto reset = state & ~kCxaGuardStateMask;
    InterlockedExchange64(guard, reset);
    const auto entry = g_cxa_guard_states.find(guard_addr);
    if (entry != g_cxa_guard_states.end()) {
        entry->second.owner_thread = 0;
        SetEvent(entry->second.completion_event);
    }
    ReleaseSRWLockExclusive(&g_cxa_guard_lock);
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_cxa_finalize(
    std::uint64_t) {
    PS5_HLE_GUARD();
    return 0;
}

// operator new/delete share the libc heap. Backing them with VirtualAlloc
// instead wasted a 64 KiB allocation granule per object and, more importantly,
// made the pointer un-freeable by free(), which the guest does mix.
extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_operator_new(
    std::uint64_t size) {
    PS5_HLE_GUARD();
    return reinterpret_cast<std::uint64_t>(ps5rt_mspace_malloc(size));
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_operator_new_nothrow(
    std::uint64_t size) {
    PS5_HLE_GUARD();
    return ps5rt_operator_new(size);
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_operator_delete(
    std::uint64_t ptr) {
    PS5_HLE_GUARD();
    ps5rt_mspace_free(reinterpret_cast<void*>(ptr));
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_operator_array_new(
    std::uint64_t size) {
    PS5_HLE_GUARD();
    return ps5rt_operator_new(size);
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_operator_array_delete(
    std::uint64_t ptr) {
    PS5_HLE_GUARD();
    return ps5rt_operator_delete(ptr);
}

// --- Kernel: nanosleep, sleep, usleep ---

// Sleep() takes whole milliseconds and rounds up to the scheduler's tick,
// which is 15.625 ms unless something in the process has asked for
// better. The title's lock backoff asks for 99, 199, 500 and 1000
// microseconds through sceKernelUsleep, so every one of those was being
// handed 15.6 ms - a factor of a hundred and fifty on the shortest of
// them, inside a spinlock that several threads are waiting on. A
// high-resolution waitable timer takes 100 ns units and honours them, so
// use one and keep Sleep only as the fallback.
#ifndef CREATE_WAITABLE_TIMER_MANUAL_RESET
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void sleep_microseconds(std::uint64_t microseconds) {
    if (microseconds == 0) {
        return;
    }
    // One timer per thread: a waitable timer cannot serve two waits at
    // once, and these are called from every guest thread.
    static thread_local HANDLE timer = nullptr;
    static thread_local bool timer_attempted = false;
    if (!timer_attempted) {
        timer_attempted = true;
        timer = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_MANUAL_RESET |
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS);
        if (timer == nullptr) {
            // The high resolution flag is rejected before Windows 10
            // 1803. A plain timer still beats Sleep's rounding.
            timer = CreateWaitableTimerExW(
                nullptr,
                nullptr,
                CREATE_WAITABLE_TIMER_MANUAL_RESET,
                TIMER_ALL_ACCESS);
        }
    }
    if (timer != nullptr) {
        LARGE_INTEGER due = {};
        // Negative means relative, in 100 ns units.
        due.QuadPart = -static_cast<LONGLONG>(microseconds * 10ULL);
        if (SetWaitableTimer(
                timer, &due, 0, nullptr, nullptr, FALSE) != 0) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep(static_cast<DWORD>((microseconds + 999ULL) / 1000ULL));
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_nanosleep(
    std::uint64_t req_addr, std::uint64_t rem_addr) {
    PS5_HLE_GUARD();
    struct GuestTimespec {
        std::int64_t seconds;
        std::int64_t nanoseconds;
    };

    GuestTimespec requested = {};
    if (req_addr == 0) {
        g_guest_errno = EINVAL;
        return std::numeric_limits<std::uint64_t>::max();
    }
    if (!try_read_process_bytes(
            req_addr,
            &requested,
            sizeof(requested))) {
        g_guest_errno = EFAULT;
        return std::numeric_limits<std::uint64_t>::max();
    }
    if (requested.seconds < 0 ||
        requested.nanoseconds < 0 ||
        requested.nanoseconds >= 1000000000LL) {
        g_guest_errno = EINVAL;
        return std::numeric_limits<std::uint64_t>::max();
    }

    // Rounding nanoseconds up to whole milliseconds here had the same
    // effect as usleep's: the shortest sleep the guest can ask for cost
    // a scheduler tick.
    auto remaining_microseconds =
        static_cast<std::uint64_t>(requested.seconds) * 1000000ULL;
    remaining_microseconds +=
        static_cast<std::uint64_t>(
            (requested.nanoseconds + 999LL) / 1000LL);
    sleep_microseconds(remaining_microseconds);

    if (rem_addr != 0) {
        const GuestTimespec remaining = {};
        if (!try_write_process_bytes(
                rem_addr,
                &remaining,
                sizeof(remaining))) {
            g_guest_errno = EFAULT;
            return std::numeric_limits<std::uint64_t>::max();
        }
    }
    g_guest_errno = 0;
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_sleep_ms(
    std::uint32_t ms) {
    PS5_HLE_GUARD();
    sleep_microseconds(static_cast<std::uint64_t>(ms) * 1000ULL);
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_usleep(
    std::uint32_t us) {
    PS5_HLE_GUARD();
    sleep_microseconds(us);
    return 0;
}

// --- Kernel: sceKernelClockGettime ---

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_clock_gettime(
    std::uint32_t clock_id, std::uint64_t tp_addr) {
    PS5_HLE_GUARD();
    if (tp_addr == 0) return 0x80020016;
    // A struct timespec: seconds, then nanoseconds. This used to put the
    // whole count of nanoseconds in the seconds field and zero in the
    // other, which nothing noticed until a sem_timedwait deadline built
    // from it was compared with a real clock and every wait timed out.
    // CLOCK_REALTIME (and its fast and precise variants) is the calendar
    // time; the others count from an arbitrary point that only moves on.
    std::uint64_t nanoseconds = 0;
    if (clock_id == 0 || clock_id == 9 || clock_id == 10) {
        FILETIME now = {};
        GetSystemTimePreciseAsFileTime(&now);
        const auto ticks =
            (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) |
            now.dwLowDateTime;
        constexpr std::uint64_t kUnixEpochIn100ns = 116444736000000000ULL;
        nanoseconds = (ticks - kUnixEpochIn100ns) * 100ULL;
    } else {
        LARGE_INTEGER freq, counter;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&counter);
        const auto count = static_cast<std::uint64_t>(counter.QuadPart);
        const auto frequency = static_cast<std::uint64_t>(freq.QuadPart);
        nanoseconds = (count / frequency) * 1000000000ULL +
            (count % frequency) * 1000000000ULL / frequency;
    }
    auto* tp = reinterpret_cast<std::uint64_t*>(tp_addr);
    tp[0] = nanoseconds / 1000000000ULL;
    tp[1] = nanoseconds % 1000000000ULL;
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_get_tsc_frequency() {
    PS5_HLE_GUARD();
    return 3579545;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_read_tsc() {
    PS5_HLE_GUARD();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart);
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_convert_utc_to_localtime(
    std::int64_t utc_value,
    std::uint64_t local_time_address,
    std::uint64_t timesec_address,
    std::uint64_t dst_seconds_address) {
    PS5_HLE_GUARD();
    if (local_time_address == 0) {
        return 0x80020016;
    }

    // Gen5 callers use millisecond epoch values while older callers use
    // seconds. Preserve the caller's unit when applying the timezone offset.
    const std::int64_t scale =
        utc_value > 253402300799LL || utc_value < -62135596800LL
            ? 1000
            : 1;
    const auto utc_seconds = static_cast<__time64_t>(utc_value / scale);
    std::tm local_tm = {};
    std::tm utc_tm = {};
    if (_localtime64_s(&local_tm, &utc_seconds) != 0 ||
        _gmtime64_s(&utc_tm, &utc_seconds) != 0) {
        return 0x80020016;
    }

    const auto local_as_utc = _mkgmtime64(&local_tm);
    const auto offset_seconds =
        static_cast<std::int64_t>(local_as_utc - utc_seconds);
    const auto dst_seconds = local_tm.tm_isdst > 0 ? 3600U : 0U;
    *reinterpret_cast<std::int64_t*>(local_time_address) =
        utc_value + offset_seconds * scale;

    if (timesec_address != 0) {
        auto* timesec =
            reinterpret_cast<std::uint8_t*>(timesec_address);
        *reinterpret_cast<std::int64_t*>(timesec) = utc_value;
        *reinterpret_cast<std::int32_t*>(timesec + 8) =
            static_cast<std::int32_t>(offset_seconds);
        *reinterpret_cast<std::uint32_t*>(timesec + 12) =
            dst_seconds;
    }
    if (dst_seconds_address != 0) {
        *reinterpret_cast<std::uint64_t*>(dst_seconds_address) =
            dst_seconds;
    }
    trace_stderr(
        "hle=sceKernelConvertUtcToLocaltime utc=%lld scale=%lld "
        "offset=%lld local=0x%016llX\n",
        static_cast<long long>(utc_value),
        static_cast<long long>(scale),
        static_cast<long long>(offset_seconds),
        static_cast<unsigned long long>(
            *reinterpret_cast<std::uint64_t*>(local_time_address)));
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_system_service_param_get_int(
    std::int32_t parameter_id,
    std::uint64_t value_address) {
    PS5_HLE_GUARD();
    if (value_address == 0) {
        return 0x80A10003ULL;
    }

    std::int32_t value = 0;
    switch (parameter_id) {
    case 1:    // Language
    case 2:    // Date format
    case 3:    // 24-hour time
    case 1000: // Cross button confirm
        value = 1;
        break;
    case 4: // UTC+03:00 in minutes
        value = 180;
        break;
    default:
        value = 0;
        break;
    }
    *reinterpret_cast<std::int32_t*>(value_address) = value;
    trace_stderr(
        "hle=sceSystemServiceParamGetInt id=%d value=%d out=0x%016llX\n",
        parameter_id,
        value,
        static_cast<unsigned long long>(value_address));
    return 0;
}

// --- Kernel: sceKernelGetCompiledSdkVersion ---

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_get_compiled_sdk_version() {
    PS5_HLE_GUARD();
    return 0x06000032;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_is_neo_mode() {
    PS5_HLE_GUARD();
    return 0;
}

constexpr std::uint64_t kFontInvalidArgument = 0x80020003;
constexpr std::uint64_t kFontMemoryFault = 0x80020101;

bool allocate_font_object(
    std::size_t size,
    std::uint16_t magic,
    std::uint64_t& address) {
    address = 0;
    auto* object = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr,
        size,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    if (object == nullptr) {
        return false;
    }
    std::memset(object, 0, size);
    std::memcpy(object, &magic, sizeof(magic));
    address = reinterpret_cast<std::uint64_t>(object);
    return true;
}

std::uint64_t create_font_handle(
    std::uint64_t output_address,
    std::size_t size,
    std::uint16_t magic,
    const char* operation) {
    if (output_address == 0) {
        return kFontMemoryFault;
    }

    std::uint64_t handle = 0;
    if (!allocate_font_object(size, magic, handle)) {
        return kFontMemoryFault;
    }
    if (!try_write_process_bytes(
            output_address,
            &handle,
            sizeof(handle))) {
        VirtualFree(reinterpret_cast<void*>(handle), 0, MEM_RELEASE);
        return kFontMemoryFault;
    }
    trace_stderr(
        "hle=%s out=0x%016llX handle=0x%016llX magic=0x%04X\n",
        operation,
        static_cast<unsigned long long>(output_address),
        static_cast<unsigned long long>(handle),
        static_cast<unsigned int>(magic));
    return 0;
}

bool write_fallback_glyph_metrics(std::uint64_t metrics_address) {
    const std::array<float, 8> values = {
        8.0f,
        16.0f,
        0.0f,
        12.0f,
        8.0f,
        0.0f,
        0.0f,
        16.0f,
    };
    return try_write_process_bytes(
        metrics_address,
        values.data(),
        sizeof(values));
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_font_memory_init(
    std::uint64_t descriptor_address,
    std::uint64_t region_address,
    std::uint64_t region_size,
    std::uint64_t interface_address,
    std::uint64_t mspace_address,
    std::uint64_t destroy_callback) {
    PS5_HLE_GUARD();
    if (descriptor_address == 0) {
        return kFontMemoryFault;
    }

    std::array<std::uint8_t, 0x40> descriptor = {};
    const std::uint32_t magic = 0x00000F00;
    const auto size32 = static_cast<std::uint32_t>(region_size);
    std::memcpy(descriptor.data() + 0x00, &magic, sizeof(magic));
    std::memcpy(descriptor.data() + 0x04, &size32, sizeof(size32));
    std::memcpy(
        descriptor.data() + 0x08,
        &region_address,
        sizeof(region_address));
    std::memcpy(
        descriptor.data() + 0x10,
        &mspace_address,
        sizeof(mspace_address));
    std::memcpy(
        descriptor.data() + 0x18,
        &interface_address,
        sizeof(interface_address));
    std::memcpy(
        descriptor.data() + 0x20,
        &destroy_callback,
        sizeof(destroy_callback));
    std::memcpy(
        descriptor.data() + 0x38,
        &mspace_address,
        sizeof(mspace_address));
    if (!try_write_process_bytes(
            descriptor_address,
            descriptor.data(),
            descriptor.size())) {
        return kFontMemoryFault;
    }
    trace_stderr(
        "hle=sceFontMemoryInit descriptor=0x%016llX "
        "region=0x%016llX size=0x%016llX\n",
        static_cast<unsigned long long>(descriptor_address),
        static_cast<unsigned long long>(region_address),
        static_cast<unsigned long long>(region_size));
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_font_select_library_ft(
    std::int32_t selection) {
    PS5_HLE_GUARD();
    const auto result = selection == 0
        ? reinterpret_cast<std::uint64_t>(&g_font_library_selection)
        : 0;
    trace_stderr(
        "hle=sceFontSelectLibraryFt selection=%d result=0x%016llX\n",
        selection,
        static_cast<unsigned long long>(result));
    return result;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_font_select_renderer_ft(
    std::int32_t selection) {
    PS5_HLE_GUARD();
    const auto result = selection == 0
        ? reinterpret_cast<std::uint64_t>(&g_font_renderer_selection)
        : 0;
    trace_stderr(
        "hle=sceFontSelectRendererFt selection=%d result=0x%016llX\n",
        selection,
        static_cast<unsigned long long>(result));
    return result;
}

extern "C" PS5_GUEST_ABI std::uint64_t
ps5rt_font_create_library_with_edition(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t output_address) {
    PS5_HLE_GUARD();
    return create_font_handle(
        output_address,
        0x100,
        0x0F01,
        "sceFontCreateLibraryWithEdition");
}

extern "C" PS5_GUEST_ABI std::uint64_t
ps5rt_font_create_renderer_with_edition(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t output_address) {
    PS5_HLE_GUARD();
    return create_font_handle(
        output_address,
        0x100,
        0x0F07,
        "sceFontCreateRendererWithEdition");
}

#define PS5RT_FONT_SUCCESS_HANDLER(handler, export_name)                 \
    extern "C" PS5_GUEST_ABI std::uint64_t handler() {                  \
        PS5_HLE_GUARD();                                                \
        trace_stderr("hle=" export_name "\n");                          \
        return 0;                                                       \
    }

PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_destroy_renderer,
    "sceFontDestroyRenderer")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_bind_renderer,
    "sceFontBindRenderer")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_unbind_renderer,
    "sceFontUnbindRenderer")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_set_scale_pixel,
    "sceFontSetScalePixel")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_set_effect_slant,
    "sceFontSetEffectSlant")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_set_effect_weight,
    "sceFontSetEffectWeight")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_setup_render_scale_pixel,
    "sceFontSetupRenderScalePixel")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_setup_render_effect_slant,
    "sceFontSetupRenderEffectSlant")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_setup_render_effect_weight,
    "sceFontSetupRenderEffectWeight")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_close_font,
    "sceFontCloseFont")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_support_system_fonts,
    "sceFontSupportSystemFonts")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_support_external_fonts,
    "sceFontSupportExternalFonts")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_attach_device_cache_buffer,
    "sceFontAttachDeviceCacheBuffer")
PS5RT_FONT_SUCCESS_HANDLER(
    ps5rt_font_glyph_define_attribute,
    "sceFontGlyphDefineAttribute")

#undef PS5RT_FONT_SUCCESS_HANDLER

extern "C" PS5_GUEST_ABI std::uint64_t
ps5rt_font_get_horizontal_layout(
    std::uint64_t,
    std::uint64_t layout_address) {
    PS5_HLE_GUARD();
    if (layout_address == 0) {
        return kFontInvalidArgument;
    }
    const std::array<float, 3> values = {
        12.0f,
        16.0f,
        0.0f,
    };
    if (!try_write_process_bytes(
            layout_address,
            values.data(),
            sizeof(values))) {
        return kFontMemoryFault;
    }
    trace_stderr(
        "hle=sceFontGetHorizontalLayout out=0x%016llX\n",
        static_cast<unsigned long long>(layout_address));
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_font_open_font_set(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t output_address,
    std::uint64_t) {
    PS5_HLE_GUARD();
    return create_font_handle(
        output_address,
        0x100,
        0x0F02,
        "sceFontOpenFontSet");
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_font_open_font_memory(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t output_address) {
    PS5_HLE_GUARD();
    return create_font_handle(
        output_address,
        0x100,
        0x0F02,
        "sceFontOpenFontMemory");
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_font_open_font_instance(
    std::uint64_t source_handle,
    std::uint64_t setup_handle,
    std::uint64_t output_address) {
    PS5_HLE_GUARD();
    if (output_address == 0) {
        return kFontInvalidArgument;
    }
    if (setup_handle != 0) {
        return try_write_process_bytes(
                   output_address,
                   &setup_handle,
                   sizeof(setup_handle))
            ? 0
            : kFontMemoryFault;
    }

    std::uint64_t handle = 0;
    if (!allocate_font_object(0x100, 0x0F02, handle)) {
        return kFontMemoryFault;
    }
    if (source_handle != 0) {
        std::array<std::uint8_t, 0x100> source = {};
        if (try_read_process_bytes(
                source_handle,
                source.data(),
                source.size())) {
            std::memcpy(
                reinterpret_cast<void*>(handle),
                source.data(),
                source.size());
            const std::uint16_t magic = 0x0F02;
            std::memcpy(
                reinterpret_cast<void*>(handle),
                &magic,
                sizeof(magic));
        }
    }
    if (!try_write_process_bytes(
            output_address,
            &handle,
            sizeof(handle))) {
        VirtualFree(reinterpret_cast<void*>(handle), 0, MEM_RELEASE);
        return kFontMemoryFault;
    }
    trace_stderr(
        "hle=sceFontOpenFontInstance out=0x%016llX "
        "handle=0x%016llX source=0x%016llX\n",
        static_cast<unsigned long long>(output_address),
        static_cast<unsigned long long>(handle),
        static_cast<unsigned long long>(source_handle));
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t
ps5rt_font_get_render_char_glyph_metrics(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t metrics_address) {
    PS5_HLE_GUARD();
    if (metrics_address == 0) {
        return kFontInvalidArgument;
    }
    return write_fallback_glyph_metrics(metrics_address)
        ? 0
        : kFontMemoryFault;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_font_generate_char_glyph(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t descriptor_address,
    std::uint64_t output_address) {
    PS5_HLE_GUARD();
    if (descriptor_address == 0 || output_address == 0) {
        return kFontInvalidArgument;
    }
    return try_write_process_bytes(
               output_address,
               &descriptor_address,
               sizeof(descriptor_address))
        ? 0
        : kFontMemoryFault;
}

extern "C" PS5_GUEST_ABI std::uint64_t
ps5rt_font_render_char_glyph_image_horizontal(
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::uint64_t metrics_address,
    std::uint64_t) {
    PS5_HLE_GUARD();
    if (metrics_address == 0) {
        return kFontInvalidArgument;
    }
    return write_fallback_glyph_metrics(metrics_address)
        ? 0
        : kFontMemoryFault;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_font_delete_glyph(
    std::uint64_t,
    std::uint64_t glyph_address) {
    PS5_HLE_GUARD();
    if (glyph_address == 0) {
        return kFontInvalidArgument;
    }
    const std::uint64_t empty = 0;
    return try_write_process_bytes(
               glyph_address,
               &empty,
               sizeof(empty))
        ? 0
        : kFontMemoryFault;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_font_render_surface_init(
    std::uint64_t surface_address,
    std::uint64_t buffer_address,
    std::uint64_t width_bytes,
    std::uint64_t pixel_bytes,
    std::uint64_t width,
    std::uint64_t height) {
    PS5_HLE_GUARD();
    if (surface_address == 0) {
        return kFontMemoryFault;
    }
    std::array<std::uint8_t, 0x28> surface = {};
    const auto width_bytes32 = static_cast<std::uint32_t>(width_bytes);
    const auto pixel_bytes32 =
        static_cast<std::uint32_t>(pixel_bytes) & 0xFFU;
    const auto width32 = static_cast<std::uint32_t>(width);
    const auto height32 = static_cast<std::uint32_t>(height);
    std::memcpy(
        surface.data() + 0x00,
        &buffer_address,
        sizeof(buffer_address));
    std::memcpy(
        surface.data() + 0x08,
        &width_bytes32,
        sizeof(width_bytes32));
    std::memcpy(
        surface.data() + 0x0C,
        &pixel_bytes32,
        sizeof(pixel_bytes32));
    std::memcpy(surface.data() + 0x10, &width32, sizeof(width32));
    std::memcpy(surface.data() + 0x14, &height32, sizeof(height32));
    std::memcpy(surface.data() + 0x20, &width32, sizeof(width32));
    std::memcpy(surface.data() + 0x24, &height32, sizeof(height32));
    return try_write_process_bytes(
               surface_address,
               surface.data(),
               surface.size())
        ? 0
        : kFontMemoryFault;
}

// --- Kernel: sceKernelVirtualQuery ---

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_virtual_query(
    std::uint64_t addr, std::uint32_t flags, std::uint64_t info_addr,
    std::uint64_t info_size) {
    PS5_HLE_GUARD();
    if (info_addr == 0) return 0x80020016;
    std::memset(reinterpret_cast<void*>(info_addr), 0,
        static_cast<std::size_t>(info_size));
    auto* info = reinterpret_cast<std::uint64_t*>(info_addr);
    info[0] = align_down(addr, kPageSize);
    info[1] = 0x1000;
    info[2] = 7;
    return 0;
}

// --- Kernel: sceKernelMapNamedFlexibleMemory ---

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_map_named_flexible_memory(
    std::uint64_t in_out_addr, std::uint64_t len, std::uint64_t type,
    std::uint64_t flags, std::uint64_t name) {
    PS5_HLE_GUARD();
    if (in_out_addr == 0 || len == 0) return 0x80020016;
    auto* addr_ptr = reinterpret_cast<std::uint64_t*>(in_out_addr);
    void* mapped = VirtualAlloc(
        (*addr_ptr != 0 && (flags & 0x10)) ?
            reinterpret_cast<void*>(*addr_ptr) : nullptr,
        static_cast<SIZE_T>(align_up(len, kPageSize)),
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mapped == nullptr) {
        mapped = VirtualAlloc(nullptr,
            static_cast<SIZE_T>(align_up(len, kPageSize)),
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    if (mapped == nullptr) {
        restore_guest_fs();
        return 0x8002000C;
    }
    *addr_ptr = reinterpret_cast<std::uint64_t>(mapped);
    trace_stderr("memory.map_flexible addr=0x%016llX len=0x%016llX -> 0x%016llX\n",
        static_cast<unsigned long long>(in_out_addr),
        static_cast<unsigned long long>(len),
        static_cast<unsigned long long>(*addr_ptr));
    restore_guest_fs();
    return 0;
}

// --- Kernel: sceKernelMprotect stub ---

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_mprotect(
    std::uint64_t addr, std::uint64_t len, std::int32_t prot) {
    PS5_HLE_GUARD();
    trace_stderr("hle=sceKernelMprotect addr=0x%016llX len=0x%016llX prot=%d\n",
        static_cast<unsigned long long>(addr),
        static_cast<unsigned long long>(len), prot);
    return 0;
}

// --- Kernel: sceKernelGetProcessTime / sceKernelGetProcessTimeCounter ---

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_get_process_time() {
    PS5_HLE_GUARD();
    FILETIME ft;
    GetProcessTimes(GetCurrentProcess(), &ft, &ft, &ft, &ft);
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) |
           static_cast<std::uint64_t>(ft.dwLowDateTime);
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_get_process_time_counter() {
    PS5_HLE_GUARD();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart);
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_get_process_time_counter_frequency() {
    PS5_HLE_GUARD();
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return static_cast<std::uint64_t>(freq.QuadPart);
}

// --- Kernel: sceKernelGetSystemSwVersion ---

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_get_system_sw_version(
    std::uint64_t version_addr) {
    PS5_HLE_GUARD();
    if (version_addr != 0) {
        *reinterpret_cast<std::uint32_t*>(version_addr) = 0x06000032;
    }
    return 0;
}

// --- Kernel: sceKernelSetVirtualRangeName stub ---

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_set_virtual_range_name(
    std::uint64_t, std::uint64_t, std::uint64_t) {
    PS5_HLE_GUARD();
    return 0;
}

// --- Kernel: sceKernelGetAppInfo stub ---

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_get_app_info(
    std::uint64_t info_addr) {
    PS5_HLE_GUARD();
    if (info_addr != 0) {
        std::memset(reinterpret_cast<void*>(info_addr), 0, 0x100);
    }
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_get_direct_memory_size() {
    PS5_HLE_GUARD();
    trace_stderr(
        "hle=sceKernelGetDirectMemorySize result=0x%016llX\n",
        static_cast<unsigned long long>(kDirectMemorySize));
    return kDirectMemorySize;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_allocate_direct_memory(
    std::int64_t search_start,
    std::int64_t search_end,
    std::uint64_t length,
    std::uint64_t alignment,
    std::uint64_t,
    std::uint64_t out_address) {
    PS5_HLE_GUARD();
    trace_stderr(
        "memory.allocate_direct start=0x%016llX "
        "end=0x%016llX length=0x%016llX alignment=0x%016llX "
        "out=0x%016llX\n",
        static_cast<unsigned long long>(search_start),
        static_cast<unsigned long long>(search_end),
        static_cast<unsigned long long>(length),
        static_cast<unsigned long long>(alignment),
        static_cast<unsigned long long>(out_address));
    if (length == 0 || out_address == 0) {
        return 0x80020016;
    }
    const auto effective_alignment = alignment == 0 ? 0x4000ULL : alignment;
    auto start = search_start < 0
        ? 0ULL
        : static_cast<std::uint64_t>(search_start);
    auto end = search_end <= 0
        ? kDirectMemorySize
        : std::min(
            static_cast<std::uint64_t>(search_end),
            kDirectMemorySize);
    auto selected = align_up(
        std::max(start, g_next_direct_memory_offset),
        effective_alignment);
    if (selected >= end || length > end - selected) {
        return 0x8002000B;
    }
    *reinterpret_cast<std::uint64_t*>(out_address) = selected;
    g_next_direct_memory_offset = selected + length;
    return 0;
}

// A map this large is nearly all untouched: the title asks for 10.2 GB
// across six calls and the commit for it is what runs the host out. Above
// the threshold the range is reserved and committed as it is used, by the
// fault handler and by ps5rt_guest_commit_range on the paths that write
// guest memory from this side. PS5RT_EAGER_COMMIT=1 restores the old
// behaviour.
constexpr std::uint64_t kLazyCommitThreshold =
    64ULL * 1024ULL * 1024ULL;

bool lazy_commit_enabled() {
    static const bool enabled =
        !environment_flag_enabled("PS5RT_EAGER_COMMIT");
    return enabled;
}

DWORD guest_allocation_flags(std::uint64_t length) {
    return lazy_commit_enabled() && length >= kLazyCommitThreshold
        ? MEM_RESERVE
        : (MEM_RESERVE | MEM_COMMIT);
}

void note_guest_allocation(void* mapped, std::uint64_t length) {
    if (mapped == nullptr ||
        guest_allocation_flags(length) != MEM_RESERVE) {
        return;
    }
    ps5rt_guest_reservation_add(
        reinterpret_cast<std::uint64_t>(mapped), length);
}

void* try_allocate_guest_virtual_range_exact(
    std::uint64_t address,
    std::uint64_t length) {
    if (address == 0 ||
        address >= kGuestVirtualAddressEnd ||
        length > kGuestVirtualAddressEnd - address ||
        (address & (kAllocationGranularity - 1)) != 0) {
        return nullptr;
    }

    void* const expected = reinterpret_cast<void*>(address);
    void* const mapped = VirtualAlloc(
        expected,
        static_cast<SIZE_T>(length),
        guest_allocation_flags(length),
        PAGE_READWRITE);
    if (mapped == expected) {
        note_guest_allocation(mapped, length);
        return mapped;
    }
    if (mapped != nullptr) {
        VirtualFree(mapped, 0, MEM_RELEASE);
    }
    return nullptr;
}

void* scan_guest_virtual_range(
    std::uint64_t search_begin,
    std::uint64_t search_end,
    std::uint64_t length,
    std::uint64_t alignment) {
    if (search_begin >= search_end ||
        length == 0 ||
        length > search_end - search_begin) {
        return nullptr;
    }

    auto cursor = align_up(search_begin, alignment);
    while (cursor < search_end && length <= search_end - cursor) {
        MEMORY_BASIC_INFORMATION information = {};
        if (VirtualQuery(
                reinterpret_cast<const void*>(cursor),
                &information,
                sizeof(information)) == 0) {
            return nullptr;
        }

        const auto region_begin = reinterpret_cast<std::uint64_t>(
            information.BaseAddress);
        const auto region_size =
            static_cast<std::uint64_t>(information.RegionSize);
        const auto region_end = region_size <=
                std::numeric_limits<std::uint64_t>::max() - region_begin
            ? region_begin + region_size
            : std::numeric_limits<std::uint64_t>::max();

        if (information.State == MEM_FREE) {
            const auto candidate = align_up(
                std::max(cursor, region_begin),
                alignment);
            const auto available_end = std::min(region_end, search_end);
            if (candidate < available_end &&
                length <= available_end - candidate) {
                if (auto* mapped = try_allocate_guest_virtual_range_exact(
                        candidate,
                        length)) {
                    return mapped;
                }
            }
        }

        if (region_end <= cursor) {
            return nullptr;
        }
        cursor = align_up(region_end, alignment);
    }
    return nullptr;
}

void* allocate_guest_virtual_range(
    std::uint64_t requested_address,
    std::uint64_t direct_memory_start,
    std::uint64_t length,
    std::uint64_t alignment,
    bool fixed) {
    AcquireSRWLockExclusive(&g_guest_virtual_mapping_lock);

    void* mapped = nullptr;
    if (requested_address != 0) {
        mapped = try_allocate_guest_virtual_range_exact(
            requested_address,
            length);
    }

    if (mapped == nullptr && !fixed) {
        auto search_start = requested_address != 0
            ? requested_address
            : direct_memory_start != 0
                ? direct_memory_start
                : kGuestVirtualSearchStart;
        search_start = align_up(
            std::max(search_start, kGuestVirtualSearchStart),
            alignment);

        if (search_start < kGuestVirtualAddressEnd) {
            mapped = scan_guest_virtual_range(
                search_start,
                kGuestVirtualAddressEnd,
                length,
                alignment);
        }
        if (mapped == nullptr &&
            search_start > kGuestVirtualSearchStart) {
            mapped = scan_guest_virtual_range(
                kGuestVirtualSearchStart,
                std::min(search_start, kGuestVirtualAddressEnd),
                length,
                alignment);
        }
    }

    ReleaseSRWLockExclusive(&g_guest_virtual_mapping_lock);
    return mapped;
}

extern "C" PS5_GUEST_ABI std::uint64_t ps5rt_map_direct_memory(
    std::uint64_t in_out_address,
    std::uint64_t length,
    std::uint64_t,
    std::uint64_t flags,
    std::uint64_t direct_memory_start,
    std::uint64_t alignment) {
    PS5_HLE_GUARD();
    trace_stderr(
        "memory.map_direct inout=0x%016llX length=0x%016llX "
        "flags=0x%016llX direct=0x%016llX alignment=0x%016llX\n",
        static_cast<unsigned long long>(in_out_address),
        static_cast<unsigned long long>(length),
        static_cast<unsigned long long>(flags),
        static_cast<unsigned long long>(direct_memory_start),
        static_cast<unsigned long long>(alignment));
    if (in_out_address == 0 || length == 0) {
        return 0x80020016;
    }
    if (length > std::numeric_limits<std::uint64_t>::max() -
            (kPageSize - 1)) {
        return 0x80020016;
    }
    auto* address_pointer =
        reinterpret_cast<std::uint64_t*>(in_out_address);
    const auto requested_address = *address_pointer;
    const bool fixed = (flags & 0x10ULL) != 0;
    const auto mapped_length = align_up(length, kPageSize);
    const auto requested_alignment = alignment == 0
        ? 0x4000ULL
        : alignment;
    if ((requested_alignment & (requested_alignment - 1)) != 0) {
        return 0x80020016;
    }
    const auto effective_alignment = std::max(
        requested_alignment,
        kAllocationGranularity);
    if (fixed &&
        (requested_address == 0 ||
         (requested_address & (effective_alignment - 1)) != 0)) {
        return 0x80020016;
    }

    void* const mapped = allocate_guest_virtual_range(
        requested_address,
        direct_memory_start,
        mapped_length,
        effective_alignment,
        fixed);
    if (mapped == nullptr) {
        restore_guest_fs();
        return 0x8002000C;
    }
    *address_pointer = reinterpret_cast<std::uint64_t>(mapped);
    trace_stderr(
        "memory.map_direct_result requested=0x%016llX "
        "mapped=0x%016llX size=0x%016llX alignment=0x%016llX "
        "relocated=%u\n",
        static_cast<unsigned long long>(requested_address),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uint64_t>(mapped)),
        static_cast<unsigned long long>(mapped_length),
        static_cast<unsigned long long>(effective_alignment),
        mapped != reinterpret_cast<void*>(requested_address) ? 1U : 0U);
    restore_guest_fs();
    return 0;
}

std::uint64_t find_runtime_symbol_address(std::string_view nid) {
    for (const auto& symbol : generated::kRuntimeSymbols) {
        if (symbol.nid != nullptr && nid == symbol.nid) {
            return symbol.address;
        }
    }
    return 0;
}

extern "C" PS5_GUEST_ABI std::uint64_t
ps5rt_libc_mspace_destroy_dispatch(std::uint64_t mspace) {
    if (mspace == 0) {
        return 0;
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        return ps5rt_libc_mspace_destroy(mspace);
    }
    const auto address = find_runtime_symbol_address("W6SiVSiCDtI");
    if (address == 0) {
        return ps5rt_libc_mspace_destroy(mspace);
    }
    using GuestFunction = std::uint64_t (PS5_GUEST_ABI*)(std::uint64_t);
    return reinterpret_cast<GuestFunction>(address)(mspace);
}

extern "C" PS5_GUEST_ABI void* ps5rt_libc_mspace_malloc_dispatch(
    std::uint64_t mspace,
    std::uint64_t size) {
    if (mspace == 0) {
        return ps5rt_mspace_malloc(size);
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        return ps5rt_libc_mspace_malloc(mspace, size);
    }
    const auto address = find_runtime_symbol_address("OJjm-QOIHlI");
    if (address == 0) {
        return ps5rt_libc_mspace_malloc(mspace, size);
    }
    using GuestFunction = void* (PS5_GUEST_ABI*)(
        std::uint64_t,
        std::uint64_t);
    return reinterpret_cast<GuestFunction>(address)(mspace, size);
}

extern "C" PS5_GUEST_ABI void ps5rt_libc_mspace_free_dispatch(
    std::uint64_t mspace,
    void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    if (mspace == 0) {
        ps5rt_mspace_free(pointer);
        return;
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        ps5rt_libc_mspace_free(mspace, pointer);
        return;
    }

    const auto address = find_runtime_symbol_address("Vla-Z+eXlxo");
    if (address == 0) {
        ps5rt_libc_mspace_free(mspace, pointer);
        return;
    }
    using GuestMspaceFree = void (PS5_GUEST_ABI*)(
        std::uint64_t,
        void*);
    reinterpret_cast<GuestMspaceFree>(address)(mspace, pointer);
}

extern "C" PS5_GUEST_ABI void* ps5rt_libc_mspace_calloc_dispatch(
    std::uint64_t mspace,
    std::uint64_t count,
    std::uint64_t size) {
    if (mspace == 0) {
        return ps5rt_calloc(count, size);
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        return ps5rt_libc_mspace_calloc(mspace, count, size);
    }
    const auto address = find_runtime_symbol_address("LYo3GhIlB38");
    if (address == 0) {
        return ps5rt_libc_mspace_calloc(mspace, count, size);
    }
    using GuestFunction = void* (PS5_GUEST_ABI*)(
        std::uint64_t,
        std::uint64_t,
        std::uint64_t);
    return reinterpret_cast<GuestFunction>(address)(mspace, count, size);
}

extern "C" PS5_GUEST_ABI void* ps5rt_libc_mspace_realloc_dispatch(
    std::uint64_t mspace,
    void* pointer,
    std::uint64_t size) {
    if (mspace == 0) {
        return ps5rt_realloc(pointer, size);
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        return ps5rt_libc_mspace_realloc(mspace, pointer, size);
    }
    const auto address = find_runtime_symbol_address("gigoVHZvVPE");
    if (address == 0) {
        return ps5rt_libc_mspace_realloc(mspace, pointer, size);
    }
    using GuestFunction = void* (PS5_GUEST_ABI*)(
        std::uint64_t,
        void*,
        std::uint64_t);
    return reinterpret_cast<GuestFunction>(address)(mspace, pointer, size);
}

extern "C" PS5_GUEST_ABI void* ps5rt_libc_mspace_memalign_dispatch(
    std::uint64_t mspace,
    std::uint64_t alignment,
    std::uint64_t size) {
    if (mspace == 0) {
        return ps5rt_mspace_memalign(alignment, size);
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        return ps5rt_libc_mspace_memalign(mspace, alignment, size);
    }
    const auto address = find_runtime_symbol_address("iF1iQHzxBJU");
    if (address == 0) {
        return ps5rt_libc_mspace_memalign(mspace, alignment, size);
    }
    using GuestFunction = void* (PS5_GUEST_ABI*)(
        std::uint64_t,
        std::uint64_t,
        std::uint64_t);
    return reinterpret_cast<GuestFunction>(address)(
        mspace,
        alignment,
        size);
}

extern "C" PS5_GUEST_ABI std::int32_t
ps5rt_libc_mspace_posix_memalign_dispatch(
    std::uint64_t mspace,
    void** output,
    std::uint64_t alignment,
    std::uint64_t size) {
    if (mspace == 0) {
        return ps5rt_posix_memalign(output, alignment, size);
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        if (output == nullptr ||
            alignment < sizeof(void*) ||
            (alignment & (alignment - 1)) != 0) {
            return 22;
        }
        auto* allocated =
            ps5rt_libc_mspace_memalign(mspace, alignment, size);
        if (allocated == nullptr) {
            return 12;
        }
        *output = allocated;
        return 0;
    }
    const auto address = find_runtime_symbol_address("qWESlyXMI3E");
    if (address == 0) {
        return 22;
    }
    using GuestFunction = std::int32_t (PS5_GUEST_ABI*)(
        std::uint64_t,
        void**,
        std::uint64_t,
        std::uint64_t);
    return reinterpret_cast<GuestFunction>(address)(
        mspace,
        output,
        alignment,
        size);
}

extern "C" PS5_GUEST_ABI void*
ps5rt_libc_mspace_aligned_alloc_dispatch(
    std::uint64_t mspace,
    std::uint64_t alignment,
    std::uint64_t size) {
    if (mspace == 0) {
        return ps5rt_aligned_alloc(alignment, size);
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        return ps5rt_libc_mspace_memalign(mspace, alignment, size);
    }
    const auto address = find_runtime_symbol_address("ljkqMcC4-mk");
    if (address == 0) {
        return ps5rt_libc_mspace_memalign(mspace, alignment, size);
    }
    using GuestFunction = void* (PS5_GUEST_ABI*)(
        std::uint64_t,
        std::uint64_t,
        std::uint64_t);
    return reinterpret_cast<GuestFunction>(address)(
        mspace,
        alignment,
        size);
}

extern "C" PS5_GUEST_ABI void*
ps5rt_libc_mspace_reallocalign_dispatch(
    std::uint64_t mspace,
    void* pointer,
    std::uint64_t size,
    std::uint64_t alignment) {
    if (mspace == 0) {
        return ps5rt_realloc(pointer, size);
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        return ps5rt_libc_mspace_realloc(mspace, pointer, size);
    }
    const auto address = find_runtime_symbol_address("p6lrRW8-MLY");
    if (address == 0) {
        return ps5rt_libc_mspace_realloc(mspace, pointer, size);
    }
    using GuestFunction = void* (PS5_GUEST_ABI*)(
        std::uint64_t,
        void*,
        std::uint64_t,
        std::uint64_t);
    return reinterpret_cast<GuestFunction>(address)(
        mspace,
        pointer,
        size,
        alignment);
}

extern "C" PS5_GUEST_ABI std::uint64_t
ps5rt_libc_mspace_stats_dispatch(
    std::uint64_t mspace,
    std::uint64_t stats) {
    if (mspace == 0) {
        return 0;
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        return ps5rt_libc_mspace_malloc_stats(mspace, stats);
    }
    const auto address = find_runtime_symbol_address("mfHdJTIvhuo");
    if (address == 0) {
        return ps5rt_libc_mspace_malloc_stats(mspace, stats);
    }
    using GuestFunction = std::uint64_t (PS5_GUEST_ABI*)(
        std::uint64_t,
        std::uint64_t);
    return reinterpret_cast<GuestFunction>(address)(mspace, stats);
}

extern "C" PS5_GUEST_ABI std::uint64_t
ps5rt_libc_mspace_stats_fast_dispatch(
    std::uint64_t mspace,
    std::uint64_t stats) {
    if (mspace == 0) {
        return 0;
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        return ps5rt_libc_mspace_malloc_stats(mspace, stats);
    }
    const auto address = find_runtime_symbol_address("k04jLXu3+Ic");
    if (address == 0) {
        return ps5rt_libc_mspace_malloc_stats(mspace, stats);
    }
    using GuestFunction = std::uint64_t (PS5_GUEST_ABI*)(
        std::uint64_t,
        std::uint64_t);
    return reinterpret_cast<GuestFunction>(address)(mspace, stats);
}

extern "C" PS5_GUEST_ABI std::uint64_t
ps5rt_libc_mspace_malloc_usable_size_dispatch(
    std::uint64_t mspace,
    const void* pointer) {
    if (mspace == 0) {
        return 0;
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        return ps5rt_libc_mspace_malloc_usable_size(mspace, pointer);
    }
    const auto address = find_runtime_symbol_address("fEoW6BJsPt4");
    if (address == 0) {
        return 0;
    }
    using GuestFunction = std::uint64_t (PS5_GUEST_ABI*)(
        std::uint64_t,
        const void*);
    return reinterpret_cast<GuestFunction>(address)(mspace, pointer);
}

extern "C" PS5_GUEST_ABI std::uint64_t
ps5rt_libc_mspace_is_empty_dispatch(std::uint64_t mspace) {
    if (mspace == 0) {
        return 1;
    }
    if (ps5rt_libc_mspace_is_native(mspace)) {
        return ps5rt_libc_mspace_is_heap_empty(mspace);
    }
    const auto address = find_runtime_symbol_address("pzUa7KEoydw");
    if (address == 0) {
        return 0;
    }
    using GuestFunction = std::uint64_t (PS5_GUEST_ABI*)(std::uint64_t);
    return reinterpret_cast<GuestFunction>(address)(mspace);
}

// The two submit entry points are where the managed path spends its time:
// measured at 121 s in one sceAgcDriverSubmitAcb call and 73 s in one
// sceAgcDriverSubmitDcb, on the same thread, covering the whole delay to
// the first frame. Both already have complete native implementations that
// run as a shadow, so taking authority is a resolution change rather than
// new code. Off by default until parity is established.
// On by default since parity was established: the frame the native path
// presents is byte-identical to the managed one - same MD5, zero of
// 8,294,400 pixels differing - while arriving at 84 s instead of 188 and
// producing five flips instead of one. PS5RECOMP_AGC_NATIVE_SUBMIT=0
// returns to the managed path; parity was measured on one frame of one
// title, so the way back stays.
bool native_agc_submit_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        const auto length = GetEnvironmentVariableA(
            "PS5RECOMP_AGC_NATIVE_SUBMIT",
            value,
            static_cast<DWORD>(sizeof(value)));
        return !(length == 1 && value[0] == '0');
    }();
    return enabled;
}

bool is_agc_submit_nid(std::string_view nid) {
    return nid == "UglJIZjGssM" || nid == "gSRnr79F8tQ";
}

bool prefer_sharpemu_gpu_hle(std::string_view nid) {
    if (native_agc_submit_enabled() && is_agc_submit_nid(nid)) {
        return false;
    }
#if defined(PS5RT_HAS_GENERATED_HLE)
    const auto* symbol = ps5rt::hle::HleLookup(nid);
    if (symbol == nullptr || symbol->library == nullptr) {
        return false;
    }
    const std::string_view library(symbol->library);
    const bool is_gpu_library =
        library == "libSceAgc" ||
        library == "libSceAgcDriver" ||
        library == "libSceVideoOut";
    if (!is_gpu_library) {
        return false;
    }

    const bool keep_native_agc_bootstrap =
        nid == "23LRUSvYu1M" || // sceAgcInit
        nid == "kW3GLb7QfPg" || // sceAgcInit alias
        nid == "2JtWUUiYBXs" || // sceAgcGetRegisterDefaults2
        nid == "wRbq6ZjNop4" || // sceAgcGetRegisterDefaults2Internal
        nid == "V++UgBtQhn0" || // sceAgcGetDataPacketPayloadAddress
        nid == "fPSCdQxgpSw" || // sceAgcWriteDataPatchAddress
        nid == "3KDcnM3lrcU" || // sceAgcWaitRegMemPatchAddress
        nid == "f3dg2CSgRKY" || // sceAgcCreateShader
        nid == "1-gUn1PI4Sw";   // sceAgcDcbAtomicMem

    // The command buffer writers. These build a PM4 packet of a dozen dwords
    // and return; the bridge crossing around that work cost between four and
    // forty milliseconds a call, and the title makes thousands per frame.
    // The Acb entry points write the same packet to a different queue, so
    // they share the implementation here as they do in the bridge.
    const bool keep_native_agc_writers =
        nid == "wr23dPKyWc0" || // sceAgcCbReleaseMem
        nid == "n2fD4A+pb+g" || // sceAgcCbSetShRegisterRangeDirect
        nid == "UZbQjYAwwXM" || // sceAgcCbSetShRegistersDirect
        nid == "+kSrjIVxKFE" || // sceAgcDcbPushMarker
        nid == "cpCILPya5Zk" || // sceAgcAcbPushMarker
        nid == "H7uZqCoNuWk" || // sceAgcDcbPopMarker
        nid == "6mFxkVqdmbQ" || // sceAgcAcbPopMarker
        nid == "57labkp+rSQ" || // sceAgcDcbAcquireMem
        nid == "d-6uF9sZDIU" || // sceAgcSetCxRegIndirectPatchAddRegisters
        nid == "z2duB-hHQSM" || // sceAgcSetShRegIndirectPatchAddRegisters
        nid == "vRoArM9zaIk" || // sceAgcSetUcRegIndirectPatchAddRegisters
        nid == "ZvwO9euwYzc" || // sceAgcDcbSetCxRegistersIndirect
        nid == "-HOOCn0JY48" || // sceAgcDcbSetShRegistersIndirect
        nid == "hvUfkUIQcOE" || // sceAgcDcbSetUcRegistersIndirect
        nid == "aJf+j5yntiU" || // sceAgcDcbEventWrite
        nid == "VmW0Tdpy420" || // sceAgcDcbWaitRegMem
        nid == "tSBxhAPyytQ" || // sceAgcDcbSetNumInstances
        nid == "8N2tmT3jmC8" || // sceAgcDcbSetIndexCount
        nid == "0fWWK5uG9rQ" || // sceAgcQueueEndOfPipeActionPatchAddress
        nid == "vcmNN+AAXnY" || // sceAgcSetCxRegIndirectPatchSetAddress
        nid == "Qrj4c+61z4A" || // sceAgcSetShRegIndirectPatchSetAddress
        nid == "6lNcCp+fxi4" || // sceAgcSetUcRegIndirectPatchSetAddress
        nid == "Yw0jKSqop+E" || // sceAgcDcbDrawIndexAuto
        nid == "l4fM9K-Lyks" || // sceAgcDcbSetIndexBuffer
        nid == "k3GhuSNmBLU" || // sceAgcCbDispatch
        nid == "D9sr1xGUriE" || // sceAgcCreatePrimState
        nid == "LtTouSCZjHM" || // sceAgcCbNop
        nid == "i1jyy49AjXU" || // sceAgcDcbWriteData
        nid == "eZ4+17OQz4Q" || // sceAgcAcbWriteData
        nid == "u2T2DiA5hRI" || // sceAgcDcbStallCommandBufferParser
        nid == "WmAc2MEj6Io" || // sceAgcDcbDmaData
        nid == "-RnpfpxIhec" || // sceAgcAcbDmaData
        nid == "RmaJwLtc8rY" || // sceAgcDcbSetBaseIndirectArgs
        nid == "CtB+A9-VxO0" || // sceAgcDcbDispatchIndirect
        nid == "j3EtxFkSIhQ" || // sceAgcAcbDispatchIndirect
        nid == "t1vNu082-jM" || // sceAgcDcbDrawIndexIndirect
        nid == "vuSXe69VILM" || // sceAgcDcbGetLodStats
        nid == "MWiElSNE8j8" || // sceAgcDcbWaitUntilSafeForRendering
        nid == "YUeqkyT7mEQ" || // sceAgcDcbSetFlip
        nid == "cFazmnXpJOE" || // sceAgcAcbEventWrite
        nid == "KT-hTp-Ch14" || // sceAgcAcbAcquireMem
        nid == "htn36gPnBk4" || // sceAgcAcbWaitRegMem
        nid == "HV4j+E0MBHE" || // sceAgcCreateInterpolantMapping
        nid == "qj7QZpgr9Uw";   // uncatalogued, one dword
    return !keep_native_agc_bootstrap && !keep_native_agc_writers &&
        hle_bridge_nid_support(nid) == 1;
#else
    (void)nid;
    return false;
#endif
}

void* resolve_import(std::string_view nid, bool& resolved) {
    // The video player is native: the bridge's could not create a player
    // in this runtime, and the title skipped its intro for want of one.
    if (void* handler = ps5rt_avplayer_handler(nid.data(), nid.size());
        handler != nullptr) {
        resolved = true;
        return handler;
    }
    // libSceJson too: the bridge's String::c_str() returned null, and the
    // loader that reads it after the intros died on it.
    if (void* handler = ps5rt_json_handler(nid.data(), nid.size());
        handler != nullptr) {
        resolved = true;
        return handler;
    }
    // sceAgcDriverUnknown_KRzWekV120: the bridge only traces its arguments
    // and returns zero, and the title's AGC thread calls it thousands of
    // times a minute - each a crossing into the managed runtime.
    if (nid == "-KRzWekV120") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_return_zero);
    }
    // sceAgcSuspendPoint: nothing to suspend for here, and it returns zero.
    if (nid == "h9z6+0hEydk") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_return_zero);
    }
    // sceAgcDcbDrawIndexOffset.
    if (nid == "B+aG9DUnTKA") {
        resolved = true;
        return reinterpret_cast<void*>(
            &ps5rt_agc_dcb_draw_index_offset_real);
    }
    if (native_agc_submit_enabled() && is_agc_submit_nid(nid)) {
        resolved = true;
        return nid == "UglJIZjGssM"
            ? reinterpret_cast<void*>(&ps5rt_agc_driver_submit_dcb_real)
            : reinterpret_cast<void*>(&ps5rt_agc_driver_submit_acb_real);
    }
    if (prefer_sharpemu_gpu_hle(nid)) {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_sharpemu_import);
    }

    // Bundled zz_libc owns the dlmalloc mstate ABI. Resolve this family before
    // generated/native HLE bindings so create, allocate, resize, and free all
    // operate on the same guest allocator state.
    const bool prefer_bundled_mspace =
        nid == "-hn1tcVHq5Q" || // sceLibcMspaceCreate
        nid == "W6SiVSiCDtI" || // sceLibcMspaceDestroy
        nid == "OJjm-QOIHlI" || // sceLibcMspaceMalloc
        nid == "Vla-Z+eXlxo" || // sceLibcMspaceFree
        nid == "LYo3GhIlB38" || // sceLibcMspaceCalloc
        nid == "gigoVHZvVPE" || // sceLibcMspaceRealloc
        nid == "iF1iQHzxBJU" || // sceLibcMspaceMemalign
        nid == "qWESlyXMI3E" || // sceLibcMspacePosixMemalign
        nid == "ljkqMcC4-mk" || // sceLibcMspaceAlignedAlloc
        nid == "p6lrRW8-MLY" || // sceLibcMspaceReallocalign
        nid == "mfHdJTIvhuo" || // sceLibcMspaceMallocStats
        nid == "k04jLXu3+Ic" || // sceLibcMspaceMallocStatsFast
        nid == "fEoW6BJsPt4" || // sceLibcMspaceMallocUsableSize
        nid == "pzUa7KEoydw";   // sceLibcMspaceIsHeapEmpty
    if (prefer_bundled_mspace) {
        if (nid == "-hn1tcVHq5Q") {
            resolved = true;
            return reinterpret_cast<void*>(&ps5rt_libc_mspace_create);
        }
        const auto address = find_runtime_symbol_address(nid);
        if (address != 0) {
            resolved = true;
            if (nid == "W6SiVSiCDtI") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_destroy_dispatch);
            }
            if (nid == "OJjm-QOIHlI") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_malloc_dispatch);
            }
            if (nid == "Vla-Z+eXlxo") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_free_dispatch);
            }
            if (nid == "LYo3GhIlB38") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_calloc_dispatch);
            }
            if (nid == "gigoVHZvVPE") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_realloc_dispatch);
            }
            if (nid == "iF1iQHzxBJU") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_memalign_dispatch);
            }
            if (nid == "qWESlyXMI3E") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_posix_memalign_dispatch);
            }
            if (nid == "ljkqMcC4-mk") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_aligned_alloc_dispatch);
            }
            if (nid == "p6lrRW8-MLY") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_reallocalign_dispatch);
            }
            if (nid == "mfHdJTIvhuo") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_stats_dispatch);
            }
            if (nid == "k04jLXu3+Ic") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_stats_fast_dispatch);
            }
            if (nid == "fEoW6BJsPt4") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_malloc_usable_size_dispatch);
            }
            if (nid == "pzUa7KEoydw") {
                return reinterpret_cast<void*>(
                    &ps5rt_libc_mspace_is_empty_dispatch);
            }
            return reinterpret_cast<void*>(address);
        }
    }

#if defined(PS5RT_HAS_GENERATED_HLE)
#define PS5RT_HLE_BIND(binding_nid, handler, export_name, binding_status) \
    if (nid == binding_nid) {                                           \
        resolved = true;                                                \
        return reinterpret_cast<void*>(&handler);                       \
    }
#include "hle_bindings.generated.inc"
#undef PS5RT_HLE_BIND
#endif

    if (nid == "ps5rt-test") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_test_hle);
    }
    if (nid == "vNe1w4diLCs") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_tls_get_addr);
    }
    if (nid == "f7uOxY9mM1U") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_stack_check_guard);
    }
    if (nid == "Ou3iL1abvng") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_stack_check_fail);
    }
    if (nid == "959qrazPIrg") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_get_proc_param);
    }
    if (nid == "9BcDykPmo1I") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_get_errno_address);
    }
    if (nid == "rNhWz+lvOMU") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_set_thread_dtors);
    }
    if (nid == "pB-yGZ2nQ9o") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_set_thread_atexit_count);
    }
    if (nid == "WhCc1w3EhSI") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_set_thread_atexit_report);
    }
    if (nid == "aI+OeCz8xrQ") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_self);
    }
    if (nid == "EI-5-jlq2dE" || nid == "3eqs37G74-s") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_getthreadid);
    }
    if (nid == "3PtV6p3QNX4") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_equal);
    }
    if (nid == "T72hz6ffq08") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_yield);
    }
    if (nid == "14bOACANTBo") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_once);
    }
    if (nid == "mqULNdimTn0" || nid == "geDaqgH9lTg") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_key_create);
    }
    if (nid == "6BpEZuDT7YI" || nid == "PrdHuuDekhY") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_key_delete);
    }
    if (nid == "WrOLvHU0yQM" || nid == "+BzXYkqYeLE") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_setspecific);
    }
    if (nid == "0-KXaS70xy4" || nid == "eoht7mQOCmo") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_getspecific);
    }
    if (nid == "F8bUHwAG284" || nid == "dQHWEsJtoE4") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_mutexattr_init);
    }
    if (nid == "iMp8QpE+XO4" || nid == "mDmgMOGVUqg") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_mutexattr_settype);
    }
    if (nid == "1FGvU0i9saQ" || nid == "5txKfcMUAok") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_mutexattr_setprotocol);
    }
    if (nid == "smWEktiyyG0" || nid == "HF7lK46xzjY") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_mutexattr_destroy);
    }
    if (nid == "cmo1RIYva9o" || nid == "ttHNfU+qDBU") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_mutex_init);
    }
    if (nid == "2Of0f+3mhhE" || nid == "ltCfaGr2JGE") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_mutex_destroy);
    }
    if (nid == "nsYoNRywwNg" || nid == "wtkt-teR1so") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_attr_init);
    }
    if (nid == "62KCwEMmzcM" || nid == "zHchY8ft5pk") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_attr_destroy);
    }
    if (nid == "8+s5BzZjxSg") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_pthread_attr_getaffinity);
    }
    if (nid == "FxVZqBAA7ks") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_write);
    }
    if (nid == "p5EcQeEeJAE") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_set_application_heap_api);
    }
    if (nid == "NWtTN10cJzE") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_libc_heap_get_trace_info);
    }
    if (nid == "pO96TwzOm5E") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_get_direct_memory_size);
    }
    if (nid == "rTXw65xmLIA") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_allocate_direct_memory);
    }
    if (nid == "NcaWUxfMNIQ" || nid == "L-Q3LEjIbgA") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_map_direct_memory);
    }
    if (nid == "bzQExy189ZI") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_init_env);
    }
    if (nid == "Q3VBxCXhUHs") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_memcpy);
    }
    if (nid == "+P6FRGH4LfA") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_memmove);
    }
    if (nid == "QrZZdJ8XsX0" || nid == "8zTFvBIAIN8") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_memset);
    }
    if (nid == "DfivPArhucg" || nid == "5TjaJwkLWxE") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_memcmp);
    }
    if (nid == "8u8lPzUEq+U") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_memchr);
    }
    if (nid == "j4ViWNHEgww") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_strlen);
    }
    if (nid == "aesyjrHVWy4") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_strncmp);
    }
    if (nid == "Ovb2dSJOAuE") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_strcmp);
    }
    if (nid == "kiZSXIWd9vg") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_strcpy);
    }
    if (nid == "6sJWiWSRuqk") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_strncpy);
    }
    if (nid == "Nmtr628eA3A") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_random_device);
    }
    if (nid == "oVkZ8W8-Q8A") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_strtok);
    }
    if (nid == "YQ0navp+YIc") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_puts);
    }
    if (nid == "m5wN+SwZOR4") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_putchar);
    }
    if (nid == "wLlFkwG9UcQ") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_time);
    }
    if (nid == "mXlxhmLNMPg") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_strtol);
    }
    if (nid == "H8ya2H00jbI") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_sin);
    }
    if (nid == "Q4rRL34CEeE") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_sinf);
    }
    if (nid == "2WE3BTYVwKM") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_cos);
    }
    if (nid == "-P6FNMzk2Kc") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_cosf);
    }
    if (nid == "efhK-YSUYYQ") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_localtime);
    }
    if (nid == "jT3xiGpA3B4") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_asctime);
    }
    if (nid == "5Xa2ACNECdo") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_strcpy_s);
    }
    if (nid == "xEszJVGpybs") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_sprintf_s);
    }
    if (nid == "cpCOXWMgha0") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_rand);
    }
    if (nid == "NFLs+dRJGNg") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_memcpy_s);
    }
    if (nid == "O1lQ2+do5r4") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_ipmi_config_construct);
    }
    if (nid == "AEJdIVZTEmo") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_qsort);
    }
    if (nid == "0zsTiDhM0nU") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_ipmi_client_create);
    }
    if (nid == "R9lA82OraNs") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_app_content_initialize);
    }
    if (nid == "99b82IKXpH4") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_app_content_param_get_int);
    }
    if (nid == "GGeRJk1XdWc") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_get_compiled_sdk_version);
    }
    // Plain libc allocators. Every one of these must reach the same heap as
    // free(): a pointer handed to a different allocator family corrupts the
    // host heap (STATUS_HEAP_CORRUPTION).
    if (nid == "gQX+4GDQjpM") { // malloc
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_mspace_malloc);
    }
    if (nid == "tIhsqj0qsFE") { // free
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_mspace_free);
    }
    if (nid == "2X5agFjKxMc") { // calloc
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_calloc);
    }
    if (nid == "Y7aJ1uydPMo") { // realloc
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_realloc);
    }
    if (nid == "Ujf3KzMvRmI") { // memalign
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_mspace_memalign);
    }
    if (nid == "2Btkg8k24Zg") { // aligned_alloc
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_aligned_alloc);
    }
    if (nid == "cVSk9y8URbc") { // posix_memalign
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_posix_memalign);
    }
    // Native mspace fallbacks still preserve the leading handle argument when
    // a package does not contain a bundled libc implementation.
    if (nid == "-hn1tcVHq5Q") { // sceLibcMspaceCreate
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_libc_mspace_create);
    }
    if (nid == "W6SiVSiCDtI") { // sceLibcMspaceDestroy
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_libc_mspace_destroy);
    }
    if (nid == "OJjm-QOIHlI") { // sceLibcMspaceMalloc
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_libc_mspace_malloc);
    }
    if (nid == "Vla-Z+eXlxo") { // sceLibcMspaceFree
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_libc_mspace_free);
    }
    if (nid == "gigoVHZvVPE") { // sceLibcMspaceRealloc
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_libc_mspace_realloc);
    }
    if (nid == "iF1iQHzxBJU") { // sceLibcMspaceMemalign
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_libc_mspace_memalign);
    }
    if (nid == "mfHdJTIvhuo") { // sceLibcMspaceMallocStats
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_libc_mspace_malloc_stats);
    }
    if (nid == "k04jLXu3+Ic") { // sceLibcMspaceMallocStatsFast
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_libc_mspace_malloc_stats);
    }
    // C++ ABI
    if (nid == "tsvEmnenz48") { resolved = true; return reinterpret_cast<void*>(&ps5rt_cxa_atexit); }
    if (nid == "H2e8t5ScQGc") { resolved = true; return reinterpret_cast<void*>(&ps5rt_cxa_finalize); }
    if (nid == "8G2LB+A3rzg") { resolved = true; return reinterpret_cast<void*>(&ps5rt_cxa_atexit); } // atexit alias
    if (nid == "3GPpjQdAMTw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_cxa_guard_acquire); }
    if (nid == "9rAeANT2tyE") { resolved = true; return reinterpret_cast<void*>(&ps5rt_cxa_guard_release); }
    if (nid == "2emaaluWzUw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_cxa_guard_abort); }
    if (nid == "fJnpuVVBbKk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_operator_new); }
    if (nid == "z+P+xCnWLBk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_operator_delete); }
    if (nid == "MLWl90SFWNE") { resolved = true; return reinterpret_cast<void*>(&ps5rt_operator_array_delete); }
    if (nid == "hdm0YfMa7TQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_operator_array_new); }
    // Kernel: sleep/time
    if (nid == "NhpspxdjEKU" || nid == "yS8U2TGCe1A") { resolved = true; return reinterpret_cast<void*>(&ps5rt_nanosleep); }
    if (nid == "lLMT9vJAck0") { resolved = true; return reinterpret_cast<void*>(&ps5rt_clock_gettime); }
    if (nid == "n88vx3C5nW8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_clock_gettime); }
    if (nid == "1j3S3n-tTW4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_get_tsc_frequency); }
    if (nid == "-2IRUCO--PM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_read_tsc); }
    if (nid == "4J2sUJmuHZQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_get_process_time); }
    if (nid == "fgxnMeTNUtY") { resolved = true; return reinterpret_cast<void*>(&ps5rt_get_process_time_counter); }
    if (nid == "BNowx2l588E") { resolved = true; return reinterpret_cast<void*>(&ps5rt_get_process_time_counter_frequency); }
    if (nid == "-o5uEDpN+oY") { resolved = true; return reinterpret_cast<void*>(&ps5rt_convert_utc_to_localtime); }
    if (nid == "fZo48un7LK4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_system_service_param_get_int); }
    // Font
    if (nid == "whrS4oksXc4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_memory_init); }
    if (nid == "oM+XCzVG3oM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_select_library_ft); }
    if (nid == "Xx974EW-QFY") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_select_renderer_ft); }
    if (nid == "n590hj5Oe-k") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_create_library_with_edition); }
    if (nid == "WaSFJoRWXaI") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_create_renderer_with_edition); }
    if (nid == "exAxkyVLt0s") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_destroy_renderer); }
    if (nid == "3OdRkSjOcog") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_bind_renderer); }
    if (nid == "1QjhKxrsOB8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_unbind_renderer); }
    if (nid == "N1EBMeGhf7E") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_set_scale_pixel); }
    if (nid == "TMtqoFQjjbA") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_set_effect_slant); }
    if (nid == "v0phZwa4R5o") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_set_effect_weight); }
    if (nid == "6vGCkkQJOcI") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_setup_render_scale_pixel); }
    if (nid == "lz9y9UFO2UU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_setup_render_effect_slant); }
    if (nid == "XIGorvLusDQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_setup_render_effect_weight); }
    if (nid == "imxVx8lm+KM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_get_horizontal_layout); }
    if (nid == "cKYtVmeSTcw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_open_font_set); }
    if (nid == "KXUpebrFk1U") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_open_font_memory); }
    if (nid == "JzCH3SCFnAU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_open_font_instance); }
    if (nid == "vzHs3C8lWJk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_close_font); }
    if (nid == "SsRbbCiWoGw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_support_system_fonts); }
    if (nid == "mz2iTY0MK4A") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_support_external_fonts); }
    if (nid == "CUKn5pX-NVY") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_attach_device_cache_buffer); }
    if (nid == "IQtleGLL5pQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_get_render_char_glyph_metrics); }
    if (nid == "C-4Qw5Srlyw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_generate_char_glyph); }
    if (nid == "8-zmgsxkBek") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_glyph_define_attribute); }
    if (nid == "kAenWy1Zw5o") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_render_char_glyph_image_horizontal); }
    if (nid == "LHDoRWVFGqk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_delete_glyph); }
    if (nid == "gdUCnU0gHdI") { resolved = true; return reinterpret_cast<void*>(&ps5rt_font_render_surface_init); }
    // Kernel: system info
    if (nid == "WB66evu8bsU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_get_compiled_sdk_version); }
    if (nid == "WslcK1FQcGI") { resolved = true; return reinterpret_cast<void*>(&ps5rt_is_neo_mode); }
    if (nid == "Mv1zUObHvXI") { resolved = true; return reinterpret_cast<void*>(&ps5rt_get_system_sw_version); }
    if (nid == "VOx8NGmHXTs") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "G-MYv5erXaU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_get_app_info); }
    if (nid == "g0VTBxfJyu0") { resolved = true; return reinterpret_cast<void*>(&ps5rt_get_current_cpu); }
    if (nid == "LwG8g3niqwA") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_dlsym); }
    // Kernel: memory
    if (nid == "n1-v6FgU7MQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_configured_flexible_memory_size); }
    if (nid == "rVjRvHJ0X6c") { resolved = true; return reinterpret_cast<void*>(&ps5rt_virtual_query); }
    if (nid == "mL8NDH86iQI") { resolved = true; return reinterpret_cast<void*>(&ps5rt_map_named_flexible_memory); }
    if (nid == "vSMAm3cxYTY") { resolved = true; return reinterpret_cast<void*>(&ps5rt_mprotect); }
    if (nid == "YQOfxL4QfeU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_mprotect); }
    if (nid == "3k6kx-zOOSQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "DGMG3JshrZU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_set_virtual_range_name); }
    if (nid == "RpQJJVKTiFM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "f7KBOafysXo") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    // libc: string search. These three are 39 per cent of the
    // crossings into the managed side in a run.
    if (nid == "ob5xAW4ln-0") { resolved = true; return reinterpret_cast<void*>(&ps5rt_libc_strchr_real); }
    if (nid == "9yDWMxEFdJU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_libc_strrchr_real); }
    if (nid == "viiwFMaNamA") { resolved = true; return reinterpret_cast<void*>(&ps5rt_libc_strstr_real); }
    // Kernel: I/O
    if (nid == "uWyW3v98sU4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_check_reachability_real); }
    // Kernel APR and libSceAmpr: how the title streams its assets. These
    // sit ahead of the bridge, which answers them permissively with zero -
    // enough for the loader to build a command buffer and never fill it.
    if (nid == "WT-5NKy42fw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_apr_resolve_filepaths_to_ids_real); }
    if (nid == "gEpBkcwxUjw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_apr_resolve_filepaths_to_ids_and_sizes_real); }
    if (nid == "WvEu7yl3Ivg") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_apr_get_file_size_real); }
    if (nid == "eE4Szl8sil8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_apr_submit_command_buffer_real); }
    if (nid == "qvMUCyyaCSI") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_apr_submit_command_buffer_and_get_id_real); }
    if (nid == "ASoW5WE-UPo") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_apr_submit_command_buffer_and_get_result_real); }
    if (nid == "rqwFKI4PAiM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_apr_wait_command_buffer_real); }
    if (nid == "8aI7R7WaOlc") { resolved = true; return reinterpret_cast<void*>(&ps5rt_ampr_command_buffer_constructor_real); }
    if (nid == "GuchCTefuZw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_ampr_command_buffer_destructor_real); }
    if (nid == "a8uLzYY--tM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_ampr_apr_command_buffer_constructor_real); }
    if (nid == "Qs1xtplKo0U") { resolved = true; return reinterpret_cast<void*>(&ps5rt_ampr_apr_command_buffer_destructor_real); }
    if (nid == "N-FSPA4S3nI") { resolved = true; return reinterpret_cast<void*>(&ps5rt_ampr_command_buffer_set_buffer_real); }
    if (nid == "ULvXMDz56po") { resolved = true; return reinterpret_cast<void*>(&ps5rt_ampr_command_buffer_clear_buffer_real); }
    if (nid == "baQO9ez2gL4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_ampr_command_buffer_reset_real); }
    if (nid == "GnxKOHEawhk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_ampr_command_buffer_get_current_offset_real); }
    if (nid == "gzndltBEzWc") { resolved = true; return reinterpret_cast<void*>(&ps5rt_ampr_command_buffer_get_num_commands_real); }
    if (nid == "tZDDEo2tE5k") { resolved = true; return reinterpret_cast<void*>(&ps5rt_ampr_command_buffer_get_size_real); }
    if (nid == "mQ16-QdKv7k") { resolved = true; return reinterpret_cast<void*>(&ps5rt_ampr_apr_command_buffer_read_file_real); }
    if (nid == "vWU-odnS+fU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_ampr_measure_command_size_read_file_real); }
    if (nid == "wuCroIGjt2g") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_open_real); }
    if (nid == "bY-PO6JhzhQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_close_real); }
    if (nid == "AqBioC2vF3I") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_read_real); }
    if (nid == "Oy6IpwgtYOk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_lseek_real); }
    if (nid == "HoLVWNanBBc") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "BPE9s9vQQXo") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "UqDGjXA5yUM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "mqQMh1zPPT8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "E6ao34wPw+U") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    // Offline networking compatibility. Astro Bot only needs these teardown
    // and initialization calls to succeed when no PSN/RUDP backend exists.
    if (nid == "amuBfI-AQc4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "9X9+cneTGUU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    // Pthread: create/join/lock
    if (nid == "6UgtwV+0zb4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_pthread_create_real); }
    if (nid == "onNY9Byn-W8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_pthread_join_real); }
    if (nid == "9UK1vLZQft4" || nid == "7H0iTOciTLo") { resolved = true; return reinterpret_cast<void*>(&ps5rt_pthread_mutex_lock_real); }
    if (nid == "upoVrzMHFeE" || nid == "K-jXhbt2gn4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_pthread_mutex_trylock_real); }
    if (nid == "tn3VlD0hG60" || nid == "2Z+PpY6CaJg") { resolved = true; return reinterpret_cast<void*>(&ps5rt_pthread_mutex_unlock_real); }
    if (nid == "2Tb92quprl0" || nid == "0TyVk4MSLt0") { resolved = true; return reinterpret_cast<void*>(&ps5rt_pthread_cond_init_real); }
    if (nid == "g+PZd2hiacg" || nid == "RXXqi4CtF8w") { resolved = true; return reinterpret_cast<void*>(&ps5rt_pthread_cond_destroy_real); }
    if (nid == "JGgj7Uvrl+A" || nid == "mkx2fVhNMsg") { resolved = true; return reinterpret_cast<void*>(&ps5rt_pthread_cond_broadcast_real); }
    if (nid == "kDh-NfxgMtE" || nid == "2MOy+rUfuhQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_pthread_cond_signal_real); }
    if (nid == "WKAXJ4XBPQ4" || nid == "Op8TBGY5KHg") { resolved = true; return reinterpret_cast<void*>(&ps5rt_pthread_cond_wait_real); }
    if (nid == "o69RpYO-Mu0") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "Ox9i0c7L5w0") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "+L98PIbGttk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "mqdNorrB+gI") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    // Semaphores
    if (nid == "188x57JYp0g") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_create_sema_real); }
    // PS5RT_POSIX_SEM_MANAGED=1 leaves the POSIX semaphores to the bridge.
    static const bool posix_sem_managed = [] {
        const auto* value = std::getenv("PS5RT_POSIX_SEM_MANAGED");
        return value != nullptr && value[0] == '1';
    }();
    if (!posix_sem_managed) {
    if (nid == "pDuPEf3m4fI") { resolved = true; return reinterpret_cast<void*>(&ps5rt_posix_sem_init_real); }
    if (nid == "GEnUkDZoUwY") { resolved = true; return reinterpret_cast<void*>(&ps5rt_pthread_sem_init_real); }
    if (nid == "YCV5dGGBcCo") { resolved = true; return reinterpret_cast<void*>(&ps5rt_posix_sem_wait_real); }
    if (nid == "C36iRE0F5sE") { resolved = true; return reinterpret_cast<void*>(&ps5rt_posix_sem_wait_real); }
    if (nid == "WBWzsRifCEA") { resolved = true; return reinterpret_cast<void*>(&ps5rt_posix_sem_trywait_real); }
    if (nid == "H2a+IN9TP0E") { resolved = true; return reinterpret_cast<void*>(&ps5rt_pthread_sem_trywait_real); }
    if (nid == "w5IHyvahg-o") { resolved = true; return reinterpret_cast<void*>(&ps5rt_posix_sem_timedwait_real); }
    if (nid == "IKP8typ0QUk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_posix_sem_post_real); }
    if (nid == "aishVAiFaYM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_posix_sem_post_real); }
    if (nid == "Bq+LRV-N6Hk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_posix_sem_getvalue_real); }
    if (nid == "cDW233RAwWo") { resolved = true; return reinterpret_cast<void*>(&ps5rt_posix_sem_destroy_real); }
    if (nid == "Vwc+L05e6oE") { resolved = true; return reinterpret_cast<void*>(&ps5rt_posix_sem_destroy_real); }
    }
    if (nid == "Zxa0VhQVTsk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_wait_sema_real); }
    if (nid == "4czppHBiriw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_signal_sema_real); }
    if (nid == "R1Jvn8bSCW8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_delete_sema_real); }
    // Event flags
    if (nid == "BpFoboUJoZU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_create_event_flag_real); }
    if (nid == "8mql9OcQnd4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_delete_event_flag_real); }
    if (nid == "IOnSvHzqu6A") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_set_event_flag_real); }
    if (nid == "JTvBflhYazQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_kernel_wait_event_flag_real); }
    // VideoOut
    if (nid == "Up36PTk687E") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_open_real); }
    if (nid == "uquVH4-Du78") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_close_real); }
    if (nid == "w3BY+tAEiQY") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_register_buffers_real); }
    if (nid == "rKBUtgRrtbk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_register_buffers2_real); }
    if (nid == "U46NwOiJpys") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_submit_flip_real); }
    if (nid == "CBiu4mCE1DA") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_set_flip_rate_real); }
    if (nid == "zgXifHT9ErY") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_is_flip_pending_real); }
    if (nid == "1FZBKy8HeNU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_get_vblank_status_real); }
    if (nid == "HXzjK9yI30k") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_add_flip_event_real); }
    if (nid == "i6-sR91Wt-4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_set_buffer_attribute_real); }
    if (nid == "PjS5uASwcV8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_set_buffer_attribute2_real); }
    if (nid == "HuViW4HnrOw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "utPrVdxio-8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_get_output_status_real); }
    if (nid == "DYhhWbJSeRg") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_color_settings_real); }
    if (nid == "pv9CI5VC+R0") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_adjust_color_real); }
    if (nid == "Nv8c-Kb+DUM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_is_output_supported_real); }
    if (nid == "j6RaAUlaLv0") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_wait_vblank_real); }
    if (nid == "N5KDtkIjjJ4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_videoout_unregister_buffers_real); }
    // AGC
    if (nid == "23LRUSvYu1M" || nid == "kW3GLb7QfPg") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_init_real); }
    if (nid == "2JtWUUiYBXs") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_get_register_defaults_real); }
    if (nid == "wRbq6ZjNop4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_get_register_defaults_internal_real); }
    if (nid == "V++UgBtQhn0") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_get_data_packet_payload_address_real); }
    if (nid == "fPSCdQxgpSw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_write_data_patch_address_real); }
    if (nid == "3KDcnM3lrcU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_wait_reg_mem_patch_address_real); }
    if (nid == "f3dg2CSgRKY") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_create_shader_real); }
    if (nid == "1-gUn1PI4Sw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_return_zero); }
    if (nid == "UglJIZjGssM") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_sharpemu_import);
    }
    if (nid == "gSRnr79F8tQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_driver_submit_acb_real); }
    if (nid == "YUeqkyT7mEQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_set_flip_real); }
    if (nid == "q88lQ+GP5Yk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_draw_index_real); }
    if (nid == "Yw0jKSqop+E") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_draw_index_auto_real); }
    if (nid == "rjMI+3Rz-2Y") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_set_sh_registers_real); }
    if (nid == "LtTouSCZjHM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_cb_nop_real); }
    if (nid == "k3GhuSNmBLU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_cb_dispatch_real); }
    if (nid == "aJf+j5yntiU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_event_write_real); }
    if (nid == "57labkp+rSQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_acquire_mem_real); }
    if (nid == "i1jyy49AjXU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_write_data_real); }
    if (nid == "VmW0Tdpy420") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_wait_reg_mem_real); }

    // The command buffer writers. Astro calls these thousands of times a
    // frame; served from the managed bridge each one paid for a crossing
    // that cost far more than the dozen dwords it went there to write.
    // The Acb entry points share the Dcb implementation, as they do in the
    // managed version - the packet is the same, only the queue differs.
    if (nid == "wr23dPKyWc0") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_cb_release_mem_real); }
    if (nid == "n2fD4A+pb+g") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_cb_set_sh_register_range_direct_real); }
    if (nid == "UZbQjYAwwXM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_cb_set_sh_registers_direct_real); }
    if (nid == "+kSrjIVxKFE" || nid == "cpCILPya5Zk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_push_marker_real); }
    if (nid == "H7uZqCoNuWk" || nid == "6mFxkVqdmbQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_pop_marker_real); }
    if (nid == "d-6uF9sZDIU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_set_cx_reg_indirect_patch_add_registers_real); }
    if (nid == "z2duB-hHQSM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_set_sh_reg_indirect_patch_add_registers_real); }
    if (nid == "vRoArM9zaIk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_set_uc_reg_indirect_patch_add_registers_real); }
    if (nid == "ZvwO9euwYzc") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_set_cx_registers_indirect_real); }
    if (nid == "-HOOCn0JY48") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_set_sh_registers_indirect_real); }
    if (nid == "hvUfkUIQcOE") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_set_uc_registers_indirect_real); }
    if (nid == "tSBxhAPyytQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_set_num_instances_real); }
    if (nid == "8N2tmT3jmC8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_set_index_count_real); }
    if (nid == "0fWWK5uG9rQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_queue_end_of_pipe_action_patch_address_real); }
    if (nid == "vcmNN+AAXnY") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_set_cx_reg_indirect_patch_set_address_real); }
    if (nid == "Qrj4c+61z4A") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_set_sh_reg_indirect_patch_set_address_real); }
    if (nid == "6lNcCp+fxi4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_set_uc_reg_indirect_patch_set_address_real); }
    if (nid == "Yw0jKSqop+E") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_draw_index_auto_real); }
    if (nid == "l4fM9K-Lyks") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_set_index_buffer_real); }
    if (nid == "D9sr1xGUriE") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_create_prim_state_real); }
    if (nid == "LtTouSCZjHM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_cb_nop_real); }
    if (nid == "i1jyy49AjXU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_write_data_real); }
    if (nid == "eZ4+17OQz4Q") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_write_data_real); }
    if (nid == "u2T2DiA5hRI") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_stall_command_buffer_parser_real); }
    if (nid == "WmAc2MEj6Io") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_dma_data_real); }
    if (nid == "-RnpfpxIhec") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_acb_dma_data_real); }
    if (nid == "RmaJwLtc8rY") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_set_base_indirect_args_real); }
    if (nid == "CtB+A9-VxO0") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_dispatch_indirect_real); }
    if (nid == "j3EtxFkSIhQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_acb_dispatch_indirect_real); }
    if (nid == "t1vNu082-jM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_draw_index_indirect_real); }
    if (nid == "vuSXe69VILM") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_get_lod_stats_real); }
    if (nid == "MWiElSNE8j8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_wait_until_safe_for_rendering_real); }
    if (nid == "YUeqkyT7mEQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_dcb_set_flip_real); }
    if (nid == "cFazmnXpJOE") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_acb_event_write_real); }
    if (nid == "KT-hTp-Ch14") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_acb_acquire_mem_real); }
    if (nid == "htn36gPnBk4") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_acb_wait_reg_mem_real); }
    if (nid == "HV4j+E0MBHE") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_create_interpolant_mapping_real); }
    if (nid == "qj7QZpgr9Uw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_agc_unknown_qj7_real); }

    // The online imports the title reaches once it gets far enough. These
    // had no handler at all and fell through to the managed auto-stub, one
    // dedup-ed log line each and zero in RAX. Same value, named, traced,
    // and no crossing.
    if (nid == "SUEVes8gvmw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_rudp_set_event_handler_real); } // sceRudpSetEventHandler
    if (nid == "6PBNpsgyaxw") { resolved = true; return reinterpret_cast<void*>(&ps5rt_rudp_enable_internal_io_thread_real); } // sceRudpEnableInternalIOThread
    if (nid == "hoOAofhhRvE") { resolved = true; return reinterpret_cast<void*>(&ps5rt_net_getsockname_real); } // sceNetGetsockname
    if (nid == "gvD1greCu0A") { resolved = true; return reinterpret_cast<void*>(&ps5rt_net_sendto_real); } // sceNetSendto
    if (nid == "Y295ygEccqk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_cppwebapi_lib_context_ctor_real); } // Common::LibContext::LibContext
    if (nid == "8x++mBOUeso") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_cppwebapi_init_params_ctor_real); } // Common::InitParams::InitParams
    if (nid == "52AlYvq+dmk") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_cppwebapi_init_params_dtor_real); } // Common::InitParams::~InitParams
    if (nid == "W+-RA2Vn-cc") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_response_ptr_ctor_real); } // IntrusivePtr<GetPublicProfilesResponse>::IntrusivePtr
    if (nid == "aEt4aNpeLwQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_response_ptr_dtor_real); } // IntrusivePtr<GetPublicProfilesResponse>::~IntrusivePtr
    if (nid == "ZYTehDq4VkA") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_response_ptr_arrow_real); } // IntrusivePtr<GetPublicProfilesResponse>::operator->
    if (nid == "DT39dZn0J+c") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_profile_vector_ptr_ctor_real); } // IntrusivePtr<Vector<BasicProfile>>::IntrusivePtr
    if (nid == "6QL0WMBsvbs") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_profile_vector_ptr_dtor_real); } // IntrusivePtr<Vector<BasicProfile>>::~IntrusivePtr
    if (nid == "ZPUDWmgk5Jo") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_profile_vector_ptr_assign_real); } // IntrusivePtr<Vector<BasicProfile>>::operator=
    if (nid == "WN8MUUVljFU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_profile_vector_ptr_get_real); } // IntrusivePtr<Vector<BasicProfile>>::get
    if (nid == "rq73eO5xSRc") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_transaction_ctor_real); } // Transaction<...>::Transaction
    if (nid == "5UV12de1fGo") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_transaction_dtor_real); } // Transaction<...>::~Transaction
    if (nid == "cMtWfMUFIY8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_transaction_start_real); } // TransactionBase<...>::start
    if (nid == "FywKncPHxLc") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_transaction_finish_real); } // TransactionBase<...>::finish
    if (nid == "xugo4hJuBQQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_transaction_set_response_option_real); } // TransactionBase<...>::setResponseInformationOption
    if (nid == "GBILv-xY3eU") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_transaction_get_response_real); } // Transaction<...>::getResponse
    if (nid == "q0CBI7DJ0X8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_public_profiles_parameter_ctor_real); } // ParameterToGetPublicProfiles::ParameterToGetPublicProfiles
    if (nid == "o7Rj82lRZ98") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_public_profiles_parameter_dtor_real); } // ParameterToGetPublicProfiles::~ParameterToGetPublicProfiles
    if (nid == "58HnTZbC0+k") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_public_profiles_parameter_initialize_real); } // ParameterToGetPublicProfiles::initialize
    if (nid == "-NWoybNRyYQ") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_public_profiles_parameter_terminate_real); } // ParameterToGetPublicProfiles::terminate
    if (nid == "X1JE3HkJST8") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_get_public_profiles_real); } // BasicProfileApi::getPublicProfiles
    if (nid == "dv8KUvfjc8c") { resolved = true; return reinterpret_cast<void*>(&ps5rt_np_response_get_profiles_real); } // GetPublicProfilesResponse::getProfiles

    if (nid == "OMDRKKAZ8I4" ||
        nid == "zE-wXIZjLoM" ||
        nid == "jh+8XiK4LeE" ||
        nid == "5txKfcMUAok" ||
        nid == "4oXYe9Xmk0Q" ||
        nid == "x1X76arYMxU" ||
        nid == "m5-2bsNfv7s" ||
        nid == "waPcxYiR3WA") {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_return_zero);
    }

    // Keep HLE ahead of bundled runtime code. A bundled libc exports thousands
    // of functions, but its internal ABI and initialization state are not
    // interchangeable with the host HLE heap, pthread, and stdio state. The
    // Native SharpEmu exports stay ahead of bundled runtime code. Policy and
    // permissive bridge stubs stay behind it so they cannot shadow LLE
    // functions such as vsprintf_s.
    const auto bridge_support = hle_bridge_nid_support(nid);
    if (bridge_support == 1) {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_sharpemu_import);
    }

    for (const auto& symbol : generated::kRuntimeSymbols) {
        if (symbol.nid != nullptr && nid == symbol.nid) {
            resolved = true;
            return reinterpret_cast<void*>(symbol.address);
        }
    }

    if (bridge_support > 1) {
        resolved = true;
        return reinterpret_cast<void*>(&ps5rt_sharpemu_import);
    }

    resolved = false;
    return reinterpret_cast<void*>(&ps5rt_unresolved_import);
}

bool resolve_data_symbol(std::string_view nid, std::uint64_t& address) {
    if (nid == "f7uOxY9mM1U") {
        address = reinterpret_cast<std::uint64_t>(g_stack_check_guard);
        return true;
    }
    if (nid == "djxxOmW6-aw") {
        address = reinterpret_cast<std::uint64_t>(&g_process_name_pointer);
        return true;
    }
    if (nid == "P330P3dFF68") {
        address = reinterpret_cast<std::uint64_t>(&g_libc_need_flag);
        return true;
    }
    if (nid == "ZT4ODD2Ts9o") {
        address = reinterpret_cast<std::uint64_t>(
            &g_libc_internal_need_flag);
        return true;
    }
    return false;
}

bool resolve_symbol_address(std::string_view nid, std::uint64_t& address) {
    for (const auto& symbol : generated::kRuntimeSymbols) {
        if (symbol.nid != nullptr && nid == symbol.nid) {
            address = symbol.address;
            return true;
        }
    }
    return resolve_data_symbol(nid, address);
}

bool resolve_import_stub_address(
    std::string_view nid,
    std::uint64_t& address) {
    for (const auto& import : generated::kImports) {
        if (import.nid != nullptr && nid == import.nid) {
            address = import.stub_address;
            return true;
        }
    }
    return false;
}

RelocationSetupResult setup_imported_relocations() {
    RelocationSetupResult result;
    for (const auto& relocation : generated::kImportedRelocations) {
        std::uint64_t base_address = 0;
        bool address_resolved = false;
        if (!relocation.is_data) {
            bool import_resolved = false;
            void* handler = resolve_import(relocation.nid, import_resolved);
            if (import_resolved) {
                if (handler ==
                    reinterpret_cast<void*>(&ps5rt_sharpemu_import)) {
                    address_resolved = resolve_import_stub_address(
                        relocation.nid,
                        base_address);
                } else if (
                    handler !=
                    reinterpret_cast<void*>(&ps5rt_unresolved_import)) {
                    base_address = reinterpret_cast<std::uint64_t>(handler);
                    address_resolved = true;
                }
            }
        }
        if (!address_resolved) {
            address_resolved =
                resolve_symbol_address(relocation.nid, base_address);
        }
        if (!address_resolved) {
            ++result.unresolved;
            if (result.first_unresolved.empty()) {
                result.first_unresolved = relocation.nid;
            }
            continue;
        }

        const auto value = relocation.addend >= 0
            ? base_address + static_cast<std::uint64_t>(relocation.addend)
            : base_address - static_cast<std::uint64_t>(-relocation.addend);
        std::memcpy(
            reinterpret_cast<void*>(relocation.target_address),
            &value,
            sizeof(value));
        ++result.resolved;
    }
    return result;
}

bool patch_import_stub(void* address, void* handler) {
    DWORD old_protection = 0;
    if (!VirtualProtect(
            address,
            16,
            PAGE_EXECUTE_READWRITE,
            &old_protection)) {
        return false;
    }

    std::uint8_t stub[16] = {
        0x48, 0xB8,                   // mov rax, handler
        0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xE0,                   // jmp rax
        0x90, 0x90, 0x90, 0x90,
    };
    const auto target = reinterpret_cast<std::uint64_t>(handler);
    std::memcpy(stub + 2, &target, sizeof(target));
    std::memcpy(address, stub, sizeof(stub));
    FlushInstructionCache(GetCurrentProcess(), address, sizeof(stub));

    DWORD ignored = 0;
    return VirtualProtect(address, 16, old_protection, &ignored) != 0;
}

bool patch_relative_import_stub(void* address, void* thunk) {
    const auto source = reinterpret_cast<std::intptr_t>(address);
    const auto destination = reinterpret_cast<std::intptr_t>(thunk);
    const auto displacement = destination - (source + 5);
    if (displacement < std::numeric_limits<std::int32_t>::min() ||
        displacement > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(
            address,
            16,
            PAGE_EXECUTE_READWRITE,
            &old_protection)) {
        return false;
    }
    std::uint8_t stub[16] = {
        0xE9, 0, 0, 0, 0,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90,
    };
    const auto relative = static_cast<std::int32_t>(displacement);
    std::memcpy(stub + 1, &relative, sizeof(relative));
    std::memcpy(address, stub, sizeof(stub));
    FlushInstructionCache(GetCurrentProcess(), address, sizeof(stub));
    DWORD ignored = 0;
    return VirtualProtect(address, 16, old_protection, &ignored) != 0;
}

void* create_unresolved_thunks() {
    if (generated::kImportCount == 0) {
        return nullptr;
    }
    const auto raw_size = generated::kImportCount * kUnresolvedThunkSize;
    const auto size = align_up(raw_size, kAllocationGranularity);
    void* allocation = VirtualAlloc(
        reinterpret_cast<void*>(kUnresolvedThunkAddress),
        static_cast<SIZE_T>(size),
        MEM_RESERVE | MEM_COMMIT,
        PAGE_EXECUTE_READWRITE);
    if (allocation != reinterpret_cast<void*>(kUnresolvedThunkAddress)) {
        if (allocation != nullptr) {
            VirtualFree(allocation, 0, MEM_RELEASE);
        }
        return nullptr;
    }

    for (std::size_t index = 0; index < generated::kImportCount; ++index) {
        auto* thunk = static_cast<std::uint8_t*>(allocation) +
            index * kUnresolvedThunkSize;
        std::uint8_t code[kUnresolvedThunkSize] = {
            0x49, 0xBA,                         // mov r10, nid
            0, 0, 0, 0, 0, 0, 0, 0,
            0x48, 0xB8,                         // mov rax, handler
            0, 0, 0, 0, 0, 0, 0, 0,
            0xFF, 0xE0,                         // jmp rax
            0x90, 0x90, 0x90, 0x90, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90,
        };
        const auto nid = reinterpret_cast<std::uint64_t>(
            generated::kImports[index].nid);
        bool resolved = false;
        void* resolved_handler = resolve_import(
            generated::kImports[index].nid,
            resolved);
        const auto handler = reinterpret_cast<std::uint64_t>(
            resolved &&
                resolved_handler ==
                    reinterpret_cast<void*>(&ps5rt_sharpemu_import)
                ? resolved_handler
                : reinterpret_cast<void*>(&ps5rt_unresolved_import));
        std::memcpy(code + 2, &nid, sizeof(nid));
        std::memcpy(code + 12, &handler, sizeof(handler));
        std::memcpy(thunk, code, sizeof(code));
    }
    FlushInstructionCache(GetCurrentProcess(), allocation, raw_size);
    DWORD ignored = 0;
    if (!VirtualProtect(
            allocation,
            static_cast<SIZE_T>(size),
            PAGE_EXECUTE_READ,
            &ignored)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return nullptr;
    }
    return allocation;
}

ImportSetupResult setup_generated_imports(void* unresolved_thunks) {
    ImportSetupResult result;
    for (const auto& import : generated::kImports) {
        bool resolved = false;
        void* handler = resolve_import(import.nid, resolved);
        const auto import_index = static_cast<std::size_t>(
            &import - generated::kImports.data());
        const bool use_nid_thunk =
            !resolved ||
            handler == reinterpret_cast<void*>(&ps5rt_sharpemu_import);
        const bool patched = !use_nid_thunk
            ? patch_import_stub(
                reinterpret_cast<void*>(import.stub_address),
                handler)
            : unresolved_thunks != nullptr &&
              patch_relative_import_stub(
                  reinterpret_cast<void*>(import.stub_address),
                  static_cast<std::uint8_t*>(unresolved_thunks) +
                      import_index * kUnresolvedThunkSize);
        if (!patched) {
            throw std::runtime_error(
                "Could not patch import stub at 0x" +
                std::to_string(import.stub_address));
        }
        if (resolved) {
            ++result.resolved;
        } else {
            ++result.unresolved;
            if (result.first_unresolved.empty()) {
                result.first_unresolved = import.nid;
            }
        }
    }
    return result;
}

void* create_bootstrap_hle_import() {
    void* page = VirtualAlloc(
        reinterpret_cast<void*>(kBootstrapHleAddress),
        static_cast<SIZE_T>(kPageSize),
        MEM_RESERVE | MEM_COMMIT,
        PAGE_EXECUTE_READ);
    if (page != reinterpret_cast<void*>(kBootstrapHleAddress)) {
        if (page != nullptr) {
            VirtualFree(page, 0, MEM_RELEASE);
        }
        return nullptr;
    }

    bool resolved = false;
    void* handler = resolve_import("ps5rt-test", resolved);
    if (!resolved || !patch_import_stub(page, handler)) {
        VirtualFree(page, 0, MEM_RELEASE);
        return nullptr;
    }
    return page;
}

std::vector<AddressRange> build_reservation_ranges() {
    std::vector<AddressRange> ranges;
    ranges.reserve(generated::kSegmentCount);
    for (const auto& segment : generated::kSegments) {
        if (segment.memory_size == 0 ||
            segment.memory_size >
                std::numeric_limits<std::uint64_t>::max() -
                    segment.virtual_address) {
            throw std::runtime_error("Invalid segment address range");
        }
        ranges.push_back({
            align_down(segment.virtual_address, kAllocationGranularity),
            align_up(
                segment.virtual_address + segment.memory_size,
                kAllocationGranularity),
        });
    }
    std::sort(
        ranges.begin(),
        ranges.end(),
        [](const AddressRange& left, const AddressRange& right) {
            return left.base < right.base;
        });

    std::vector<AddressRange> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.base <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, range.end);
        } else {
            merged.push_back(range);
        }
    }
    return merged;
}

std::vector<Reservation> reserve_guest_ranges() {
    const auto ranges = build_reservation_ranges();
    std::vector<Reservation> reservations;
    reservations.reserve(ranges.size());
    for (const auto& range : ranges) {
        const auto size = range.end - range.base;
        void* reservation = VirtualAlloc(
            reinterpret_cast<void*>(range.base),
            static_cast<SIZE_T>(size),
            MEM_RESERVE,
            PAGE_NOACCESS);
        if (reservation != reinterpret_cast<void*>(range.base)) {
            const auto error = GetLastError();
            if (reservation != nullptr) {
                VirtualFree(reservation, 0, MEM_RELEASE);
            }
            for (const auto& allocated : reservations) {
                VirtualFree(allocated.base, 0, MEM_RELEASE);
            }
            throw std::runtime_error(
                "Could not reserve guest range: " + windows_error(error));
        }
        reservations.push_back({reservation, size});
    }
    return reservations;
}

void release_guest_ranges(const std::vector<Reservation>& reservations) {
    for (const auto& reservation : reservations) {
        VirtualFree(reservation.base, 0, MEM_RELEASE);
    }
}

int run(const Options& options) {
    start_sampler();
    const auto game_root = options.app0_directory.empty()
        ? std::filesystem::absolute(options.package_directory)
        : std::filesystem::absolute(options.app0_directory);
    const auto game_root_utf8 = path_utf8(game_root);
    ps5rt_set_game_root(game_root_utf8.c_str());
    trace_stderr("app0_root=%s\n", game_root_utf8.c_str());
    g_ignore_guest_int41 = environment_flag_enabled("PS5RT_IGNORE_INT41");
    if (g_ignore_guest_int41) {
        trace_stderr("guest_int41_policy=ignore\n");
    }
    if (g_guest_tls_base_tls_index == TLS_OUT_OF_INDEXES) {
        g_guest_tls_base_tls_index = TlsAlloc();
        if (g_guest_tls_base_tls_index == TLS_OUT_OF_INDEXES) {
            std::cerr << "Could not allocate guest TLS base slot\n";
            return 14;
        }
    }
    AddVectoredExceptionHandler(1, &trace_guest_exception);

    Ps5RtCreateInfo runtime_options{
        sizeof(Ps5RtCreateInfo),
        PS5RT_ABI_VERSION,
        options.gpu_backend,
        PS5RT_UPSCALER_NATIVE,
        options.headless ? PS5RT_CREATE_HEADLESS : 0U,
    };
    RuntimeHandle runtime;
    const auto runtime_result =
        ps5rt_create(&runtime_options, runtime.address());
    if (runtime_result != PS5RT_OK) {
        std::cerr << "Could not initialize Ps5Runtime: "
                  << ps5rt_get_result_name(runtime_result) << '\n';
        return 12;
    }

    Ps5RtGpuInfo gpu_info{};
    gpu_info.struct_size = sizeof(Ps5RtGpuInfo);
    const auto gpu_info_result =
        ps5rt_get_gpu_info(runtime.get(), &gpu_info);
    if (gpu_info_result != PS5RT_OK) {
        std::cerr << "Could not query Ps5Runtime GPU information: "
                  << ps5rt_get_result_name(gpu_info_result) << '\n';
        return 12;
    }

    std::cout << "runtime_abi=0x" << std::hex
              << ps5rt_get_abi_version() << '\n'
              << "gpu_backend="
              << ps5rt_get_backend_name(gpu_info.active_backend)
              << " gpu_adapter=" << gpu_info.adapter_name << '\n';

    if (generated::kManifestFormatVersion != 6) {
        std::cerr << "Unsupported package format "
                  << generated::kManifestFormatVersion << '\n';
        return 3;
    }
    if (generated::kSegmentCount == 0 || !contains_executable_entry()) {
        std::cerr << "Package has no executable entry point\n";
        return 3;
    }
    std::vector<Reservation> reservations;
    try {
        reservations = reserve_guest_ranges();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 5;
    }

    for (const auto& segment : generated::kSegments) {
        const auto commit_base = align_down(segment.virtual_address, kPageSize);
        const auto commit_end = align_up(
            segment.virtual_address + segment.memory_size,
            kPageSize);
        void* committed = VirtualAlloc(
            reinterpret_cast<void*>(commit_base),
            static_cast<SIZE_T>(commit_end - commit_base),
            MEM_COMMIT,
            PAGE_READWRITE);
        if (committed != reinterpret_cast<void*>(commit_base)) {
            std::cerr << "Could not commit segment at 0x" << std::hex
                      << segment.virtual_address << ": "
                      << windows_error(GetLastError()) << '\n';
            release_guest_ranges(reservations);
            return 7;
        }
        if (!read_segment(options.package_directory, segment)) {
            std::cerr << "Could not read segment bytes at blob offset 0x"
                      << std::hex << segment.blob_offset
                      << " from image index " << std::dec
                      << segment.image_index << '\n';
            release_guest_ranges(reservations);
            return 8;
        }
    }

    initialize_hle_bridge(options);
    const auto relocations = setup_imported_relocations();

    std::map<std::uint64_t, std::uint32_t> page_protections;
    for (const auto& segment : generated::kSegments) {
        const auto page_begin =
            align_down(segment.virtual_address, kPageSize);
        const auto page_end = align_up(
            segment.virtual_address + segment.memory_size,
            kPageSize);
        for (auto page = page_begin; page < page_end; page += kPageSize) {
            page_protections[page] |= segment.protection;
        }
    }

    for (auto page = page_protections.begin();
         page != page_protections.end();) {
        const auto protect_base = page->first;
        const auto flags = page->second;
        auto protect_end = protect_base + kPageSize;
        auto next = std::next(page);
        while (next != page_protections.end() &&
               next->first == protect_end &&
               next->second == flags) {
            protect_end += kPageSize;
            ++next;
        }

        DWORD old_protection = 0;
        if (!VirtualProtect(
                reinterpret_cast<void*>(protect_base),
                static_cast<SIZE_T>(protect_end - protect_base),
                final_protection(flags),
                &old_protection)) {
            std::cerr << "Could not protect guest pages at 0x" << std::hex
                      << protect_base << ": "
                      << windows_error(GetLastError()) << '\n';
            release_guest_ranges(reservations);
            return 9;
        }
        page = next;
    }

    for (const auto& segment : generated::kSegments) {
        FlushInstructionCache(
            GetCurrentProcess(),
            reinterpret_cast<void*>(segment.virtual_address),
            static_cast<SIZE_T>(segment.memory_size));
    }

    initialize_gpu_bridge();
    initialize_native_gpu_runtime();
    NativeGpuRuntimeScope native_gpu_runtime_scope;
    patch_agc_runtime_exports();

    void* unresolved_thunks = create_unresolved_thunks();
    if (generated::kImportCount != 0 && unresolved_thunks == nullptr) {
        std::cerr << "Could not create import thunk table\n";
        release_guest_ranges(reservations);
        return 4;
    }

    ImportSetupResult imports;
    try {
        imports = setup_generated_imports(unresolved_thunks);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (unresolved_thunks != nullptr) {
            VirtualFree(unresolved_thunks, 0, MEM_RELEASE);
        }
        release_guest_ranges(reservations);
        return 4;
    }

    void* bootstrap_hle = create_bootstrap_hle_import();
    if (bootstrap_hle == nullptr) {
        std::cerr << "Could not create bootstrap HLE thunk at 0x" << std::hex
                  << kBootstrapHleAddress << ": "
                  << windows_error(GetLastError()) << '\n';
        release_guest_ranges(reservations);
        return 11;
    }

    std::cout << "ps5rt native package runner\n"
              << "images=" << std::dec << generated::kImageCount
              << " segments=" << generated::kSegmentCount
              << " imports=" << generated::kImportCount << '\n'
              << "guest_regions=" << reservations.size() << '\n'
              << "runtime_symbols=" << generated::kRuntimeSymbolCount
              << " relocations_resolved=" << relocations.resolved
              << " relocations_unresolved=" << relocations.unresolved << '\n'
              << "imports_resolved=" << imports.resolved
              << " imports_unresolved=" << imports.unresolved << '\n'
              << "entry=0x" << std::hex << generated::kEntryPoint << '\n';

    if (imports.unresolved != 0 &&
        !options.allow_unresolved_imports &&
        !options.validate_only) {
        std::cerr << "Unresolved imports remain; first="
                  << imports.first_unresolved
                  << ". Use --validate-only to verify package mapping.\n";
        VirtualFree(bootstrap_hle, 0, MEM_RELEASE);
        if (unresolved_thunks != nullptr) {
            VirtualFree(unresolved_thunks, 0, MEM_RELEASE);
        }
        release_guest_ranges(reservations);
        return 4;
    }

    if (options.validate_only) {
        std::cout << "validation=PASS\n";
        VirtualFree(bootstrap_hle, 0, MEM_RELEASE);
        if (unresolved_thunks != nullptr) {
            VirtualFree(unresolved_thunks, 0, MEM_RELEASE);
        }
        release_guest_ranges(reservations);
        return 0;
    }

    GuestTls guest_tls;
    if (!guest_tls.initialize()) {
        std::cerr << "Could not initialize guest FS/TLS\n";
        VirtualFree(bootstrap_hle, 0, MEM_RELEASE);
        if (unresolved_thunks != nullptr) {
            VirtualFree(unresolved_thunks, 0, MEM_RELEASE);
        }
        release_guest_ranges(reservations);
        return 13;
    }
    ps5rt_set_guest_tls_hooks(
        &create_guest_tls_context,
        &destroy_guest_tls_context,
        &restore_guest_fs);
    patch_guest_tls_loads();

    using GuestFunction = std::uint64_t (PS5_GUEST_ABI*)(
        std::uint64_t,
        std::uint64_t,
        std::uint64_t);
    if (options.run_initializers) {
        for (const auto& initializer : generated::kInitializers) {
            g_active_initializer = initializer.address;
            g_active_initializer_name = initializer.image_name;
            trace_stderr(
                "initializer=%s@0x%016llX\n",
                initializer.image_name,
                static_cast<unsigned long long>(initializer.address));
            restore_guest_fs();
            const auto result =
                reinterpret_cast<GuestFunction>(initializer.address)(0, 0, 0);
            trace_stderr(
                "initializer_return=%llu\n",
                static_cast<unsigned long long>(result));
        }
        g_active_initializer = 0;
        g_active_initializer_name = nullptr;
        VirtualFree(bootstrap_hle, 0, MEM_RELEASE);
        if (unresolved_thunks != nullptr) {
            VirtualFree(unresolved_thunks, 0, MEM_RELEASE);
        }
        release_guest_ranges(reservations);
        return 0;
    }

    const auto entry =
        reinterpret_cast<GuestFunction>(generated::kEntryPoint);
    g_guest_arguments.clear();
    g_guest_arguments.emplace_back(g_process_name);
    if (environment_flag_enabled("PS5RT_PACKAGE_ARGS")) {
        std::ifstream arguments_file(game_root / "args.txt");
        std::string token;
        while (arguments_file >> token &&
               g_guest_arguments.size() + 3 < kGuestEntryBlockSlots) {
            g_guest_arguments.push_back(token);
        }
    }
    std::size_t slot = 0;
    g_guest_entry_block[slot++] =
        static_cast<std::uint64_t>(g_guest_arguments.size());
    for (const auto& argument : g_guest_arguments) {
        g_guest_entry_block[slot++] =
            reinterpret_cast<std::uint64_t>(argument.c_str());
    }
    g_guest_entry_block[slot++] = 0;  // end of argv
    g_guest_entry_block[slot++] = 0;  // end of envp
    const auto entry_parameters_address =
        reinterpret_cast<std::uint64_t>(g_guest_entry_block);
    {
        std::string joined;
        for (std::size_t index = 1; index < g_guest_arguments.size();
             ++index) {
            joined += ' ';
            joined += g_guest_arguments[index];
        }
        trace_stderr(
            "guest_arguments count=%zu%s\n",
            g_guest_arguments.size(),
            joined.c_str());
    }
    const auto exit_handler_address =
        reinterpret_cast<std::uint64_t>(&ps5rt_return_zero);
    g_active_initializer = generated::kEntryPoint;
    g_active_initializer_name = "eboot.entry";
    trace_stderr(
        "entry_call=0x%016llX params=0x%016llX\n",
        static_cast<unsigned long long>(generated::kEntryPoint),
        static_cast<unsigned long long>(
            entry_parameters_address));
    restore_guest_fs();
    const std::uint64_t result = entry(
        entry_parameters_address,
        exit_handler_address,
        0);
    g_active_initializer = 0;
    g_active_initializer_name = nullptr;
    std::cout << "guest_return=" << std::dec << result << '\n';

    const bool matches = !options.expected_return.has_value() ||
        result == *options.expected_return;
    if (!matches) {
        std::cerr << "Guest return mismatch: expected "
                  << *options.expected_return << ", received " << result
                  << '\n';
    }

    VirtualFree(bootstrap_hle, 0, MEM_RELEASE);
    if (unresolved_thunks != nullptr) {
        VirtualFree(unresolved_thunks, 0, MEM_RELEASE);
    }
    release_guest_ranges(reservations);
    return matches ? 0 : 10;
}

} // namespace

extern "C" Ps5GpuResult ps5rt_gpu_compile_spirv(
    const Ps5GpuShaderRequest* request,
    Ps5GpuShaderResult* result,
    char* error_buffer,
    std::uint32_t error_buffer_size) {
    if (g_gpu_compile_spirv == nullptr) {
        if (result != nullptr) {
            result->status = PS5GPU_ERROR_INTERNAL;
        }
        if (error_buffer != nullptr && error_buffer_size != 0) {
            std::snprintf(
                error_buffer,
                error_buffer_size,
                "Ps5GpuBridge.dll is unavailable");
        }
        return PS5GPU_ERROR_INTERNAL;
    }
    return g_gpu_compile_spirv(
        request,
        result,
        error_buffer,
        error_buffer_size);
}

extern "C" void ps5rt_gpu_free(void* allocation) {
    if (allocation != nullptr && g_gpu_free != nullptr) {
        g_gpu_free(allocation);
    }
}

extern "C" bool ps5rt_native_gpu_register_shader_state(
    const Ps5GpuNativeShaderState* state,
    std::uint32_t* state_id) {
    return g_native_gpu_runtime != nullptr &&
        g_native_gpu_register_shader_state != nullptr &&
        g_native_gpu_register_shader_state(
            g_native_gpu_runtime,
            state,
            state_id) == PS5GPU_NATIVE_OK;
}

extern "C" bool ps5rt_native_gpu_register_compute_state(
    const Ps5GpuNativeComputeState* state,
    std::uint32_t* state_id) {
    return g_native_gpu_runtime != nullptr &&
        g_native_gpu_register_compute_state != nullptr &&
        g_native_gpu_register_compute_state(
            g_native_gpu_runtime,
            state,
            state_id) == PS5GPU_NATIVE_OK;
}

// Tells the GPU runtime that guest memory is being written behind its write
// watch - by a DMA packet this runtime executes. Looked up on first use:
// the export is optional.
extern "C" void ps5rt_native_gpu_guest_written(
    std::uint64_t address, std::uint64_t size) {
    static Ps5GpuNativeGuestMemoryWritten written = nullptr;
    static bool looked_up = false;
    if (!looked_up && g_native_gpu_runtime_module != nullptr) {
        written = reinterpret_cast<Ps5GpuNativeGuestMemoryWritten>(
            GetProcAddress(
                g_native_gpu_runtime_module,
                "ps5gpu_native_guest_memory_written"));
        looked_up = true;
    }
    static std::atomic<std::uint32_t> shown{0};
    if (size >= (1u << 20) && shown.fetch_add(1) < 8) {
        trace_stderr(
            "native_gpu.guest_written_forward address=0x%016llX bytes=%llu "
            "module=%d export=%d\n",
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(size),
            g_native_gpu_runtime_module != nullptr ? 1 : 0,
            written != nullptr ? 1 : 0);
    }
    if (written != nullptr) {
        written(address, size);
    }
}

extern "C" bool ps5rt_native_gpu_submit_draw(
    const Ps5GpuNativeDraw* draw) {
    return g_native_gpu_runtime != nullptr &&
        g_native_gpu_submit_draw != nullptr &&
        g_native_gpu_submit_draw(g_native_gpu_runtime, draw) ==
            PS5GPU_NATIVE_OK;
}

extern "C" bool ps5rt_native_gpu_submit_compute(
    const Ps5GpuNativeComputeDispatch* dispatch) {
    return g_native_gpu_runtime != nullptr &&
        g_native_gpu_submit_compute != nullptr &&
        g_native_gpu_submit_compute(
            g_native_gpu_runtime,
            dispatch) == PS5GPU_NATIVE_OK;
}

extern "C" bool ps5rt_native_gpu_submit_flip(
    const Ps5GpuNativeFlip* flip) {
    return g_native_gpu_runtime != nullptr &&
        g_native_gpu_submit_flip != nullptr &&
        g_native_gpu_submit_flip(g_native_gpu_runtime, flip) ==
            PS5GPU_NATIVE_OK;
}

extern "C" void ps5rt_native_gpu_flush() {
    if (g_native_gpu_runtime != nullptr &&
        g_native_gpu_flush != nullptr) {
        (void)g_native_gpu_flush(g_native_gpu_runtime, 5000);
    }
}

// Started with no arguments - double-clicked - the runner configures itself
// from <executable>.launch.ini beside it, which astro-cycle.ps1 writes on
// every run: the environment the runtime reads, where the game's files are
// and which GPU backend to use. Variables already set in the environment
// win, so a script can still override any of them.
//
// Traces go to <executable>.log rather than to the console: the runtime
// writes tens of megabytes of them, and a console window taking them line
// by line slows the title to a crawl.
//
// The live shader cache is emptied first. Its files are named by state id,
// ids follow the order states are first seen, and a file from an earlier
// run is a different shader under the same name - the driver has crashed on
// exactly that. This run's states are built from memory anyway.
std::vector<std::string> launch_arguments_from_ini(const char* argv0) {
    std::vector<std::string> arguments;
    std::array<wchar_t, 32768> module_path = {};
    const auto length = GetModuleFileNameW(
        nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    const std::filesystem::path executable = length == 0
        ? std::filesystem::path(argv0)
        : std::filesystem::path(std::wstring(module_path.data(), length));
    auto ini = executable;
    ini.replace_extension(".launch.ini");
    std::ifstream input(ini);
    if (!input) {
        return arguments;
    }
    std::string app0;
    std::string backend = "auto";
    // The managed window shows nothing the native runtime draws; when the
    // runtime has a window of its own the managed one is left out.
    bool headless = false;
    std::string line;
    while (std::getline(input, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos || equals == 0) {
            continue;
        }
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        if (key == "app0") {
            app0 = value;
        } else if (key == "gpu_backend") {
            backend = value;
        } else if (key == "headless") {
            headless = value == "1";
        } else {
            char existing[4] = {};
            if (GetEnvironmentVariableA(key.c_str(), existing, sizeof(existing)) == 0 &&
                GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                SetEnvironmentVariableA(key.c_str(), value.c_str());
                // And into the C runtime's copy, which getenv reads and
                // SetEnvironmentVariable does not touch. Without it every
                // setting the GPU runtime reads with getenv - its window,
                // live graphics - was missing from a double-click launch,
                // and only the probe script, which starts the process with
                // them already set, ever saw them.
                _putenv_s(key.c_str(), value.c_str());
            }
        }
    }
    if (app0.empty()) {
        return arguments;
    }
    std::array<char, 4096> live_cache = {};
    if (GetEnvironmentVariableA(
            "PS5GPU_NATIVE_LIVE_SHADER_CACHE_DIR",
            live_cache.data(),
            static_cast<DWORD>(live_cache.size())) != 0) {
        std::error_code error;
        for (const auto& entry :
             std::filesystem::directory_iterator(live_cache.data(), error)) {
            const auto name = entry.path().filename().string();
            if (name.rfind("state-", 0) == 0) {
                std::filesystem::remove(entry.path(), error);
            }
        }
    }
    auto log = executable;
    log.replace_extension(".log");
    FILE* redirected = nullptr;
    redirected = _wfreopen(log.wstring().c_str(), L"w", stderr);
    if (redirected != nullptr) {
        SetStdHandle(
            STD_ERROR_HANDLE,
            reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
    }
    arguments.push_back("--package");
    arguments.push_back(path_utf8(executable.parent_path()));
    arguments.push_back("--app0");
    arguments.push_back(app0);
    arguments.push_back("--allow-unresolved-imports");
    arguments.push_back("--gpu-backend");
    arguments.push_back(backend);
    if (headless) {
        arguments.push_back("--headless");
    }
    std::cout << "Starting from " << path_utf8(ini) << "\n"
              << "Game files: " << app0 << "\n"
              << "Log: " << path_utf8(log) << "\n";
    return arguments;
}

int main(int argc, char** argv) {
    std::vector<std::string> ini_arguments;
    std::vector<char*> ini_argv;
    if (argc == 1) {
        ini_arguments = launch_arguments_from_ini(argv[0]);
        if (!ini_arguments.empty()) {
            ini_argv.push_back(argv[0]);
            for (auto& argument : ini_arguments) {
                ini_argv.push_back(argument.data());
            }
            argc = static_cast<int>(ini_argv.size());
            argv = ini_argv.data();
        }
    }
    const auto options = parse_options(argc, argv);
    if (!options.has_value()) {
        std::cerr << "Usage: Game.exe [--package <directory>] "
                     "[--app0 <directory>] "
                     "[--expect-return <value>] [--validate-only] "
                     "[--allow-unresolved-imports] [--headless] "
                     "[--run-initializers] "
                     "[--gpu-backend <auto|d3d12|vulkan|none>]\n";
        return 1;
    }
    return run(*options);
}
