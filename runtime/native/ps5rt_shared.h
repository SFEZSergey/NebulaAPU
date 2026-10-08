// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>

#if defined(__GNUC__) && defined(_WIN32)
#define PS5_GUEST_ABI __attribute__((sysv_abi))
#else
#define PS5_GUEST_ABI
#endif

inline void write_fs_base_raw(std::uint64_t value) {
#if defined(__GNUC__) && defined(_WIN32)
    __asm__ volatile("wrfsbase %0" : : "r"(value));
#else
    (void)value;
#endif
}

inline std::uint64_t read_fs_base_raw() {
    std::uint64_t value = 0;
#if defined(__GNUC__) && defined(_WIN32)
    __asm__ volatile("rdfsbase %0" : "=r"(value));
#endif
    return value;
}
