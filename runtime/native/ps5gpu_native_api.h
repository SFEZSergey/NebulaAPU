// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <stdint.h>

#ifdef _WIN32
#define PS5GPU_NATIVE_CALL __cdecl
#if defined(PS5GPU_NATIVE_BUILD)
#define PS5GPU_NATIVE_API __declspec(dllexport)
#else
#define PS5GPU_NATIVE_API
#endif
#else
#define PS5GPU_NATIVE_CALL
#define PS5GPU_NATIVE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PS5GPU_NATIVE_ABI_VERSION 0x00010005u

#define PS5GPU_NATIVE_DRAW_COUNT_KNOWN (1u << 0)
#define PS5GPU_NATIVE_DRAW_INDEXED (1u << 1)
#define PS5GPU_NATIVE_DRAW_TARGET_KNOWN (1u << 2)
#define PS5GPU_NATIVE_DRAW_VIEWPORT_KNOWN (1u << 3)
#define PS5GPU_NATIVE_DRAW_SCISSOR_KNOWN (1u << 4)
#define PS5GPU_NATIVE_DRAW_SCISSOR_FULL (1u << 5)
#define PS5GPU_NATIVE_DRAW_SHADER_STATE_KNOWN (1u << 6)
#define PS5GPU_NATIVE_DRAW_MASKS_KNOWN (1u << 7)
/* The target was fast cleared before this draw: fill it with clear_word0/1
   (CB_COLOR0_CLEAR_WORD0/1, in the target's own format) first. */
#define PS5GPU_NATIVE_DRAW_CLEAR_FIRST (1u << 8)

#define PS5GPU_NATIVE_SHADER_STATE_HAS_EXPORT_HEADER (1u << 0)
#define PS5GPU_NATIVE_SHADER_STATE_HAS_PIXEL_HEADER (1u << 1)

typedef enum Ps5GpuNativeResult {
    PS5GPU_NATIVE_OK = 0,
    PS5GPU_NATIVE_ERROR_INVALID_ARGUMENT = 1,
    PS5GPU_NATIVE_ERROR_ABI_MISMATCH = 2,
    PS5GPU_NATIVE_ERROR_OUT_OF_MEMORY = 3,
    PS5GPU_NATIVE_ERROR_QUEUE_FULL = 4,
    PS5GPU_NATIVE_ERROR_TIMEOUT = 5,
    PS5GPU_NATIVE_ERROR_STOPPED = 6,
    PS5GPU_NATIVE_ERROR_INTERNAL = 7,
} Ps5GpuNativeResult;

typedef void* Ps5GpuNativeHandle;

typedef struct Ps5GpuNativeCreateInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t flags;
    uint32_t queue_capacity;
} Ps5GpuNativeCreateInfo;

typedef struct Ps5GpuNativeRegisterValue {
    uint32_t address;
    uint32_t value;
} Ps5GpuNativeRegisterValue;

typedef struct Ps5GpuNativeShaderState {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t es_address;
    uint64_t ps_address;
    uint64_t es_header_address;
    uint64_t ps_header_address;
    uint32_t export_user_data_base_register;
    uint32_t pixel_user_data_base_register;
    uint32_t pixel_input_enable;
    uint32_t pixel_input_address;
    const Ps5GpuNativeRegisterValue* sh_registers;
    uint32_t sh_register_count;
    uint32_t reserved1;
    const Ps5GpuNativeRegisterValue* cx_registers;
    uint32_t cx_register_count;
    uint32_t reserved2;
    const uint8_t* es_spirv;
    uint32_t es_spirv_size;
    uint32_t reserved3;
    const uint8_t* es_resource_manifest;
    uint32_t es_resource_manifest_size;
    uint32_t reserved4;
    const uint8_t* ps_spirv;
    uint32_t ps_spirv_size;
    uint32_t reserved5;
    const uint8_t* ps_resource_manifest;
    uint32_t ps_resource_manifest_size;
    uint32_t reserved6;
} Ps5GpuNativeShaderState;

typedef struct Ps5GpuNativeDraw {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t submission_id;
    uint64_t draw_id;
    uint64_t total_draw_id;
    uint32_t packet_offset_dwords;
    uint32_t packet_opcode;
    uint32_t packet_register;
    uint32_t flags;
    uint32_t vertex_count;
    uint32_t primitive_type;
    uint32_t render_target_count;
    uint32_t render_target_slot;
    uint64_t es_address;
    uint64_t ps_address;
    uint64_t render_target_address;
    uint32_t render_target_width;
    uint32_t render_target_height;
    uint32_t render_target_format;
    uint32_t render_target_number_type;
    uint32_t render_target_tile_mode;
    uint32_t reserved0;
    float viewport_x;
    float viewport_y;
    float viewport_width;
    float viewport_height;
    float viewport_min_depth;
    float viewport_max_depth;
    int32_t scissor_x;
    int32_t scissor_y;
    uint32_t scissor_width;
    uint32_t scissor_height;
    /* GE_INDX_OFFSET: the vertex this draw starts at. Recompiled vertex
       shaders index their vertex buffer with gl_VertexIndex, so a draw that
       starts part way through the buffer reads the wrong vertices without it.
       Appended rather than folded into reserved0, which despite its name
       carries the shader state id. Growing the struct invalidates captures
       written by older builds; the replay tool rejects them outright rather
       than reading them wrong. */
    uint32_t first_vertex;
    /* CB_TARGET_MASK and CB_SHADER_MASK, four bits per render target: which
       channels the target accepts, and which the pixel shader exports. The
       guest could not write the target mask at all until the register
       defaults descriptor was answered, so every draw arrived here claiming
       all four channels. */
    uint32_t target_mask;
    uint32_t shader_mask;
    uint32_t reserved1;
    /* Where this draw's indices are and how wide they are. Every geometry
       draw this title issues is indexed, and without these the draw was
       issued as if the vertices were sequential - 316 of them left the
       scene target byte for byte identical to a run that drew none.
       Zero means the draw is not indexed, or its base was never seen. */
    uint64_t index_address;
    uint32_t index_bytes;
    uint32_t index_max;
    /* The fast clear's value, for PS5GPU_NATIVE_DRAW_CLEAR_FIRST. */
    uint32_t clear_word0;
    uint32_t clear_word1;
} Ps5GpuNativeDraw;

typedef struct Ps5GpuNativeComputeState {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t state_hash;
    uint64_t shader_address;
    uint64_t shader_header_address;
    const uint8_t* spirv;
    uint32_t spirv_size;
    uint32_t reserved1;
    const uint8_t* resource_manifest;
    uint32_t resource_manifest_size;
    uint32_t reserved2;
} Ps5GpuNativeComputeState;

typedef struct Ps5GpuNativeComputeDispatch {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t submission_id;
    uint64_t dispatch_id;
    uint32_t owner_handle;
    uint32_t packet_offset_dwords;
    uint32_t packet_opcode;
    uint32_t flags;
    uint32_t compute_state_id;
    uint32_t base_group_x;
    uint32_t base_group_y;
    uint32_t base_group_z;
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
    uint32_t local_size_x;
    uint32_t local_size_y;
    uint32_t local_size_z;
    uint32_t thread_count_x;
    uint32_t thread_count_y;
    uint32_t thread_count_z;
    uint32_t reserved0;
} Ps5GpuNativeComputeDispatch;

typedef struct Ps5GpuNativeFlip {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t flip_id;
    uint64_t display_address;
    uint64_t flip_argument;
    uint64_t pixel_format;
    uint32_t video_handle;
    int32_t buffer_index;
    uint32_t flip_mode;
    uint32_t tiling_mode;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_in_pixels;
    uint32_t reserved0;
} Ps5GpuNativeFlip;

typedef struct Ps5GpuNativeStats {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t draws_submitted;
    uint64_t draws_processed;
    uint64_t draws_dropped;
    uint64_t compute_dispatches_submitted;
    uint64_t compute_dispatches_processed;
    uint64_t compute_dispatches_dropped;
    uint64_t flips_submitted;
    uint64_t flips_processed;
    uint64_t flips_dropped;
    uint64_t queue_depth;
    uint64_t queue_high_watermark;
    uint64_t commands_in_flight;
    uint32_t worker_running;
    uint32_t reserved0;
} Ps5GpuNativeStats;

PS5GPU_NATIVE_API uint32_t PS5GPU_NATIVE_CALL
ps5gpu_native_get_abi_version(void);
PS5GPU_NATIVE_API Ps5GpuNativeResult PS5GPU_NATIVE_CALL
ps5gpu_native_create(
    const Ps5GpuNativeCreateInfo* create_info,
    Ps5GpuNativeHandle* handle);
PS5GPU_NATIVE_API void PS5GPU_NATIVE_CALL
ps5gpu_native_destroy(Ps5GpuNativeHandle handle);
PS5GPU_NATIVE_API Ps5GpuNativeResult PS5GPU_NATIVE_CALL
ps5gpu_native_register_shader_state(
    Ps5GpuNativeHandle handle,
    const Ps5GpuNativeShaderState* state,
    uint32_t* state_id);
PS5GPU_NATIVE_API Ps5GpuNativeResult PS5GPU_NATIVE_CALL
ps5gpu_native_register_compute_state(
    Ps5GpuNativeHandle handle,
    const Ps5GpuNativeComputeState* state,
    uint32_t* state_id);
PS5GPU_NATIVE_API Ps5GpuNativeResult PS5GPU_NATIVE_CALL
ps5gpu_native_submit_draw(
    Ps5GpuNativeHandle handle,
    const Ps5GpuNativeDraw* draw);
PS5GPU_NATIVE_API Ps5GpuNativeResult PS5GPU_NATIVE_CALL
ps5gpu_native_submit_compute(
    Ps5GpuNativeHandle handle,
    const Ps5GpuNativeComputeDispatch* dispatch);
PS5GPU_NATIVE_API Ps5GpuNativeResult PS5GPU_NATIVE_CALL
ps5gpu_native_submit_flip(
    Ps5GpuNativeHandle handle,
    const Ps5GpuNativeFlip* flip);
PS5GPU_NATIVE_API Ps5GpuNativeResult PS5GPU_NATIVE_CALL
ps5gpu_native_flush(Ps5GpuNativeHandle handle, uint32_t timeout_ms);
PS5GPU_NATIVE_API Ps5GpuNativeResult PS5GPU_NATIVE_CALL
ps5gpu_native_get_stats(
    Ps5GpuNativeHandle handle,
    Ps5GpuNativeStats* stats);
/* Guest memory is about to be written by something other than a guest
   store - a DMA packet the runtime executes. The write watch only sees
   faulting stores, so without this a texture filled that way is never
   uploaded again. Optional: a runtime without it is loaded all the same. */
PS5GPU_NATIVE_API void PS5GPU_NATIVE_CALL
ps5gpu_native_guest_memory_written(uint64_t address, uint64_t size);
typedef void (PS5GPU_NATIVE_CALL* Ps5GpuNativeGuestMemoryWritten)(
    uint64_t, uint64_t);

typedef uint32_t (PS5GPU_NATIVE_CALL* Ps5GpuNativeGetAbiVersion)(void);
typedef Ps5GpuNativeResult (PS5GPU_NATIVE_CALL* Ps5GpuNativeCreate)(
    const Ps5GpuNativeCreateInfo*, Ps5GpuNativeHandle*);
typedef void (PS5GPU_NATIVE_CALL* Ps5GpuNativeDestroy)(
    Ps5GpuNativeHandle);
typedef Ps5GpuNativeResult (
    PS5GPU_NATIVE_CALL* Ps5GpuNativeRegisterShaderState)(
    Ps5GpuNativeHandle, const Ps5GpuNativeShaderState*, uint32_t*);
typedef Ps5GpuNativeResult (
    PS5GPU_NATIVE_CALL* Ps5GpuNativeRegisterComputeState)(
    Ps5GpuNativeHandle, const Ps5GpuNativeComputeState*, uint32_t*);
typedef Ps5GpuNativeResult (PS5GPU_NATIVE_CALL* Ps5GpuNativeSubmitDraw)(
    Ps5GpuNativeHandle, const Ps5GpuNativeDraw*);
typedef Ps5GpuNativeResult (
    PS5GPU_NATIVE_CALL* Ps5GpuNativeSubmitCompute)(
    Ps5GpuNativeHandle, const Ps5GpuNativeComputeDispatch*);
typedef Ps5GpuNativeResult (PS5GPU_NATIVE_CALL* Ps5GpuNativeSubmitFlip)(
    Ps5GpuNativeHandle, const Ps5GpuNativeFlip*);
typedef Ps5GpuNativeResult (PS5GPU_NATIVE_CALL* Ps5GpuNativeFlush)(
    Ps5GpuNativeHandle, uint32_t);
typedef Ps5GpuNativeResult (PS5GPU_NATIVE_CALL* Ps5GpuNativeGetStats)(
    Ps5GpuNativeHandle, Ps5GpuNativeStats*);

#ifdef __cplusplus
}
#endif
