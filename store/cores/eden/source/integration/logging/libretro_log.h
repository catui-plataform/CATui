// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eden_libretro/libretro.h"

namespace Libretro {

class LibretroLog {
public:
    static void Initialize(retro_environment_t env_cb);
    static void Log(enum retro_log_level level, const char* fmt, ...);

private:
    static inline retro_log_printf_t log_cb{nullptr};
};

} // namespace Libretro
