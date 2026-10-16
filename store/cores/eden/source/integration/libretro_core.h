// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include "common/common_types.h"
#include "core/core.h"
#include "eden_libretro/audio/libretro_sink.h"
#include "eden_libretro/emu_window/emu_window_libretro.h"
#include "eden_libretro/input/libretro_driver.h"
#include "eden_libretro/libretro.h"

namespace FileSys {
class ManualContentProvider;
}

namespace Libretro {

class LibretroSession {
public:
    static LibretroSession& GetInstance();

    void Init();
    void Deinit();

    bool LoadGame(const char* filepath);
    void UnloadGame();
    void Reset();

    void Run();
    void SetPaused(bool paused);
    bool IsPaused() const;
    int InstallContent(const char* filepath, const char* expected_title_id);
    const char* ListInstalledContent(const char* expected_title_id);
    bool SetInstalledContentEnabled(const char* expected_title_id, const char* content_key,
                                    bool enabled);

    void SetVideoRefreshCallback(retro_video_refresh_t cb);
    void SetAudioSampleBatchCallback(retro_audio_sample_batch_t cb);
    void SetInputPollCallback(retro_input_poll_t cb);
    void SetInputStateCallback(retro_input_state_t cb);

    void GetSystemAVInfo(struct retro_system_av_info* info);

    [[nodiscard]] double GetGameFPS() const {
        return cached_game_fps.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double GetEmulationFPS() const {
        return cached_system_fps.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t GetEmulatedFrameCount() const {
        return emulated_frames.load(std::memory_order_relaxed);
    }

private:
    LibretroSession();
    ~LibretroSession();

    std::unique_ptr<Core::System> system;
    std::unique_ptr<EmuWindow_Libretro> emu_window;
    std::unique_ptr<FileSys::ManualContentProvider> manual_content_provider;
    std::shared_ptr<LibretroDriver> input_driver;
    LibretroAudioSink* audio_sink{nullptr};

    retro_video_refresh_t video_cb{nullptr};
    retro_audio_sample_batch_t audio_batch_cb{nullptr};
    retro_input_poll_t input_poll_cb{nullptr};
    retro_input_state_t input_state_cb{nullptr};

    bool is_initialized{false};
    bool is_game_loaded{false};
    std::string current_game_path{};

    bool runtime_shader_reporting_armed{false};
    int last_runtime_shader_count{};
	int last_runtime_shader_started{};
	int runtime_shader_start_base{};

    std::vector<u32> frame_buffer_front;
    std::vector<u32> frame_buffer_back;
    std::atomic<bool> frame_ready{false};
    mutable std::mutex frame_mutex;

    u32 current_fb_width{1280};
    u32 current_fb_height{720};

    std::atomic<double> cached_game_fps{0.0};
    std::atomic<double> cached_system_fps{0.0};
    std::atomic<uint64_t> emulated_frames{0};
    std::chrono::steady_clock::time_point last_perf_sample{};

    mutable std::mutex run_mutex;
};

} // namespace Libretro
