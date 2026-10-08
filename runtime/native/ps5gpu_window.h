// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

// A window of the native runtime's own, and the swapchain that shows the
// buffer each flip names.
//
// The runtime renders into its own Vulkan images. The window the title ran
// in until now belongs to the managed presenter, which shows only what the
// managed renderer drew - so a frame the native path got right was never on
// screen. This shows it: after a flip's work, the presented surface is
// blitted, scaled, into a swapchain image and presented.
//
// PS5GPU_NATIVE_WINDOW=1 turns it on. The window lives on a thread of its
// own that does nothing but pump messages; the swapchain is driven from the
// worker, which is the only thread that touches the device.
#ifndef PS5GPU_WINDOW_H
#define PS5GPU_WINDOW_H

#include <windows.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace ps5gpu {

// Steps of the first frames, straight to stderr and flushed: the driver
// takes the process down in the middle of one of them when an overlay is
// hooked in, and the buffered trace never reaches the log.
inline void window_step(const char* step, long long value = 0) {
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1) < 40) {
        std::fprintf(stderr, "native_window.step %s %lld\n", step, value);
        std::fflush(stderr);
    }
}

inline bool native_window_requested() {
    static const bool requested = [] {
        const auto* value = std::getenv("PS5GPU_NATIVE_WINDOW");
        return value != nullptr && value[0] == '1';
    }();
    return requested;
}

class NativeWindowPresenter {
public:
    // The instance extensions the window needs, for the caller to enable.
    static const char* const* instance_extensions(std::uint32_t& count) {
        static const char* const names[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };
        count = 2;
        return names;
    }

    static const char* const* device_extensions(std::uint32_t& count) {
        static const char* const names[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        count = 1;
        return names;
    }

    ~NativeWindowPresenter() { shutdown(); }

    bool start(
        VkInstance instance,
        PFN_vkGetInstanceProcAddr get_instance_proc,
        VkPhysicalDevice physical_device,
        VkDevice device,
        PFN_vkGetDeviceProcAddr get_device_proc,
        std::uint32_t queue_family,
        VkQueue queue) {
        instance_ = instance;
        physical_device_ = physical_device;
        device_ = device;
        queue_family_ = queue_family;
        queue_ = queue;
        const auto instance_function = [&](const char* name) {
            return get_instance_proc(instance, name);
        };
        const auto device_function = [&](const char* name) {
            return get_device_proc(device, name);
        };
        create_win32_surface_ = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
            instance_function("vkCreateWin32SurfaceKHR"));
        destroy_surface_ = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
            instance_function("vkDestroySurfaceKHR"));
        surface_support_ =
            reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
                instance_function("vkGetPhysicalDeviceSurfaceSupportKHR"));
        surface_capabilities_ =
            reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
                instance_function(
                    "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        surface_formats_ =
            reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
                instance_function("vkGetPhysicalDeviceSurfaceFormatsKHR"));
        create_swapchain_ = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
            device_function("vkCreateSwapchainKHR"));
        destroy_swapchain_ = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
            device_function("vkDestroySwapchainKHR"));
        get_swapchain_images_ = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
            device_function("vkGetSwapchainImagesKHR"));
        acquire_next_image_ = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
            device_function("vkAcquireNextImageKHR"));
        queue_present_ = reinterpret_cast<PFN_vkQueuePresentKHR>(
            device_function("vkQueuePresentKHR"));
        create_fence_ = reinterpret_cast<PFN_vkCreateFence>(
            device_function("vkCreateFence"));
        destroy_fence_ = reinterpret_cast<PFN_vkDestroyFence>(
            device_function("vkDestroyFence"));
        wait_for_fences_ = reinterpret_cast<PFN_vkWaitForFences>(
            device_function("vkWaitForFences"));
        reset_fences_ = reinterpret_cast<PFN_vkResetFences>(
            device_function("vkResetFences"));
        cmd_pipeline_barrier_ = reinterpret_cast<PFN_vkCmdPipelineBarrier>(
            device_function("vkCmdPipelineBarrier"));
        cmd_blit_image_ = reinterpret_cast<PFN_vkCmdBlitImage>(
            device_function("vkCmdBlitImage"));
        if (create_win32_surface_ == nullptr || destroy_surface_ == nullptr ||
            surface_support_ == nullptr || surface_capabilities_ == nullptr ||
            surface_formats_ == nullptr || create_swapchain_ == nullptr ||
            destroy_swapchain_ == nullptr || get_swapchain_images_ == nullptr ||
            acquire_next_image_ == nullptr || queue_present_ == nullptr ||
            create_fence_ == nullptr || wait_for_fences_ == nullptr ||
            reset_fences_ == nullptr || cmd_pipeline_barrier_ == nullptr ||
            cmd_blit_image_ == nullptr) {
            return false;
        }

        create_semaphore_ = reinterpret_cast<PFN_vkCreateSemaphore>(
            get_device_proc(device, "vkCreateSemaphore"));
        destroy_semaphore_ = reinterpret_cast<PFN_vkDestroySemaphore>(
            get_device_proc(device, "vkDestroySemaphore"));
        if (create_semaphore_ == nullptr || destroy_semaphore_ == nullptr) {
            return false;
        }
        // The window itself is made by the first frame, on the thread that
        // presents: one thread owns the window, pumps its messages and
        // presents to it, as a game's render loop does. Made on a thread of
        // its own and presented to from the worker, the driver took the
        // process down on the first present whenever the NVIDIA overlay
        // was on.
        started_ = true;
        return true;
    }

    bool started() const { return started_; }

    // What happened, for a trace: frames recorded, frames presented, the
    // last present's result, the swapchain extent.
    std::uint64_t recorded() const { return recorded_; }
    std::uint64_t presented() const { return presented_; }
    int last_result() const { return last_result_; }
    VkExtent2D extent() const { return extent_; }
    HWND window() const { return window_; }

    // Before the frame's command buffer ends: takes a swapchain image and
    // records the blit of the presented surface into it. The source must be
    // in TRANSFER_SRC_OPTIMAL; the caller puts it there and back.
    bool record(VkCommandBuffer command_buffer, VkImage source,
                std::uint32_t source_width, std::uint32_t source_height) {
        pending_ = false;
        if (!started_) {
            return false;
        }
        if (window_ == nullptr && !open_window()) {
            started_ = false;
            return false;
        }
        pump_messages();
        if (minimized()) {
            return false;
        }
        if (needs_rebuild_ && !create_swapchain()) {
            return false;
        }
        // The standard handshake, which is what overlays and recorders
        // hook: the acquire signals a semaphore the frame's submission
        // waits on, and the submission signals one the present waits on.
        window_step("acquire");
        auto result = acquire_next_image_(device_, swapchain_, 1000000000ull,
                                          image_available_, VK_NULL_HANDLE,
                                          &image_index_);
        window_step("acquire_done", result);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            needs_rebuild_ = true;
            last_result_ = static_cast<int>(result);
            return false;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            last_result_ = static_cast<int>(result);
            return false;
        }
        const auto target = images_[image_index_];

        VkImageMemoryBarrier to_destination = {};
        to_destination.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_destination.srcAccessMask = 0;
        to_destination.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_destination.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_destination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_destination.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_destination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_destination.image = target;
        to_destination.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                           1};
        cmd_pipeline_barrier_(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                              nullptr, 1, &to_destination);

        // Letterboxed: the whole frame, scaled to fit, centred.
        const auto scale = std::min(
            static_cast<double>(extent_.width) / source_width,
            static_cast<double>(extent_.height) / source_height);
        const auto width = static_cast<std::int32_t>(source_width * scale);
        const auto height = static_cast<std::int32_t>(source_height * scale);
        const auto x = (static_cast<std::int32_t>(extent_.width) - width) / 2;
        const auto y = (static_cast<std::int32_t>(extent_.height) - height) / 2;
        VkImageBlit blit = {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<std::int32_t>(source_width),
                              static_cast<std::int32_t>(source_height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x, y, 0};
        blit.dstOffsets[1] = {x + width, y + height, 1};
        cmd_blit_image_(command_buffer, source,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                        VK_FILTER_LINEAR);

        VkImageMemoryBarrier to_present = to_destination;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        cmd_pipeline_barrier_(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &to_present);
        pending_ = true;
        ++recorded_;
        return true;
    }

    // For the frame's submission, while a present is pending: wait for the
    // image, and say when the frame is done with it.
    bool pending() const { return pending_; }
    VkSemaphore image_available() const { return image_available_; }
    VkSemaphore frame_finished() const { return frame_finished_; }

    // After the frame's submission has completed.
    void present() {
        if (!pending_) {
            return;
        }
        pending_ = false;
        pump_messages();
        VkPresentInfoKHR present_info = {};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &frame_finished_;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain_;
        present_info.pImageIndices = &image_index_;
        window_step("present");
        const auto result = queue_present_(queue_, &present_info);
        window_step("present_done", result);
        last_result_ = static_cast<int>(result);
        if (result == VK_SUCCESS) {
            ++presented_;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            needs_rebuild_ = true;
        }
    }

    void shutdown() {
        if (device_ != VK_NULL_HANDLE) {
            if (swapchain_ != VK_NULL_HANDLE && destroy_swapchain_ != nullptr) {
                destroy_swapchain_(device_, swapchain_, nullptr);
                swapchain_ = VK_NULL_HANDLE;
            }
            if (destroy_semaphore_ != nullptr) {
                if (image_available_ != VK_NULL_HANDLE) {
                    destroy_semaphore_(device_, image_available_, nullptr);
                    image_available_ = VK_NULL_HANDLE;
                }
                if (frame_finished_ != VK_NULL_HANDLE) {
                    destroy_semaphore_(device_, frame_finished_, nullptr);
                    frame_finished_ = VK_NULL_HANDLE;
                }
            }
        }
        if (surface_ != VK_NULL_HANDLE && destroy_surface_ != nullptr) {
            destroy_surface_(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        started_ = false;
    }

private:
    // The window, its surface, the semaphores and the swapchain, on the
    // calling thread.
    bool open_window() {
        window_ = create_window();
        if (window_ == nullptr) {
            return false;
        }
        // Full screen unless PS5GPU_NATIVE_WINDOW_MODE=windowed.
        const auto* mode = std::getenv("PS5GPU_NATIVE_WINDOW_MODE");
        if (mode == nullptr || std::strcmp(mode, "windowed") != 0) {
            set_fullscreen(true);
        }
        VkWin32SurfaceCreateInfoKHR surface_info = {};
        surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        surface_info.hinstance = GetModuleHandleW(nullptr);
        surface_info.hwnd = window_;
        window_step("create_surface");
        if (create_win32_surface_(instance_, &surface_info, nullptr,
                                  &surface_) != VK_SUCCESS) {
            return false;
        }
        VkBool32 supported = VK_FALSE;
        surface_support_(physical_device_, queue_family_, surface_,
                         &supported);
        if (supported != VK_TRUE) {
            return false;
        }
        VkSemaphoreCreateInfo semaphore_info = {};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (create_semaphore_(device_, &semaphore_info, nullptr,
                              &image_available_) != VK_SUCCESS ||
            create_semaphore_(device_, &semaphore_info, nullptr,
                              &frame_finished_) != VK_SUCCESS) {
            return false;
        }
        return create_swapchain();
    }

    // The window's messages, on the thread that made it.
    void pump_messages() {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    static LRESULT CALLBACK window_procedure(
        HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<NativeWindowPresenter*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_CLOSE) {
            // Closing the window ends the title, as closing a game does.
            ExitProcess(0);
        }
        if (self != nullptr) {
            // Alt+Enter and F11 switch between full screen and a window.
            const auto alt_enter =
                message == WM_SYSKEYDOWN && wparam == VK_RETURN &&
                (lparam & (1 << 29)) != 0;
            const auto f11 = message == WM_KEYDOWN && wparam == VK_F11;
            if (alt_enter || f11) {
                self->set_fullscreen(!self->fullscreen_);
                return 0;
            }
            if (message == WM_SYSCHAR && wparam == VK_RETURN) {
                // Swallowed, or Windows beeps for the Alt+Enter.
                return 0;
            }
            if (message == WM_SIZE) {
                self->needs_rebuild_ = true;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    // Borderless and covering the monitor the window is on, or back to the
    // window it was before.
    void set_fullscreen(bool fullscreen) {
        if (window_ == nullptr || fullscreen == fullscreen_) {
            return;
        }
        if (fullscreen) {
            GetWindowRect(window_, &windowed_rect_);
            MONITORINFO monitor = {};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(
                MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                &monitor);
            SetWindowLongPtrW(window_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(
                window_, HWND_TOP, monitor.rcMonitor.left,
                monitor.rcMonitor.top,
                monitor.rcMonitor.right - monitor.rcMonitor.left,
                monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        } else {
            SetWindowLongPtrW(
                window_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPos(
                window_, HWND_NOTOPMOST, windowed_rect_.left,
                windowed_rect_.top,
                windowed_rect_.right - windowed_rect_.left,
                windowed_rect_.bottom - windowed_rect_.top,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        fullscreen_ = fullscreen;
        needs_rebuild_ = true;
    }

    HWND create_window() {
        WNDCLASSW window_class = {};
        window_class.lpfnWndProc = window_procedure;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        // Black, without gdi32 for a stock brush.
        window_class.hbrBackground =
            reinterpret_cast<HBRUSH>(COLOR_WINDOWTEXT + 1);
        window_class.lpszClassName = L"Ps5GpuNativeWindow";
        RegisterClassW(&window_class);
        RECT rectangle = {0, 0, 1600, 900};
        AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
        const auto window = CreateWindowExW(
            0, window_class.lpszClassName, L"Astro Bot (native)",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
            rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
            nullptr, nullptr, window_class.hInstance, nullptr);
        if (window != nullptr) {
            SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(window, SW_SHOW);
            UpdateWindow(window);
        }
        return window;
    }

    bool minimized() const {
        return window_ != nullptr && IsIconic(window_);
    }

    bool create_swapchain() {
        needs_rebuild_ = false;
        VkSurfaceCapabilitiesKHR capabilities = {};
        if (surface_capabilities_(physical_device_, surface_,
                                  &capabilities) != VK_SUCCESS) {
            return false;
        }
        extent_ = capabilities.currentExtent;
        if (extent_.width == 0xFFFFFFFFu || extent_.width == 0 ||
            extent_.height == 0) {
            RECT client = {};
            GetClientRect(window_, &client);
            extent_.width = static_cast<std::uint32_t>(
                std::max<LONG>(1, client.right - client.left));
            extent_.height = static_cast<std::uint32_t>(
                std::max<LONG>(1, client.bottom - client.top));
        }
        if ((capabilities.supportedUsageFlags &
             VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
            return false;
        }
        std::uint32_t format_count = 0;
        surface_formats_(physical_device_, surface_, &format_count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        surface_formats_(physical_device_, surface_, &format_count,
                         formats.data());
        if (formats.empty()) {
            return false;
        }
        auto chosen = formats[0];
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM ||
                format.format == VK_FORMAT_R8G8B8A8_UNORM) {
                chosen = format;
                break;
            }
        }
        VkSwapchainCreateInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface_;
        info.minImageCount = std::max(2u, capabilities.minImageCount);
        if (capabilities.maxImageCount != 0) {
            info.minImageCount =
                std::min(info.minImageCount, capabilities.maxImageCount);
        }
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = extent_;
        info.imageArrayLayers = 1;
        // Transfer is all this needs; colour attachment is what overlays
        // need. The NVIDIA overlay draws into the swapchain image through
        // a layer of its own, and a swapchain that could not be rendered
        // to took the process down inside the driver on the first present.
        // Recording reads the image back as well, so transfer source too.
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (capabilities.supportedUsageFlags &
             (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = swapchain_;
        VkSwapchainKHR created = VK_NULL_HANDLE;
        window_step("create_swapchain", static_cast<long long>(info.imageFormat));
        const auto swapchain_result =
            create_swapchain_(device_, &info, nullptr, &created);
        window_step("create_swapchain_done", swapchain_result);
        if (swapchain_result != VK_SUCCESS) {
            return false;
        }
        if (swapchain_ != VK_NULL_HANDLE) {
            destroy_swapchain_(device_, swapchain_, nullptr);
        }
        swapchain_ = created;
        std::uint32_t image_count = 0;
        get_swapchain_images_(device_, swapchain_, &image_count, nullptr);
        images_.resize(image_count);
        get_swapchain_images_(device_, swapchain_, &image_count,
                              images_.data());
        return true;
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    HWND window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    VkExtent2D extent_ = {};
    VkSemaphore image_available_ = VK_NULL_HANDLE;
    VkSemaphore frame_finished_ = VK_NULL_HANDLE;
    PFN_vkCreateSemaphore create_semaphore_ = nullptr;
    PFN_vkDestroySemaphore destroy_semaphore_ = nullptr;
    std::uint32_t image_index_ = 0;
    bool started_ = false;
    bool pending_ = false;
    bool needs_rebuild_ = false;
    bool fullscreen_ = false;
    RECT windowed_rect_ = {};
    std::uint64_t recorded_ = 0;
    std::uint64_t presented_ = 0;
    int last_result_ = 0;

    PFN_vkCreateWin32SurfaceKHR create_win32_surface_ = nullptr;
    PFN_vkDestroySurfaceKHR destroy_surface_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR surface_support_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surface_capabilities_ =
        nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR surface_formats_ = nullptr;
    PFN_vkCreateSwapchainKHR create_swapchain_ = nullptr;
    PFN_vkDestroySwapchainKHR destroy_swapchain_ = nullptr;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images_ = nullptr;
    PFN_vkAcquireNextImageKHR acquire_next_image_ = nullptr;
    PFN_vkQueuePresentKHR queue_present_ = nullptr;
    PFN_vkCreateFence create_fence_ = nullptr;
    PFN_vkDestroyFence destroy_fence_ = nullptr;
    PFN_vkWaitForFences wait_for_fences_ = nullptr;
    PFN_vkResetFences reset_fences_ = nullptr;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier_ = nullptr;
    PFN_vkCmdBlitImage cmd_blit_image_ = nullptr;
};

}  // namespace ps5gpu

#endif  // PS5GPU_WINDOW_H
