// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_stream.h"
#include "common/common_types.h"
#include "eden_libretro/libretro.h"

namespace Core {
class System;
}

namespace Libretro {

class LibretroSinkStream;

class LibretroAudioSink final : public AudioCore::Sink::Sink {
public:
    explicit LibretroAudioSink();
    ~LibretroAudioSink() override;

    AudioCore::Sink::SinkStream* AcquireSinkStream(Core::System& system, u32 system_channels,
                                                   const std::string& name,
                                                   AudioCore::Sink::StreamType type) override;

    void CloseStream(AudioCore::Sink::SinkStream* stream) override;
    void CloseStreams() override;

    f32 GetDeviceVolume() const override;
    void SetDeviceVolume(f32 volume) override;
    void SetSystemVolume(f32 volume) override;

    void SetAudioCallback(retro_audio_sample_batch_t callback);
    void FlushAudio(size_t frames_to_drain);
    void PrepareForShutdown();

private:
    mutable std::mutex stream_mutex;
    std::vector<std::unique_ptr<LibretroSinkStream>> sink_streams;
    retro_audio_sample_batch_t audio_batch_callback{nullptr};
    f32 device_volume{1.0f};
};

class LibretroSinkStream final : public AudioCore::Sink::SinkStream {
public:
    LibretroSinkStream(u32 device_channels_, u32 system_channels_,
                       AudioCore::Sink::StreamType type_, Core::System& system_);
    ~LibretroSinkStream() override;

    void Finalize() override;
    void Start(bool resume = false) override;
    void Stop() override;

    void Drain(std::span<s16> output_buffer, size_t num_frames);
};

} // namespace Libretro
