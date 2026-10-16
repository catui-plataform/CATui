// Small ABI/lifecycle smoke test. It does not emulate a PS4 title.
#include "catui_libretro_api.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <vector>

namespace {
Catui::Libretro::Callbacks g_callbacks{};
std::vector<std::uint32_t> g_frame;
unsigned g_frame_count{};
std::uint32_t g_first_pixel{};
Catui::Libretro::VideoSettings g_settings{};
bool g_variables_registered{};
bool g_xrgb8888_negotiated{};
bool g_analog_polled{};
bool g_touch_polled{};
bool g_rumble_strong{};
bool g_rumble_weak{};
bool g_backend_paused{};

bool load(void*, const std::filesystem::path&, const Catui::Libretro::VideoSettings& settings,
          const Catui::Libretro::Callbacks& callbacks) {
    g_callbacks = callbacks;
    g_settings = settings;
    return true;
}
bool run(void*) {
    if (g_callbacks.input) (void)g_callbacks.input(g_callbacks.user, 0);
    if (g_callbacks.rumble) (void)g_callbacks.rumble(g_callbacks.user, 0, 0x8080, 0x4040);
    if (g_callbacks.video) {
        static std::uint32_t pixels[1600 * 900];
        const std::uint32_t color = g_frame_count == 0 ? 0xff203040u : 0xffc08020u;
        std::fill(std::begin(pixels), std::end(pixels), color);
        Catui::Libretro::VideoFrame frame{pixels, 1600, 900, 1600 * sizeof(std::uint32_t),
                                          Catui::Libretro::PixelFormat::XRGB8888, g_frame_count};
        g_callbacks.video(g_callbacks.user, frame);
    }
    return true;
}
void unload(void*) {}
void set_paused(void*, bool paused) { g_backend_paused = paused; }
Catui::Libretro::Backend backend() {
    return {nullptr, load, run, unload, nullptr, set_paused};
}

void video(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    g_frame.resize(width * height);
    for (unsigned row = 0; row < height; ++row) {
        std::memcpy(g_frame.data() + row * width,
                    static_cast<const std::byte*>(data) + row * pitch,
                    width * sizeof(std::uint32_t));
    }
    if (g_frame_count == 0) g_first_pixel = g_frame.front();
    ++g_frame_count;
}

bool environment(unsigned command, void* data) {
    if (command == RETRO_ENVIRONMENT_SET_VARIABLES) {
        g_variables_registered = data != nullptr;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
        g_xrgb8888_negotiated =
            data && *static_cast<retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
        return g_xrgb8888_negotiated;
    }
    if (command == RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE) {
        if (!data) return false;
        auto* rumble = static_cast<retro_rumble_interface*>(data);
        rumble->set_rumble_state = [](unsigned port, retro_rumble_effect effect,
                                      std::uint16_t strength) {
            if (port != 0) return false;
            if (effect == RETRO_RUMBLE_STRONG && strength == 0x8080) g_rumble_strong = true;
            if (effect == RETRO_RUMBLE_WEAK && strength == 0x4040) g_rumble_weak = true;
            return true;
        };
        return true;
    }
    if (command != RETRO_ENVIRONMENT_GET_VARIABLE || !data) return false;

    auto* variable = static_cast<retro_variable*>(data);
    const std::string_view key{variable->key ? variable->key : ""};
    if (key == "shadps4_renderer_backend") variable->value = "vulkan";
    else if (key == "shadps4_window_width") variable->value = "1600";
    else if (key == "shadps4_window_height") variable->value = "900";
    else if (key == "shadps4_resolution_scale") variable->value = "1.5";
    else if (key == "shadps4_neo_mode") variable->value = "enabled";
    else if (key == "shadps4_windows_red_zone_protection") variable->value = "static_patching";
    else variable->value = nullptr;
    return variable->value != nullptr;
}

std::int16_t input(unsigned, unsigned device, unsigned, unsigned) {
    if (device == RETRO_DEVICE_ANALOG) g_analog_polled = true;
    if (device == RETRO_DEVICE_POINTER) g_touch_polled = true;
    return 0;
}
} // namespace

int main() {
    catui_libretro_bind_backend(backend);
    retro_set_environment(environment);
    retro_set_video_refresh(video);
    retro_set_input_state(input);
    retro_init();
    retro_game_info game{"smoke.elf", nullptr, 0, nullptr};
    if (!retro_load_game(&game)) return 2;
    retro_run();
    retro_set_paused(true);
    if (!retro_get_paused() || !g_backend_paused) return 9;
    retro_run();
    if (g_frame_count != 1) return 10;
    retro_set_paused(false);
    if (retro_get_paused() || g_backend_paused) return 11;
    retro_run();
    retro_unload_game();
    if (!g_variables_registered || !g_xrgb8888_negotiated) return 3;
    if (g_settings.renderer_backend != "vulkan" || g_settings.window_width != 1600 ||
        g_settings.window_height != 900 || g_settings.resolution_scale != "1.5" ||
        !g_settings.neo_mode || g_settings.windows_red_zone_protection != "static_patching") return 4;
    if (!g_analog_polled || !g_touch_polled) return 5;
    if (!g_rumble_strong || !g_rumble_weak) return 8;
    if (g_frame_count != 2 || g_frame.size() != 1600 * 900 ||
        g_frame.front() == g_first_pixel) return 6;

    const char* output = std::getenv("CATUI_SMOKE_OUTPUT");
    std::ofstream out(output ? output : "catui-libretro-smoke.ppm", std::ios::binary);
    out << "P6\n1600 900\n255\n";
    for (std::uint32_t pixel : g_frame) {
        out.put(static_cast<char>((pixel >> 16) & 0xff));
        out.put(static_cast<char>((pixel >> 8) & 0xff));
        out.put(static_cast<char>(pixel & 0xff));
    }
    return out.good() ? 0 : 7;
}
