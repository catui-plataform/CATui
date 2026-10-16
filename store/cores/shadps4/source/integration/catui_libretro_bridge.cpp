// SPDX-License-Identifier: GPL-2.0-or-later
#include "catui_libretro_bridge.h"

#include <utility>

namespace Catui::Libretro {

Bridge::Bridge(Backend backend) : backend_(backend) {}

Bridge::~Bridge() {
    Unload();
    if (backend_.destroy) {
        backend_.destroy(backend_.user);
        backend_.user = nullptr;
    }
}

void Bridge::SetCallbacks(Callbacks callbacks) {
    callbacks_ = callbacks;
}

void Bridge::SetVideoSettings(VideoSettings settings) {
    // Settings are only consumed by the next load. This prevents changing
    // Vulkan ownership or resolution while a frame callback is in flight.
    settings_ = std::move(settings);
}

bool Bridge::Load(const std::filesystem::path& content) {
    Unload();
    if (backend_.load == nullptr) {
        return false;
    }
    loaded_ = backend_.load(backend_.user, content, settings_, callbacks_);
    return loaded_;
}

bool Bridge::RunFrame() {
    return loaded_ && backend_.run_frame != nullptr && backend_.run_frame(backend_.user);
}

void Bridge::SetPaused(bool paused) {
    if (loaded_ && backend_.set_paused) backend_.set_paused(backend_.user, paused);
}

void Bridge::Unload() {
    if (loaded_ && backend_.unload != nullptr) {
        backend_.unload(backend_.user);
    }
    loaded_ = false;
}

} // namespace Catui::Libretro
