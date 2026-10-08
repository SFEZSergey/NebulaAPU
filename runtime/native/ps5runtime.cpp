// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstring>
#include <new>

#include "ps5rt_api.h"

struct Ps5RtRuntime {
    IDXGIFactory6* dxgi_factory = nullptr;
    IDXGIAdapter1* dxgi_adapter = nullptr;
    ID3D12Device* d3d12_device = nullptr;
    Ps5RtGpuInfo gpu_info{};
};

namespace {

template <typename T>
void release_com(T*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

void copy_adapter_name(char (&destination)[128], const wchar_t* source) {
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        source,
        -1,
        destination,
        static_cast<int>(sizeof(destination)),
        nullptr,
        nullptr);
    if (converted == 0) {
        destination[0] = '\0';
    } else {
        destination[sizeof(destination) - 1] = '\0';
    }
}

bool try_create_d3d12_device(Ps5RtRuntime& runtime) {
    HRESULT result = CreateDXGIFactory2(
        0,
        __uuidof(IDXGIFactory6),
        reinterpret_cast<void**>(&runtime.dxgi_factory));
    if (FAILED(result)) {
        return false;
    }

    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        result = runtime.dxgi_factory->EnumAdapterByGpuPreference(
            index,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            __uuidof(IDXGIAdapter1),
            reinterpret_cast<void**>(&adapter));
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result) || adapter == nullptr) {
            continue;
        }

        DXGI_ADAPTER_DESC1 descriptor{};
        if (FAILED(adapter->GetDesc1(&descriptor)) ||
            (descriptor.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            adapter->Release();
            continue;
        }

        ID3D12Device* device = nullptr;
        result = D3D12CreateDevice(
            adapter,
            D3D_FEATURE_LEVEL_12_0,
            __uuidof(ID3D12Device),
            reinterpret_cast<void**>(&device));
        if (FAILED(result)) {
            adapter->Release();
            continue;
        }

        runtime.dxgi_adapter = adapter;
        runtime.d3d12_device = device;
        runtime.gpu_info.active_backend = PS5RT_GPU_D3D12;
        runtime.gpu_info.active_upscaler = PS5RT_UPSCALER_NATIVE;
        runtime.gpu_info.feature_level = D3D_FEATURE_LEVEL_12_0;
        runtime.gpu_info.dedicated_video_memory =
            descriptor.DedicatedVideoMemory;
        copy_adapter_name(runtime.gpu_info.adapter_name, descriptor.Description);
        return true;
    }

    return false;
}

void destroy_runtime(Ps5RtRuntime& runtime) {
    release_com(runtime.d3d12_device);
    release_com(runtime.dxgi_adapter);
    release_com(runtime.dxgi_factory);
}

} // namespace

extern "C" PS5RT_API uint32_t ps5rt_get_abi_version(void) {
    return PS5RT_ABI_VERSION;
}

extern "C" PS5RT_API const char* ps5rt_get_backend_name(
    Ps5RtGpuBackend backend) {
    switch (backend) {
    case PS5RT_GPU_AUTO:
        return "auto";
    case PS5RT_GPU_D3D12:
        return "d3d12";
    case PS5RT_GPU_VULKAN:
        return "vulkan";
    case PS5RT_GPU_NONE:
        return "none";
    default:
        return "unknown";
    }
}

extern "C" PS5RT_API const char* ps5rt_get_result_name(Ps5RtResult result) {
    switch (result) {
    case PS5RT_OK:
        return "ok";
    case PS5RT_ERROR_INVALID_ARGUMENT:
        return "invalid_argument";
    case PS5RT_ERROR_ABI_MISMATCH:
        return "abi_mismatch";
    case PS5RT_ERROR_BACKEND_UNAVAILABLE:
        return "backend_unavailable";
    case PS5RT_ERROR_BACKEND_INITIALIZATION:
        return "backend_initialization";
    default:
        return "unknown";
    }
}

extern "C" PS5RT_API Ps5RtResult ps5rt_create(
    const Ps5RtCreateInfo* create_info,
    Ps5RtRuntime** runtime) {
    if (create_info == nullptr ||
        runtime == nullptr ||
        create_info->struct_size < sizeof(Ps5RtCreateInfo)) {
        return PS5RT_ERROR_INVALID_ARGUMENT;
    }
    *runtime = nullptr;
    if (create_info->abi_version != PS5RT_ABI_VERSION) {
        return PS5RT_ERROR_ABI_MISMATCH;
    }

    auto* instance = new (std::nothrow) Ps5RtRuntime{};
    if (instance == nullptr) {
        return PS5RT_ERROR_BACKEND_INITIALIZATION;
    }
    instance->gpu_info.struct_size = sizeof(Ps5RtGpuInfo);
    instance->gpu_info.active_backend = PS5RT_GPU_NONE;
    instance->gpu_info.active_upscaler = PS5RT_UPSCALER_NATIVE;
    std::strcpy(instance->gpu_info.adapter_name, "headless");

    if ((create_info->flags & PS5RT_CREATE_HEADLESS) != 0 ||
        create_info->gpu_backend == PS5RT_GPU_NONE) {
        *runtime = instance;
        return PS5RT_OK;
    }

    const auto requested = create_info->gpu_backend == PS5RT_GPU_AUTO
        ? PS5RT_GPU_D3D12
        : create_info->gpu_backend;
    if (requested != PS5RT_GPU_D3D12) {
        delete instance;
        return PS5RT_ERROR_BACKEND_UNAVAILABLE;
    }

    if (!try_create_d3d12_device(*instance)) {
        destroy_runtime(*instance);
        delete instance;
        return PS5RT_ERROR_BACKEND_INITIALIZATION;
    }

    *runtime = instance;
    return PS5RT_OK;
}

extern "C" PS5RT_API void ps5rt_destroy(Ps5RtRuntime* runtime) {
    if (runtime == nullptr) {
        return;
    }
    destroy_runtime(*runtime);
    delete runtime;
}

extern "C" PS5RT_API Ps5RtResult ps5rt_get_gpu_info(
    const Ps5RtRuntime* runtime,
    Ps5RtGpuInfo* gpu_info) {
    if (runtime == nullptr ||
        gpu_info == nullptr ||
        gpu_info->struct_size < sizeof(Ps5RtGpuInfo)) {
        return PS5RT_ERROR_INVALID_ARGUMENT;
    }
    *gpu_info = runtime->gpu_info;
    return PS5RT_OK;
}
