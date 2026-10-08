// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <stdint.h>

#if defined(_WIN32)
#if defined(PS5RT_BUILD_DLL)
#define PS5RT_API __declspec(dllexport)
#else
#define PS5RT_API __declspec(dllimport)
#endif
#else
#define PS5RT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PS5RT_ABI_VERSION 0x00010000u

typedef struct Ps5RtRuntime Ps5RtRuntime;

typedef enum Ps5RtResult {
    PS5RT_OK = 0,
    PS5RT_ERROR_INVALID_ARGUMENT = 1,
    PS5RT_ERROR_ABI_MISMATCH = 2,
    PS5RT_ERROR_BACKEND_UNAVAILABLE = 3,
    PS5RT_ERROR_BACKEND_INITIALIZATION = 4,
} Ps5RtResult;

typedef enum Ps5RtGpuBackend {
    PS5RT_GPU_AUTO = 0,
    PS5RT_GPU_D3D12 = 1,
    PS5RT_GPU_VULKAN = 2,
    PS5RT_GPU_NONE = 3,
} Ps5RtGpuBackend;

typedef enum Ps5RtUpscaler {
    PS5RT_UPSCALER_NATIVE = 0,
    PS5RT_UPSCALER_FSR = 1,
    PS5RT_UPSCALER_DLSS = 2,
    PS5RT_UPSCALER_AUTO = 3,
} Ps5RtUpscaler;

enum {
    PS5RT_CREATE_HEADLESS = 1u << 0,
};

typedef struct Ps5RtCreateInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    Ps5RtGpuBackend gpu_backend;
    Ps5RtUpscaler upscaler;
    uint32_t flags;
} Ps5RtCreateInfo;

typedef struct Ps5RtGpuInfo {
    uint32_t struct_size;
    Ps5RtGpuBackend active_backend;
    Ps5RtUpscaler active_upscaler;
    uint32_t feature_level;
    uint64_t dedicated_video_memory;
    char adapter_name[128];
} Ps5RtGpuInfo;

PS5RT_API uint32_t ps5rt_get_abi_version(void);
PS5RT_API const char* ps5rt_get_backend_name(Ps5RtGpuBackend backend);
PS5RT_API const char* ps5rt_get_result_name(Ps5RtResult result);

PS5RT_API Ps5RtResult ps5rt_create(
    const Ps5RtCreateInfo* create_info,
    Ps5RtRuntime** runtime);

PS5RT_API void ps5rt_destroy(Ps5RtRuntime* runtime);

PS5RT_API Ps5RtResult ps5rt_get_gpu_info(
    const Ps5RtRuntime* runtime,
    Ps5RtGpuInfo* gpu_info);

#ifdef __cplusplus
}
#endif
