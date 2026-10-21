// SPDX-License-Identifier: GPL-2.0-or-later
#include "catui_libretro_api.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <string_view>

namespace {
retro_environment_t g_environment{};
retro_video_refresh_t g_video{};
retro_audio_sample_t g_audio{};
retro_audio_sample_batch_t g_audio_batch{};
retro_input_poll_t g_input_poll{};
retro_input_state_t g_input_state{};
retro_rumble_interface g_rumble{};
#ifdef CATUI_SHADPS4_REAL_BACKEND
Catui::Libretro::BackendFactory g_factory{Catui::Libretro::CreateShadps4Backend};
#else
Catui::Libretro::BackendFactory g_factory{};
#endif
Catui::Libretro::Bridge* g_bridge{};
bool g_paused{};
unsigned g_width = 1280;
unsigned g_height = 720;

constexpr retro_variable k_variables[] = {
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

const char* GetVariable(const char* key) {
    if (!g_environment) return nullptr;
    retro_variable variable{key, nullptr};
    return g_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) ? variable.value : nullptr;
}

bool GetBool(const char* key, bool fallback) {
    const char* value = GetVariable(key);
    if (!value) return fallback;
    return std::strcmp(value, "enabled") == 0 || std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "1") == 0;
}

template <typename T>
T GetInteger(const char* key, T fallback) {
    const char* value = GetVariable(key);
    if (!value) return fallback;
    T parsed{};
    const std::string_view text{value};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? parsed : fallback;
}

std::string GetString(const char* key, std::string fallback) {
    if (const char* value = GetVariable(key)) return value;
    return fallback;
}

void ConfigureSaveDirectory() {
    if (!g_environment) return;
    const char* save_directory = nullptr;
    if (!g_environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_directory) ||
        !save_directory || !*save_directory) {
        return;
    }
#if defined(_WIN32)
    _putenv_s("CATUI_SHADPS4_HOME_DIR", save_directory);
#else
    setenv("CATUI_SHADPS4_HOME_DIR", save_directory, 1);
#endif
}

Catui::Libretro::VideoSettings ReadSettings() {
    Catui::Libretro::VideoSettings settings{};
    settings.renderer_backend = GetString("shadps4_renderer_backend", "vulkan");
    settings.resolution_scale = GetString("shadps4_resolution_scale", "1.0");
    settings.present_mode = GetString("shadps4_present_mode", "fifo");
    settings.readbacks_mode = GetString("shadps4_readbacks_mode", "disabled");
    settings.windows_red_zone_protection =
        GetString("shadps4_windows_red_zone_protection", "static_patching");
    settings.window_width = GetInteger<unsigned>("shadps4_window_width", 1280);
    settings.window_height = GetInteger<unsigned>("shadps4_window_height", 720);
    settings.vblank_frequency = GetInteger<unsigned>("shadps4_vblank_frequency", 60);
    settings.gpu_id = GetInteger<int>("shadps4_gpu_id", -1);
    settings.rcas_attenuation = GetInteger<int>("shadps4_rcas_attenuation", 25);
    settings.neo_mode = GetBool("shadps4_neo_mode", false);
    settings.hdr_output = GetBool("shadps4_hdr_output", false);
    settings.fsr_enabled = GetBool("shadps4_fsr_enabled", false);
    settings.rcas_enabled = GetBool("shadps4_rcas_enabled", true);
    settings.pipeline_cache_enabled = GetBool("shadps4_pipeline_cache_enabled", true);
    settings.pipeline_cache_archived = GetBool("shadps4_pipeline_cache_archived", true);
    settings.copy_gpu_buffers = GetBool("shadps4_copy_gpu_buffers", false);
    settings.readback_linear_images_enabled =
        GetBool("shadps4_readback_linear_images_enabled", false);
    settings.direct_memory_access_enabled =
        GetBool("shadps4_direct_memory_access_enabled", false);
    settings.userfaultfd_tracking = GetBool("shadps4_userfaultfd_tracking", false);
    settings.vk_crash_diagnostics = GetBool("shadps4_vk_crash_diagnostics", false);
    settings.rumble = GetBool("shadps4_rumble", true);
    settings.touch_motion = GetBool("shadps4_touch_motion", true);
    return settings;
}
}

extern "C" {

void retro_set_environment(retro_environment_t cb) {
    g_environment = cb;
    if (g_environment) {
        g_environment(RETRO_ENVIRONMENT_SET_VARIABLES,
                      const_cast<retro_variable*>(k_variables));
    }
}
void retro_set_video_refresh(retro_video_refresh_t cb) { g_video = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { g_audio = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { g_audio_batch = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { g_input_poll = cb; }
void retro_set_input_state(retro_input_state_t cb) { g_input_state = cb; }

unsigned retro_api_version() { return 1; }

void retro_get_system_info(retro_system_info* info) {
    if (!info) return;
    *info = {"shadPS4 CATUI (Vulkan)", "isolated-boundary", "elf|self|bin|zar", true, false};
}

void retro_get_system_av_info(retro_system_av_info* info) {
    if (!info) return;
    info->geometry = {g_width, g_height, 3840, 2160,
                      static_cast<float>(g_width) / static_cast<float>(g_height)};
    info->timing = {60.0, 48000.0};
}

void retro_init() {
    if (g_environment) {
        auto format = RETRO_PIXEL_FORMAT_XRGB8888;
        g_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
    }
    retro_system_av_info info{};
    retro_get_system_av_info(&info);
    g_width = info.geometry.base_width;
    g_height = info.geometry.base_height;
}

void retro_deinit() {
    retro_unload_game();
}

void retro_reset() {}

bool retro_load_game(const retro_game_info* game) {
    retro_unload_game();
    if (!game || !game->path || !g_factory) return false;

    // CATUI owns the per-profile runtime directory. Pass it to the isolated
    // shadPS4 backend before it initializes its process-wide settings object.
    ConfigureSaveDirectory();

    auto* bridge = new Catui::Libretro::Bridge(g_factory());
    if (!g_video && !g_audio_batch) {
        delete bridge;
        return false;
    }

    Catui::Libretro::Callbacks callbacks{};
    callbacks.video = [](void*, const Catui::Libretro::VideoFrame& frame) {
        if (g_video && frame.data) g_video(frame.data, frame.width, frame.height, frame.pitch);
    };
    callbacks.audio = [](void*, const std::int16_t* samples, std::size_t frames) {
        if (g_paused) return;
        if (g_audio_batch && samples) g_audio_batch(samples, frames);
        else if (g_audio && samples) {
            for (std::size_t i = 0; i < frames; ++i) g_audio(samples[i * 2], samples[i * 2 + 1]);
        }
    };
    callbacks.input = [](void*, std::uint32_t port) {
        Catui::Libretro::InputState state{};
        if (g_input_poll) g_input_poll();
        if (g_input_state) {
            for (unsigned i = 0; i < 16; ++i)
                state.buttons[i] = g_input_state(port, RETRO_DEVICE_JOYPAD, 0, i);
            state.axes[0] = g_input_state(port, RETRO_DEVICE_ANALOG,
                                          RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                          RETRO_DEVICE_ID_ANALOG_X);
            state.axes[1] = g_input_state(port, RETRO_DEVICE_ANALOG,
                                          RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                          RETRO_DEVICE_ID_ANALOG_Y);
            state.axes[2] = g_input_state(port, RETRO_DEVICE_ANALOG,
                                          RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                                          RETRO_DEVICE_ID_ANALOG_X);
            state.axes[3] = g_input_state(port, RETRO_DEVICE_ANALOG,
                                          RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                                          RETRO_DEVICE_ID_ANALOG_Y);
            state.touch_x = g_input_state(port, RETRO_DEVICE_POINTER, 0,
                                          RETRO_DEVICE_ID_POINTER_X);
            state.touch_y = g_input_state(port, RETRO_DEVICE_POINTER, 0,
                                          RETRO_DEVICE_ID_POINTER_Y);
            state.touch_pressed = g_input_state(port, RETRO_DEVICE_POINTER, 0,
                                                RETRO_DEVICE_ID_POINTER_PRESSED) != 0;
        }
        return state;
    };

    const auto settings = ReadSettings();
    if (settings.renderer_backend != "vulkan") {
        delete bridge;
        return false;
    }
    g_rumble = {};
    if (settings.rumble && g_environment &&
        g_environment(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &g_rumble) &&
        g_rumble.set_rumble_state) {
        callbacks.rumble = [](void*, std::uint32_t port, std::uint16_t strong,
                              std::uint16_t weak) {
            const bool strong_ok =
                g_rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, strong);
            const bool weak_ok = g_rumble.set_rumble_state(port, RETRO_RUMBLE_WEAK, weak);
            return strong_ok && weak_ok;
        };
    }
    g_width = settings.window_width;
    g_height = settings.window_height;
    g_bridge = bridge;
    g_bridge->SetCallbacks(callbacks);
    g_bridge->SetVideoSettings(settings);
    if (!g_bridge->Load(std::filesystem::path(game->path))) {
        delete g_bridge;
        g_bridge = nullptr;
        return false;
    }
    return true;
}

void retro_unload_game() {
    if (g_bridge) g_bridge->SetPaused(false);
    delete g_bridge;
    g_bridge = nullptr;
    g_paused = false;
    g_rumble = {};
}

void retro_run() {
    if (g_bridge && !g_paused) g_bridge->RunFrame();
}

void retro_set_paused(bool paused) {
    if (g_paused == paused) return;
    g_paused = paused;
    if (g_bridge) g_bridge->SetPaused(paused);
}

bool retro_get_paused() { return g_paused; }

std::size_t retro_serialize_size() { return 0; }

bool retro_serialize(void*, std::size_t) { return false; }

bool retro_unserialize(const void*, std::size_t) { return false; }

void* retro_get_memory_data(unsigned) { return nullptr; }

std::size_t retro_get_memory_size(unsigned) { return 0; }

void catui_libretro_bind_backend(Catui::Libretro::BackendFactory factory) { g_factory = factory; }

} // extern "C"
