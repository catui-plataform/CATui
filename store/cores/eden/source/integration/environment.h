// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include "eden_libretro/libretro.h"

namespace Libretro {

class Environment {
public:
    static void SetEnvironmentCallback(retro_environment_t cb);
    static retro_environment_t GetEnvironmentCallback();

    static void InitializeDirectories();
    static void RegisterCoreOptions();
    static void UpdateVariables();
    static void RegisterInputDescriptors();

    static bool IsVariableUpdated();
    static std::string GetVariable(const char* key);

    static const std::filesystem::path& GetSystemDir();
    static const std::filesystem::path& GetSaveDir();

private:
    static inline retro_environment_t env_cb{nullptr};
    static inline std::filesystem::path system_dir{};
    static inline std::filesystem::path save_dir{};
};

} // namespace Libretro
