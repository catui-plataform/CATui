// SPDX-License-Identifier: GPL-2.0-or-later
// The only file that translates between the stable CATUI seam and shadPS4 internals.
#include "libretro/catui_libretro_api.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_main.h>

#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/catui_runtime.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "core/ipc/ipc.h"
#include "core/user_settings.h"
#include "emulator.h"

namespace Catui::Libretro {
namespace {

constexpr int kMaxAudioPorts = 25;
constexpr std::size_t kMaxPortQueueSamples = 4096 * 2;

struct AudioPortState {
    std::deque<float> samples{};
    std::chrono::steady_clock::time_point last_submit{};
    bool active{};
};

struct Shadps4Backend {
    Callbacks callbacks{};
    VideoSettings settings{};
    std::filesystem::path content{};
    std::thread owner_thread{};
    std::mutex frame_mutex{};
    std::vector<std::uint32_t> frame{};
    std::uint32_t frame_width{};
    std::uint32_t frame_height{};
    std::size_t frame_pitch{};
    std::uint64_t produced_serial{};
    std::uint64_t consumed_serial{};
    std::mutex audio_mutex{};
    std::array<AudioPortState, kMaxAudioPorts> audio_ports{};
    std::vector<float> mix_scratch{};
    std::vector<std::int16_t> pcm_scratch{};
    std::atomic_bool running{};
    std::atomic_bool paused{};
    Core::Emulator* emulator{};
};

void Configure(const VideoSettings& settings) {
    const float resolution_scale = settings.resolution_scale == "1.5" ? 1.5f
                                   : settings.resolution_scale == "2.0" ? 2.0f
                                   : settings.resolution_scale == "3.0" ? 3.0f
                                   : settings.resolution_scale == "4.0" ? 4.0f
                                                                         : 1.0f;
    EmulatorSettings.SetConfigMode(ConfigMode::Clean);
    EmulatorSettings.SetFullScreen(false);
    EmulatorSettings.SetWindowWidth(static_cast<int>(settings.window_width));
    EmulatorSettings.SetWindowHeight(static_cast<int>(settings.window_height));
    EmulatorSettings.SetInternalScreenWidth(
        static_cast<std::uint32_t>(settings.window_width * resolution_scale));
    EmulatorSettings.SetInternalScreenHeight(
        static_cast<std::uint32_t>(settings.window_height * resolution_scale));
    EmulatorSettings.SetVblankFrequency(settings.vblank_frequency);
    EmulatorSettings.SetNeo(settings.neo_mode);
    EmulatorSettings.SetGpuId(settings.gpu_id);
    EmulatorSettings.SetFsrEnabled(settings.fsr_enabled);
    EmulatorSettings.SetRcasEnabled(settings.rcas_enabled);
    EmulatorSettings.SetRcasAttenuation(settings.rcas_attenuation * 10);
    EmulatorSettings.SetHdrAllowed(settings.hdr_output);
    EmulatorSettings.SetCopyGpuBuffers(settings.copy_gpu_buffers);
    EmulatorSettings.SetReadbackLinearImagesEnabled(settings.readback_linear_images_enabled);
    EmulatorSettings.SetDirectMemoryAccessEnabled(settings.direct_memory_access_enabled);
    EmulatorSettings.SetUserfaultfdTracking(settings.userfaultfd_tracking);
    EmulatorSettings.SetVkCrashDiagnosticEnabled(settings.vk_crash_diagnostics);
    EmulatorSettings.SetPipelineCacheEnabled(settings.pipeline_cache_enabled);
    EmulatorSettings.SetPipelineCacheArchived(settings.pipeline_cache_archived);
    EmulatorSettings.SetMotionControlsEnabled(settings.touch_motion);
    EmulatorSettings.SetRedZonePatchingEnabled(
        settings.windows_red_zone_protection == "static_patching" ||
        settings.windows_red_zone_protection == "enabled");

    const int readbacks = settings.readbacks_mode == "synchronous"
                              ? GpuReadbacksMode::Precise
                              : settings.readbacks_mode == "asynchronous"
                                    ? GpuReadbacksMode::Relaxed
                                    : GpuReadbacksMode::Disabled;
    EmulatorSettings.SetReadbacksMode(readbacks);

    std::string present_mode = "Fifo";
    if (settings.present_mode == "mailbox") present_mode = "Mailbox";
    else if (settings.present_mode == "immediate") present_mode = "Immediate";
    EmulatorSettings.SetPresentMode(present_mode);
}

void ReceiveVideo(void* user, const std::uint8_t* rgba, const std::uint32_t width,
                  const std::uint32_t height, const std::size_t pitch) {
    auto& backend = *static_cast<Shadps4Backend*>(user);
    std::scoped_lock lock{backend.frame_mutex};
    backend.frame.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* source = rgba + static_cast<std::size_t>(y) * pitch;
        auto* destination = backend.frame.data() + static_cast<std::size_t>(y) * width;
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto* pixel = source + static_cast<std::size_t>(x) * 4;
            destination[x] = (static_cast<std::uint32_t>(pixel[0]) << 16) |
                             (static_cast<std::uint32_t>(pixel[1]) << 8) | pixel[2];
        }
    }
    backend.frame_width = width;
    backend.frame_height = height;
    backend.frame_pitch = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    ++backend.produced_serial;
}

float SoftLimit(const float sample) {
    constexpr float knee = 0.9f;
    if (sample > knee) {
        return knee + (1.0f - knee) * std::tanh((sample - knee) / (1.0f - knee));
    }
    if (sample < -knee) {
        return -knee - (1.0f - knee) * std::tanh((-sample - knee) / (1.0f - knee));
    }
    return sample;
}

std::int16_t FloatToS16(const float sample) {
    const float limited = SoftLimit(sample);
    return static_cast<std::int16_t>(
        std::clamp(std::lrintf(std::clamp(limited, -1.0f, 1.0f) * 32767.0f), -32768l, 32767l));
}

float ReadNormalizedSample(const void* samples, const std::size_t index, const bool is_float) {
    if (is_float) {
        return static_cast<const float*>(samples)[index];
    }
    constexpr float inv_s16 = 1.0f / 32768.0f;
    return static_cast<float>(static_cast<const std::int16_t*>(samples)[index]) * inv_s16;
}

void DownmixPortToStereoFloat(const void* samples, const std::size_t frames,
                              const std::uint32_t channels, const bool is_float,
                              const bool is_std, const int* channel_layout, const int* volume,
                              const float slider_gain, std::vector<float>& out_stereo) {
    out_stereo.resize(frames * 2);
    constexpr float inv_vol_0db = 1.0f / 32768.0f;
    std::array<float, 8> ch_gain{};
    for (std::size_t i = 0; i < 8; ++i) {
        const float v = volume ? (static_cast<float>(std::clamp(volume[i], 0, 32768)) * inv_vol_0db)
                               : 1.0f;
        ch_gain[i] = v * slider_gain;
    }

    if (channels == 1) {
        const float g0 = ch_gain[0];
        for (std::size_t f = 0; f < frames; ++f) {
            const float s = ReadNormalizedSample(samples, f, is_float) * g0;
            out_stereo[f * 2] = s;
            out_stereo[f * 2 + 1] = s;
        }
        return;
    }

    if (channels >= 8) {
        // PS4 downmix coefficients matching standalone SDLAudioOut:
        // L = 1.0 * FL + 0.7071 * FC + 0.7071 * (SL + BL)
        // R = 1.0 * FR + 0.7071 * FC + 0.7071 * (SR + BR)
        constexpr float kFront = 1.0f;
        constexpr float kCenter = 0.70710678f;
        constexpr float kSurround = 0.70710678f;
        const float g_fl = ch_gain[0] * kFront;
        const float g_fr = ch_gain[1] * kFront;
        const float g_fc = ch_gain[2] * kCenter;
        // In non-Std 8CH: ch4=SL(vol 4), ch5=SR(vol 5), ch6=BL(vol 6), ch7=BR(vol 7)
        // In Std 8CH:     ch4=BL(vol 6), ch5=BR(vol 7), ch6=SL(vol 4), ch7=SR(vol 5)
        const float g_ch4 = (is_std ? ch_gain[6] : ch_gain[4]) * kSurround;
        const float g_ch5 = (is_std ? ch_gain[7] : ch_gain[5]) * kSurround;
        const float g_ch6 = (is_std ? ch_gain[4] : ch_gain[6]) * kSurround;
        const float g_ch7 = (is_std ? ch_gain[5] : ch_gain[7]) * kSurround;

        for (std::size_t f = 0; f < frames; ++f) {
            const std::size_t base = f * channels;
            const float fl = ReadNormalizedSample(samples, base + 0, is_float) * g_fl;
            const float fr = ReadNormalizedSample(samples, base + 1, is_float) * g_fr;
            const float fc = ReadNormalizedSample(samples, base + 2, is_float) * g_fc;
            const float s4 = ReadNormalizedSample(samples, base + 4, is_float) * g_ch4;
            const float s5 = ReadNormalizedSample(samples, base + 5, is_float) * g_ch5;
            const float s6 = ReadNormalizedSample(samples, base + 6, is_float) * g_ch6;
            const float s7 = ReadNormalizedSample(samples, base + 7, is_float) * g_ch7;
            out_stereo[f * 2] = fl + fc + s4 + s6;
            out_stereo[f * 2 + 1] = fr + fc + s5 + s7;
        }
        return;
    }

    const std::uint32_t left_idx =
        channel_layout ? static_cast<std::uint32_t>(
                             std::clamp(channel_layout[0], 0, static_cast<int>(channels - 1)))
                       : 0u;
    const std::uint32_t right_idx =
        channel_layout ? static_cast<std::uint32_t>(
                             std::clamp(channel_layout[1], 0, static_cast<int>(channels - 1)))
                       : std::min(1u, channels - 1);
    const float gl = ch_gain[0];
    const float gr = ch_gain[1];
    for (std::size_t f = 0; f < frames; ++f) {
        const std::size_t base = f * channels;
        out_stereo[f * 2] = ReadNormalizedSample(samples, base + left_idx, is_float) * gl;
        out_stereo[f * 2 + 1] = ReadNormalizedSample(samples, base + right_idx, is_float) * gr;
    }
}

bool ReceiveAudio(void* user, const int port_id, const void* samples, const std::size_t frames,
                  const std::uint32_t channels, const bool is_float, const bool is_std,
                  const int* channel_layout, const int* volume) {
    if (!samples || frames == 0 || channels == 0) return true;
    auto& backend = *static_cast<Shadps4Backend*>(user);
    if (backend.paused.load(std::memory_order_acquire)) return true;

    const int clamped_port = std::clamp(port_id, 0, kMaxAudioPorts - 1);
    const float slider_gain = EmulatorSettings.GetVolumeSlider() * 0.01f;

    std::scoped_lock lock{backend.audio_mutex};
    DownmixPortToStereoFloat(samples, frames, channels, is_float, is_std, channel_layout, volume,
                             slider_gain, backend.mix_scratch);

    const auto now = std::chrono::steady_clock::now();
    constexpr auto kActiveWindow = std::chrono::milliseconds(100);
    auto& self_port = backend.audio_ports[clamped_port];
    self_port.last_submit = now;
    self_port.active = true;

    int primary_port = clamped_port;
    for (int p = 0; p < kMaxAudioPorts; ++p) {
        auto& state = backend.audio_ports[p];
        if (!state.active) continue;
        if (now - state.last_submit > kActiveWindow) {
            state.active = false;
            state.samples.clear();
            continue;
        }
        if (p < primary_port) {
            primary_port = p;
        }
    }

    if (clamped_port != primary_port) {
        // Secondary active port (e.g. Bgm, Voice, PadSpk): queue for parallel mixing into primary.
        if (self_port.samples.size() + backend.mix_scratch.size() > kMaxPortQueueSamples) {
            const std::size_t overflow =
                (self_port.samples.size() + backend.mix_scratch.size()) - kMaxPortQueueSamples;
            self_port.samples.erase(
                self_port.samples.begin(),
                self_port.samples.begin() +
                    static_cast<std::ptrdiff_t>(std::min(overflow, self_port.samples.size())));
        }
        self_port.samples.insert(self_port.samples.end(), backend.mix_scratch.begin(),
                                 backend.mix_scratch.end());
        return true;
    }

    // If this port just became primary and had queued samples, drain them first.
    if (!self_port.samples.empty()) {
        self_port.samples.clear();
    }

    // Mix all secondary active ports in parallel into backend.mix_scratch.
    const std::size_t sample_count = frames * 2;
    for (int p = 0; p < kMaxAudioPorts; ++p) {
        if (p == primary_port) continue;
        auto& other = backend.audio_ports[p];
        if (other.samples.empty()) continue;
        const std::size_t mix_count = std::min(sample_count, other.samples.size());
        for (std::size_t i = 0; i < mix_count; ++i) {
            backend.mix_scratch[i] += other.samples[i];
        }
        other.samples.erase(other.samples.begin(),
                            other.samples.begin() + static_cast<std::ptrdiff_t>(mix_count));
    }

    backend.pcm_scratch.resize(sample_count);
    for (std::size_t i = 0; i < sample_count; ++i) {
        backend.pcm_scratch[i] = FloatToS16(backend.mix_scratch[i]);
    }

    if (backend.callbacks.audio) {
        backend.callbacks.audio(backend.callbacks.user, backend.pcm_scratch.data(), frames);
    }
    return true;
}

bool ReceiveRumble(void* user, const std::uint8_t small_motor,
                   const std::uint8_t large_motor) {
    auto& backend = *static_cast<Shadps4Backend*>(user);
    if (!backend.settings.rumble) return true;
    if (!backend.callbacks.rumble) return false;
    constexpr std::uint16_t byte_to_u16 = 0x101;
    return backend.callbacks.rumble(backend.callbacks.user, 0,
                                    static_cast<std::uint16_t>(large_motor) * byte_to_u16,
                                    static_cast<std::uint16_t>(small_motor) * byte_to_u16);
}

bool Load(void* user, const std::filesystem::path& content, const VideoSettings& settings,
          const Callbacks& callbacks) {
    auto& backend = *static_cast<Shadps4Backend*>(user);
    if (backend.running.exchange(true)) return false;
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    backend.callbacks = callbacks;
    backend.settings = settings;
    backend.content = content;
    backend.paused.store(false, std::memory_order_release);

    static std::once_flag initialize_once;
    std::call_once(initialize_once, [] {
        Common::Log::Setup("shadps4-libretro.log");
        IPC::Instance().Init();
        EmulatorState::SetInstance(std::make_shared<EmulatorState>());
        UserSettings.Load();
        KeyManager::GetInstance()->LoadFromFile();
        auto emulator_settings = std::make_shared<EmulatorSettingsImpl>();
        EmulatorSettingsImpl::SetInstance(emulator_settings);
        emulator_settings->Load();
        if (const char* home_dir = std::getenv("CATUI_SHADPS4_HOME_DIR"); home_dir && *home_dir) {
            emulator_settings->SetHomeDir(home_dir);
        }
    });
    Configure(settings);

    Core::CatuiRuntime::SetCallbacks({&backend, ReceiveVideo, ReceiveAudio, ReceiveRumble});
    backend.emulator = Common::Singleton<Core::Emulator>::Instance();
    backend.emulator->SetEmbeddedMode(true);
    std::vector<std::string> game_args;
    const auto args_path = std::filesystem::is_directory(backend.content)
                               ? backend.content / "args.txt"
                               : backend.content.parent_path() / "args.txt";
    if (std::ifstream args_file{args_path}; args_file) {
        std::string line;
        std::getline(args_file, line);
        std::istringstream tokens{line};
        for (std::string token; tokens >> token;) game_args.push_back(std::move(token));
        if (!game_args.empty()) game_args.insert(game_args.begin(), "/app0/eboot.bin");
    }
    backend.owner_thread = std::thread([&backend, args = std::move(game_args)] {
        SDL_SetMainReady();
        backend.emulator->Run(backend.content, args);
        backend.running.store(false, std::memory_order_release);
    });
    return true;
}

bool RunFrame(void* user) {
    auto& backend = *static_cast<Shadps4Backend*>(user);
    if (backend.callbacks.input) {
        const auto input = backend.callbacks.input(backend.callbacks.user, 0);
        Core::CatuiRuntime::InputState state{};
        std::copy(std::begin(input.buttons), std::end(input.buttons), std::begin(state.buttons));
        std::copy(std::begin(input.axes), std::end(input.axes), std::begin(state.axes));
        state.touch_x = input.touch_x;
        state.touch_y = input.touch_y;
        state.touch_pressed = input.touch_pressed;
        std::copy(std::begin(input.gyro), std::end(input.gyro), std::begin(state.gyro));
        if (backend.emulator && backend.emulator->IsEmbeddedReady()) {
            backend.emulator->ApplyEmbeddedInput(0, state);
        }
    }

    if (backend.callbacks.video) {
        std::scoped_lock lock{backend.frame_mutex};
        if (!backend.frame.empty() && backend.produced_serial != backend.consumed_serial) {
            const VideoFrame frame{backend.frame.data(), backend.frame_width, backend.frame_height,
                                   backend.frame_pitch, PixelFormat::XRGB8888,
                                   backend.produced_serial};
            backend.callbacks.video(backend.callbacks.user, frame);
            backend.consumed_serial = backend.produced_serial;
        }
    }

    return backend.running.load(std::memory_order_acquire);
}

void Unload(void* user) {
    auto& backend = *static_cast<Shadps4Backend*>(user);
    backend.paused.store(false, std::memory_order_release);
    if (backend.emulator) backend.emulator->SetEmbeddedPaused(false);
    if (backend.emulator) backend.emulator->RequestStop();
    if (backend.owner_thread.joinable()) backend.owner_thread.join();
    Core::CatuiRuntime::ClearCallbacks();
    {
        std::scoped_lock lock{backend.audio_mutex};
        for (auto& port : backend.audio_ports) {
            port.samples.clear();
            port.active = false;
        }
    }
    backend.running.store(false, std::memory_order_release);
}

void SetPaused(void* user, bool paused) {
    auto& backend = *static_cast<Shadps4Backend*>(user);
    backend.paused.store(paused, std::memory_order_release);
    if (paused) {
        std::scoped_lock lock{backend.audio_mutex};
        for (auto& port : backend.audio_ports) {
            port.samples.clear();
        }
    }
    if (backend.emulator) backend.emulator->SetEmbeddedPaused(paused);
}

void Destroy(void* user) {
    delete static_cast<Shadps4Backend*>(user);
}

} // namespace

Backend CreateShadps4Backend() {
    auto* backend = new Shadps4Backend{};
    return {backend, Load, RunFrame, Unload, Destroy, SetPaused};
}

} // namespace Catui::Libretro
