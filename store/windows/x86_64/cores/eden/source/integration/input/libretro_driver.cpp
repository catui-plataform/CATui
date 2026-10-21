// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eden_libretro/input/libretro_driver.h"

#include <algorithm>
#include "common/settings.h"
#include "common/settings_enums.h"

namespace Libretro {

LibretroDriver::LibretroDriver() : InputEngine("libretro") {
    vibration_enabled.fill(true);
    for (std::size_t i = 0; i < MaxSupportedPlayers; ++i) {
        const auto id = GetIdentifier(i);
        PreSetController(id);
        for (int btn = 0; btn < static_cast<int>(LibretroButton::NumButtons); ++btn) {
            PreSetButton(id, btn);
        }
        for (int axis = 0; axis < static_cast<int>(LibretroAxis::NumAxes); ++axis) {
            PreSetAxis(id, axis);
        }
    }
}

LibretroDriver::~LibretroDriver() = default;

PadIdentifier LibretroDriver::GetIdentifier(std::size_t player_index) const {
    return PadIdentifier{
        .guid = Common::UUID{},
        .port = player_index,
        .pad = 0,
    };
}

void LibretroDriver::SetRumbleCallback(retro_set_rumble_state_t callback) {
    rumble_cb = callback;
}

Common::Input::DriverResult LibretroDriver::SetVibration(
    const PadIdentifier& identifier,
    const Common::Input::VibrationStatus& vibration) {
    if (!rumble_cb || identifier.port >= MaxSupportedPlayers) {
        return Common::Input::DriverResult::NotSupported;
    }

    const auto port = static_cast<unsigned>(identifier.port);
    const auto strong = static_cast<uint16_t>(std::clamp(vibration.low_amplitude, 0.0f, 1.0f) * 65535.0f);
    const auto weak = static_cast<uint16_t>(std::clamp(vibration.high_amplitude, 0.0f, 1.0f) * 65535.0f);

    rumble_cb(port, RETRO_RUMBLE_STRONG, strong);
    rumble_cb(port, RETRO_RUMBLE_WEAK, weak);

    return Common::Input::DriverResult::Success;
}

bool LibretroDriver::IsVibrationEnabled(const PadIdentifier& identifier) {
    if (identifier.port < MaxSupportedPlayers) {
        return vibration_enabled[identifier.port];
    }
    return false;
}

void LibretroDriver::PollInputs(retro_input_poll_t poll_cb, retro_input_state_t state_cb,
                                [[maybe_unused]] bool is_handheld) {
    if (poll_cb) {
        poll_cb();
    }
    if (!state_cb) {
        return;
    }

    auto update_button = [this](const PadIdentifier& id, std::size_t player, LibretroButton btn, bool val) {
        const auto idx = static_cast<std::size_t>(btn);
        if (idx < last_player_states[player].buttons.size()) {
            if (!last_player_states[player].initialized || last_player_states[player].buttons[idx] != val) {
                last_player_states[player].buttons[idx] = val;
                SetButton(id, static_cast<int>(btn), val);
            }
        }
    };

    auto update_axis = [this](const PadIdentifier& id, std::size_t player, LibretroAxis axis, float val) {
        const auto idx = static_cast<std::size_t>(axis);
        if (idx < last_player_states[player].axes.size()) {
            if (!last_player_states[player].initialized ||
                std::abs(last_player_states[player].axes[idx] - val) > 0.001f) {
                last_player_states[player].axes[idx] = val;
                SetAxis(id, static_cast<int>(axis), val);
            }
        }
    };

    const auto& players = Settings::values.players.GetValue();
    for (std::size_t player = 0; player < MaxSupportedPlayers; ++player) {
        if (player != 0 && (player >= players.size() || !players[player].connected)) {
            continue;
        }
        const auto port = static_cast<unsigned>(player);
        const auto id = GetIdentifier(player);

        // Buttons
        update_button(id, player, LibretroButton::A,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) != 0);
        update_button(id, player, LibretroButton::B,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) != 0);
        update_button(id, player, LibretroButton::X,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X) != 0);
        update_button(id, player, LibretroButton::Y,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y) != 0);
        update_button(id, player, LibretroButton::DUp,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP) != 0);
        update_button(id, player, LibretroButton::DDown,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN) != 0);
        update_button(id, player, LibretroButton::DLeft,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT) != 0);
        update_button(id, player, LibretroButton::DRight,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT) != 0);
        update_button(id, player, LibretroButton::L,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L) != 0);
        update_button(id, player, LibretroButton::R,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R) != 0);
        update_button(id, player, LibretroButton::ZL,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2) != 0);
        update_button(id, player, LibretroButton::ZR,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2) != 0);
        update_button(id, player, LibretroButton::LStick,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3) != 0);
        update_button(id, player, LibretroButton::RStick,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3) != 0);
        update_button(id, player, LibretroButton::Plus,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START) != 0);
        update_button(id, player, LibretroButton::Minus,
                      state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT) != 0);

        // Analog sticks (normalized -1.0 .. 1.0, Y axis inverted for Switch)
        const int16_t lx_raw = state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
        const int16_t ly_raw = state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
        const int16_t rx_raw = state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
        const int16_t ry_raw = state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);

        update_axis(id, player, LibretroAxis::LeftStickX, std::clamp(static_cast<float>(lx_raw) / 32767.0f, -1.0f, 1.0f));
        update_axis(id, player, LibretroAxis::LeftStickY, std::clamp(-static_cast<float>(ly_raw) / 32767.0f, -1.0f, 1.0f));
        update_axis(id, player, LibretroAxis::RightStickX, std::clamp(static_cast<float>(rx_raw) / 32767.0f, -1.0f, 1.0f));
        update_axis(id, player, LibretroAxis::RightStickY, std::clamp(-static_cast<float>(ry_raw) / 32767.0f, -1.0f, 1.0f));

        last_player_states[player].initialized = true;
    }

	// The Switch tablet is a console-wide device. CATui exposes its currently
	// active contact through libretro's normalized pointer on port zero.
	const auto touch_id = GetIdentifier(0);
	const auto pointer_x = state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
	const auto pointer_y = state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
	const bool pointer_pressed =
	    state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) != 0;
	update_axis(touch_id, 0, LibretroAxis::TouchX,
	            std::clamp((static_cast<float>(pointer_x) / 32767.0f + 1.0f) * 0.5f, 0.0f, 1.0f));
	update_axis(touch_id, 0, LibretroAxis::TouchY,
	            std::clamp((static_cast<float>(pointer_y) / 32767.0f + 1.0f) * 0.5f, 0.0f, 1.0f));
	update_button(touch_id, 0, LibretroButton::Touch, pointer_pressed);
}

InputCommon::ButtonMapping LibretroDriver::GetButtonMappingForDevice(const Common::ParamPackage& params) {
    InputCommon::ButtonMapping mapping;
    const auto port = params.Get("port", 0);

    auto make_param = [&](LibretroButton btn) {
        Common::ParamPackage p;
        p.Set("engine", "libretro");
        p.Set("port", port);
        p.Set("button", static_cast<int>(btn));
        return p;
    };

    mapping[Settings::NativeButton::A] = make_param(LibretroButton::A);
    mapping[Settings::NativeButton::B] = make_param(LibretroButton::B);
    mapping[Settings::NativeButton::X] = make_param(LibretroButton::X);
    mapping[Settings::NativeButton::Y] = make_param(LibretroButton::Y);
    mapping[Settings::NativeButton::DUp] = make_param(LibretroButton::DUp);
    mapping[Settings::NativeButton::DDown] = make_param(LibretroButton::DDown);
    mapping[Settings::NativeButton::DLeft] = make_param(LibretroButton::DLeft);
    mapping[Settings::NativeButton::DRight] = make_param(LibretroButton::DRight);
    mapping[Settings::NativeButton::L] = make_param(LibretroButton::L);
    mapping[Settings::NativeButton::R] = make_param(LibretroButton::R);
    mapping[Settings::NativeButton::ZL] = make_param(LibretroButton::ZL);
    mapping[Settings::NativeButton::ZR] = make_param(LibretroButton::ZR);
    mapping[Settings::NativeButton::LStick] = make_param(LibretroButton::LStick);
    mapping[Settings::NativeButton::RStick] = make_param(LibretroButton::RStick);
    mapping[Settings::NativeButton::Plus] = make_param(LibretroButton::Plus);
    mapping[Settings::NativeButton::Minus] = make_param(LibretroButton::Minus);
    mapping[Settings::NativeButton::Home] = make_param(LibretroButton::Home);
    mapping[Settings::NativeButton::Screenshot] = make_param(LibretroButton::Capture);

    return mapping;
}

InputCommon::AnalogMapping LibretroDriver::GetAnalogMappingForDevice(const Common::ParamPackage& params) {
    InputCommon::AnalogMapping mapping;
    const auto port = params.Get("port", 0);

    auto make_stick = [&](LibretroAxis x_axis, LibretroAxis y_axis) {
        Common::ParamPackage p;
        p.Set("engine", "libretro");
        p.Set("port", port);
        p.Set("axis_x", static_cast<int>(x_axis));
        p.Set("axis_y", static_cast<int>(y_axis));
        p.Set("deadzone", 0.05f);
        p.Set("range", 1.0f);
        return p;
    };

    mapping[Settings::NativeAnalog::LStick] = make_stick(LibretroAxis::LeftStickX, LibretroAxis::LeftStickY);
    mapping[Settings::NativeAnalog::RStick] = make_stick(LibretroAxis::RightStickX, LibretroAxis::RightStickY);

    return mapping;
}

InputCommon::MotionMapping LibretroDriver::GetMotionMappingForDevice([[maybe_unused]] const Common::ParamPackage& params) {
    return {};
}

} // namespace Libretro
