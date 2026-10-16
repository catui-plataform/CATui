// SPDX-License-Identifier: GPL-2.0-or-later
// CATUI keeps the Libretro boundary in this small, dependency-free interface.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Catui::Libretro {

enum class PixelFormat : std::uint32_t { XRGB8888, RGB565 };

struct VideoFrame {
    const void* data{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t pitch{};
    PixelFormat format{PixelFormat::XRGB8888};
    std::uint64_t serial{};
};

struct InputState {
    std::int16_t buttons[17]{};
    std::int16_t axes[4]{};
    std::int16_t touch_x{};
    std::int16_t touch_y{};
    bool touch_pressed{};
    float gyro[3]{};
};

struct VideoSettings {
    std::string renderer_backend{"vulkan"};
    std::string resolution_scale{"1.0"};
    std::string present_mode{"fifo"};
    std::string readbacks_mode{"disabled"};
    std::string windows_red_zone_protection{"static_patching"};
    std::uint32_t window_width{1280};
    std::uint32_t window_height{720};
    std::uint32_t vblank_frequency{60};
    std::int32_t gpu_id{-1};
    std::int32_t rcas_attenuation{25};
    bool neo_mode{false};
    bool hdr_output{false};
    bool fsr_enabled{false};
    bool rcas_enabled{true};
    bool pipeline_cache_archived{true};
    bool pipeline_cache_enabled{true};
    bool copy_gpu_buffers{false};
    bool readback_linear_images_enabled{false};
    bool direct_memory_access_enabled{false};
    bool userfaultfd_tracking{false};
    bool vk_crash_diagnostics{false};
    bool rumble{true};
    bool touch_motion{true};
};

struct Callbacks {
    void* user{};
    void (*video)(void* user, const VideoFrame& frame){};
    void (*audio)(void* user, const std::int16_t* samples, std::size_t frames){};
    InputState (*input)(void* user, std::uint32_t port){};
    bool (*rumble)(void* user, std::uint32_t port, std::uint16_t strong,
                   std::uint16_t weak){};
    void (*log)(void* user, const char* message){};
};

// The emulator owns the implementation of these hooks. Keeping the hooks
// outside the Libretro ABI makes upstream rebases local to this adapter.
struct Backend {
    void* user{};
    bool (*load)(void* user, const std::filesystem::path& content,
                const VideoSettings& settings, const Callbacks& callbacks){};
    bool (*run_frame)(void* user){};
    void (*unload)(void* user){};
    void (*destroy)(void* user){};
    void (*set_paused)(void* user, bool paused){};
};

class Bridge final {
public:
    explicit Bridge(Backend backend);
    ~Bridge();

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    void SetCallbacks(Callbacks callbacks);
    void SetVideoSettings(VideoSettings settings);
    bool Load(const std::filesystem::path& content);
    bool RunFrame();
    void SetPaused(bool paused);
    void Unload();
    bool IsLoaded() const { return loaded_; }

private:
    Backend backend_{};
    Callbacks callbacks_{};
    VideoSettings settings_{};
    bool loaded_{};
};

} // namespace Catui::Libretro
