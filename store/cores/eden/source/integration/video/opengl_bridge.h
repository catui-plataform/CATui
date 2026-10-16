// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include "common/common_types.h"
#include "eden_libretro/libretro.h"

namespace Libretro {

class OpenGLBridge {
public:
    static OpenGLBridge& GetInstance();

    bool InitHardwareRender(retro_environment_t environ_cb);
    void SetVideoRefreshCallback(retro_video_refresh_t callback);

    void OnContextReset();
    void OnContextDestroy();

    uintptr_t GetCurrentFramebuffer() const;
    void PresentFrame(u32 width, u32 height);

    bool IsHardwareRenderActive() const;

private:
    OpenGLBridge();
    ~OpenGLBridge();

    struct retro_hw_render_callback hw_render_cb{};
    retro_video_refresh_t video_refresh_cb{nullptr};
    bool hw_render_active{false};
    mutable std::mutex bridge_mutex;
};

} // namespace Libretro
