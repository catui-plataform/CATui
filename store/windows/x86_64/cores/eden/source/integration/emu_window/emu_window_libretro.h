// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include "core/frontend/emu_window.h"

namespace Libretro {

class EmuWindow_Libretro final : public Core::Frontend::EmuWindow {
public:
    using FrameDisplayedCallback = std::function<void()>;

    explicit EmuWindow_Libretro();
    ~EmuWindow_Libretro() override;

    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override;
    bool IsShown() const override;
    void OnFrameDisplayed() override;

    void SetFrameDisplayedCallback(FrameDisplayedCallback callback);
    bool IsOpen() const;
    void SetOpen(bool open);

    void UpdateLayout(u32 width, u32 height);

private:
    std::atomic<bool> is_open{true};
    mutable std::mutex frame_callback_mutex;
    FrameDisplayedCallback frame_displayed_cb{nullptr};
};

} // namespace Libretro
