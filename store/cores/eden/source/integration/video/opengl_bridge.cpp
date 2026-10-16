// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eden_libretro/video/opengl_bridge.h"
#include "eden_libretro/environment.h"
#include "common/logging.h"
#include <glad/glad.h>

namespace Libretro {

static void GLContextResetCallback() {
    OpenGLBridge::GetInstance().OnContextReset();
}

static void GLContextDestroyCallback() {
    OpenGLBridge::GetInstance().OnContextDestroy();
}

OpenGLBridge& OpenGLBridge::GetInstance() {
    static OpenGLBridge instance;
    return instance;
}

OpenGLBridge::OpenGLBridge() = default;
OpenGLBridge::~OpenGLBridge() = default;

bool OpenGLBridge::InitHardwareRender(retro_environment_t environ_cb) {
    if (!environ_cb) {
        return false;
    }

    hw_render_cb.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
    hw_render_cb.version_major = 4;
    hw_render_cb.version_minor = 6;
    hw_render_cb.context_reset = GLContextResetCallback;
    hw_render_cb.context_destroy = GLContextDestroyCallback;
    hw_render_cb.depth = true;
    hw_render_cb.stencil = true;
    hw_render_cb.bottom_left_origin = false;
    hw_render_cb.cache_context = true;

    if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render_cb)) {
        LOG_WARNING(Render_OpenGL, "Libretro frontend does not support OpenGL Core HW Render callback");
        return false;
    }

    return true;
}

void OpenGLBridge::SetVideoRefreshCallback(retro_video_refresh_t callback) {
    video_refresh_cb = callback;
}

void OpenGLBridge::OnContextReset() {
    std::lock_guard lock{bridge_mutex};
    if (hw_render_cb.get_proc_address) {
        gladLoadGLLoader(reinterpret_cast<GLADloadproc>(hw_render_cb.get_proc_address));
        hw_render_active = true;
        LOG_INFO(Render_OpenGL, "Libretro OpenGL HW Context loaded successfully via get_proc_address");
    }
}

void OpenGLBridge::OnContextDestroy() {
    std::lock_guard lock{bridge_mutex};
    hw_render_active = false;
    LOG_INFO(Render_OpenGL, "Libretro OpenGL HW Context destroyed");
}

uintptr_t OpenGLBridge::GetCurrentFramebuffer() const {
    if (hw_render_cb.get_current_framebuffer) {
        return hw_render_cb.get_current_framebuffer();
    }
    return 0;
}

void OpenGLBridge::PresentFrame(u32 width, u32 height) {
    std::lock_guard lock{bridge_mutex};
    if (!video_refresh_cb) {
        return;
    }

    if (hw_render_active) {
        video_refresh_cb(reinterpret_cast<const void*>(-1), width, height, 0);
    } else {
        video_refresh_cb(nullptr, width, height, 0);
    }
}

bool OpenGLBridge::IsHardwareRenderActive() const {
    return hw_render_active;
}

} // namespace Libretro
