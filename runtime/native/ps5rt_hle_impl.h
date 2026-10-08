// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "ps5gpu_native_api.h"

#if defined(__GNUC__) && defined(_WIN32)
#define PS5RT_GUEST_ABI __attribute__((sysv_abi))
#else
#define PS5RT_GUEST_ABI
#endif

using Ps5RtGuestTlsEnter = void* (*)();
using Ps5RtGuestTlsLeave = void (*)(void*);
using Ps5RtGuestTlsRestore = void (*)();

extern "C" void ps5rt_set_guest_tls_hooks(
    Ps5RtGuestTlsEnter, Ps5RtGuestTlsLeave, Ps5RtGuestTlsRestore);
extern "C" bool ps5rt_is_guest_worker_thread();
extern "C" [[noreturn]] void ps5rt_abort_current_guest_thread();
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_create_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_join_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_exit_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_self_real();
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_setprio_real(
    std::uint64_t, std::int32_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_getprio_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_mutex_init_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_mutex_destroy_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_mutex_lock_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_mutex_unlock_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_mutex_trylock_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_cond_init_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_cond_destroy_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_cond_wait_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_cond_signal_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_cond_broadcast_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_create_sema_real(
    std::uint64_t, std::uint64_t, std::uint32_t, std::int32_t,
    std::int32_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_wait_sema_real(
    std::uint64_t, std::int32_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_signal_sema_real(
    std::uint64_t, std::int32_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_delete_sema_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_posix_sem_init_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_sem_init_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_posix_sem_wait_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_posix_sem_trywait_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_pthread_sem_trywait_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_posix_sem_timedwait_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_posix_sem_post_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_posix_sem_getvalue_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_posix_sem_destroy_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_create_event_flag_real(
    std::uint64_t, std::uint32_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_delete_event_flag_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_set_event_flag_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_wait_event_flag_real(
    std::uint64_t, std::uint64_t, std::uint32_t, std::uint64_t*,
    std::uint32_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_open_real(
    std::uint64_t, std::int32_t, std::int32_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_kernel_check_reachability_real(std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_close_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_read_real(
    std::uint64_t, std::uint64_t, std::size_t, std::size_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_write_real(
    std::uint64_t, std::uint64_t, std::size_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_kernel_lseek_real(
    std::uint64_t, std::int64_t, std::int32_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_videoout_open_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_videoout_close_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_videoout_register_buffers_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_videoout_register_buffers2_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_videoout_submit_flip_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_videoout_set_flip_rate_real(
    std::uint64_t, std::int32_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_videoout_is_flip_pending_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_videoout_get_vblank_status_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_videoout_add_flip_event_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_videoout_set_buffer_attribute_real(
    std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t,
    std::uint32_t, std::uint32_t, std::uint32_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_videoout_set_buffer_attribute2_real(
    std::uint64_t, std::uint64_t, std::uint32_t, std::uint32_t,
    std::uint32_t, std::uint64_t, std::uint32_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_videoout_get_output_status_real(std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_videoout_color_settings_real(
    std::uint64_t, float);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_videoout_adjust_color_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_videoout_is_output_supported_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_videoout_wait_vblank_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_videoout_unregister_buffers_real(std::uint64_t, std::int32_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_init_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_get_register_defaults_real(std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_get_register_defaults_internal_real(std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_get_data_packet_payload_address_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_write_data_patch_address_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_wait_reg_mem_patch_address_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_create_shader_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_get_fused_shader_size_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_fuse_shader_halves_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_driver_submit_dcb_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_driver_submit_acb_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_dcb_set_flip_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_dcb_draw_index_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_set_sh_registers_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_dcb_nop_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_dcb_event_write_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
// The command buffer writers take their arguments unpacked, the way the
// guest passes them, and build the PM4 packet here rather than in the
// managed bridge. Parameters seven and up arrive on the guest stack under
// the SysV ABI, which is what PS5RT_GUEST_ABI asks the compiler for.
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_dcb_acquire_mem_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_cb_release_mem_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_cb_set_sh_register_range_direct_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_cb_set_sh_registers_direct_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_dcb_push_marker_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_dcb_pop_marker_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_set_cx_reg_indirect_patch_add_registers_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_set_sh_reg_indirect_patch_add_registers_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_set_uc_reg_indirect_patch_add_registers_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_set_cx_registers_indirect_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_set_sh_registers_indirect_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_set_uc_registers_indirect_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_dcb_write_data_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_dcb_wait_reg_mem_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_set_num_instances_real(std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_set_index_count_real(std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_draw_index_offset_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_queue_end_of_pipe_action_patch_address_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_set_cx_reg_indirect_patch_set_address_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_set_sh_reg_indirect_patch_set_address_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_set_uc_reg_indirect_patch_set_address_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_cb_nop_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_cb_dispatch_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_draw_index_auto_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_set_index_buffer_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_create_prim_state_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t);

extern "C" void ps5rt_videoout_shadow_observe(
    const char* nid,
    std::uint64_t arg0,
    std::uint64_t arg1,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4,
    std::uint64_t arg5,
    std::uint64_t guest_rsp,
    std::uint64_t managed_result);
extern "C" bool ps5rt_videoout_authority_dispatch(
    const char* nid,
    std::uint64_t arg0,
    std::uint64_t arg1,
    std::uint64_t arg2,
    std::uint64_t arg3,
    std::uint64_t arg4,
    std::uint64_t arg5,
    std::uint64_t guest_rsp,
    std::uint64_t xmm0_low,
    std::uint64_t* result);
extern "C" void ps5rt_agc_shadow_submit_dcb(std::uint64_t packet_address);
extern "C" void ps5rt_report_outstanding_waits(std::uint64_t minimum_ms);
extern "C" void ps5rt_agc_shadow_submit_acb(
    std::uint64_t owner_handle,
    std::uint64_t packet_address);
extern "C" bool ps5rt_native_gpu_register_shader_state(
    const Ps5GpuNativeShaderState* state,
    std::uint32_t* state_id);
extern "C" bool ps5rt_native_gpu_register_compute_state(
    const Ps5GpuNativeComputeState* state,
    std::uint32_t* state_id);
extern "C" bool ps5rt_native_gpu_submit_draw(
    const Ps5GpuNativeDraw* draw);
extern "C" bool ps5rt_native_gpu_submit_compute(
    const Ps5GpuNativeComputeDispatch* dispatch);
extern "C" bool ps5rt_native_gpu_submit_flip(
    const Ps5GpuNativeFlip* flip);
extern "C" void ps5rt_native_gpu_flush();

extern "C" void ps5rt_set_game_root(const char*);
extern "C" PS5RT_GUEST_ABI void ps5rt_init_env();
extern "C" PS5RT_GUEST_ABI void* ps5rt_mspace_malloc(std::uint64_t);
extern "C" PS5RT_GUEST_ABI void ps5rt_mspace_free(void*);
extern "C" PS5RT_GUEST_ABI void* ps5rt_mspace_memalign(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI void* ps5rt_calloc(std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI void* ps5rt_realloc(void*, std::uint64_t);
extern "C" PS5RT_GUEST_ABI void* ps5rt_aligned_alloc(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::int32_t ps5rt_posix_memalign(
    void**, std::uint64_t, std::uint64_t);
extern "C" void ps5rt_describe_libc_address(
    std::uint64_t, const char*);

// sceLibcMspace*: the mspace handle occupies the first argument slot.
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_libc_mspace_create(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" bool ps5rt_libc_mspace_is_native(std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_libc_mspace_destroy(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI void* ps5rt_libc_mspace_malloc(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI void ps5rt_libc_mspace_free(
    std::uint64_t, void*);
extern "C" PS5RT_GUEST_ABI void* ps5rt_libc_mspace_calloc(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI void* ps5rt_libc_mspace_realloc(
    std::uint64_t, void*, std::uint64_t);
extern "C" PS5RT_GUEST_ABI void* ps5rt_libc_mspace_memalign(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_libc_mspace_malloc_stats(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_libc_mspace_malloc_usable_size(std::uint64_t, const void*);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_libc_mspace_is_heap_empty(std::uint64_t);

// The third tier of AGC packet writers: the async-compute mirrors of the
// direct ones above, plus the indirect draw and dispatch forms and the two
// exports that are not packet writers at all. Parameters seven and up
// arrive on the guest stack under the SysV ABI. sceAgcAcbDmaData is the
// odd one: it reads its source and length from positions seven and eight
// and never touches five or six, so those are declared and ignored.
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_stall_command_buffer_parser_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_dcb_dma_data_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_acb_dma_data_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_set_base_indirect_args_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_dispatch_indirect_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_acb_dispatch_indirect_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_draw_index_indirect_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_dcb_get_lod_stats_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_dcb_wait_until_safe_for_rendering_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_acb_event_write_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_acb_acquire_mem_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_acb_wait_reg_mem_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_agc_create_interpolant_mapping_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_agc_unknown_qj7_real(
    std::uint64_t, std::uint64_t, std::uint64_t);

// The online imports the title reaches once it gets far enough: two
// libSceRudp entry points, two libSceNet, and the twenty-two that make
// up one BasicProfileApi::getPublicProfiles transaction. They return
// what the managed auto-stub was returning; what they add is a name and
// a first-call trace. See the block in the .cpp for why IntrusivePtr is
// not implemented for real yet.
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_rudp_enable_internal_io_thread_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_cppwebapi_lib_context_ctor_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_cppwebapi_init_params_ctor_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_cppwebapi_init_params_dtor_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_response_ptr_ctor_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_response_ptr_dtor_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_response_ptr_arrow_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_profile_vector_ptr_ctor_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_profile_vector_ptr_dtor_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_profile_vector_ptr_get_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_transaction_ctor_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_transaction_dtor_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_transaction_finish_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_public_profiles_parameter_ctor_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_public_profiles_parameter_dtor_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_public_profiles_parameter_terminate_real(
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_rudp_set_event_handler_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_profile_vector_ptr_assign_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_transaction_start_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_transaction_set_response_option_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_transaction_get_response_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_response_get_profiles_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_net_getsockname_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_public_profiles_parameter_initialize_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_np_get_public_profiles_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_net_sendto_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t);

// APR / AMPR: the asset streaming path. See the block at the end of
// ps5rt_hle_impl.cpp for the command buffer layout and where it came from.
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_ampr_command_buffer_constructor_real(std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_ampr_command_buffer_destructor_real(std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_ampr_apr_command_buffer_constructor_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_ampr_apr_command_buffer_destructor_real(std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_ampr_command_buffer_set_buffer_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_ampr_command_buffer_clear_buffer_real(std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_ampr_command_buffer_reset_real(std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_ampr_command_buffer_get_current_offset_real(std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_ampr_command_buffer_get_num_commands_real(std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_ampr_command_buffer_get_size_real(std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_ampr_measure_command_size_read_file_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_ampr_apr_command_buffer_read_file_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_kernel_apr_resolve_filepaths_to_ids_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_kernel_apr_resolve_filepaths_to_ids_and_sizes_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_kernel_apr_get_file_size_real(std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_kernel_apr_submit_command_buffer_real(std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_kernel_apr_submit_command_buffer_and_get_id_real(
    std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_kernel_apr_submit_command_buffer_and_get_result_real(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t
ps5rt_kernel_apr_wait_command_buffer_real(std::uint64_t);

// Guest direct memory is reserved rather than committed above a size
// threshold; these are how the rest of the runtime keeps that honest.
extern "C" void ps5rt_guest_reservation_add(std::uint64_t, std::uint64_t);
extern "C" bool ps5rt_guest_reservation_contains(std::uint64_t);
extern "C" bool ps5rt_guest_commit_range(std::uint64_t, std::uint64_t);
extern "C" std::uint64_t ps5rt_guest_commit_megabytes();

// Bumped by the GPU module whenever it changes guest page protection, so
// this side's writability cache can tell a stale entry from a live one.
extern "C" __declspec(dllexport) void ps5rt_guest_protect_generation_bump();

// libc string search, taken over from the bridge: 39 per cent of the
// managed crossings in a run were these three.
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_libc_strchr_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_libc_strrchr_real(
    std::uint64_t, std::uint64_t);
extern "C" PS5RT_GUEST_ABI std::uint64_t ps5rt_libc_strstr_real(
    std::uint64_t, std::uint64_t);
