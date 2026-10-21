#include "catui_libretro_api.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace {
std::filesystem::path first_output = "catui-real-first.png";
std::filesystem::path last_output = "catui-real-last.png";
std::atomic_uint total_frames{};
std::atomic_uint visible_frames{};
std::atomic_uint audio_frames{};
std::atomic_uint64_t first_hash{};
std::atomic_uint64_t last_hash{};
std::atomic_bool capture_enabled{};
std::mutex capture_mutex{};

std::uint64_t Hash(const std::vector<std::uint8_t>& pixels) {
    std::uint64_t value = 1469598103934665603ull;
    for (const auto byte : pixels) {
        value ^= byte;
        value *= 1099511628211ull;
    }
    return value;
}

void Video(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    ++total_frames;
    if (!capture_enabled.load(std::memory_order_acquire) || !data || !width || !height) return;

    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    std::uint64_t intensity{};
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(data) + y * pitch;
        for (unsigned x = 0; x < width; ++x) {
            const auto source = static_cast<std::size_t>(x) * 4;
            const auto destination = (static_cast<std::size_t>(y) * width + x) * 3;
            rgb[destination] = row[source + 2];
            rgb[destination + 1] = row[source + 1];
            rgb[destination + 2] = row[source];
            intensity += rgb[destination] + rgb[destination + 1] + rgb[destination + 2];
        }
    }

    // Ignore swapchain clears and nearly-black boot frames. A valid capture
    // must average more than eight intensity steps per RGB channel.
    if (intensity <= static_cast<std::uint64_t>(width) * height * 3 * 8) return;

    const auto hash = Hash(rgb);
    std::scoped_lock lock{capture_mutex};
    if (visible_frames.load() == 0) {
        first_hash.store(hash);
        stbi_write_png(first_output.string().c_str(), static_cast<int>(width),
                       static_cast<int>(height), 3, rgb.data(), static_cast<int>(width * 3));
    }
    last_hash.store(hash);
    stbi_write_png(last_output.string().c_str(), static_cast<int>(width),
                   static_cast<int>(height), 3, rgb.data(), static_cast<int>(width * 3));
    ++visible_frames;
}

bool Environment(unsigned command, void* data) {
    if (command == RETRO_ENVIRONMENT_SET_VARIABLES || command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT)
        return true;
    if (command != RETRO_ENVIRONMENT_GET_VARIABLE || !data) return false;
    auto* variable = static_cast<retro_variable*>(data);
    const std::string key = variable->key ? variable->key : "";
    if (key == "shadps4_renderer_backend") variable->value = "vulkan";
    else if (key == "shadps4_window_width") variable->value = "1280";
    else if (key == "shadps4_window_height") variable->value = "720";
    else if (key == "shadps4_windows_red_zone_protection") variable->value = "static_patching";
    else variable->value = nullptr;
    return variable->value != nullptr;
}

std::int16_t Input(unsigned, unsigned, unsigned, unsigned) { return 0; }
void Poll() {}
std::size_t Audio(const std::int16_t*, std::size_t count) {
    audio_frames.fetch_add(static_cast<unsigned>(count), std::memory_order_relaxed);
    return count;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    const char* second_game = nullptr;
    if (argc > 2) {
        const std::filesystem::path candidate_output = argv[2];
        auto ext = candidate_output.extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".png") {
            first_output = candidate_output;
            if (argc > 3) second_game = argv[3];
        } else {
            second_game = argv[2];
        }
    }
    last_output = first_output.parent_path() /
                  (first_output.stem().string() + ".last" + first_output.extension().string());

    retro_set_environment(Environment);
    retro_set_video_refresh(Video);
    retro_set_input_state(Input);
    retro_set_input_poll(Poll);
    retro_set_audio_sample_batch(Audio);
    int result = 0;
    const int runs = second_game ? 2 : 1;
    for (int run = 0; run < runs; ++run) {
        total_frames = 0;
        visible_frames = 0;
        audio_frames = 0;
        first_hash = 0;
        last_hash = 0;
        capture_enabled = false;
        retro_init();
        retro_game_info game{run == 0 ? argv[1] : second_game, nullptr, 0, nullptr};
        if (!retro_load_game(&game)) {
            result = 3;
            break;
        }

        auto next_frame = std::chrono::steady_clock::now();
        for (int i = 0; i < 1200; ++i) {
            if (i == 500) capture_enabled.store(true, std::memory_order_release);
            retro_run();
            next_frame += std::chrono::microseconds(16667);
            const auto now = std::chrono::steady_clock::now();
            if (next_frame < now) next_frame = now;
            std::this_thread::sleep_until(next_frame);
            if (visible_frames.load() >= 2 && first_hash.load() != last_hash.load()) break;
        }

        retro_unload_game();
        retro_deinit();
        std::fprintf(stderr, "run=%d frames_total=%u frames_visible=%u audio_frames=%u first_hash=%llu last_hash=%llu\n",
                     run + 1, total_frames.load(), visible_frames.load(), audio_frames.load(),
                     static_cast<unsigned long long>(first_hash.load()),
                     static_cast<unsigned long long>(last_hash.load()));
        if (visible_frames.load() < 2) result = 4;
        else if (first_hash.load() == last_hash.load()) result = 5;
        else if (audio_frames.load() == 0) result = 6;
        if (result != 0) break;
    }
    std::fprintf(stderr, "probe_result=%d\n", result);
    // The standalone shadPS4 process uses a process boundary because its
    // Vulkan/HLE singletons are intentionally process-lifetime objects. The
    // Libretro lifecycle itself was already exercised by retro_unload_game
    // and retro_deinit above. TerminateProcess avoids DLL_PROCESS_DETACH
    // re-entering upstream singleton destructors after the embedded owner has
    // already shut down; this is only the probe process boundary.
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
#else
    std::_Exit(result);
#endif
}
