// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Catui::Ps4Ipc {

constexpr unsigned Version = 1;
constexpr unsigned MaxWidth = 3840;
constexpr unsigned MaxHeight = 2160;
constexpr unsigned MaxFrameBytes = MaxWidth * MaxHeight * 4;
constexpr unsigned AudioFrames = 48000;
constexpr unsigned MaxOptions = 32;

struct Option {
    char key[64]{};
    char value[128]{};
};

struct Shared {
    unsigned version{Version};
    volatile LONG ready{}; // 1: loaded; -1: load failed
    volatile LONG stop{};
    volatile LONG paused{};
    volatile LONG published_slot{-1};
    volatile LONG reading_slot{-1};
    volatile LONG frame_serial{};
    volatile LONG audio_write{};
    volatile LONG audio_read{};
    volatile LONG rumble_strong{};
    volatile LONG rumble_weak{};
    volatile LONG input_buttons[16]{};
    volatile LONG input_axes[4]{};
    volatile LONG touch_x{};
    volatile LONG touch_y{};
    volatile LONG touch_pressed{};
    unsigned width[2]{};
    unsigned height[2]{};
    unsigned pitch[2]{};
    unsigned option_count{};
    Option options[MaxOptions]{};
    std::int16_t audio[AudioFrames * 2]{};
    std::uint8_t pixels[2][MaxFrameBytes]{};
};

inline LONG Read(volatile LONG* value) {
    return InterlockedCompareExchange(value, 0, 0);
}

inline void Write(volatile LONG* value, LONG next) {
    InterlockedExchange(value, next);
}

} // namespace Catui::Ps4Ipc
