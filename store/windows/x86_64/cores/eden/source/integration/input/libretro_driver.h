// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include "common/settings_input.h"
#include "eden_libretro/libretro.h"
#include "input_common/input_engine.h"

namespace Libretro {

enum class LibretroButton : int {
    A = Settings::NativeButton::A,
    B = Settings::NativeButton::B,
    X = Settings::NativeButton::X,
    Y = Settings::NativeButton::Y,
    LStick = Settings::NativeButton::LStick,
    RStick = Settings::NativeButton::RStick,
    L = Settings::NativeButton::L,
    R = Settings::NativeButton::R,
    ZL = Settings::NativeButton::ZL,
    ZR = Settings::NativeButton::ZR,
    Plus = Settings::NativeButton::Plus,
    Minus = Settings::NativeButton::Minus,
    DLeft = Settings::NativeButton::DLeft,
    DUp = Settings::NativeButton::DUp,
    DRight = Settings::NativeButton::DRight,
    DDown = Settings::NativeButton::DDown,
    Home = Settings::NativeButton::Home,
    Capture = Settings::NativeButton::Screenshot,
    Touch = Settings::NativeButton::NumButtons,
    NumButtons
};

enum class LibretroAxis : int {
    LeftStickX = 0,
    LeftStickY = 1,
    RightStickX = 2,
    RightStickY = 3,
    TouchX = 4,
    TouchY = 5,
    NumAxes = 6
};

class LibretroDriver final : public InputCommon::InputEngine {
public:
    static constexpr std::size_t MaxSupportedPlayers = 8;

    explicit LibretroDriver();
    ~LibretroDriver() override;

    void PollInputs(retro_input_poll_t poll_cb, retro_input_state_t state_cb, bool is_handheld);

    void SetRumbleCallback(retro_set_rumble_state_t callback);

    Common::Input::DriverResult SetVibration(
        const PadIdentifier& identifier,
        const Common::Input::VibrationStatus& vibration) override;
    bool IsVibrationEnabled(const PadIdentifier& identifier) override;

    InputCommon::ButtonMapping GetButtonMappingForDevice(
        const Common::ParamPackage& params) override;
    InputCommon::AnalogMapping GetAnalogMappingForDevice(
        const Common::ParamPackage& params) override;
    InputCommon::MotionMapping GetMotionMappingForDevice(
        const Common::ParamPackage& params) override;

private:
    PadIdentifier GetIdentifier(std::size_t player_index) const;

    retro_set_rumble_state_t rumble_cb{nullptr};
    std::array<bool, MaxSupportedPlayers> vibration_enabled{};

    struct PlayerState {
        std::array<bool, static_cast<std::size_t>(LibretroButton::NumButtons)> buttons{};
        std::array<float, static_cast<std::size_t>(LibretroAxis::NumAxes)> axes{};
        bool initialized{false};
    };
    std::array<PlayerState, MaxSupportedPlayers> last_player_states{};
};

} // namespace Libretro
