// SPDX-License-Identifier: GPL-2.0-or-later
// Lightweight Libretro boundary. Every PS4 title runs in a disposable host
// because shadPS4's guest, Vulkan and HLE singletons are process-lifetime.
#include "catui_libretro_api.h"
#include "catui_ps4_ipc.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

namespace {
using namespace Catui::Ps4Ipc;

retro_environment_t environment{};
retro_video_refresh_t video{};
retro_audio_sample_t audio_sample{};
retro_audio_sample_batch_t audio_batch{};
retro_input_poll_t input_poll{};
retro_input_state_t input_state{};
retro_rumble_interface rumble{};
HANDLE mapping{};
PROCESS_INFORMATION host{};
Shared* shared{};
LONG last_frame_serial{};
std::vector<std::uint8_t> frame_copy{};
std::vector<std::int16_t> audio_copy{};
std::string last_error{};
std::atomic<double> host_video_fps{};
ULONGLONG fps_sample_tick{};
LONG fps_sample_serial{};

constexpr retro_variable variables[] = {
    {"shadps4_renderer_backend", "Renderer; vulkan"},
    {"shadps4_resolution_scale", "Internal Resolution; 1.0|1.5|2.0|3.0|4.0"},
    {"shadps4_present_mode", "Present Mode; fifo|mailbox|immediate"},
    {"shadps4_neo_mode", "PS4 Pro Mode; disabled|enabled"},
    {"shadps4_hdr_output", "HDR Output; disabled|enabled"},
    {"shadps4_window_width", "Output Width; 1280|1920|2560|3840"},
    {"shadps4_window_height", "Output Height; 720|1080|1440|2160"},
    {"shadps4_vblank_frequency", "VBlank Frequency; 60|30|120"},
    {"shadps4_fsr_enabled", "AMD FSR; disabled|enabled"},
    {"shadps4_rcas_enabled", "RCAS Sharpening; enabled|disabled"},
    {"shadps4_rcas_attenuation", "RCAS Attenuation; 25|0|50|75|100"},
    {"shadps4_pipeline_cache_enabled", "Vulkan Pipeline Cache; enabled|disabled"},
    {"shadps4_pipeline_cache_archived", "Archived Pipeline Cache; enabled|disabled"},
    {"shadps4_gpu_id", "Vulkan GPU Device; -1|0|1|2"},
    {"shadps4_readbacks_mode", "GPU Readbacks; disabled|synchronous|asynchronous"},
    {"shadps4_readback_linear_images_enabled", "Linear Readback Images; disabled|enabled"},
    {"shadps4_direct_memory_access_enabled", "Direct Memory Access; disabled|enabled"},
    {"shadps4_copy_gpu_buffers", "Copy GPU Buffers; disabled|enabled"},
    {"shadps4_userfaultfd_tracking", "Userfaultfd Tracking; disabled|enabled"},
    {"shadps4_vk_crash_diagnostics", "Vulkan Crash Diagnostics; disabled|enabled"},
    {"shadps4_windows_red_zone_protection", "Windows Guest Red Zone; static_patching|disabled"},
    {"shadps4_rumble", "DualShock Rumble; enabled|disabled"},
    {"shadps4_touch_motion", "Touchpad and Motion; enabled|disabled"},
    {nullptr, nullptr},
};

std::filesystem::path ModuleDirectory() {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&retro_init), &module)) return {};
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, 32768);
    return length ? std::filesystem::path(path).parent_path() : std::filesystem::path{};
}

void CopyOptions() {
    if (!environment || !shared) return;
    for (const auto& descriptor : variables) {
        if (!descriptor.key) break;
        retro_variable variable{descriptor.key, nullptr};
        if (!environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) || !variable.value) continue;
        if (shared->option_count >= MaxOptions) break;
        auto& option = shared->options[shared->option_count++];
        strncpy_s(option.key, descriptor.key, _TRUNCATE);
        strncpy_s(option.value, variable.value, _TRUNCATE);
    }
}

void CloseHost() {
    if (shared) Write(&shared->stop, 1);
    if (host.hProcess) {
        // Only the host launched by this session can be terminated here.
        if (WaitForSingleObject(host.hProcess, 10000) == WAIT_TIMEOUT) {
            TerminateProcess(host.hProcess, 1);
            WaitForSingleObject(host.hProcess, 2000);
        }
        CloseHandle(host.hProcess);
        CloseHandle(host.hThread);
        host = {};
    }
    if (shared) {
        UnmapViewOfFile(shared);
        shared = nullptr;
    }
    if (mapping) {
        CloseHandle(mapping);
        mapping = nullptr;
    }
    frame_copy.clear();
    audio_copy.clear();
    last_frame_serial = 0;
    host_video_fps = 0.0;
    fps_sample_tick = 0;
    fps_sample_serial = 0;
}

void PollInput() {
    if (!shared) return;
    if (input_poll) input_poll();
    if (!input_state) return;
    for (unsigned button = 0; button < 16; ++button) {
        Write(&shared->input_buttons[button], input_state(0, RETRO_DEVICE_JOYPAD, 0, button));
    }
    for (unsigned stick = 0; stick < 2; ++stick) {
        for (unsigned axis = 0; axis < 2; ++axis) {
            Write(&shared->input_axes[stick * 2 + axis],
                  input_state(0, RETRO_DEVICE_ANALOG, stick, axis));
        }
    }
    Write(&shared->touch_x, input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X));
    Write(&shared->touch_y, input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y));
    Write(&shared->touch_pressed,
          input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED));
}

void PollVideo() {
    if (!shared || !video) return;
    const LONG serial = Read(&shared->frame_serial);
    if (serial == last_frame_serial) return;
    const LONG slot = Read(&shared->published_slot);
    if (slot < 0 || slot > 1) return;
    Write(&shared->reading_slot, slot);
    if (Read(&shared->published_slot) != slot) {
        Write(&shared->reading_slot, -1);
        return;
    }
    const unsigned width = shared->width[slot];
    const unsigned height = shared->height[slot];
    const unsigned pitch = shared->pitch[slot];
    const std::size_t bytes = static_cast<std::size_t>(pitch) * height;
    if (width && height && width <= MaxWidth && height <= MaxHeight &&
        pitch >= width * 4 && bytes <= MaxFrameBytes) {
        frame_copy.resize(bytes);
        std::memcpy(frame_copy.data(), shared->pixels[slot], bytes);
        last_frame_serial = serial;
        Write(&shared->reading_slot, -1);
        video(frame_copy.data(), width, height, pitch);
    } else {
        Write(&shared->reading_slot, -1);
    }
}

void PollAudio() {
    if (!shared || (!audio_batch && !audio_sample)) return;
    LONG read = Read(&shared->audio_read);
    const LONG write = Read(&shared->audio_write);
    unsigned available = static_cast<unsigned>(write - read);
    if (!available) return;
    if (available > 4800u) {
        const unsigned skip = available - 2048u;
        read += static_cast<LONG>(skip);
        Write(&shared->audio_read, read);
        available = 2048u;
    }
    const unsigned frames = std::min(available, 4096u);
    audio_copy.resize(frames * 2);
    for (unsigned i = 0; i < frames; ++i) {
        const unsigned index = (static_cast<unsigned>(read) + i) % AudioFrames;
        audio_copy[i * 2] = shared->audio[index * 2];
        audio_copy[i * 2 + 1] = shared->audio[index * 2 + 1];
    }
    std::size_t consumed = frames;
    if (audio_batch) consumed = std::min(audio_batch(audio_copy.data(), frames),
                                         static_cast<std::size_t>(frames));
    else for (unsigned i = 0; i < frames; ++i)
        audio_sample(audio_copy[i * 2], audio_copy[i * 2 + 1]);
    Write(&shared->audio_read, read + static_cast<LONG>(consumed));
}
} // namespace

extern "C" {
void retro_set_environment(retro_environment_t callback) {
    environment = callback;
    if (environment) environment(RETRO_ENVIRONMENT_SET_VARIABLES,
                                  const_cast<retro_variable*>(variables));
}
void retro_set_video_refresh(retro_video_refresh_t callback) { video = callback; }
void retro_set_audio_sample(retro_audio_sample_t callback) { audio_sample = callback; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) { audio_batch = callback; }
void retro_set_input_poll(retro_input_poll_t callback) { input_poll = callback; }
void retro_set_input_state(retro_input_state_t callback) { input_state = callback; }
unsigned retro_api_version() { return 1; }
void retro_get_system_info(retro_system_info* info) {
    if (info) *info = {"shadPS4 CATUI Proxy", "isolated-boundary",
                       "elf|self|bin|zar", true, false};
}
void retro_get_system_av_info(retro_system_av_info* info) {
    if (info) *info = {{1280, 720, MaxWidth, MaxHeight, 16.0f / 9.0f}, {60.0, 48000.0}};
}
void retro_init() {
    if (environment) {
        auto format = RETRO_PIXEL_FORMAT_XRGB8888;
        environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
    }
}
void retro_deinit() { retro_unload_game(); }
bool retro_load_game(const retro_game_info* game) {
    retro_unload_game();
    last_error.clear();
    if (!game || !game->path || !std::filesystem::exists(game->path)) {
        last_error = "PS4 game executable is missing";
        return false;
    }
    const auto host_path = ModuleDirectory() / L"catui_shadps4_host.exe";
    if (!std::filesystem::exists(host_path)) {
        last_error = "PS4 host executable is missing beside the core DLL";
        return false;
    }
    const std::wstring map_name = L"Local\\CATuiPS4_" + std::to_wstring(GetCurrentProcessId()) +
                                  L"_" + std::to_wstring(GetTickCount64());
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                 static_cast<DWORD>(sizeof(Shared)), map_name.c_str());
    if (!mapping) {
        last_error = "Could not create PS4 video/audio transport";
        return false;
    }
    shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared) {
        last_error = "Could not map PS4 video/audio transport";
        CloseHost();
        return false;
    }
    new (shared) Shared{};
    CopyOptions();
    rumble = {};
    if (environment) environment(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble);
    const char* save_directory_utf8 = nullptr;
    if (environment) environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_directory_utf8);
    const std::filesystem::path save_directory =
        save_directory_utf8 && *save_directory_utf8 ? std::filesystem::path(save_directory_utf8)
                                                     : std::filesystem::path{};
    std::wstring command = L"\"" + host_path.wstring() + L"\" \"" + map_name +
                           L"\" \"" + std::filesystem::path(game->path).wstring() + L"\" \"" +
                           save_directory.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(host_path.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, host_path.parent_path().c_str(),
                        &startup, &host)) {
        last_error = "Could not start isolated PS4 host";
        CloseHost();
        return false;
    }
    const ULONGLONG deadline = GetTickCount64() + 45000;
    while (GetTickCount64() < deadline) {
        const LONG ready = Read(&shared->ready);
        if (ready == 1) {
            fps_sample_tick = GetTickCount64();
            fps_sample_serial = Read(&shared->frame_serial);
            return true;
        }
        if (ready < 0 || WaitForSingleObject(host.hProcess, 0) == WAIT_OBJECT_0) break;
        Sleep(5);
    }
    DWORD exit_code = STILL_ACTIVE;
    if (host.hProcess) GetExitCodeProcess(host.hProcess, &exit_code);
    last_error = "PS4 host failed to load the game or timed out (exit=" +
                 std::to_string(exit_code) + ")";
    std::fprintf(stderr, "%s\\n", last_error.c_str());
    CloseHost();
    return false;
}
void retro_unload_game() { CloseHost(); }
void retro_run() {
    if (!shared) return;
    if (WaitForSingleObject(host.hProcess, 0) == WAIT_OBJECT_0) {
        host_video_fps = 0.0;
        if (environment) environment(7, nullptr); // RETRO_ENVIRONMENT_SHUTDOWN
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (fps_sample_tick && now - fps_sample_tick >= 500) {
        const LONG serial = Read(&shared->frame_serial);
        const auto frames = static_cast<unsigned long>(serial) -
                            static_cast<unsigned long>(fps_sample_serial);
        host_video_fps = Read(&shared->paused) ? 0.0 :
            static_cast<double>(frames) * 1000.0 / (now - fps_sample_tick);
        fps_sample_serial = serial;
        fps_sample_tick = now;
    }
    PollInput();
    PollVideo();
    PollAudio();
    if (rumble.set_rumble_state) {
        rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG,
                                static_cast<std::uint16_t>(Read(&shared->rumble_strong)));
        rumble.set_rumble_state(0, RETRO_RUMBLE_WEAK,
                                static_cast<std::uint16_t>(Read(&shared->rumble_weak)));
    }
}
void retro_reset() {}
void retro_set_paused(bool paused) {
    if (!shared) return;
    if (paused) {
        Write(&shared->audio_read, Read(&shared->audio_write));
    }
    Write(&shared->paused, paused);
}
bool retro_get_paused() { return shared && Read(&shared->paused); }
__declspec(dllexport) std::uint32_t retro_get_process_id() {
    return host.hProcess && WaitForSingleObject(host.hProcess, 0) == WAIT_TIMEOUT
        ? host.dwProcessId : 0;
}
// Count host video publications, including frames skipped by the frontend.
__declspec(dllexport) double retro_get_game_fps() { return host_video_fps.load(); }
std::size_t retro_serialize_size() { return 0; }
bool retro_serialize(void*, std::size_t) { return false; }
bool retro_unserialize(const void*, std::size_t) { return false; }
void* retro_get_memory_data(unsigned) { return nullptr; }
std::size_t retro_get_memory_size(unsigned) { return 0; }
const char* retro_get_last_error() { return last_error.c_str(); }
}
