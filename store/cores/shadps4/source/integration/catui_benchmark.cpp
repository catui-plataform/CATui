#include "catui_libretro_api.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
std::string preset = "balanced";
std::filesystem::path output = "catui-benchmark.json";
std::atomic_uint frames{};
std::atomic_uint visible{};
std::atomic_uint audio_frames{};
std::uint64_t first_hash{};
std::uint64_t last_hash{};
std::uint64_t Hash(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t value = 1469598103934665603ull;
    for (const auto byte : bytes) {
        value ^= byte;
        value *= 1099511628211ull;
    }
    return value;
}
void Video(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    ++frames;
    if (!data || !width || !height) return;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    std::uint64_t intensity{};
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(data) + y * pitch;
        for (unsigned x = 0; x < width; ++x) {
            const auto s = static_cast<std::size_t>(x) * 4;
            const auto d = (static_cast<std::size_t>(y) * width + x) * 3;
            rgb[d] = row[s + 2]; rgb[d + 1] = row[s + 1]; rgb[d + 2] = row[s];
            intensity += rgb[d] + rgb[d + 1] + rgb[d + 2];
        }
    }
    if (intensity <= static_cast<std::uint64_t>(width) * height * 3 * 8) return;
    const auto hash = Hash(rgb);
    if (visible++ == 0) first_hash = hash;
    last_hash = hash;
}
bool Environment(unsigned command, void* data) {
    if (command == RETRO_ENVIRONMENT_SET_VARIABLES || command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT)
        return true;
    if (command != RETRO_ENVIRONMENT_GET_VARIABLE || !data) return false;
    auto* variable = static_cast<retro_variable*>(data);
    const std::string key = variable->key ? variable->key : "";
    if (key == "shadps4_renderer_backend") variable->value = "vulkan";
    else if (key == "shadps4_resolution_scale") variable->value = preset == "quality" ? "2.0" : "1.0";
    else if (key == "shadps4_present_mode") variable->value = preset == "performance" ? "mailbox" : "fifo";
    else if (key == "shadps4_fsr_enabled") variable->value = preset == "quality" ? "enabled" : "disabled";
    else if (key == "shadps4_rcas_enabled") variable->value = preset == "performance" ? "disabled" : "enabled";
    else if (key == "shadps4_windows_red_zone_protection") variable->value = "static_patching";
    else if (key == "shadps4_window_width") variable->value = "1280";
    else if (key == "shadps4_window_height") variable->value = "720";
    else variable->value = nullptr;
    return variable->value != nullptr;
}
std::int16_t Input(unsigned, unsigned, unsigned, unsigned) { return 0; }
void Poll() {}
std::size_t Audio(const std::int16_t*, std::size_t count) { audio_frames += static_cast<unsigned>(count); return count; }
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 2;
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    if (argc > 3) preset = argv[3];
    if (argc > 4) output = argv[4];
    retro_set_environment(Environment);
    retro_set_video_refresh(Video);
    retro_set_input_state(Input);
    retro_set_input_poll(Poll);
    retro_set_audio_sample_batch(Audio);
    retro_init();
    retro_game_info game{argv[1], nullptr, 0, nullptr};
    if (!retro_load_game(&game)) return 3;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(30);
    auto next_frame = started;
    while (std::chrono::steady_clock::now() < deadline) {
        retro_run();
        next_frame += std::chrono::microseconds(16667);
        const auto now = std::chrono::steady_clock::now();
        if (next_frame < now) next_frame = now;
        std::this_thread::sleep_until(next_frame);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    retro_deinit();
    const auto visible_count = visible.load();
    const int result = visible_count >= 2 && first_hash != last_hash ? 0 : 4;
    std::FILE* file = std::fopen(output.string().c_str(), "wb");
    if (file) {
        std::fprintf(file, "{\"preset\":\"%s\",\"renderer\":\"vulkan\",\"elapsed_ms\":%lld,\"frame_count\":%u,\"average_fps\":%.3f,\"audio_frames\":%u,\"first_hash\":%llu,\"last_hash\":%llu,\"result\":%d}\n", preset.c_str(), static_cast<long long>(elapsed), frames.load(), elapsed ? frames.load() * 1000.0 / elapsed : 0.0, audio_frames.load(), static_cast<unsigned long long>(first_hash), static_cast<unsigned long long>(last_hash), result);
        std::fclose(file);
    }
    std::fprintf(stderr, "benchmark_result=%d preset=%s frames=%u visible=%u\n", result, preset.c_str(), frames.load(), visible_count);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), static_cast<unsigned>(result));
#else
    std::_Exit(result);
#endif
}
