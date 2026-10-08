// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

namespace Ps5Recomp.Cli.HleGenerator;

/// <summary>
/// Native handlers already present in ps5rt. This is the only manual binding
/// list; package-specific resolver code is generated from it.
/// </summary>
internal static class HleKnownImplementations
{
    public static IReadOnlyList<HleEntry> GetKnown() =>
    [
        new("ps5rt-test", "ps5rt-test", "ps5rt", "ps5rt_test_hle"),
        new("_tls_get_addr", "vNe1w4diLCs", "ps5rt", "ps5rt_tls_get_addr"),
        new("__stack_chk_guard", "f7uOxY9mM1U", "ps5rt", "ps5rt_stack_check_guard"),
        new("__stack_chk_fail", "Ou3iL1abvng", "ps5rt", "ps5rt_stack_check_fail"),
        new("sceKernelGetProcParam", "959qrazPIrg", "ps5rt", "ps5rt_get_proc_param"),
        new("__error", "9BcDykPmo1I", "ps5rt", "ps5rt_get_errno_address"),
        new("_sceKernelSetThreadDtors", "rNhWz+lvOMU", "ps5rt", "ps5rt_set_thread_dtors"),
        new("_sceKernelSetThreadAtexitCount", "pB-yGZ2nQ9o", "ps5rt", "ps5rt_set_thread_atexit_count"),
        new("_sceKernelSetThreadAtexitReport", "WhCc1w3EhSI", "ps5rt", "ps5rt_set_thread_atexit_report"),
        new("_sceKernelRtldSetApplicationHeapAPI", "p5EcQeEeJAE", "ps5rt", "ps5rt_set_application_heap_api"),

        new("pthread_self", "aI+OeCz8xrQ", "libKernel", "ps5rt_pthread_self"),
        new("scePthreadGetthreadid", "EI-5-jlq2dE", "libKernel", "ps5rt_pthread_getthreadid"),
        new("pthread_getthreadid_np", "3eqs37G74-s", "libKernel", "ps5rt_pthread_getthreadid"),
        new("pthread_equal", "3PtV6p3QNX4", "libKernel", "ps5rt_pthread_equal"),
        new("pthread_yield", "T72hz6ffq08", "libKernel", "ps5rt_pthread_yield"),
        new("pthread_once", "14bOACANTBo", "libKernel", "ps5rt_pthread_once"),

        new("pthread_key_create", "mqULNdimTn0", "libKernel", "ps5rt_pthread_key_create"),
        new("scePthreadKeyCreate", "geDaqgH9lTg", "libKernel", "ps5rt_pthread_key_create"),
        new("pthread_key_delete", "6BpEZuDT7YI", "libKernel", "ps5rt_pthread_key_delete"),
        new("scePthreadKeyDelete", "PrdHuuDekhY", "libKernel", "ps5rt_pthread_key_delete"),
        new("pthread_setspecific", "WrOLvHU0yQM", "libKernel", "ps5rt_pthread_setspecific"),
        new("scePthreadSetspecific", "+BzXYkqYeLE", "libKernel", "ps5rt_pthread_setspecific"),
        new("pthread_getspecific", "0-KXaS70xy4", "libKernel", "ps5rt_pthread_getspecific"),
        new("scePthreadGetspecific", "eoht7mQOCmo", "libKernel", "ps5rt_pthread_getspecific"),

        new("pthread_mutexattr_init", "F8bUHwAG284", "libKernel", "ps5rt_pthread_mutexattr_init"),
        new("scePthreadMutexattrInit", "dQHWEsJtoE4", "libKernel", "ps5rt_pthread_mutexattr_init"),
        new("pthread_mutexattr_settype", "iMp8QpE+XO4", "libKernel", "ps5rt_pthread_mutexattr_settype"),
        new("scePthreadMutexattrSettype", "mDmgMOGVUqg", "libKernel", "ps5rt_pthread_mutexattr_settype"),
        new("pthread_mutexattr_destroy", "smWEktiyyG0", "libKernel", "ps5rt_pthread_mutexattr_destroy"),
        new("scePthreadMutexattrDestroy", "HF7lK46xzjY", "libKernel", "ps5rt_pthread_mutexattr_destroy"),
        new("pthread_mutex_init", "cmo1RIYva9o", "libKernel", "ps5rt_pthread_mutex_init"),
        new("scePthreadMutexInit", "ttHNfU+qDBU", "libKernel", "ps5rt_pthread_mutex_init"),
        new("pthread_mutex_destroy", "2Of0f+3mhhE", "libKernel", "ps5rt_pthread_mutex_destroy"),
        new("scePthreadMutexDestroy", "ltCfaGr2JGE", "libKernel", "ps5rt_pthread_mutex_destroy"),

        new("pthread_attr_init", "nsYoNRywwNg", "libKernel", "ps5rt_pthread_attr_init"),
        new("scePthreadAttrInit", "wtkt-teR1so", "libKernel", "ps5rt_pthread_attr_init"),
        new("pthread_attr_destroy", "62KCwEMmzcM", "libKernel", "ps5rt_pthread_attr_destroy"),
        new("scePthreadAttrDestroy", "zHchY8ft5pk", "libKernel", "ps5rt_pthread_attr_destroy"),
        new("scePthreadAttrGetaffinity", "8+s5BzZjxSg", "libKernel", "ps5rt_pthread_attr_getaffinity"),

        new("write", "FxVZqBAA7ks", "libKernel", "ps5rt_write"),
        new(
            "_nanosleep",
            "NhpspxdjEKU",
            "libKernel",
            "ps5rt_nanosleep",
            Signature:
                "int _nanosleep(const struct timespec* requested, struct timespec* remaining)"),
        new(
            "nanosleep",
            "yS8U2TGCe1A",
            "libKernel",
            "ps5rt_nanosleep",
            Signature:
                "int nanosleep(const struct timespec* requested, struct timespec* remaining)"),
        new(
            "sceKernelConfiguredFlexibleMemorySize",
            "n1-v6FgU7MQ",
            "libKernel",
            "ps5rt_configured_flexible_memory_size",
            Signature:
                "int32_t sceKernelConfiguredFlexibleMemorySize(uint64_t* size)"),
        new(
            "sceKernelCheckReachability",
            "uWyW3v98sU4",
            "libKernel",
            "ps5rt_kernel_check_reachability_real",
            Signature:
                "int32_t sceKernelCheckReachability(const char* path)"),
        new(
            "sceKernelDlsym",
            "LwG8g3niqwA",
            "libKernel",
            "ps5rt_kernel_dlsym",
            Signature:
                "int32_t sceKernelDlsym(int32_t handle, const char* symbol, void** address)"),
        new(
            "sceKernelGetCurrentCpu",
            "g0VTBxfJyu0",
            "libKernel",
            "ps5rt_get_current_cpu",
            Signature:
                "int32_t sceKernelGetCurrentCpu(void)"),
        new("sceLibcHeapGetTraceInfo", "NWtTN10cJzE", "libSceLibcInternal", "ps5rt_libc_heap_get_trace_info"),
        new("sceKernelGetDirectMemorySize", "pO96TwzOm5E", "libKernel", "ps5rt_get_direct_memory_size"),
        new("sceKernelAllocateDirectMemory", "rTXw65xmLIA", "libKernel", "ps5rt_allocate_direct_memory"),
        new("sceKernelMapDirectMemory", "NcaWUxfMNIQ", "libKernel", "ps5rt_map_direct_memory"),
        new("sceKernelMapNamedDirectMemory", "L-Q3LEjIbgA", "libKernel", "ps5rt_map_direct_memory"),
        new(
            "sceSystemServiceParamGetInt",
            "fZo48un7LK4",
            "libSceSystemService",
            "ps5rt_system_service_param_get_int",
            Signature:
                "int32_t sceSystemServiceParamGetInt(int32_t parameterId, int32_t* value)"),
        new(
            "sceFontSelectLibraryFt",
            "oM+XCzVG3oM",
            "libSceFontFt",
            "ps5rt_font_select_library_ft",
            Signature:
                "const void* sceFontSelectLibraryFt(int32_t selection)"),
        new(
            "sceFontSelectRendererFt",
            "Xx974EW-QFY",
            "libSceFontFt",
            "ps5rt_font_select_renderer_ft",
            Signature:
                "const void* sceFontSelectRendererFt(int32_t selection)"),
        new("sceFontMemoryInit", "whrS4oksXc4", "libSceFont", "ps5rt_font_memory_init"),
        new("sceFontCreateLibraryWithEdition", "n590hj5Oe-k", "libSceFont", "ps5rt_font_create_library_with_edition"),
        new("sceFontCreateRendererWithEdition", "WaSFJoRWXaI", "libSceFont", "ps5rt_font_create_renderer_with_edition"),
        new("sceFontDestroyRenderer", "exAxkyVLt0s", "libSceFont", "ps5rt_font_destroy_renderer"),
        new("sceFontBindRenderer", "3OdRkSjOcog", "libSceFont", "ps5rt_font_bind_renderer"),
        new("sceFontUnbindRenderer", "1QjhKxrsOB8", "libSceFont", "ps5rt_font_unbind_renderer"),
        new("sceFontSetScalePixel", "N1EBMeGhf7E", "libSceFont", "ps5rt_font_set_scale_pixel"),
        new("sceFontSetEffectSlant", "TMtqoFQjjbA", "libSceFont", "ps5rt_font_set_effect_slant"),
        new("sceFontSetEffectWeight", "v0phZwa4R5o", "libSceFont", "ps5rt_font_set_effect_weight"),
        new("sceFontSetupRenderScalePixel", "6vGCkkQJOcI", "libSceFont", "ps5rt_font_setup_render_scale_pixel"),
        new("sceFontSetupRenderEffectSlant", "lz9y9UFO2UU", "libSceFont", "ps5rt_font_setup_render_effect_slant"),
        new("sceFontSetupRenderEffectWeight", "XIGorvLusDQ", "libSceFont", "ps5rt_font_setup_render_effect_weight"),
        new("sceFontGetHorizontalLayout", "imxVx8lm+KM", "libSceFont", "ps5rt_font_get_horizontal_layout"),
        new("sceFontOpenFontSet", "cKYtVmeSTcw", "libSceFont", "ps5rt_font_open_font_set"),
        new("sceFontOpenFontMemory", "KXUpebrFk1U", "libSceFont", "ps5rt_font_open_font_memory"),
        new("sceFontOpenFontInstance", "JzCH3SCFnAU", "libSceFont", "ps5rt_font_open_font_instance"),
        new("sceFontCloseFont", "vzHs3C8lWJk", "libSceFont", "ps5rt_font_close_font"),
        new("sceFontSupportSystemFonts", "SsRbbCiWoGw", "libSceFont", "ps5rt_font_support_system_fonts"),
        new("sceFontSupportExternalFonts", "mz2iTY0MK4A", "libSceFont", "ps5rt_font_support_external_fonts"),
        new("sceFontAttachDeviceCacheBuffer", "CUKn5pX-NVY", "libSceFont", "ps5rt_font_attach_device_cache_buffer"),
        new("sceFontGetRenderCharGlyphMetrics", "IQtleGLL5pQ", "libSceFont", "ps5rt_font_get_render_char_glyph_metrics"),
        new("sceFontGenerateCharGlyph", "C-4Qw5Srlyw", "libSceFont", "ps5rt_font_generate_char_glyph"),
        new("sceFontGlyphDefineAttribute", "8-zmgsxkBek", "libSceFont", "ps5rt_font_glyph_define_attribute"),
        new("sceFontRenderCharGlyphImageHorizontal", "kAenWy1Zw5o", "libSceFont", "ps5rt_font_render_char_glyph_image_horizontal"),
        new("sceFontDeleteGlyph", "LHDoRWVFGqk", "libSceFont", "ps5rt_font_delete_glyph"),
        new("sceFontRenderSurfaceInit", "gdUCnU0gHdI", "libSceFont", "ps5rt_font_render_surface_init"),
        new("sceAgcInit", "23LRUSvYu1M", "libSceAgc", "ps5rt_agc_init_real"),
        new("sceAgcGetRegisterDefaults2", "2JtWUUiYBXs", "libSceAgc", "ps5rt_agc_get_register_defaults_real"),
        new("sceAgcGetRegisterDefaults2Internal", "wRbq6ZjNop4", "libSceAgc", "ps5rt_agc_get_register_defaults_internal_real"),
        new("sceAgcCreateShader", "f3dg2CSgRKY", "libSceAgc", "ps5rt_agc_create_shader_real"),
        new("sceAgcUnknownGetFusedShaderSize", "dolOmWH+huQ", "libSceAgc", "ps5rt_agc_get_fused_shader_size_real"),
        new("sceAgcUnknownFuseShaderHalves", "fd5Bp5tGTgo", "libSceAgc", "ps5rt_agc_fuse_shader_halves_real"),
        new("sceVideoOutGetOutputStatus", "utPrVdxio-8", "libSceVideoOut", "ps5rt_videoout_get_output_status_real"),
        new("sceVideoOutClose", "uquVH4-Du78", "libSceVideoOut", "ps5rt_videoout_close_real"),
        new("sceVideoOutColorSettingsSetGamma_", "DYhhWbJSeRg", "libSceVideoOut", "ps5rt_videoout_color_settings_real"),
        new("sceVideoOutGetVblankStatus", "1FZBKy8HeNU", "libSceVideoOut", "ps5rt_videoout_get_vblank_status_real"),
        new("sceVideoOutOpen", "Up36PTk687E", "libSceVideoOut", "ps5rt_videoout_open_real"),
        new("sceVideoOutSubmitChangeBufferAttribute2", "HuViW4HnrOw", "libSceVideoOut", "ps5rt_return_zero", "stubbed"),
        new("sceVideoOutSetBufferAttribute2", "PjS5uASwcV8", "libSceVideoOut", "ps5rt_videoout_set_buffer_attribute2_real"),
        new("sceVideoOutIsFlipPending", "zgXifHT9ErY", "libSceVideoOut", "ps5rt_videoout_is_flip_pending_real"),
        new("sceVideoOutAddFlipEvent", "HXzjK9yI30k", "libSceVideoOut", "ps5rt_videoout_add_flip_event_real"),
        new("sceVideoOutRegisterBuffers2", "rKBUtgRrtbk", "libSceVideoOut", "ps5rt_videoout_register_buffers2_real"),
        new("sceVideoOutAdjustColor_", "pv9CI5VC+R0", "libSceVideoOut", "ps5rt_videoout_adjust_color_real"),

        new("_init_env", "bzQExy189ZI", "libSceLibcInternal", "ps5rt_init_env"),
        new("atexit", "8G2LB+A3rzg", "libSceLibcInternal", "ps5rt_cxa_atexit"),
        new(
            "puts",
            "YQ0navp+YIc",
            "libSceLibcInternal",
            "ps5rt_puts",
            Signature: "int puts(const char* text)"),
        new(
            "time",
            "wLlFkwG9UcQ",
            "libSceLibcInternal",
            "ps5rt_time",
            Signature: "int64_t time(int64_t* output)"),
        new(
            "strtol",
            "mXlxhmLNMPg",
            "libSceLibcInternal",
            "ps5rt_strtol",
            Signature:
                "int64_t strtol(const char* text, char** end, int32_t base)"),
        new(
            "sin",
            "H8ya2H00jbI",
            "libSceLibcInternal",
            "ps5rt_sin",
            Signature: "double sin(double value)"),
        new(
            "sinf",
            "Q4rRL34CEeE",
            "libSceLibcInternal",
            "ps5rt_sinf",
            Signature: "float sinf(float value)"),
        new(
            "cos",
            "2WE3BTYVwKM",
            "libSceLibcInternal",
            "ps5rt_cos",
            Signature: "double cos(double value)"),
        new(
            "cosf",
            "-P6FNMzk2Kc",
            "libSceLibcInternal",
            "ps5rt_cosf",
            Signature: "float cosf(float value)"),
        new("sceLibcMspaceMalloc", "OJjm-QOIHlI", "libSceLibcInternal", "ps5rt_libc_mspace_malloc"),
        new("sceLibcMspaceFree", "tIhsqj0qsFE", "libSceLibcInternal", "ps5rt_mspace_free"),
        new("qsort", "AEJdIVZTEmo", "libSceLibcInternal", "ps5rt_qsort"),
        new("memcpy_s", "NFLs+dRJGNg", "libSceLibcInternal", "ps5rt_memcpy_s"),
        new("sceLibcMspaceMallocStatsFast", "k04jLXu3+Ic", "libSceLibcInternal", "ps5rt_libc_mspace_malloc_stats"),
        new("sceKernelGetProsperoCompiledSdkVersion", "GGeRJk1XdWc", "libKernel", "ps5rt_get_compiled_sdk_version"),

        new("sceRegMgrGetInt", "mPYKD12UDQI", "libSceRegMgr", "ps5rt_return_zero", "stubbed"),
        new("sceRegMgrNonSysGetInt", "dKeshzt29G4", "libSceRegMgr", "ps5rt_return_zero", "stubbed"),
        new("sceRegMgrNonSysGetStr", "DKWSr89zMsI", "libSceRegMgr", "ps5rt_return_zero", "stubbed"),
        new("sceRegMgrNonSysGetBin", "k9LC1z8kh-E", "libSceRegMgr", "ps5rt_return_zero", "stubbed"),
    ];
}
