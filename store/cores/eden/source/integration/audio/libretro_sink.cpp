// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eden_libretro/audio/libretro_sink.h"

#include <algorithm>
#include <limits>
#include "audio_core/common/common.h"
#include "common/logging.h"

namespace Libretro {

LibretroSinkStream::LibretroSinkStream(u32 device_channels_, u32 system_channels_,
                                       AudioCore::Sink::StreamType type_, Core::System& system_)
    : SinkStream(system_, type_) {
    device_channels = device_channels_;
    system_channels = system_channels_;
}

LibretroSinkStream::~LibretroSinkStream() {
    Finalize();
}

void LibretroSinkStream::Finalize() {
    Stop();
}

void LibretroSinkStream::Start([[maybe_unused]] bool resume) {
    paused = false;
}

void LibretroSinkStream::Stop() {
    if (paused) {
        return;
    }
    SignalPause();
}

void LibretroSinkStream::Drain(std::span<s16> output_buffer, size_t num_frames) {
    if (paused) {
        std::fill(output_buffer.begin(), output_buffer.end(), static_cast<s16>(0));
        return;
    }
    ProcessAudioOutAndRender(output_buffer, num_frames);
}

// -----------------------------------------------------------------------------
// LibretroAudioSink
// -----------------------------------------------------------------------------

LibretroAudioSink::LibretroAudioSink() {
    device_channels = 2; // Libretro standard is 2-channel stereo
}

LibretroAudioSink::~LibretroAudioSink() {
    CloseStreams();
}

AudioCore::Sink::SinkStream* LibretroAudioSink::AcquireSinkStream(Core::System& system,
                                                                 u32 system_channels_,
                                                                 [[maybe_unused]] const std::string& name,
                                                                 AudioCore::Sink::StreamType type) {
    std::lock_guard lock{stream_mutex};
    system_channels = system_channels_;
    auto stream = std::make_unique<LibretroSinkStream>(
        device_channels, system_channels, type, system);
    auto* ptr = stream.get();
    sink_streams.push_back(std::move(stream));
    return ptr;
}

void LibretroAudioSink::CloseStream(AudioCore::Sink::SinkStream* stream) {
    std::lock_guard lock{stream_mutex};
    std::erase_if(sink_streams, [stream](const auto& s) { return s.get() == stream; });
}

void LibretroAudioSink::CloseStreams() {
    std::lock_guard lock{stream_mutex};
    sink_streams.clear();
}

f32 LibretroAudioSink::GetDeviceVolume() const {
    return device_volume;
}

void LibretroAudioSink::SetDeviceVolume(f32 volume) {
    std::lock_guard lock{stream_mutex};
    device_volume = volume;
    for (auto& stream : sink_streams) {
        stream->SetDeviceVolume(volume);
    }
}

void LibretroAudioSink::SetSystemVolume(f32 volume) {
    std::lock_guard lock{stream_mutex};
    for (auto& stream : sink_streams) {
        stream->SetSystemVolume(volume);
    }
}

void LibretroAudioSink::SetAudioCallback(retro_audio_sample_batch_t callback) {
    audio_batch_callback = callback;
}

void LibretroAudioSink::PrepareForShutdown() {
    std::lock_guard lock{stream_mutex};
    audio_batch_callback = nullptr;
    for (auto& stream : sink_streams) {
        stream->Stop();
        stream->ClearQueue();
    }
}

void LibretroAudioSink::FlushAudio(size_t frames_to_drain) {
    if (!audio_batch_callback || frames_to_drain == 0) {
        return;
    }

    std::lock_guard lock{stream_mutex};
    if (sink_streams.empty()) {
        return;
    }

    const size_t total_samples = frames_to_drain * device_channels;
    std::vector<s16> final_output(total_samples, 0);

    if (sink_streams.size() == 1) {
        sink_streams[0]->Drain(final_output, frames_to_drain);
    } else {
        std::vector<s16> stream_temp(total_samples);
        std::vector<s32> accumulator(total_samples, 0);

        for (auto& stream : sink_streams) {
            stream->Drain(stream_temp, frames_to_drain);
            for (size_t i = 0; i < total_samples; ++i) {
                accumulator[i] += stream_temp[i];
            }
        }

        constexpr s32 min_val = (std::numeric_limits<s16>::min)();
        constexpr s32 max_val = (std::numeric_limits<s16>::max)();
        for (size_t i = 0; i < total_samples; ++i) {
            final_output[i] = static_cast<s16>(std::clamp(accumulator[i], min_val, max_val));
        }
    }

    audio_batch_callback(final_output.data(), frames_to_drain);
}

} // namespace Libretro
