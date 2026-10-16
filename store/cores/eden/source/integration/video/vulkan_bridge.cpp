// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eden_libretro/video/vulkan_bridge.h"
#include "eden_libretro/environment.h"
#include "common/logging.h"

namespace Libretro {

static void ContextResetCallback() {
    VulkanBridge::GetInstance().OnContextReset();
}

static void ContextDestroyCallback() {
    VulkanBridge::GetInstance().OnContextDestroy();
}

VulkanBridge& VulkanBridge::GetInstance() {
    static VulkanBridge instance;
    return instance;
}

VulkanBridge::VulkanBridge() = default;
VulkanBridge::~VulkanBridge() = default;

bool VulkanBridge::InitHardwareRender(retro_environment_t environ_cb) {
    if (!environ_cb) {
        return false;
    }

    hw_render_cb.context_type = RETRO_HW_CONTEXT_VULKAN;
    hw_render_cb.version_major = 1;
    hw_render_cb.version_minor = 1;
    hw_render_cb.context_reset = ContextResetCallback;
    hw_render_cb.context_destroy = ContextDestroyCallback;
    hw_render_cb.depth = true;
    hw_render_cb.stencil = true;
    hw_render_cb.bottom_left_origin = false;
    hw_render_cb.cache_context = true;

    if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render_cb)) {
        LOG_WARNING(Render_Vulkan, "Libretro frontend does not support Vulkan HW Render callback");
        return false;
    }

    return true;
}

void VulkanBridge::SetVideoRefreshCallback(retro_video_refresh_t callback) {
    video_refresh_cb = callback;
}

void VulkanBridge::OnContextReset() {
    std::lock_guard lock{bridge_mutex};
    auto env_cb = Environment::GetEnvironmentCallback();
    if (!env_cb) {
        return;
    }

    const struct retro_hw_render_interface_vulkan* iface = nullptr;
    if (env_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void*)&iface) && iface) {
        hw_interface = iface;
        hw_render_active = true;
        LOG_INFO(Render_Vulkan, "Libretro Vulkan HW Interface acquired successfully");
    }
}

void VulkanBridge::OnContextDestroy() {
    std::lock_guard lock{bridge_mutex};
    hw_render_active = false;
    hw_interface = nullptr;
    LOG_INFO(Render_Vulkan, "Libretro Vulkan HW Context destroyed");
}

void VulkanBridge::PresentFrame(void* image_view, int image_layout, void* image,
                               u32 width, u32 height, void* semaphore, u32 queue_family) {
    std::lock_guard lock{bridge_mutex};
    if (!video_refresh_cb) {
        return;
    }

    if (hw_render_active && hw_interface && hw_interface->set_image) {
        struct retro_vulkan_image retro_img{};
        retro_img.image_view = image_view;
        retro_img.image_layout = image_layout;
        retro_img.image = image;

        hw_interface->set_image(hw_interface->handle, &retro_img,
                                semaphore ? 1 : 0, semaphore ? &semaphore : nullptr,
                                queue_family);

        video_refresh_cb(reinterpret_cast<const void*>(-1), width, height, 0);
    } else {
        video_refresh_cb(nullptr, width, height, 0);
    }
}

const struct retro_hw_render_interface_vulkan* VulkanBridge::GetHwInterface() const {
    return hw_interface;
}

bool VulkanBridge::IsHardwareRenderActive() const {
    return hw_render_active;
}

} // namespace Libretro
