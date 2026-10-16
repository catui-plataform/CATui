// SPDX-License-Identifier: GPL-2.0-or-later
// CPU readback transport. This is deliberately not advertised as GPU sharing.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace CatuiProxy {
constexpr unsigned Version = 3, Ports = 4, MaxWidth = 1920, MaxHeight = 1080;
constexpr unsigned MaxFrameBytes = MaxWidth * MaxHeight * 4, AudioFrames = 48000;
struct Shared {
    unsigned version{Version};
    volatile LONG ready{}, stop{}, paused{}, frame_serial{}, published_slot{-1};
    volatile LONG slot_lock[2]{};
    volatile LONG audio_write{}, audio_read{}, audio_rate{48000};
    volatile LONG connected[Ports]{1, 0, 0, 0};
    volatile LONG input_buttons[Ports][16]{}, input_axes[Ports][4]{};
    volatile LONG rumble_strong[Ports]{}, rumble_weak[Ports]{};
    char error[1024]{};
    unsigned width[2]{}, height[2]{}, pitch[2]{};
    std::int16_t audio[AudioFrames * 2]{};
    std::uint8_t pixels[2][MaxFrameBytes]{};
};
inline LONG read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }
inline void write(volatile LONG* p, LONG n) { InterlockedExchange(p, n); }
inline Shared* transport = nullptr;
inline HANDLE mapping = nullptr;
inline bool active() { return transport != nullptr; }
inline void fail(const char* message) {
    if (!active()) return;
    std::strncpy(transport->error, message, sizeof(transport->error)-1);
    write(&transport->ready, -1);
}
inline bool open() {
    wchar_t name[256]{};
    if (!GetEnvironmentVariableW(L"CATUI_RPCS3_IPC", name, 256)) return false;
    mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping) return false;
    transport = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!transport || transport->version != Version) { transport = nullptr; CloseHandle(mapping); mapping = nullptr; return false; }
    return true;
}
inline void video(const std::uint8_t* data, unsigned pitch, unsigned w, unsigned h, bool bgra) {
    if (!active() || !w || !h || w > MaxWidth || h > MaxHeight || pitch < w * 4) return;
    auto* s = transport;
    const int slot = read(&s->published_slot) == 0 ? 1 : 0;
    if (InterlockedCompareExchange(&s->slot_lock[slot], 1, 0)) return;
    auto* dst = s->pixels[slot];
    for (unsigned y = 0; y < h; ++y) {
        const auto* row = data + y * pitch;
        for (unsigned x = 0; x < w; ++x) {
            // Libretro XRGB8888 is B,G,R,X in little-endian memory.
            *dst++ = row[x * 4 + (bgra ? 0 : 2)];
            *dst++ = row[x * 4 + 1];
            *dst++ = row[x * 4 + (bgra ? 2 : 0)];
            *dst++ = 255;
        }
    }
    s->width[slot] = w; s->height[slot] = h; s->pitch[slot] = w * 4;
    write(&s->slot_lock[slot], 0);
    write(&s->published_slot, slot);
    InterlockedIncrement(&s->frame_serial);
    write(&s->ready, 1); // Readiness means a real emulator frame was published.
}
inline void audio(void* buffer, unsigned bytes, unsigned sample_size, unsigned channels, unsigned rate) {
    if (!active() || !buffer || channels < 2 || (sample_size != 2 && sample_size != 4)) return;
    auto* s = transport;
    const unsigned frames = bytes / (sample_size * channels);
    const unsigned w = static_cast<unsigned>(read(&s->audio_write));
    const unsigned r = static_cast<unsigned>(read(&s->audio_read));
    const unsigned available = AudioFrames - std::min(AudioFrames, w - r);
    const unsigned count = std::min(frames, available);
    write(&s->audio_rate, rate);
    for (unsigned i = 0; i < count; ++i) for (unsigned c = 0; c < 2; ++c) {
        const unsigned src = i * channels + c;
        const auto value = sample_size == 2 ? static_cast<const std::int16_t*>(buffer)[src] :
            static_cast<std::int16_t>(std::lround(std::clamp(static_cast<const float*>(buffer)[src], -1.0f, 1.0f) * 32767));
        s->audio[((w + i) % AudioFrames) * 2 + c] = value;
    }
    write(&s->audio_write, static_cast<LONG>(w + count));
    std::memset(buffer, 0, bytes); // Host device clocks audio; CATUI owns audible output.
}
}
#else
namespace CatuiProxy { constexpr unsigned Ports = 4; inline bool active() { return false; } }
#endif
