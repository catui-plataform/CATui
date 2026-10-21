// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eden_libretro/emu_window/emu_window_libretro.h"
#include "eden_libretro/video/graphics_context_libretro.h"

namespace Libretro {

EmuWindow_Libretro::EmuWindow_Libretro() {
    window_info.type = Core::Frontend::WindowSystemType::Headless;
    window_info.display_connection = nullptr;
    window_info.render_surface = nullptr;
    window_info.render_surface_scale = 1.0f;

    // Default Switch 720p handheld / 1080p docked layout
    UpdateLayout(1280, 720);
}

EmuWindow_Libretro::~EmuWindow_Libretro() = default;

std::unique_ptr<Core::Frontend::GraphicsContext> EmuWindow_Libretro::CreateSharedContext() const {
    return std::make_unique<LibretroGraphicsContext>();
}

bool EmuWindow_Libretro::IsShown() const {
    return is_open.load();
}

void EmuWindow_Libretro::OnFrameDisplayed() {
    FrameDisplayedCallback callback;
    {
        std::lock_guard lock{frame_callback_mutex};
        callback = frame_displayed_cb;
    }
    if (callback) {
        callback();
    }
}

void EmuWindow_Libretro::SetFrameDisplayedCallback(FrameDisplayedCallback callback) {
    std::lock_guard lock{frame_callback_mutex};
    frame_displayed_cb = std::move(callback);
}

bool EmuWindow_Libretro::IsOpen() const {
    return is_open.load();
}

void EmuWindow_Libretro::SetOpen(bool open) {
    is_open.store(open);
}

void EmuWindow_Libretro::UpdateLayout(u32 width, u32 height) {
    NotifyClientAreaSizeChanged({width, height});
    UpdateCurrentFramebufferLayout(width, height);
}

} // namespace Libretro
