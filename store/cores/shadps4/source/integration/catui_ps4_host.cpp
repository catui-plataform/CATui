// SPDX-License-Identifier: GPL-2.0-or-later
// Disposable process for the real shadPS4 DLL. No standalone UI is created.
#include "catui_libretro_api.h"
#include "catui_ps4_ipc.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <mmsystem.h>

namespace {
using namespace Catui::Ps4Ipc;
Shared* shared{};

void Video(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    if (!shared || !data || !width || !height || width > MaxWidth || height > MaxHeight ||
        pitch < width * 4 || pitch * height > MaxFrameBytes) return;
    const LONG published = Read(&shared->published_slot);
    const LONG slot = published == 0 ? 1 : 0;
    if (Read(&shared->reading_slot) == slot) return;
    std::memcpy(shared->pixels[slot], data, pitch * height);
    shared->width[slot] = width;
    shared->height[slot] = height;
    shared->pitch[slot] = static_cast<unsigned>(pitch);
    Write(&shared->published_slot, slot);
    InterlockedIncrement(&shared->frame_serial);
}

std::size_t Audio(const std::int16_t* data, std::size_t frames) {
    if (!shared || !data || !frames) return frames;
    LONG write = Read(&shared->audio_write);
    const LONG read = Read(&shared->audio_read);
    const unsigned queued = static_cast<unsigned>(write - read);
    const unsigned writable = queued < AudioFrames ? AudioFrames - queued : 0;
    const unsigned count = static_cast<unsigned>(std::min(frames, static_cast<std::size_t>(writable)));
    for (unsigned i = 0; i < count; ++i) {
        const unsigned index = (static_cast<unsigned>(write) + i) % AudioFrames;
        shared->audio[index * 2] = data[i * 2];
        shared->audio[index * 2 + 1] = data[i * 2 + 1];
    }
    Write(&shared->audio_write, write + static_cast<LONG>(count));
    return frames;
}

std::int16_t Input(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (!shared || port != 0) return 0;
    if (device == RETRO_DEVICE_JOYPAD && id < 16)
        return static_cast<std::int16_t>(Read(&shared->input_buttons[id]));
    if (device == RETRO_DEVICE_ANALOG && index < 2 && id < 2)
        return static_cast<std::int16_t>(Read(&shared->input_axes[index * 2 + id]));
    if (device == RETRO_DEVICE_POINTER) {
        if (id == RETRO_DEVICE_ID_POINTER_X) return static_cast<std::int16_t>(Read(&shared->touch_x));
        if (id == RETRO_DEVICE_ID_POINTER_Y) return static_cast<std::int16_t>(Read(&shared->touch_y));
        if (id == RETRO_DEVICE_ID_POINTER_PRESSED) return Read(&shared->touch_pressed) != 0;
    }
    return 0;
}

bool Rumble(unsigned, retro_rumble_effect effect, std::uint16_t strength) {
    if (!shared) return false;
    Write(effect == RETRO_RUMBLE_STRONG ? &shared->rumble_strong : &shared->rumble_weak,
          strength);
    return true;
}

bool Environment(unsigned command, void* data) {
    if (command == RETRO_ENVIRONMENT_SET_VARIABLES || command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT)
        return true;
    if (command == RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE && data) {
        static retro_rumble_interface rumble{Rumble};
        *static_cast<retro_rumble_interface*>(data) = rumble;
        return true;
    }
    if (command != RETRO_ENVIRONMENT_GET_VARIABLE || !data || !shared) return false;
    auto* variable = static_cast<retro_variable*>(data);
    if (!variable->key) return false;
    for (unsigned i = 0; i < shared->option_count && i < MaxOptions; ++i) {
        if (std::strcmp(variable->key, shared->options[i].key) == 0) {
            variable->value = shared->options[i].value;
            return true;
        }
    }
    variable->value = nullptr;
    return false;
}

void Poll() {}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) return 2;
    timeBeginPeriod(1);
    if (!SetEnvironmentVariableW(L"CATUI_SHADPS4_HOME_DIR", argv[3])) return 2;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, argv[1]);
    if (!mapping) return 3;
    shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared || shared->version != Version) return 4;

    retro_set_environment(Environment);
    retro_set_video_refresh(Video);
    retro_set_audio_sample_batch(Audio);
    retro_set_input_state(Input);
    retro_set_input_poll(Poll);
    retro_init();
    const std::string content = std::filesystem::path(argv[2]).string();
    retro_game_info game{content.c_str(), nullptr, 0, nullptr};
    if (!retro_load_game(&game)) {
        Write(&shared->ready, -1);
        TerminateProcess(GetCurrentProcess(), 5);
    }
    Write(&shared->ready, 1);

    bool was_paused = false;
    auto next_frame = std::chrono::steady_clock::now();
    while (!Read(&shared->stop)) {
        const bool paused = Read(&shared->paused) != 0;
        if (paused != was_paused) {
            retro_set_paused(paused);
            was_paused = paused;
        }
        if (!paused) retro_run();
        next_frame += std::chrono::microseconds(16667);
        const auto now = std::chrono::steady_clock::now();
        if (next_frame < now) next_frame = now;
        std::this_thread::sleep_until(next_frame);
    }
    retro_set_paused(false);
    retro_unload_game();
    retro_deinit();
    Write(&shared->ready, 0);
    // Upstream process-lifetime singletons must not be re-entered at DLL detach.
    TerminateProcess(GetCurrentProcess(), 0);
}
