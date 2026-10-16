// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eden_libretro/logging/libretro_log.h"

#include <cstdarg>
#include <cstdio>
#include "common/logging.h"

namespace Libretro {

void LibretroLog::Initialize(retro_environment_t env_cb) {
    if (!env_cb) {
        return;
    }

    struct retro_log_callback logging{};
    if (env_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) && logging.log) {
        log_cb = logging.log;
    }
}

void LibretroLog::Log(enum retro_log_level level, const char* fmt, ...) {
    if (log_cb) {
        va_list va;
        va_start(va, fmt);
        // retro_log_printf_t is void (enum retro_log_level, const char*, ...)
        char buffer[2048];
        vsnprintf(buffer, sizeof(buffer), fmt, va);
        va_end(va);
        log_cb(level, "%s", buffer);
    }
}

} // namespace Libretro
