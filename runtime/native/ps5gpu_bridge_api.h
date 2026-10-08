// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS5GPU_ABI_VERSION 0x00010001u
#define PS5GPU_COMPILE_RESOLVE_VERTEX_INPUTS (1u << 0)

#define PS5GPU_RESOURCE_MANIFEST_MAGIC 0x4D524750u
#define PS5GPU_RESOURCE_MANIFEST_VERSION 1u

#define PS5GPU_RESOURCE_BUFFER_WRITABLE (1u << 0)
#define PS5GPU_RESOURCE_BUFFER_WRITE_BACK (1u << 1)

#define PS5GPU_RESOURCE_IMAGE_STORAGE (1u << 0)
#define PS5GPU_RESOURCE_IMAGE_HAS_SAMPLER (1u << 1)
#define PS5GPU_RESOURCE_IMAGE_VALID_EXTENT (1u << 2)

typedef enum Ps5GpuShaderStage {
    PS5GPU_STAGE_VERTEX = 0,
    PS5GPU_STAGE_PIXEL = 1,
    PS5GPU_STAGE_COMPUTE = 2,
} Ps5GpuShaderStage;

typedef enum Ps5GpuResult {
    PS5GPU_OK = 0,
    PS5GPU_ERROR_INVALID_ARGUMENT = 1,
    PS5GPU_ERROR_STATE_DECODE = 2,
    PS5GPU_ERROR_EVALUATION = 3,
    PS5GPU_ERROR_SPIRV_COMPILATION = 4,
    PS5GPU_ERROR_OUT_OF_MEMORY = 5,
    PS5GPU_ERROR_INTERNAL = 6,
} Ps5GpuResult;

typedef struct Ps5GpuRegisterValue {
    uint32_t register_address;
    uint32_t value;
} Ps5GpuRegisterValue;

typedef struct Ps5GpuPixelOutput {
    uint32_t guest_slot;
    uint32_t host_location;
    uint32_t kind;
} Ps5GpuPixelOutput;

typedef struct Ps5GpuShaderRequest {
    uint32_t struct_size;
    uint32_t abi_version;
    Ps5GpuShaderStage stage;
    uint32_t flags;
    uint64_t shader_address;
    uint64_t shader_header_address;
    const Ps5GpuRegisterValue* registers;
    uint32_t register_count;
    uint32_t user_data_base_register;
    uint32_t user_data_scalar_register_base;
    uint32_t local_size_x;
    uint32_t local_size_y;
    uint32_t local_size_z;
    uint32_t wave_lane_count;
    uint64_t storage_buffer_offset_alignment;
    const Ps5GpuPixelOutput* pixel_outputs;
    uint32_t pixel_output_count;
    uint32_t pixel_input_enable;
    uint32_t pixel_input_address;
    int32_t global_buffer_base;
    int32_t total_global_buffer_count;
    int32_t image_binding_base;
    int32_t initial_scalar_buffer_index;
    int32_t required_vertex_output_count;
    int32_t compute_work_group_x_register;
    int32_t compute_work_group_y_register;
    int32_t compute_work_group_z_register;
    int32_t compute_thread_group_size_register;
} Ps5GpuShaderRequest;

typedef struct Ps5GpuShaderResult {
    uint32_t struct_size;
    Ps5GpuResult status;
    uint8_t* spirv;
    uint32_t spirv_size;
    uint32_t attribute_count;
    uint32_t global_memory_binding_count;
    uint32_t image_binding_count;
    uint32_t vertex_input_count;
    uint8_t* resource_manifest;
    uint32_t resource_manifest_size;
} Ps5GpuShaderResult;

#pragma pack(push, 1)

typedef struct Ps5GpuResourceManifestHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t total_size;
    uint32_t stage;
    uint32_t global_count;
    uint32_t image_count;
    uint32_t vertex_count;
    uint32_t global_offset;
    uint32_t image_offset;
    uint32_t vertex_offset;
    uint32_t data_offset;
    uint32_t reserved[4];
} Ps5GpuResourceManifestHeader;

typedef struct Ps5GpuResourceGlobal {
    uint32_t descriptor_index;
    uint32_t scalar_address;
    uint64_t base_address;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t flags;
    uint32_t reserved;
} Ps5GpuResourceGlobal;

typedef struct Ps5GpuResourceImage {
    uint32_t binding;
    uint32_t pc;
    uint32_t flags;
    uint32_t mip_level;
    uint64_t base_address;
    uint32_t width;
    uint32_t height;
    uint32_t resource_descriptor[8];
    uint32_t sampler_descriptor[4];
} Ps5GpuResourceImage;

typedef struct Ps5GpuResourceVertexInput {
    uint32_t pc;
    uint32_t location;
    uint32_t component_count;
    uint32_t data_format;
    uint32_t number_format;
    uint32_t stride;
    uint32_t offset_bytes;
    uint32_t data_size;
    uint64_t base_address;
    uint32_t data_offset;
    uint32_t flags;
} Ps5GpuResourceVertexInput;

#pragma pack(pop)

typedef uint32_t (__cdecl* Ps5GpuGetAbiVersion)(void);
typedef Ps5GpuResult (__cdecl* Ps5GpuCompileSpirv)(
    const Ps5GpuShaderRequest* request,
    Ps5GpuShaderResult* result,
    char* error_buffer,
    uint32_t error_buffer_size);
typedef void (__cdecl* Ps5GpuFree)(void* allocation);

#ifdef __cplusplus
}
#endif
