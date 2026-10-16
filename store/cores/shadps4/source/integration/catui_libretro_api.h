// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "catui_libretro_bridge.h"

#include <cstddef>
#include <cstdint>

namespace Catui::Libretro {

// Stable C++ seam used by the volatile shadPS4 integration. The Libretro
// entry points never include emulator headers, so upstream rebases only need
// to update the backend implementation.
using BackendFactory = Backend (*)();

#ifdef CATUI_SHADPS4_REAL_BACKEND
Backend CreateShadps4Backend();
#endif

} // namespace Catui::Libretro

extern "C" {

struct retro_game_info {
    const char* path;
    const void* data;
    std::size_t size;
    const char* meta;
};

struct retro_system_info {
    const char* library_name;
    const char* library_version;
    const char* valid_extensions;
    bool need_fullpath;
    bool block_extract;
};

struct retro_game_geometry {
    unsigned base_width;
    unsigned base_height;
    unsigned max_width;
    unsigned max_height;
    float aspect_ratio;
};

struct retro_system_timing {
    double fps;
    double sample_rate;
};

struct retro_system_av_info {
    retro_game_geometry geometry;
    retro_system_timing timing;
};

struct retro_variable {
    const char* key;
    const char* value;
};

enum retro_rumble_effect : unsigned {
    RETRO_RUMBLE_STRONG = 0,
    RETRO_RUMBLE_WEAK = 1,
};

struct retro_rumble_interface {
    bool (*set_rumble_state)(unsigned port, retro_rumble_effect effect,
                             std::uint16_t strength);
};

enum retro_pixel_format : unsigned {
    RETRO_PIXEL_FORMAT_0RGB1555 = 0,
    RETRO_PIXEL_FORMAT_XRGB8888 = 1,
    RETRO_PIXEL_FORMAT_RGB565 = 2,
};

constexpr unsigned RETRO_ENVIRONMENT_SET_PIXEL_FORMAT = 10;
constexpr unsigned RETRO_ENVIRONMENT_GET_VARIABLE = 15;
constexpr unsigned RETRO_ENVIRONMENT_SET_VARIABLES = 16;
constexpr unsigned RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE = 17;
constexpr unsigned RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY = 31;
constexpr unsigned RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE = 23;

constexpr unsigned RETRO_DEVICE_JOYPAD = 1;
constexpr unsigned RETRO_DEVICE_ANALOG = 5;
constexpr unsigned RETRO_DEVICE_POINTER = 6;
constexpr unsigned RETRO_DEVICE_INDEX_ANALOG_LEFT = 0;
constexpr unsigned RETRO_DEVICE_INDEX_ANALOG_RIGHT = 1;
constexpr unsigned RETRO_DEVICE_ID_ANALOG_X = 0;
constexpr unsigned RETRO_DEVICE_ID_ANALOG_Y = 1;
constexpr unsigned RETRO_DEVICE_ID_POINTER_X = 0;
constexpr unsigned RETRO_DEVICE_ID_POINTER_Y = 1;
constexpr unsigned RETRO_DEVICE_ID_POINTER_PRESSED = 2;

using retro_video_refresh_t = void (*)(const void*, unsigned, unsigned, std::size_t);
using retro_audio_sample_t = void (*)(std::int16_t, std::int16_t);
using retro_audio_sample_batch_t = std::size_t (*)(const std::int16_t*, std::size_t);
using retro_input_poll_t = void (*)();
using retro_input_state_t = std::int16_t (*)(unsigned, unsigned, unsigned, unsigned);
using retro_environment_t = bool (*)(unsigned, void*);

#if defined(_WIN32)
#define CATUI_LIBRETRO_EXPORT __declspec(dllexport)
#else
#define CATUI_LIBRETRO_EXPORT __attribute__((visibility("default")))
#endif

CATUI_LIBRETRO_EXPORT void retro_set_environment(retro_environment_t);
CATUI_LIBRETRO_EXPORT void retro_set_video_refresh(retro_video_refresh_t);
CATUI_LIBRETRO_EXPORT void retro_set_audio_sample(retro_audio_sample_t);
CATUI_LIBRETRO_EXPORT void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
CATUI_LIBRETRO_EXPORT void retro_set_input_poll(retro_input_poll_t);
CATUI_LIBRETRO_EXPORT void retro_set_input_state(retro_input_state_t);
CATUI_LIBRETRO_EXPORT unsigned retro_api_version();
CATUI_LIBRETRO_EXPORT void retro_get_system_info(retro_system_info*);
CATUI_LIBRETRO_EXPORT void retro_get_system_av_info(retro_system_av_info*);
CATUI_LIBRETRO_EXPORT void retro_init();
CATUI_LIBRETRO_EXPORT void retro_deinit();
CATUI_LIBRETRO_EXPORT bool retro_load_game(const retro_game_info*);
CATUI_LIBRETRO_EXPORT void retro_unload_game();
CATUI_LIBRETRO_EXPORT void retro_run();
CATUI_LIBRETRO_EXPORT void retro_reset();
CATUI_LIBRETRO_EXPORT void retro_set_paused(bool paused);
CATUI_LIBRETRO_EXPORT bool retro_get_paused();
// The CATUI frontend loads the complete mandatory Libretro ABI. shadPS4 does
// not expose save states or guest memory yet, so these report unsupported.
CATUI_LIBRETRO_EXPORT std::size_t retro_serialize_size();
CATUI_LIBRETRO_EXPORT bool retro_serialize(void*, std::size_t);
CATUI_LIBRETRO_EXPORT bool retro_unserialize(const void*, std::size_t);
CATUI_LIBRETRO_EXPORT void* retro_get_memory_data(unsigned);
CATUI_LIBRETRO_EXPORT std::size_t retro_get_memory_size(unsigned);

// Private opt-in seam for the shadPS4 backend. It is intentionally not part
// of the frontend contract and can be removed/replaced on an upstream rebase.
CATUI_LIBRETRO_EXPORT void catui_libretro_bind_backend(Catui::Libretro::BackendFactory);

#undef CATUI_LIBRETRO_EXPORT
}
