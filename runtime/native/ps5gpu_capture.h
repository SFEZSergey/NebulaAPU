// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include "ps5gpu_native_api.h"

#include <stdint.h>

#define PS5GPU_CAPTURE_VERSION_LEGACY 1u
#define PS5GPU_CAPTURE_VERSION_SHADER_ONLY 2u
#define PS5GPU_CAPTURE_VERSION_GRAPHICS 3u
#define PS5GPU_CAPTURE_VERSION_COMPUTE 4u
/* Version 5 records flips in the command stream instead of a single trailing
   flip, so a capture can carry several frames. Everything that only ever
   renders on frame one - a pipeline bound to a state id that gets renumbered,
   for instance - is invisible to a one-frame capture, and chasing it meant a
   four-minute probe per frame. */
#define PS5GPU_CAPTURE_VERSION_MULTIFLIP 5u
#define PS5GPU_CAPTURE_VERSION PS5GPU_CAPTURE_VERSION_MULTIFLIP
#define PS5GPU_CAPTURE_MAGIC 0x5041435550473550ULL

typedef struct Ps5GpuCaptureHeaderV1 {
    uint64_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t draw_size;
    uint32_t flip_size;
    uint64_t draw_count;
    uint64_t reserved0;
} Ps5GpuCaptureHeaderV1;

typedef struct Ps5GpuCaptureHeaderV2 {
    uint64_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t draw_size;
    uint32_t flip_size;
    uint64_t draw_count;
    uint64_t reserved0;
    uint32_t shader_record_size;
    uint32_t shader_count;
    uint64_t shader_bytes;
    uint64_t reserved1;
} Ps5GpuCaptureHeaderV2;

typedef struct Ps5GpuCaptureHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t draw_size;
    uint32_t flip_size;
    uint64_t draw_count;
    uint64_t reserved0;
    uint32_t shader_record_size;
    uint32_t shader_count;
    uint64_t shader_bytes;
    uint64_t reserved1;
    uint32_t state_record_size;
    uint32_t state_count;
    uint64_t state_register_bytes;
    uint32_t memory_record_size;
    uint32_t memory_count;
    uint64_t memory_bytes;
} Ps5GpuCaptureHeader;

typedef struct Ps5GpuCaptureHeaderV4 {
    Ps5GpuCaptureHeader base;
    uint32_t compute_state_record_size;
    uint32_t compute_state_count;
    uint64_t compute_state_payload_bytes;
    uint32_t command_record_size;
    uint32_t command_count;
    uint64_t command_bytes;
} Ps5GpuCaptureHeaderV4;

#define PS5GPU_CAPTURE_SHADER_STAGE_EXPORT 0u
#define PS5GPU_CAPTURE_SHADER_STAGE_PIXEL 1u

#define PS5GPU_CAPTURE_SHADER_TERMINATED (1u << 0)
#define PS5GPU_CAPTURE_SHADER_READ_FAILED (1u << 1)
#define PS5GPU_CAPTURE_SHADER_DECODE_FAILED (1u << 2)

typedef struct Ps5GpuCaptureShader {
    uint64_t address;
    uint64_t hash;
    uint32_t stage;
    uint32_t flags;
    uint32_t byte_size;
    uint32_t instruction_count;
    uint32_t encoding_mask;
    uint32_t failure_pc;
    uint32_t failure_word;
    uint32_t reserved0;
} Ps5GpuCaptureShader;

typedef struct Ps5GpuCaptureShaderState {
    uint32_t state_id;
    uint32_t flags;
    uint64_t hash;
    uint64_t es_address;
    uint64_t ps_address;
    uint64_t es_header_address;
    uint64_t ps_header_address;
    uint32_t export_user_data_base_register;
    uint32_t pixel_user_data_base_register;
    uint32_t pixel_input_enable;
    uint32_t pixel_input_address;
    uint32_t sh_register_count;
    uint32_t cx_register_count;
    uint64_t reserved0;
} Ps5GpuCaptureShaderState;

#define PS5GPU_CAPTURE_MEMORY_SHADER_METADATA (1u << 0)
#define PS5GPU_CAPTURE_MEMORY_RESOURCE_TABLE (1u << 1)
#define PS5GPU_CAPTURE_MEMORY_IMAGE_RESOURCE (1u << 2)
#define PS5GPU_CAPTURE_MEMORY_BUFFER_RESOURCE (1u << 3)

typedef struct Ps5GpuCaptureMemory {
    uint64_t address;
    uint64_t hash;
    uint32_t byte_size;
    uint32_t flags;
} Ps5GpuCaptureMemory;

typedef struct Ps5GpuCaptureComputeState {
    uint32_t state_id;
    uint32_t flags;
    uint64_t state_hash;
    uint64_t shader_address;
    uint64_t shader_header_address;
    uint32_t spirv_size;
    uint32_t resource_manifest_size;
} Ps5GpuCaptureComputeState;

#define PS5GPU_CAPTURE_COMMAND_DRAW 1u
#define PS5GPU_CAPTURE_COMMAND_COMPUTE 2u
#define PS5GPU_CAPTURE_COMMAND_FLIP 3u

// After the flip, optionally: the modules of the graphics states, which the
// state records above do not carry. A state the title's translator compiled
// during the run has no other source, and replay drew none of them. The
// trailer is the magic, a count, and that many records each followed by its
// vertex SPIR-V, vertex manifest, pixel SPIR-V and pixel manifest. A reader
// that does not know it stops at the flip.
#define PS5GPU_CAPTURE_GRAPHICS_PAYLOAD_MAGIC 0x44415950u /* "PYAD" */

typedef struct Ps5GpuCaptureGraphicsPayload {
    uint32_t state_id;
    uint32_t es_spirv_size;
    uint32_t es_manifest_size;
    uint32_t ps_spirv_size;
    uint32_t ps_manifest_size;
    uint32_t reserved0;
} Ps5GpuCaptureGraphicsPayload;

typedef struct Ps5GpuCaptureCommand {
    uint32_t type;
    uint32_t payload_size;
    union {
        Ps5GpuNativeDraw draw;
        Ps5GpuNativeComputeDispatch compute;
        Ps5GpuNativeFlip flip;
    } payload;
} Ps5GpuCaptureCommand;
