// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// libSceAvPlayer, natively.
//
// The title's intro is a video. It asks AvPlayer for a player, hands it a
// file, starts it and then pulls decoded frames one at a time, drawing each
// as a texture. The bridge's player could not so much as create itself here
// - it allocated its handle through a memory manager this runtime does not
// run - so Init returned null, the title closed the player it never had and
// went straight to its title screen.
//
// This one decodes with Media Foundation, which ships with Windows: H.264
// to NV12 for the picture. A worker thread decodes ahead into a short
// queue; the title's own calls take frames off it against a playback clock.
// Everything that runs guest code - the texture allocator, the event
// callback, a replaced file reader - runs on the calling guest thread and
// never under the player's lock, because a callback is free to call back
// into the player.
//
// The frame layout follows what the hardware decoder produces and what
// shadPS4 reproduces: NV12 with a pitch rounded up to 64, a height rounded
// up to 16, the chroma plane straight after the luma plane at the same
// pitch, and the padding reported as crop.
//
// Included into ps5rt_hle_impl.cpp; everything here is internal to it.

#ifndef PS5RT_AVPLAYER_H
#define PS5RT_AVPLAYER_H

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <shlwapi.h>
#include <mmsystem.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Exported by mfplat since Windows 7, but declared by the headers only for
// a _WIN32_WINNT the runtime does not build with.
extern "C" HRESULT WINAPI MFCreateMFByteStreamOnStream(
    IStream* stream, IMFByteStream** byte_stream);

namespace ps5rt_avplayer {

constexpr std::int32_t kInvalidParameters = static_cast<std::int32_t>(0x806A0001u);
constexpr std::int32_t kOperationFailed = static_cast<std::int32_t>(0x806A0002u);

// Event numbers the callback receives.
constexpr std::int32_t kEventStateStop = 1;
constexpr std::int32_t kEventStateReady = 2;
constexpr std::int32_t kEventStatePlay = 3;
constexpr std::int32_t kEventStatePause = 4;

// How far the decoder runs ahead of the title, in frames. Enough to ride
// over a slow frame, few enough that a 4K frame queue stays small.
constexpr std::size_t kQueuedFrames = 6;
constexpr std::uint32_t kGuestFrameBuffers = 3;

using GuestAllocate = void* (PS5RT_GUEST_ABI*)(void*, std::uint32_t, std::uint32_t);
using GuestEvent = void (PS5RT_GUEST_ABI*)(void*, std::int32_t, std::int32_t, void*);
using GuestOpen = std::int32_t (PS5RT_GUEST_ABI*)(void*, const char*);
using GuestClose = std::int32_t (PS5RT_GUEST_ABI*)(void*);
using GuestReadOffset =
    std::int32_t (PS5RT_GUEST_ABI*)(void*, std::uint8_t*, std::uint64_t, std::uint32_t);
using GuestSize = std::uint64_t (PS5RT_GUEST_ABI*)(void*);

struct VideoFrame {
    std::vector<std::uint8_t> nv12;
    std::uint64_t timestamp_ms = 0;
};

struct Player {
    // What the title handed Init.
    std::uint64_t allocator_object = 0;
    std::uint64_t allocate = 0;
    std::uint64_t allocate_texture = 0;
    std::uint64_t file_object = 0;
    std::uint64_t file_open = 0;
    std::uint64_t file_close = 0;
    std::uint64_t file_read_offset = 0;
    std::uint64_t file_size = 0;
    std::uint64_t event_object = 0;
    std::uint64_t event_callback = 0;
    bool auto_start = false;

    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE changed = CONDITION_VARIABLE_INIT;

    // The source, and what probing it found.
    std::string guest_path;
    std::vector<std::uint8_t> file_bytes;
    bool has_source = false;
    bool probed = false;
    bool probe_failed = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t source_stride = 0;
    double frames_per_second = 30.0;
    std::uint64_t duration_ms = 0;
    bool has_audio = false;
    std::uint32_t audio_channels = 0;
    std::uint32_t audio_sample_rate = 0;

    // The decoder's side.
    HANDLE worker = nullptr;
    // The sound. The title imports no GetAudioData - it takes no audio from
    // the player - so the player plays the track itself, on a thread of its
    // own with a reader of its own.
    HANDLE audio_worker = nullptr;
    bool audio_seek_requested = false;
    std::uint64_t audio_seek_to_ms = 0;
    bool quit = false;
    bool seek_requested = false;
    std::uint64_t seek_to_ms = 0;
    bool video_done = false;
    std::deque<VideoFrame> frames;

    // The title's side.
    bool started = false;
    bool paused = false;
    bool looping = false;
    bool end_of_stream = false;
    bool stop_event_pending = false;
    // When the last frame was handed out, for PS5RT_AVPLAYER_HOLD_END_MS.
    std::uint64_t drained_at_ms = 0;
    // Twice a second, how the title's asking for frames went: calls,
    // frames handed out, calls that found the queue empty or the front
    // frame not yet due, and where the decoder and the clock were.
    LARGE_INTEGER pacing_since = {};
    std::uint32_t pacing_calls = 0;
    std::uint32_t pacing_given = 0;
    std::uint32_t pacing_empty = 0;
    std::uint32_t pacing_early = 0;
    std::uint32_t pacing_skipped = 0;
    std::uint64_t pacing_last_ts = 0;
    std::uint64_t decoded_frames = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t empty_frames = 0;
    LARGE_INTEGER clock_start = {};
    std::uint64_t clock_offset_ms = 0;
    std::uint64_t paused_at_ms = 0;
    std::uint64_t guest_buffers[kGuestFrameBuffers] = {};
    std::uint32_t guest_buffer_size = 0;
    std::uint32_t next_guest_buffer = 0;

    // Events for the title's callback, delivered by a guest thread of the
    // player's own - which is where the system's player delivers them
    // from. Delivered on the caller's thread instead, from inside
    // AddSource, the callback never returned: it waits on something the
    // thread that called AddSource holds.
    std::deque<std::int32_t> events;
    bool event_thread_started = false;
    bool closed = false;
};

inline SRWLOCK g_players_lock = SRWLOCK_INIT;
inline std::map<std::uint64_t, Player*> g_players;
inline std::atomic<std::uint32_t> g_trace_count{0};

inline void trace(const char* format, ...) {
    if (g_trace_count.fetch_add(1, std::memory_order_relaxed) >= 256) {
        return;
    }
    // One write per line: stderr is unbuffered, and a line written in
    // pieces comes out interleaved with every other thread's trace.
    char line[768] = "avplayer.";
    std::va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line + 9, sizeof(line) - 9, format, arguments);
    va_end(arguments);
    // Straight to the handle: the C runtime's unbuffered stream still
    // splits a line into several writes.
    DWORD written = 0;
    WriteFile(
        GetStdHandle(STD_ERROR_HANDLE),
        line,
        static_cast<DWORD>(std::strlen(line)),
        &written,
        nullptr);
    std::fflush(stderr);
}

inline Player* find_player(std::uint64_t handle) {
    AcquireSRWLockShared(&g_players_lock);
    const auto found = g_players.find(handle);
    auto* player = found == g_players.end() ? nullptr : found->second;
    ReleaseSRWLockShared(&g_players_lock);
    return player;
}

inline std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

inline std::uint32_t frame_pitch(const Player& player) {
    return align_up(player.width, 64);
}

inline std::uint32_t frame_rows(const Player& player) {
    return align_up(player.height, 16);
}

inline std::uint32_t frame_bytes(const Player& player) {
    return frame_pitch(player) * frame_rows(player) * 3 / 2;
}

inline std::uint64_t milliseconds_since(const LARGE_INTEGER& start) {
    LARGE_INTEGER now = {};
    LARGE_INTEGER frequency = {};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        (now.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart);
}

// Where playback is, in the stream's milliseconds. Caller holds the lock.
inline std::uint64_t playback_time(const Player& player) {
    if (!player.started) {
        return player.clock_offset_ms;
    }
    if (player.paused) {
        return player.paused_at_ms;
    }
    return player.clock_offset_ms + milliseconds_since(player.clock_start);
}

// Whether the newest queued frame is already due, so the title, whenever it
// next asks, will skip every one of them. Caller holds the lock.
inline bool queue_overdue(const Player& player) {
    return player.started && !player.paused && !player.frames.empty() &&
        player.frames.back().timestamp_ms <= playback_time(player);
}

inline void restart_clock(Player& player, std::uint64_t at_ms) {
    QueryPerformanceCounter(&player.clock_start);
    player.clock_offset_ms = at_ms;
}

// Queues an event for the player's event thread. Caller holds the lock.
inline void queue_event(Player& player, std::int32_t event) {
    if (player.event_callback == 0) {
        return;
    }
    player.events.push_back(event);
    WakeAllConditionVariable(&player.changed);
}

// The same, taking the lock.
inline void notify(Player& player, std::int32_t event) {
    AcquireSRWLockExclusive(&player.lock);
    queue_event(player, event);
    ReleaseSRWLockExclusive(&player.lock);
}

// --- The decoder -----------------------------------------------------------

inline void copy_nv12(
    const Player& player,
    const std::uint8_t* source,
    std::uint32_t source_length,
    std::uint32_t source_stride,
    std::vector<std::uint8_t>& out) {
    const auto pitch = frame_pitch(player);
    const auto rows = frame_rows(player);
    out.assign(static_cast<std::size_t>(pitch) * rows * 3 / 2, 0);
    if (source_stride == 0) {
        return;
    }
    // The decoder's own plane height is whatever its buffer holds - 1088
    // for a 1080 line picture - and the chroma starts after it, not after
    // the visible lines.
    const auto source_rows = source_length * 2 / 3 / source_stride;
    const auto copy_width = std::min(player.width, source_stride);
    for (std::uint32_t row = 0; row < player.height && row < source_rows;
         ++row) {
        std::memcpy(
            out.data() + static_cast<std::size_t>(row) * pitch,
            source + static_cast<std::size_t>(row) * source_stride,
            copy_width);
    }
    const auto* chroma = source + static_cast<std::size_t>(source_rows) * source_stride;
    auto* out_chroma = out.data() + static_cast<std::size_t>(pitch) * rows;
    for (std::uint32_t row = 0; row < player.height / 2 && row < source_rows / 2;
         ++row) {
        std::memcpy(
            out_chroma + static_cast<std::size_t>(row) * pitch,
            chroma + static_cast<std::size_t>(row) * source_stride,
            copy_width);
    }
}

template <typename T>
inline void release(T*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

// Decoding on the GPU. A 4K H.264 stream at 60 frames a second is more
// than the software decoder keeps up with once the title's own threads are
// busy: it managed 54, the picture fell two seconds behind the sound by the
// end of the intro. The decoder gets a D3D11 device through a DXGI device
// manager; the frames still come back to memory, since the title wants
// them there. PS5RT_AVPLAYER_SOFTWARE=1 keeps the software path.
struct HardwareDecoder {
    ID3D11Device* device = nullptr;
    IMFDXGIDeviceManager* manager = nullptr;
    UINT token = 0;
};

inline bool hardware_decode_wanted() {
    char value[8] = {};
    return GetEnvironmentVariableA(
               "PS5RT_AVPLAYER_SOFTWARE", value, sizeof(value)) == 0 ||
        value[0] != '1';
}

inline void release_hardware(HardwareDecoder& hardware) {
    release(hardware.manager);
    release(hardware.device);
}

inline bool create_hardware(HardwareDecoder& hardware) {
    using CreateDevice = HRESULT(WINAPI*)(
        IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL*, UINT, UINT,
        ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    static const auto create = reinterpret_cast<CreateDevice>(
        reinterpret_cast<void*>(GetProcAddress(
            LoadLibraryA("d3d11.dll"), "D3D11CreateDevice")));
    if (create == nullptr) {
        return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    auto result = create(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2, D3D11_SDK_VERSION,
        &hardware.device, nullptr, nullptr);
    if (FAILED(result)) {
        trace("hardware_device_failed hr=0x%08lX\n",
              static_cast<unsigned long>(result));
        return false;
    }
    // The decoder calls the device from its own threads.
    ID3D10Multithread* multithread = nullptr;
    if (SUCCEEDED(hardware.device->QueryInterface(
            __uuidof(ID3D10Multithread),
            reinterpret_cast<void**>(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }
    result = MFCreateDXGIDeviceManager(&hardware.token, &hardware.manager);
    if (SUCCEEDED(result)) {
        result = hardware.manager->ResetDevice(hardware.device, hardware.token);
    }
    if (FAILED(result)) {
        trace("hardware_manager_failed hr=0x%08lX\n",
              static_cast<unsigned long>(result));
        release_hardware(hardware);
        return false;
    }
    return true;
}

inline IMFSourceReader* open_reader(
    Player& player, HardwareDecoder* hardware = nullptr) {
    IStream* stream = SHCreateMemStream(
        player.file_bytes.data(),
        static_cast<UINT>(player.file_bytes.size()));
    if (stream == nullptr) {
        return nullptr;
    }
    IMFByteStream* byte_stream = nullptr;
    auto result = MFCreateMFByteStreamOnStream(stream, &byte_stream);
    stream->Release();
    if (FAILED(result)) {
        return nullptr;
    }
    IMFAttributes* attributes = nullptr;
    MFCreateAttributes(&attributes, 3);
    if (attributes != nullptr) {
        if (hardware != nullptr && hardware->manager != nullptr) {
            attributes->SetUnknown(
                MF_SOURCE_READER_D3D_MANAGER, hardware->manager);
            attributes->SetUINT32(
                MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            attributes->SetUINT32(
                MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        } else {
            attributes->SetUINT32(
                MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        }
    }
    IMFSourceReader* reader = nullptr;
    result = MFCreateSourceReaderFromByteStream(byte_stream, attributes, &reader);
    release(attributes);
    release(byte_stream);
    if (FAILED(result)) {
        trace("reader_failed hr=0x%08lX\n", static_cast<unsigned long>(result));
        return nullptr;
    }
    reader->SetStreamSelection(
        static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    IMFMediaType* wanted = nullptr;
    MFCreateMediaType(&wanted);
    if (wanted != nullptr) {
        wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        result = reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            nullptr,
            wanted);
        release(wanted);
        if (FAILED(result)) {
            trace("nv12_rejected hr=0x%08lX\n", static_cast<unsigned long>(result));
            reader->Release();
            return nullptr;
        }
    }
    return reader;
}

inline void probe(Player& player, IMFSourceReader* reader) {
    IMFMediaType* type = nullptr;
    if (SUCCEEDED(reader->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type))) {
        UINT32 width = 0;
        UINT32 height = 0;
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
        // The visible picture, which is smaller than the coded one when the
        // coded height is rounded up to whole macroblocks.
        MFVideoArea aperture = {};
        if (SUCCEEDED(type->GetBlob(
                MF_MT_MINIMUM_DISPLAY_APERTURE,
                reinterpret_cast<UINT8*>(&aperture),
                sizeof(aperture),
                nullptr)) &&
            aperture.Area.cx > 0 && aperture.Area.cy > 0) {
            width = static_cast<UINT32>(aperture.Area.cx);
            height = static_cast<UINT32>(aperture.Area.cy);
        }
        UINT32 numerator = 0;
        UINT32 denominator = 0;
        MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator, &denominator);
        UINT32 stride = 0;
        if (FAILED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
            stride = 0;
        }
        player.width = width;
        player.height = height;
        player.source_stride = stride;
        if (numerator != 0 && denominator != 0) {
            player.frames_per_second =
                static_cast<double>(numerator) / denominator;
        }
        type->Release();
    }
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
            MF_PD_DURATION,
            &duration)) &&
        duration.vt == VT_UI8) {
        player.duration_ms = duration.uhVal.QuadPart / 10000;
    }
    PropVariantClear(&duration);
    IMFMediaType* audio = nullptr;
    if (SUCCEEDED(reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &audio))) {
        player.has_audio = true;
        UINT32 value = 0;
        if (SUCCEEDED(audio->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &value))) {
            player.audio_channels = value;
        }
        if (SUCCEEDED(audio->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &value))) {
            player.audio_sample_rate = value;
        }
        audio->Release();
    }
}

inline DWORD WINAPI decoder_main(void* argument) {
    auto& player = *static_cast<Player*>(argument);
    trace("decoder_thread tid=%lu\n", GetCurrentThreadId());
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    HardwareDecoder hardware;
    IMFSourceReader* reader = nullptr;
    if (hardware_decode_wanted() && create_hardware(hardware)) {
        reader = open_reader(player, &hardware);
        if (reader == nullptr) {
            release_hardware(hardware);
        }
    }
    const auto on_gpu = reader != nullptr;
    if (reader == nullptr) {
        reader = open_reader(player);
    }
    trace("decoder hardware=%d\n", on_gpu ? 1 : 0);
    AcquireSRWLockExclusive(&player.lock);
    if (reader == nullptr) {
        player.probe_failed = true;
    } else {
        probe(player, reader);
        player.probe_failed = player.width == 0 || player.height == 0;
    }
    player.probed = true;
    WakeAllConditionVariable(&player.changed);
    ReleaseSRWLockExclusive(&player.lock);

    std::uint32_t current_stride = player.source_stride;
    std::uint64_t decoded = 0;
    while (reader != nullptr) {
        AcquireSRWLockExclusive(&player.lock);
        // A full queue waits for the title - unless the clock has run past
        // all of it. The title takes one frame per call, and at a few calls
        // a second a queue that only refilled on a call moved the picture
        // on six frames a call whatever the clock said: the intro ran in
        // slow motion, the sound, on the clock, ran out half way through.
        // Frames the clock has passed are decoded on and dropped instead.
        while (!player.quit && !player.seek_requested &&
               (player.video_done ||
                (player.frames.size() >= kQueuedFrames &&
                 !queue_overdue(player)))) {
            SleepConditionVariableSRW(&player.changed, &player.lock, 5, 0);
        }
        if (player.quit) {
            ReleaseSRWLockExclusive(&player.lock);
            break;
        }
        if (player.seek_requested) {
            player.seek_requested = false;
            player.video_done = false;
            player.frames.clear();
            const auto target = player.seek_to_ms;
            ReleaseSRWLockExclusive(&player.lock);
            PROPVARIANT position;
            PropVariantInit(&position);
            position.vt = VT_I8;
            position.hVal.QuadPart = static_cast<LONGLONG>(target) * 10000;
            const GUID time_format = {};  // GUID_NULL: 100 ns units
            reader->SetCurrentPosition(time_format, position);
            PropVariantClear(&position);
            continue;
        }
        ReleaseSRWLockExclusive(&player.lock);

        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* sample = nullptr;
        const auto result = reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            0,
            &stream_index,
            &flags,
            &timestamp,
            &sample);
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
            IMFMediaType* type = nullptr;
            if (SUCCEEDED(reader->GetCurrentMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                    &type))) {
                UINT32 stride = 0;
                if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
                    current_stride = stride;
                }
                type->Release();
            }
        }
        VideoFrame frame;
        auto have_frame = false;
        // A frame whose successor is already due is one the title will
        // never be handed: whenever it next asks, it gets that successor or
        // a later one. Such frames are dropped here, before the copy out of
        // the decoder - twelve megabytes each, and the part that kept the
        // decoder from reaching 60 frames a second at 4K.
        if (SUCCEEDED(result) && sample != nullptr) {
            const auto interval_ms = player.frames_per_second > 1.0
                ? static_cast<std::uint64_t>(1000.0 / player.frames_per_second)
                : std::uint64_t{33};
            const auto timestamp_ms = static_cast<std::uint64_t>(timestamp / 10000);
            AcquireSRWLockExclusive(&player.lock);
            const auto overdue = player.started && !player.paused &&
                !player.seek_requested &&
                timestamp_ms + interval_ms <= playback_time(player);
            ReleaseSRWLockExclusive(&player.lock);
            if (overdue) {
                sample->Release();
                sample = nullptr;
                ++decoded;
                ++player.dropped_frames;
            }
        }
        if (SUCCEEDED(result) && sample != nullptr) {
            IMFMediaBuffer* buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
                BYTE* data = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                    auto stride = current_stride;
                    if (stride == 0) {
                        stride = player.width;
                    }
                    copy_nv12(player, data, length, stride, frame.nv12);
                    // Whether the copy came back empty: a middle row of
                    // luma that is all zero is a green frame on screen.
                    {
                        const auto row = static_cast<std::size_t>(
                            frame_pitch(player)) * (player.height / 2);
                        auto empty = true;
                        for (std::size_t x = 0; x < player.width && empty; x += 64) {
                            empty = frame.nv12[row + x] == 0;
                        }
                        if (empty) {
                            ++player.empty_frames;
                            static std::atomic<std::uint32_t> shown{0};
                            if (shown.fetch_add(1) < 6) {
                                trace("empty_frame ts=%llu length=%lu stride=%u\n",
                                      static_cast<unsigned long long>(timestamp / 10000),
                                      static_cast<unsigned long>(length), stride);
                            }
                        }
                    }
                    buffer->Unlock();
                    frame.timestamp_ms =
                        static_cast<std::uint64_t>(timestamp / 10000);
                    have_frame = true;
                }
                buffer->Release();
            }
            sample->Release();
        }
        AcquireSRWLockExclusive(&player.lock);
        if (have_frame && !player.seek_requested) {
            while (player.frames.size() >= kQueuedFrames) {
                player.frames.pop_front();
            }
            player.frames.push_back(std::move(frame));
            ++decoded;
            player.decoded_frames = decoded;
        }
        if (FAILED(result) ||
            (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) != 0) {
            player.video_done = true;
            trace("decoder_end frames=%llu hr=0x%08lX flags=0x%lX\n",
                  static_cast<unsigned long long>(decoded),
                  static_cast<unsigned long>(result),
                  static_cast<unsigned long>(flags));
        }
        WakeAllConditionVariable(&player.changed);
        ReleaseSRWLockExclusive(&player.lock);
    }
    release(reader);
    release_hardware(hardware);
    MFShutdown();
    CoUninitialize();
    return 0;
}

// --- The sound ---------------------------------------------------------------

inline IMFSourceReader* open_audio_reader(
    Player& player, std::uint32_t channels, std::uint32_t rate) {
    IStream* stream = SHCreateMemStream(
        player.file_bytes.data(),
        static_cast<UINT>(player.file_bytes.size()));
    if (stream == nullptr) {
        return nullptr;
    }
    IMFByteStream* byte_stream = nullptr;
    auto result = MFCreateMFByteStreamOnStream(stream, &byte_stream);
    stream->Release();
    if (FAILED(result)) {
        return nullptr;
    }
    IMFSourceReader* reader = nullptr;
    result = MFCreateSourceReaderFromByteStream(byte_stream, nullptr, &reader);
    release(byte_stream);
    if (FAILED(result)) {
        return nullptr;
    }
    reader->SetStreamSelection(
        static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
    IMFMediaType* wanted = nullptr;
    MFCreateMediaType(&wanted);
    if (wanted == nullptr) {
        reader->Release();
        return nullptr;
    }
    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    wanted->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    wanted->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    wanted->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    wanted->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
    result = reader->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr,
        wanted);
    release(wanted);
    if (FAILED(result)) {
        trace("audio_pcm_rejected hr=0x%08lX\n",
              static_cast<unsigned long>(result));
        reader->Release();
        return nullptr;
    }
    return reader;
}

struct AudioBuffer {
    WAVEHDR header = {};
    std::vector<char> bytes;
};

inline DWORD WINAPI audio_main(void* argument) {
    auto& player = *static_cast<Player*>(argument);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    const auto channels = player.audio_channels != 0 ? player.audio_channels : 2;
    const auto rate =
        player.audio_sample_rate != 0 ? player.audio_sample_rate : 48000;
    auto* reader = open_audio_reader(player, channels, rate);
    HWAVEOUT device = nullptr;
    if (reader != nullptr) {
        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(channels);
        format.nSamplesPerSec = rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(channels * 2);
        format.nAvgBytesPerSec = rate * channels * 2;
        const auto opened =
            waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
        if (opened != MMSYSERR_NOERROR) {
            trace("audio_device_failed error=%u\n",
                  static_cast<unsigned>(opened));
            device = nullptr;
        } else {
            trace("audio_started channels=%u rate=%u\n", channels, rate);
        }
    }
    std::deque<AudioBuffer*> queued;
    const auto reclaim = [&](bool all) {
        while (!queued.empty() &&
               (all || (queued.front()->header.dwFlags & WHDR_DONE) != 0)) {
            waveOutUnprepareHeader(
                device, &queued.front()->header, sizeof(WAVEHDR));
            delete queued.front();
            queued.pop_front();
        }
    };
    auto device_paused = false;
    auto finished = false;
    std::uint64_t written_bytes = 0;
    // Where the device's position counter started from, in stream time:
    // zero, or where the last seek went. waveOutReset puts the counter back
    // to zero.
    std::uint64_t audio_base_ms = 0;
    while (reader != nullptr && device != nullptr) {
        AcquireSRWLockExclusive(&player.lock);
        const auto quit = player.quit;
        const auto seek = player.audio_seek_requested;
        const auto seek_to = player.audio_seek_to_ms;
        player.audio_seek_requested = false;
        const auto playing = player.started && !player.paused;
        ReleaseSRWLockExclusive(&player.lock);
        if (quit) {
            break;
        }
        if (seek) {
            waveOutReset(device);
            reclaim(true);
            PROPVARIANT position;
            PropVariantInit(&position);
            position.vt = VT_I8;
            position.hVal.QuadPart = static_cast<LONGLONG>(seek_to) * 10000;
            const GUID time_format = {};
            reader->SetCurrentPosition(time_format, position);
            PropVariantClear(&position);
            finished = false;
            audio_base_ms = seek_to;
            continue;
        }
        if (!playing) {
            if (!device_paused) {
                waveOutPause(device);
                device_paused = true;
            }
            Sleep(5);
            continue;
        }
        if (device_paused) {
            waveOutRestart(device);
            device_paused = false;
        }
        reclaim(false);
        // The picture follows the sound. The clock started when the title
        // pressed play, the sound some hundreds of milliseconds later - the
        // device opening, the first block decoding - and the intro's sound
        // stayed that far behind its picture to the end. While sound is
        // actually coming out, the clock is set to what the device has
        // played.
        if (!queued.empty()) {
            MMTIME position = {};
            position.wType = TIME_SAMPLES;
            if (waveOutGetPosition(device, &position, sizeof(position)) ==
                    MMSYSERR_NOERROR &&
                position.wType == TIME_SAMPLES && position.u.sample != 0) {
                const auto heard_ms = audio_base_ms +
                    static_cast<std::uint64_t>(position.u.sample) * 1000 /
                        rate;
                AcquireSRWLockExclusive(&player.lock);
                if (player.started && !player.paused &&
                    !player.audio_seek_requested) {
                    const auto shown_ms = playback_time(player);
                    const auto drift = static_cast<std::int64_t>(heard_ms) -
                        static_cast<std::int64_t>(shown_ms);
                    if (drift > 20 || drift < -20) {
                        restart_clock(player, heard_ms);
                        static std::atomic<std::uint32_t> shown{0};
                        if (shown.fetch_add(1, std::memory_order_relaxed) < 8) {
                            trace("clock_to_audio heard=%llu shown=%llu\n",
                                  static_cast<unsigned long long>(heard_ms),
                                  static_cast<unsigned long long>(shown_ms));
                        }
                    }
                }
                ReleaseSRWLockExclusive(&player.lock);
            }
        }
        if (finished || queued.size() >= 8) {
            Sleep(5);
            continue;
        }
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* sample = nullptr;
        const auto result = reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
            &stream_index, &flags, &timestamp, &sample);
        if (FAILED(result) ||
            (flags & (MF_SOURCE_READERF_ENDOFSTREAM |
                      MF_SOURCE_READERF_ERROR)) != 0) {
            finished = true;
        }
        if (sample == nullptr) {
            continue;
        }
        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)) &&
                length != 0) {
                auto* block = new AudioBuffer();
                block->bytes.assign(
                    reinterpret_cast<char*>(data),
                    reinterpret_cast<char*>(data) + length);
                buffer->Unlock();
                block->header.lpData = block->bytes.data();
                block->header.dwBufferLength = length;
                waveOutPrepareHeader(device, &block->header, sizeof(WAVEHDR));
                waveOutWrite(device, &block->header, sizeof(WAVEHDR));
                queued.push_back(block);
                written_bytes += length;
            }
            buffer->Release();
        }
        sample->Release();
    }
    if (device != nullptr) {
        waveOutReset(device);
        reclaim(true);
        waveOutClose(device);
    }
    trace("audio_end bytes=%llu\n",
          static_cast<unsigned long long>(written_bytes));
    release(reader);
    MFShutdown();
    CoUninitialize();
    return 0;
}

inline bool audio_disabled() {
    char value[4] = {};
    return GetEnvironmentVariableA("PS5RT_AVPLAYER_NO_AUDIO", value, 4) == 1 &&
        value[0] == '1';
}

// --- Reading the source ----------------------------------------------------

// Guest code when the title replaced the file reader; the host file
// otherwise. Called with no lock held.
inline bool read_source(Player& player, const std::string& guest_path) {
    player.file_bytes.clear();
    if (player.file_open != 0 && player.file_read_offset != 0 &&
        player.file_size != 0) {
        restore_guest_fs();
        auto* object = reinterpret_cast<void*>(player.file_object);
        if (reinterpret_cast<GuestOpen>(player.file_open)(
                object, guest_path.c_str()) < 0) {
            return false;
        }
        const auto size = reinterpret_cast<GuestSize>(player.file_size)(object);
        if (size == 0 || size > (1ull << 31)) {
            if (player.file_close != 0) {
                reinterpret_cast<GuestClose>(player.file_close)(object);
            }
            return false;
        }
        player.file_bytes.resize(static_cast<std::size_t>(size));
        std::uint64_t done = 0;
        while (done < size) {
            const auto chunk = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(size - done, 1u << 20));
            const auto got = reinterpret_cast<GuestReadOffset>(
                player.file_read_offset)(
                object, player.file_bytes.data() + done, done, chunk);
            if (got <= 0) {
                break;
            }
            done += static_cast<std::uint64_t>(got);
        }
        if (player.file_close != 0) {
            reinterpret_cast<GuestClose>(player.file_close)(object);
        }
        player.file_bytes.resize(static_cast<std::size_t>(done));
        return done != 0;
    }
    const auto host_path = map_ps5_path(guest_path.c_str());
    HANDLE file = CreateFileA(
        host_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        trace("source_missing guest=%s host=%s\n",
              guest_path.c_str(), host_path.c_str());
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (1ll << 31)) {
        CloseHandle(file);
        return false;
    }
    player.file_bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const auto ok = ReadFile(
        file,
        player.file_bytes.data(),
        static_cast<DWORD>(size.QuadPart),
        &read,
        nullptr);
    CloseHandle(file);
    if (!ok || read != static_cast<DWORD>(size.QuadPart)) {
        player.file_bytes.clear();
        return false;
    }
    return true;
}

// --- Frames to the title ---------------------------------------------------

// Guest texture memory for the frames, from the title's own allocator so
// its GPU can read it. Called with no lock held.
inline bool ensure_guest_buffers(Player& player, std::uint32_t size) {
    if (player.guest_buffers[0] != 0 && player.guest_buffer_size >= size) {
        return true;
    }
    for (std::uint32_t index = 0; index < kGuestFrameBuffers; ++index) {
        void* memory = nullptr;
        const auto allocator = player.allocate_texture != 0
            ? player.allocate_texture
            : player.allocate;
        if (allocator != 0) {
            restore_guest_fs();
            memory = reinterpret_cast<GuestAllocate>(allocator)(
                reinterpret_cast<void*>(player.allocator_object), 0x100, size);
        }
        if (memory == nullptr) {
            memory = VirtualAlloc(
                nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        }
        if (memory == nullptr) {
            return false;
        }
        player.guest_buffers[index] = reinterpret_cast<std::uint64_t>(memory);
        trace("frame_buffer index=%u data=0x%016llX size=%u\n",
              index,
              static_cast<unsigned long long>(player.guest_buffers[index]),
              size);
    }
    player.guest_buffer_size = size;
    return true;
}

// Takes the frame that is due, if one is. Returns whether the title gets a
// new frame; `stop` is set when the stream just ended and the title must
// hear about it.
inline bool take_frame(
    Player& player, VideoFrame& frame, bool& stop, bool& restarted) {
    stop = false;
    restarted = false;
    if (!player.started || player.paused || player.end_of_stream ||
        !player.probed || player.probe_failed) {
        return false;
    }
    if (player.frames.empty()) {
        if (!player.video_done) {
            ++player.pacing_empty;
            return false;
        }
        if (player.looping) {
            player.seek_requested = true;
            player.audio_seek_requested = true;
            player.seek_to_ms = 0;
            player.audio_seek_to_ms = 0;
            restart_clock(player, 0);
            WakeAllConditionVariable(&player.changed);
            restarted = true;
            return false;
        }
        // PS5RT_AVPLAYER_HOLD_END_MS, an experiment: hold the last frame
        // this long before the title hears the stream ended - whether what
        // follows the intro fails because loading has not caught up.
        static const std::uint64_t hold_ms = [] {
            const auto* value = std::getenv("PS5RT_AVPLAYER_HOLD_END_MS");
            return value == nullptr ? 0ULL : std::strtoull(value, nullptr, 0);
        }();
        if (hold_ms != 0) {
            const auto now_ms = GetTickCount64();
            if (player.drained_at_ms == 0) {
                player.drained_at_ms = now_ms;
                trace("end_held ms=%llu\n",
                      static_cast<unsigned long long>(hold_ms));
            }
            if (now_ms - player.drained_at_ms < hold_ms) {
                return false;
            }
        }
        player.end_of_stream = true;
        stop = true;
        return false;
    }
    const auto now = playback_time(player);
    if (player.frames.front().timestamp_ms > now) {
        ++player.pacing_early;
        return false;
    }
    // The latest frame that is due; older ones were missed.
    while (player.frames.size() > 1 &&
           player.frames[1].timestamp_ms <= now) {
        player.frames.pop_front();
        ++player.pacing_skipped;
    }
    frame = std::move(player.frames.front());
    player.frames.pop_front();
    WakeAllConditionVariable(&player.changed);
    return true;
}

inline std::int32_t get_video_data(
    std::uint64_t handle, std::uint64_t info, bool extended) {
    auto* player = find_player(handle);
    if (player == nullptr || info == 0) {
        return 0;
    }
    VideoFrame frame;
    bool stop = false;
    bool restarted = false;
    AcquireSRWLockExclusive(&player->lock);
    const auto have = take_frame(*player, frame, stop, restarted);
    const auto size = frame_bytes(*player);
    ++player->pacing_calls;
    if (have) {
        ++player->pacing_given;
        player->pacing_last_ts = frame.timestamp_ms;
    }
    if (player->pacing_since.QuadPart == 0) {
        QueryPerformanceCounter(&player->pacing_since);
    } else if (milliseconds_since(player->pacing_since) >= 500) {
        trace("pacing clock=%llu calls=%u given=%u empty=%u early=%u "
              "skipped=%u last_ts=%llu queued=%zu decoded=%llu dropped=%llu\n",
              static_cast<unsigned long long>(playback_time(*player)),
              player->pacing_calls, player->pacing_given,
              player->pacing_empty, player->pacing_early,
              player->pacing_skipped,
              static_cast<unsigned long long>(player->pacing_last_ts),
              player->frames.size(),
              static_cast<unsigned long long>(player->decoded_frames),
              static_cast<unsigned long long>(player->dropped_frames));
        QueryPerformanceCounter(&player->pacing_since);
        player->pacing_calls = player->pacing_given = 0;
        player->pacing_empty = player->pacing_early = 0;
        player->pacing_skipped = 0;
    }
    ReleaseSRWLockExclusive(&player->lock);
    if (stop) {
        trace("end_of_stream\n");
        notify(*player, kEventStateStop);
        return 0;
    }
    if (!have || frame.nv12.size() != size ||
        !ensure_guest_buffers(*player, size)) {
        return 0;
    }
    const auto buffer =
        player->guest_buffers[player->next_guest_buffer % kGuestFrameBuffers];
    ++player->next_guest_buffer;
    std::memcpy(reinterpret_cast<void*>(buffer), frame.nv12.data(), size);
    // The title samples this buffer as its video texture. The GPU runtime
    // re-reads a texture only when it is told the memory changed, and a
    // host memcpy tells it nothing: the intro stayed on its first frame.
    ps5rt_native_gpu_guest_written(buffer, size);

    const auto pitch = frame_pitch(*player);
    const auto rows = frame_rows(*player);
    const auto width16 = align_up(player->width, 16);
    auto* out = reinterpret_cast<std::uint8_t*>(info);
    std::memset(out, 0, extended ? 104 : 40);
    std::memcpy(out + 0, &buffer, 8);
    std::memcpy(out + 16, &frame.timestamp_ms, 8);
    std::memcpy(out + 24, &width16, 4);
    std::memcpy(out + 28, &rows, 4);
    const float aspect = 1.0f;
    std::memcpy(out + 32, &aspect, 4);
    if (extended) {
        const std::uint32_t crop_right = pitch - player->width;
        const std::uint32_t crop_bottom = rows - player->height;
        std::memcpy(out + 48, &crop_right, 4);
        std::memcpy(out + 56, &crop_bottom, 4);
        std::memcpy(out + 60, &pitch, 4);
        out[64] = 8;
        out[65] = 8;
    }
    // A diagnostic: PS5RT_AVPLAYER_DUMP=<path> writes the frame handed out
    // at about four seconds in, as the NV12 bytes the title receives.
    {
        static std::atomic<bool> dumped{false};
        char dump_path[512] = {};
        if (!dumped.load(std::memory_order_relaxed) &&
            frame.timestamp_ms >= 4000 &&
            GetEnvironmentVariableA(
                "PS5RT_AVPLAYER_DUMP", dump_path, sizeof(dump_path)) != 0 &&
            !dumped.exchange(true)) {
            HANDLE file = CreateFileA(
                dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(file, frame.nv12.data(), size, &written, nullptr);
                CloseHandle(file);
            }
            trace("frame_dumped ts=%llu path=%s pitch=%u rows=%u\n",
                  static_cast<unsigned long long>(frame.timestamp_ms),
                  dump_path, pitch, rows);
        }
    }
    static std::atomic<std::uint32_t> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 8) {
        trace("video_frame ts=%llu data=0x%016llX %ux%u pitch=%u ex=%d\n",
              static_cast<unsigned long long>(frame.timestamp_ms),
              static_cast<unsigned long long>(buffer),
              player->width,
              player->height,
              pitch,
              extended ? 1 : 0);
    }
    return 1;
}

// --- Events ----------------------------------------------------------------

// A guest thread: it runs with the title's TLS, so it may call the title's
// callback. It outlives nothing - Close marks the player closed and the
// thread leaves at its next wake, and the player is kept for it.
inline PS5RT_GUEST_ABI std::uint64_t event_thread_main(std::uint64_t argument) {
    auto& player = *reinterpret_cast<Player*>(argument);
    while (true) {
        AcquireSRWLockExclusive(&player.lock);
        while (!player.closed && player.events.empty()) {
            SleepConditionVariableSRW(&player.changed, &player.lock, INFINITE, 0);
        }
        if (player.closed) {
            ReleaseSRWLockExclusive(&player.lock);
            break;
        }
        const auto event = player.events.front();
        player.events.pop_front();
        ReleaseSRWLockExclusive(&player.lock);
        restore_guest_fs();
        reinterpret_cast<GuestEvent>(player.event_callback)(
            reinterpret_cast<void*>(player.event_object), event, 0, nullptr);
        trace("event id=%d\n", event);
    }
    return 0;
}

// --- The exports -----------------------------------------------------------

inline Player* create_player(
    std::uint64_t memory, std::uint64_t file, std::uint64_t event,
    bool auto_start) {
    auto* player = new Player();
    const auto* words = reinterpret_cast<const std::uint64_t*>(memory);
    player->allocator_object = words[0];
    player->allocate = words[1];
    player->allocate_texture = words[3];
    const auto* files = reinterpret_cast<const std::uint64_t*>(file);
    player->file_object = files[0];
    player->file_open = files[1];
    player->file_close = files[2];
    player->file_read_offset = files[3];
    player->file_size = files[4];
    const auto* events = reinterpret_cast<const std::uint64_t*>(event);
    player->event_object = events[0];
    player->event_callback = events[1];
    player->auto_start = auto_start;
    AcquireSRWLockExclusive(&g_players_lock);
    g_players[reinterpret_cast<std::uint64_t>(player)] = player;
    ReleaseSRWLockExclusive(&g_players_lock);
    if (player->event_callback != 0) {
        std::uint64_t thread_id = 0;
        player->event_thread_started = ps5rt_pthread_create_real(
            reinterpret_cast<std::uint64_t>(&thread_id),
            0,
            reinterpret_cast<std::uint64_t>(&event_thread_main),
            reinterpret_cast<std::uint64_t>(player)) == 0;
    }
    trace("init handle=0x%016llX alloc_texture=0x%016llX event=0x%016llX "
          "file_open=0x%016llX auto_start=%d\n",
          static_cast<unsigned long long>(reinterpret_cast<std::uint64_t>(player)),
          static_cast<unsigned long long>(player->allocate_texture),
          static_cast<unsigned long long>(player->event_callback),
          static_cast<unsigned long long>(player->file_open),
          auto_start ? 1 : 0);
    return player;
}

inline std::int32_t add_source(std::uint64_t handle, const std::string& path) {
    auto* player = find_player(handle);
    if (player == nullptr || path.empty()) {
        return kInvalidParameters;
    }
    if (player->worker != nullptr) {
        // A second source replaces the first.
        AcquireSRWLockExclusive(&player->lock);
        player->quit = true;
        WakeAllConditionVariable(&player->changed);
        ReleaseSRWLockExclusive(&player->lock);
        WaitForSingleObject(player->worker, INFINITE);
        CloseHandle(player->worker);
        player->worker = nullptr;
        if (player->audio_worker != nullptr) {
            WaitForSingleObject(player->audio_worker, INFINITE);
            CloseHandle(player->audio_worker);
            player->audio_worker = nullptr;
        }
    }
    if (!read_source(*player, path)) {
        restore_guest_fs();
        return kOperationFailed;
    }
    AcquireSRWLockExclusive(&player->lock);
    player->guest_path = path;
    player->has_source = true;
    player->probed = false;
    player->probe_failed = false;
    player->quit = false;
    player->video_done = false;
    player->end_of_stream = false;
    player->frames.clear();
    ReleaseSRWLockExclusive(&player->lock);
    player->worker = CreateThread(nullptr, 0, &decoder_main, player, 0, nullptr);
    // The title asks for the stream's shape as soon as it is told the
    // source is ready, so wait for the probe rather than report a picture
    // of no size.
    AcquireSRWLockExclusive(&player->lock);
    while (!player->probed) {
        SleepConditionVariableSRW(&player->changed, &player->lock, 5000, 0);
        if (!player->probed) {
            break;
        }
    }
    const auto failed = !player->probed || player->probe_failed;
    if (!failed && player->auto_start) {
        player->started = true;
        restart_clock(*player, 0);
    }
    ReleaseSRWLockExclusive(&player->lock);
    restore_guest_fs();
    trace("source path=%s bytes=%zu %ux%u fps=%.3f duration_ms=%llu "
          "audio=%d ch=%u rate=%u failed=%d\n",
          path.c_str(),
          player->file_bytes.size(),
          player->width,
          player->height,
          player->frames_per_second,
          static_cast<unsigned long long>(player->duration_ms),
          player->has_audio ? 1 : 0,
          player->audio_channels,
          player->audio_sample_rate,
          failed ? 1 : 0);
    if (failed) {
        return kOperationFailed;
    }
    if (player->has_audio && !audio_disabled()) {
        player->audio_worker =
            CreateThread(nullptr, 0, &audio_main, player, 0, nullptr);
    }
    notify(*player, kEventStateReady);
    if (player->auto_start) {
        notify(*player, kEventStatePlay);
    }
    return 0;
}

}  // namespace ps5rt_avplayer

extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_avplayer_init(std::uint64_t data) {
    using namespace ps5rt_avplayer;
    if (data == 0) {
        return 0;
    }
    const auto auto_start = reinterpret_cast<const std::uint8_t*>(data)[108] != 0;
    return reinterpret_cast<std::uint64_t>(
        create_player(data, data + 40, data + 80, auto_start));
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_init_ex(
    std::uint64_t data, std::uint64_t out_handle) {
    using namespace ps5rt_avplayer;
    if (data == 0 || out_handle == 0) {
        return kInvalidParameters;
    }
    const auto auto_start = reinterpret_cast<const std::uint8_t*>(data)[168] != 0;
    const auto handle = reinterpret_cast<std::uint64_t>(
        create_player(data + 8, data + 48, data + 88, auto_start));
    std::memcpy(reinterpret_cast<void*>(out_handle), &handle, 8);
    return 0;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_post_init(
    std::uint64_t handle, std::uint64_t) {
    return ps5rt_avplayer::find_player(handle) != nullptr
        ? 0 : ps5rt_avplayer::kInvalidParameters;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_add_source(
    std::uint64_t handle, std::uint64_t path) {
    std::string value;
    if (!read_process_c_string(path, 4096, value)) {
        return ps5rt_avplayer::kInvalidParameters;
    }
    return ps5rt_avplayer::add_source(handle, value);
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_add_source_ex(
    std::uint64_t handle, std::uint64_t uri_type, std::uint64_t details) {
    if (details == 0 || uri_type != 0) {
        return ps5rt_avplayer::kInvalidParameters;
    }
    std::uint64_t name = 0;
    std::uint32_t length = 0;
    std::memcpy(&name, reinterpret_cast<const void*>(details), 8);
    std::memcpy(&length, reinterpret_cast<const void*>(details + 8), 4);
    if (name == 0 || length == 0 || length > 4096) {
        return ps5rt_avplayer::kInvalidParameters;
    }
    std::string value(reinterpret_cast<const char*>(name), length);
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return ps5rt_avplayer::add_source(handle, value);
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_start(std::uint64_t handle) {
    using namespace ps5rt_avplayer;
    auto* player = find_player(handle);
    if (player == nullptr) {
        return kInvalidParameters;
    }
    AcquireSRWLockExclusive(&player->lock);
    const auto ready = player->has_source;
    if (ready) {
        player->started = true;
        player->paused = false;
        player->end_of_stream = false;
        restart_clock(*player, 0);
    }
    ReleaseSRWLockExclusive(&player->lock);
    if (!ready) {
        return kInvalidParameters;
    }
    trace("start handle=0x%016llX\n", static_cast<unsigned long long>(handle));
    notify(*player, kEventStatePlay);
    return 0;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_stop(std::uint64_t handle) {
    using namespace ps5rt_avplayer;
    trace("stop handle=0x%016llX\n", static_cast<unsigned long long>(handle));
    auto* player = find_player(handle);
    if (player == nullptr) {
        return kInvalidParameters;
    }
    AcquireSRWLockExclusive(&player->lock);
    player->started = false;
    player->paused = false;
    player->seek_requested = true;
    player->audio_seek_requested = true;
    player->seek_to_ms = 0;
    player->audio_seek_to_ms = 0;
    player->clock_offset_ms = 0;
    WakeAllConditionVariable(&player->changed);
    ReleaseSRWLockExclusive(&player->lock);
    notify(*player, kEventStateStop);
    return 0;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_pause(std::uint64_t handle) {
    using namespace ps5rt_avplayer;
    trace("pause handle=0x%016llX\n", static_cast<unsigned long long>(handle));
    auto* player = find_player(handle);
    if (player == nullptr) {
        return kInvalidParameters;
    }
    AcquireSRWLockExclusive(&player->lock);
    if (!player->paused) {
        player->paused_at_ms = playback_time(*player);
        player->paused = true;
    }
    ReleaseSRWLockExclusive(&player->lock);
    notify(*player, kEventStatePause);
    return 0;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_resume(std::uint64_t handle) {
    using namespace ps5rt_avplayer;
    trace("resume handle=0x%016llX\n", static_cast<unsigned long long>(handle));
    auto* player = find_player(handle);
    if (player == nullptr) {
        return kInvalidParameters;
    }
    AcquireSRWLockExclusive(&player->lock);
    if (player->paused) {
        player->paused = false;
        restart_clock(*player, player->paused_at_ms);
    }
    ReleaseSRWLockExclusive(&player->lock);
    notify(*player, kEventStatePlay);
    return 0;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_set_looping(
    std::uint64_t handle, std::uint64_t looping) {
    using namespace ps5rt_avplayer;
    auto* player = find_player(handle);
    if (player == nullptr) {
        return kInvalidParameters;
    }
    AcquireSRWLockExclusive(&player->lock);
    player->looping = (looping & 0xFF) != 0;
    ReleaseSRWLockExclusive(&player->lock);
    return 0;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_validate(std::uint64_t handle) {
    return ps5rt_avplayer::find_player(handle) != nullptr
        ? 0 : ps5rt_avplayer::kInvalidParameters;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_jump_to_time(
    std::uint64_t handle, std::uint64_t milliseconds) {
    using namespace ps5rt_avplayer;
    trace("jump handle=0x%016llX\n", static_cast<unsigned long long>(handle));
    auto* player = find_player(handle);
    if (player == nullptr) {
        return kInvalidParameters;
    }
    AcquireSRWLockExclusive(&player->lock);
    player->seek_requested = true;
    player->audio_seek_requested = true;
    player->seek_to_ms = milliseconds;
    player->audio_seek_to_ms = milliseconds;
    player->end_of_stream = false;
    restart_clock(*player, milliseconds);
    player->paused_at_ms = milliseconds;
    WakeAllConditionVariable(&player->changed);
    ReleaseSRWLockExclusive(&player->lock);
    return 0;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_is_active(std::uint64_t handle) {
    using namespace ps5rt_avplayer;
    auto* player = find_player(handle);
    if (player == nullptr) {
        return 0;
    }
    AcquireSRWLockExclusive(&player->lock);
    const auto active = player->has_source && !player->probe_failed &&
        !player->end_of_stream;
    ReleaseSRWLockExclusive(&player->lock);
    return active ? 1 : 0;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_get_video_data(
    std::uint64_t handle, std::uint64_t info) {
    return ps5rt_avplayer::get_video_data(handle, info, false);
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_get_video_data_ex(
    std::uint64_t handle, std::uint64_t info) {
    return ps5rt_avplayer::get_video_data(handle, info, true);
}

// The title does not import this - it takes no audio from the player - so
// it answers "nothing yet" rather than decode a stream nobody reads.
extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_get_audio_data(
    std::uint64_t, std::uint64_t) {
    return 0;
}

extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_avplayer_current_time(
    std::uint64_t handle) {
    using namespace ps5rt_avplayer;
    auto* player = find_player(handle);
    if (player == nullptr) {
        return 0;
    }
    AcquireSRWLockShared(&player->lock);
    const auto now = playback_time(*player);
    ReleaseSRWLockShared(&player->lock);
    return now;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_stream_count(
    std::uint64_t handle) {
    using namespace ps5rt_avplayer;
    auto* player = find_player(handle);
    if (player == nullptr) {
        return kInvalidParameters;
    }
    return player->has_audio ? 2 : 1;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_get_stream_info(
    std::uint64_t handle, std::uint64_t index, std::uint64_t info) {
    using namespace ps5rt_avplayer;
    auto* player = find_player(handle);
    const auto stream = static_cast<std::uint32_t>(index);
    if (player == nullptr || info == 0 ||
        stream > (player->has_audio ? 1u : 0u)) {
        return kInvalidParameters;
    }
    // Thirty-two bytes, not the forty of the PS4 layout: the title keeps
    // this in a 0x20 slot with its stack guard right above, and the last
    // eight bytes of a forty-byte clear landed on the guard - its event
    // thread died on __stack_chk_fail the moment the intro was ready. And
    // the video stream is type 1: the title looks for "Video on Stream"
    // with type 1 and enables it, and type 0 had it skip the stream.
    auto* out = reinterpret_cast<std::uint8_t*>(info);
    std::memset(out, 0, 32);
    const std::uint32_t type = stream == 0 ? 1u : 0u;
    std::memcpy(out + 0, &type, 4);
    if (stream == 0) {
        const float aspect = player->height == 0
            ? 1.0f
            : static_cast<float>(player->width) / static_cast<float>(player->height);
        std::memcpy(out + 8, &player->width, 4);
        std::memcpy(out + 12, &player->height, 4);
        std::memcpy(out + 16, &aspect, 4);
    } else {
        const auto channels = static_cast<std::uint16_t>(player->audio_channels);
        std::memcpy(out + 8, &channels, 2);
        std::memcpy(out + 12, &player->audio_sample_rate, 4);
    }
    std::memcpy(out + 24, &player->duration_ms, 8);
    return 0;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_close(std::uint64_t handle) {
    using namespace ps5rt_avplayer;
    auto* player = find_player(handle);
    if (player == nullptr) {
        return kInvalidParameters;
    }
    AcquireSRWLockExclusive(&g_players_lock);
    g_players.erase(handle);
    ReleaseSRWLockExclusive(&g_players_lock);
    if (player->worker != nullptr) {
        AcquireSRWLockExclusive(&player->lock);
        player->quit = true;
        WakeAllConditionVariable(&player->changed);
        ReleaseSRWLockExclusive(&player->lock);
        WaitForSingleObject(player->worker, INFINITE);
        CloseHandle(player->worker);
        if (player->audio_worker != nullptr) {
            WaitForSingleObject(player->audio_worker, INFINITE);
            CloseHandle(player->audio_worker);
            player->audio_worker = nullptr;
        }
    }
    // The guest frame buffers belong to the title's allocator and the
    // title may still be drawing the last one; they are left to it. The
    // player itself stays for its event thread, which leaves on this.
    AcquireSRWLockExclusive(&player->lock);
    player->closed = true;
    player->frames.clear();
    player->frames.shrink_to_fit();
    player->file_bytes.clear();
    player->file_bytes.shrink_to_fit();
    WakeAllConditionVariable(&player->changed);
    ReleaseSRWLockExclusive(&player->lock);
    restore_guest_fs();
    trace("close handle=0x%016llX\n", static_cast<unsigned long long>(handle));
    return 0;
}

extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_avplayer_ok() {
    return 0;
}

// Which handler a libSceAvPlayer NID goes to, or null for one this does
// not implement. PS5RT_AVPLAYER_BRIDGE=1 leaves them all to the bridge.
extern "C" void* ps5rt_avplayer_handler(const char* nid, std::size_t length) {
    static const bool bridge = [] {
        char value[4] = {};
        return GetEnvironmentVariableA("PS5RT_AVPLAYER_BRIDGE", value, 4) == 1 &&
            value[0] == '1';
    }();
    if (bridge) {
        return nullptr;
    }
    const std::string_view name(nid, length);
    struct Entry {
        std::string_view nid;
        void* handler;
    };
    static const Entry entries[] = {
        {"aS66RI0gGgo", reinterpret_cast<void*>(&ps5rt_avplayer_init)},
        {"o9eWRkSL+M4", reinterpret_cast<void*>(&ps5rt_avplayer_init_ex)},
        {"HD1YKVU26-M", reinterpret_cast<void*>(&ps5rt_avplayer_post_init)},
        {"KMcEa+rHsIo", reinterpret_cast<void*>(&ps5rt_avplayer_add_source)},
        {"x8uvuFOPZhU", reinterpret_cast<void*>(&ps5rt_avplayer_add_source_ex)},
        {"ET4Gr-Uu07s", reinterpret_cast<void*>(&ps5rt_avplayer_start)},
        {"ZC17w3vB5Lo", reinterpret_cast<void*>(&ps5rt_avplayer_stop)},
        {"9y5v+fGN4Wk", reinterpret_cast<void*>(&ps5rt_avplayer_pause)},
        {"w5moABNwnRY", reinterpret_cast<void*>(&ps5rt_avplayer_resume)},
        {"OVths0xGfho", reinterpret_cast<void*>(&ps5rt_avplayer_set_looping)},
        {"ODJK2sn9w4A", reinterpret_cast<void*>(&ps5rt_avplayer_validate)},
        {"k-q+xOxdc3E", reinterpret_cast<void*>(&ps5rt_avplayer_validate)},
        {"XC9wM+xULz8", reinterpret_cast<void*>(&ps5rt_avplayer_jump_to_time)},
        {"UbQoYawOsfY", reinterpret_cast<void*>(&ps5rt_avplayer_is_active)},
        {"o3+RWnHViSg", reinterpret_cast<void*>(&ps5rt_avplayer_get_video_data)},
        {"JdksQu8pNdQ", reinterpret_cast<void*>(&ps5rt_avplayer_get_video_data_ex)},
        {"Wnp1OVcrZgk", reinterpret_cast<void*>(&ps5rt_avplayer_get_audio_data)},
        {"wwM99gjFf1Y", reinterpret_cast<void*>(&ps5rt_avplayer_current_time)},
        {"hdTyRzCXQeQ", reinterpret_cast<void*>(&ps5rt_avplayer_stream_count)},
        {"d8FcbzfAdQw", reinterpret_cast<void*>(&ps5rt_avplayer_get_stream_info)},
        {"NkJwDzKmIlw", reinterpret_cast<void*>(&ps5rt_avplayer_close)},
        {"yN7Jhuv8g24", reinterpret_cast<void*>(&ps5rt_avplayer_ok)},
    };
    for (const auto& entry : entries) {
        if (entry.nid == name) {
            return entry.handler;
        }
    }
    return nullptr;
}

#endif  // PS5RT_AVPLAYER_H
