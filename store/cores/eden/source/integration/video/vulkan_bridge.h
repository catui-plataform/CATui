// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include "common/common_types.h"
#include "eden_libretro/libretro.h"

namespace Libretro {

class VulkanBridge {
public:
    static VulkanBridge& GetInstance();

    bool InitHardwareRender(retro_environment_t environ_cb);
    void SetVideoRefreshCallback(retro_video_refresh_t callback);

    void OnContextReset();
    void OnContextDestroy();

    void PresentFrame(void* image_view, int image_layout, void* image,
                      u32 width, u32 height, void* semaphore, u32 queue_family);

    const struct retro_hw_render_interface_vulkan* GetHwInterface() const;
    bool IsHardwareRenderActive() const;

private:
    VulkanBridge();
    ~VulkanBridge();

    struct retro_hw_render_callback hw_render_cb{};
    const struct retro_hw_render_interface_vulkan* hw_interface{nullptr};
    retro_video_refresh_t video_refresh_cb{nullptr};
    bool hw_render_active{false};
    mutable std::mutex bridge_mutex;
};

} // namespace Libretro
