#include <libretro.h>

#include "modern_resources.h"

#include "common/Error.h"
#include "common/MemorySettingsInterface.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/MemoryTypes.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/SPU2/spu2.h"
#include "pcsx2/VMManager.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void ModernHostSetFrameCaptureEnabled(bool enabled);
void ModernHostSetCaptureResolution(u32 internal_scale, u32 output_scale);
void ModernHostResetFrameCapture();
bool ModernHostTakeFrame(std::vector<u32>* pixels, unsigned* width, unsigned* height);

namespace
{
	retro_environment_t s_environment_callback = nullptr;
	retro_video_refresh_t s_video_callback = nullptr;
	retro_audio_sample_t s_audio_callback = nullptr;
	retro_audio_sample_batch_t s_audio_batch_callback = nullptr;
	retro_input_poll_t s_input_poll_callback = nullptr;
	retro_input_state_t s_input_state_callback = nullptr;
	retro_log_printf_t s_log_callback = nullptr;
	retro_rumble_interface s_rumble_interface{};
	MemorySettingsInterface s_settings;
	std::string s_last_error;
	bool s_runtime_ready = false;
	bool s_cpu_thread_ready = false;
	bool s_game_loaded = false;
	bool s_fast_boot = true;
	std::vector<u32> s_video_frame;
	std::array<unsigned, 2> s_controller_devices = {RETRO_DEVICE_ANALOG, RETRO_DEVICE_NONE};
	std::array<float, 2048 * 2> s_audio_float{};
	std::array<int16_t, 2048 * 2> s_audio_s16{};

	bool Fail(std::string message)
	{
		s_last_error = std::move(message);
		if (s_log_callback)
			s_log_callback(RETRO_LOG_ERROR, "[LRPS2 Modern] %s\n", s_last_error.c_str());
		return false;
	}

	static constexpr retro_variable s_core_variables[] = {
		{"pcsx2_renderer", "Renderer (Restart Required); D3D11|OpenGL|Vulkan|Software"},
		{"pcsx2_upscale_multiplier", "Internal Resolution; 1x Native (PS2)|2x Native (~720p)|3x Native (~1080p)|4x Native (~1440p/2K)|6x Native (~2160p/4K)"},
		{"pcsx2_frame_transfer_resolution", "Frame Transfer Resolution; Full Internal|Native (Fast)|2x Native"},
		{"pcsx2_mtvu", "Multi-Threaded VU1 (Restart Required); enabled|disabled"},
		{"pcsx2_instant_vu1", "Instant VU1 (Restart Required); enabled|disabled"},
		{"pcsx2_fastboot", "Fast Boot; enabled|disabled"},
		{"pcsx2_fastcdvd", "Fast CDVD; disabled|enabled"},
		{"pcsx2_hw_download_mode", "Hardware Download Mode; Accurate|Unsynchronized|Disable|Disable Readbacks"},
		{"pcsx2_blending_accuracy", "Blending Accuracy; Basic|Medium|High|Minimum|Full|Maximum"},
		{"pcsx2_texture_filtering", "Texture Filtering; Bilinear (PS2)|Nearest|Bilinear (Forced)|Bilinear (Forced excluding sprite)"},
		{"pcsx2_anisotropic_filtering", "Anisotropic Filtering; disabled|2x|4x|8x|16x"},
		{"pcsx2_dithering", "Dithering; Unscaled|Scaled|disabled|Force 32bit"},
		{"pcsx2_ee_cycle_rate", "EE Cycle Rate; 100% (Normal Speed)|75% (Underclock)|60% (Underclock)|50% (Underclock)|130% (Overclock)|180% (Overclock)|300% (Overclock)"},
		{"pcsx2_ee_cycle_skip", "EE Cycle Skipping; disabled|Mild Underclock|Moderate Underclock|Maximum Underclock"},
		{"pcsx2_preload_frame_data", "Preload Frame Data; disabled|enabled"},
		{"pcsx2_half_pixel_offset", "Half Pixel Offset; disabled|Normal (Vertex)|Special (Texture)|Special (Texture - Aggressive)|Align to Native"},
		{"pcsx2_widescreen_hint", "Widescreen Patches; disabled|enabled|enabled (16:9)"},
		{nullptr, nullptr},
	};

	static constexpr retro_controller_description s_controller_types[] = {
		{"DualShock 2", RETRO_DEVICE_ANALOG},
		{"None", RETRO_DEVICE_NONE},
	};

	static constexpr retro_controller_info s_controller_info[] = {
		{s_controller_types, std::size(s_controller_types)},
		{s_controller_types, std::size(s_controller_types)},
		{nullptr, 0},
	};

	std::string GetFrontendDirectory(unsigned command)
	{
		const char* path = nullptr;
		if (s_environment_callback && s_environment_callback(command, &path) && path)
			return path;
		return {};
	}

	const char* GetCoreVariable(const char* key)
	{
		retro_variable variable{key, nullptr};
		return (s_environment_callback && s_environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &variable)) ? variable.value : nullptr;
	}

	bool CoreVariableEnabled(const char* value)
	{
		return value && (std::strcmp(value, "enabled") == 0 || std::strcmp(value, "true") == 0 ||
			std::strcmp(value, "ON") == 0);
	}

	GSRendererType GetSelectedRenderer()
	{
		retro_variable variable{"pcsx2_renderer", nullptr};
		if (!s_environment_callback || !s_environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) || !variable.value)
			return GSRendererType::DX11;
		if (std::strcmp(variable.value, "OpenGL") == 0)
			return GSRendererType::OGL;
		if (std::strcmp(variable.value, "Vulkan") == 0)
			return GSRendererType::VK;
		if (std::strcmp(variable.value, "Software") == 0 || std::strcmp(variable.value, "Software (SW)") == 0)
			return GSRendererType::SW;
		return GSRendererType::DX11;
	}

	void ApplyCoreOptions()
	{
		const char* value = GetCoreVariable("pcsx2_upscale_multiplier");
		u32 internal_scale = 1;
		if (value)
		{
			internal_scale = static_cast<u32>(std::clamp(std::atoi(value), 1, 16));
			s_settings.SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(internal_scale));
		}
		u32 output_scale = 0;
		if ((value = GetCoreVariable("pcsx2_frame_transfer_resolution")))
		{
			if (std::strcmp(value, "Full Internal") == 0)
				output_scale = 0;
			else if (std::strcmp(value, "2x Native") == 0)
				output_scale = 2;
			else if (std::strcmp(value, "Native (Fast)") == 0)
				output_scale = 1;
		}
		ModernHostSetCaptureResolution(internal_scale, output_scale);

		if ((value = GetCoreVariable("pcsx2_mtvu")))
			s_settings.SetBoolValue("EmuCore/Speedhacks", "vuThread", CoreVariableEnabled(value));
		if ((value = GetCoreVariable("pcsx2_instant_vu1")))
			s_settings.SetBoolValue("EmuCore/Speedhacks", "vu1Instant", CoreVariableEnabled(value));
		if ((value = GetCoreVariable("pcsx2_fastboot")))
			s_fast_boot = CoreVariableEnabled(value);
		if ((value = GetCoreVariable("pcsx2_fastcdvd")))
			s_settings.SetBoolValue("EmuCore/Speedhacks", "fastCDVD", CoreVariableEnabled(value));
		if ((value = GetCoreVariable("pcsx2_preload_frame_data")))
			s_settings.SetBoolValue("EmuCore/GS", "preload_frame_with_gs_data", CoreVariableEnabled(value));

		if ((value = GetCoreVariable("pcsx2_hw_download_mode")))
		{
			GSHardwareDownloadMode mode = GSHardwareDownloadMode::Enabled;
			if (std::strcmp(value, "Unsynchronized") == 0)
				mode = GSHardwareDownloadMode::Unsynchronized;
			else if (std::strcmp(value, "Disable Readbacks") == 0)
				mode = GSHardwareDownloadMode::NoReadbacks;
			else if (std::strcmp(value, "Disable") == 0 || std::strcmp(value, "Disabled") == 0)
				mode = GSHardwareDownloadMode::Disabled;
			s_settings.SetIntValue("EmuCore/GS", "HWDownloadMode", static_cast<int>(mode));
		}

		if ((value = GetCoreVariable("pcsx2_blending_accuracy")))
		{
			AccBlendLevel level = AccBlendLevel::Basic;
			if (std::strcmp(value, "Minimum") == 0) level = AccBlendLevel::Minimum;
			else if (std::strcmp(value, "Medium") == 0) level = AccBlendLevel::Medium;
			else if (std::strcmp(value, "High") == 0) level = AccBlendLevel::High;
			else if (std::strcmp(value, "Full") == 0) level = AccBlendLevel::Full;
			else if (std::strcmp(value, "Maximum") == 0) level = AccBlendLevel::Maximum;
			s_settings.SetIntValue("EmuCore/GS", "accurate_blending_unit", static_cast<int>(level));
		}

		if ((value = GetCoreVariable("pcsx2_texture_filtering")))
		{
			BiFiltering filter = BiFiltering::PS2;
			if (std::strcmp(value, "Nearest") == 0) filter = BiFiltering::Nearest;
			else if (std::strcmp(value, "Bilinear (Forced)") == 0) filter = BiFiltering::Forced;
			else if (std::strcmp(value, "Bilinear (Forced excluding sprite)") == 0) filter = BiFiltering::Forced_But_Sprite;
			s_settings.SetIntValue("EmuCore/GS", "filter", static_cast<int>(filter));
		}

		if ((value = GetCoreVariable("pcsx2_anisotropic_filtering")))
			s_settings.SetUIntValue("EmuCore/GS", "MaxAnisotropy", static_cast<u32>(std::max(std::atoi(value), 0)));
		if ((value = GetCoreVariable("pcsx2_dithering")))
		{
			u32 mode = 2;
			if (std::strcmp(value, "disabled") == 0) mode = 0;
			else if (std::strcmp(value, "Scaled") == 0) mode = 1;
			else if (std::strcmp(value, "Force 32bit") == 0) mode = 3;
			s_settings.SetUIntValue("EmuCore/GS", "dithering_ps2", mode);
		}

		if ((value = GetCoreVariable("pcsx2_ee_cycle_rate")))
		{
			int rate = 0;
			if (std::strncmp(value, "50%", 3) == 0) rate = -3;
			else if (std::strncmp(value, "60%", 3) == 0) rate = -2;
			else if (std::strncmp(value, "75%", 3) == 0) rate = -1;
			else if (std::strncmp(value, "130%", 4) == 0) rate = 1;
			else if (std::strncmp(value, "180%", 4) == 0) rate = 2;
			else if (std::strncmp(value, "300%", 4) == 0) rate = 3;
			s_settings.SetIntValue("EmuCore/Speedhacks", "EECycleRate", rate);
		}
		if ((value = GetCoreVariable("pcsx2_ee_cycle_skip")))
		{
			int skip = 0;
			if (std::strcmp(value, "Mild Underclock") == 0) skip = 1;
			else if (std::strcmp(value, "Moderate Underclock") == 0) skip = 2;
			else if (std::strcmp(value, "Maximum Underclock") == 0) skip = 3;
			s_settings.SetIntValue("EmuCore/Speedhacks", "EECycleSkip", skip);
		}

		if ((value = GetCoreVariable("pcsx2_half_pixel_offset")))
		{
			GSHalfPixelOffset offset = GSHalfPixelOffset::Off;
			if (std::strcmp(value, "Normal (Vertex)") == 0) offset = GSHalfPixelOffset::Normal;
			else if (std::strcmp(value, "Special (Texture)") == 0) offset = GSHalfPixelOffset::Special;
			else if (std::strcmp(value, "Special (Texture - Aggressive)") == 0) offset = GSHalfPixelOffset::SpecialAggressive;
			else if (std::strcmp(value, "Align to Native") == 0) offset = GSHalfPixelOffset::Native;
			s_settings.SetBoolValue("EmuCore/GS", "UserHacks", offset != GSHalfPixelOffset::Off);
			s_settings.SetIntValue("EmuCore/GS", "UserHacks_HalfPixelOffset", static_cast<int>(offset));
		}

		if ((value = GetCoreVariable("pcsx2_widescreen_hint")))
		{
			const bool enabled = CoreVariableEnabled(value) || std::strncmp(value, "enabled (", 9) == 0;
			s_settings.SetBoolValue("EmuCore", "EnableWideScreenPatches", enabled);
			if (enabled)
				s_settings.SetStringValue("EmuCore/GS", "AspectRatio", "16:9");
		}
	}

	bool InitializeRuntime()
	{
		if (s_runtime_ready)
			return true;

		const char* hardware_error = nullptr;
		if (!VMManager::PerformEarlyHardwareChecks(&hardware_error))
			return Fail(std::string("early hardware check failed: ") +
				(hardware_error ? hardware_error : "unknown error"));

		const std::string system_directory = GetFrontendDirectory(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY);
		if (system_directory.empty())
			return Fail("frontend returned an empty system directory");

		// CATui keeps the BIOS and writable PCSX2 data under this root. The Windows
		// core carries its own versioned resource archive and expands it on demand.
		EmuFolders::AppRoot = Path::Combine(system_directory, "pcsx2");
		EmuFolders::DataRoot = EmuFolders::AppRoot;
		EmuFolders::Settings = Path::Combine(EmuFolders::DataRoot, "inis");
		EmuFolders::Resources = Path::Combine(EmuFolders::DataRoot, "resources");
		if (!ModernHostEnsureEmbeddedResources(EmuFolders::AppRoot))
			return Fail("embedded PCSX2 resources could not be extracted or validated under " + EmuFolders::AppRoot);
		const std::string font_path = EmuFolders::GetOverridableResourcePath(
			"fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto font_data = FileSystem::MapBinaryFileForRead(font_path.c_str());
		if (font_data.empty())
			return Fail("required font could not be read: " + font_path);
		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo font{};
		font.data = font_data;
		fonts.push_back(font);
		ImGuiManager::SetFonts(std::move(fonts));

		// A frontend may keep the DLL loaded and call retro_init() again after
		// retro_deinit(). PCSX2's layered settings singleton survives that cycle,
		// so registering the same base layer twice trips its fatal assertion.
		// Reuse our process-lifetime MemorySettingsInterface when it is already
		// installed, while still rejecting an unexpected foreign settings layer.
		SettingsInterface* const base_settings = Host::Internal::GetBaseSettingsLayer();
		if (base_settings == nullptr)
			Host::Internal::SetBaseSettingsLayer(&s_settings);
		else if (base_settings != &s_settings)
			return Fail("PCSX2 base settings layer belongs to another runtime instance");
		VMManager::SetDefaultSettings(s_settings, true, true, true, true, true);

		// The first VM-only milestone used the Null renderer. With lifecycle and
		// one-VSync-per-retro_run verified, exercise the real modern D3D11 GS on its
		// supported surfaceless path before adding frontend frame transfer.
		s_settings.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GetSelectedRenderer()));
		s_settings.SetBoolValue("InputSources", "SDL", false);
		s_settings.SetBoolValue("InputSources", "XInput", false);
		s_settings.SetStringValue("SPU2/Output", "Backend", "Null");
		s_settings.SetBoolValue("EmuCore/GS", "VsyncEnable", false);
		ApplyCoreOptions();
		for (u32 port = 0; port < s_controller_devices.size(); port++)
		{
			const std::string section = Pad::GetConfigSection(port);
			s_settings.SetStringValue(section.c_str(), "Type",
				s_controller_devices[port] == RETRO_DEVICE_NONE ? "None" : "DualShock2");
		}
		VMManager::Internal::LoadStartupSettings();
		s_runtime_ready = true;
		return true;
	}

	void ShutdownGame()
	{
		if (s_game_loaded && VMManager::HasValidVM())
		{
			MTGS::RunOnGSThread(ModernHostResetFrameCapture);
			MTGS::WaitGS(false);
		}
		if (s_game_loaded && VMManager::HasValidVM())
			VMManager::Shutdown(false);
		s_game_loaded = false;
		if (s_cpu_thread_ready)
		{
			VMManager::Internal::CPUThreadShutdown();
			s_cpu_thread_ready = false;
		}
	}

	float ReadAxis(unsigned port, unsigned index, unsigned id)
	{
		const int16_t value = s_input_state_callback(port, RETRO_DEVICE_ANALOG, index, id);
		return value < 0 ? static_cast<float>(value) / 32768.0f : static_cast<float>(value) / 32767.0f;
	}

	void UpdateControllers()
	{
		if (!s_input_state_callback)
			return;

		using Input = PadDualshock2::Inputs;
		static constexpr std::pair<unsigned, Input> buttons[] = {
			{RETRO_DEVICE_ID_JOYPAD_UP, Input::PAD_UP}, {RETRO_DEVICE_ID_JOYPAD_RIGHT, Input::PAD_RIGHT},
			{RETRO_DEVICE_ID_JOYPAD_DOWN, Input::PAD_DOWN}, {RETRO_DEVICE_ID_JOYPAD_LEFT, Input::PAD_LEFT},
			{RETRO_DEVICE_ID_JOYPAD_X, Input::PAD_TRIANGLE}, {RETRO_DEVICE_ID_JOYPAD_A, Input::PAD_CIRCLE},
			{RETRO_DEVICE_ID_JOYPAD_B, Input::PAD_CROSS}, {RETRO_DEVICE_ID_JOYPAD_Y, Input::PAD_SQUARE},
			{RETRO_DEVICE_ID_JOYPAD_SELECT, Input::PAD_SELECT}, {RETRO_DEVICE_ID_JOYPAD_START, Input::PAD_START},
			{RETRO_DEVICE_ID_JOYPAD_L, Input::PAD_L1}, {RETRO_DEVICE_ID_JOYPAD_L2, Input::PAD_L2},
			{RETRO_DEVICE_ID_JOYPAD_R, Input::PAD_R1}, {RETRO_DEVICE_ID_JOYPAD_R2, Input::PAD_R2},
			{RETRO_DEVICE_ID_JOYPAD_L3, Input::PAD_L3}, {RETRO_DEVICE_ID_JOYPAD_R3, Input::PAD_R3},
		};

		for (u32 port = 0; port < s_controller_devices.size(); port++)
		{
			if (s_controller_devices[port] == RETRO_DEVICE_NONE || !Pad::GetPad(static_cast<u8>(port)))
				continue;
			for (const auto& [retro_button, ps2_button] : buttons)
				Pad::SetControllerState(port, ps2_button,
					s_input_state_callback(port, RETRO_DEVICE_JOYPAD, 0, retro_button) ? 1.0f : 0.0f);

			const float lx = ReadAxis(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
			const float ly = ReadAxis(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
			const float rx = ReadAxis(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
			const float ry = ReadAxis(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
			Pad::SetControllerState(port, Input::PAD_L_LEFT, std::max(-lx, 0.0f));
			Pad::SetControllerState(port, Input::PAD_L_RIGHT, std::max(lx, 0.0f));
			Pad::SetControllerState(port, Input::PAD_L_UP, std::max(-ly, 0.0f));
			Pad::SetControllerState(port, Input::PAD_L_DOWN, std::max(ly, 0.0f));
			Pad::SetControllerState(port, Input::PAD_R_LEFT, std::max(-rx, 0.0f));
			Pad::SetControllerState(port, Input::PAD_R_RIGHT, std::max(rx, 0.0f));
			Pad::SetControllerState(port, Input::PAD_R_UP, std::max(-ry, 0.0f));
			Pad::SetControllerState(port, Input::PAD_R_DOWN, std::max(ry, 0.0f));
		}
	}

	void DrainAudio()
	{
		if (!s_audio_batch_callback && !s_audio_callback)
			return;
		for (;;)
		{
			const u32 frames = SPU2::ReadOutputFrames(s_audio_float.data(), s_audio_float.size() / 2);
			if (frames == 0)
				break;
			for (u32 i = 0; i < frames * 2; i++)
				s_audio_s16[i] = static_cast<int16_t>(std::clamp(s_audio_float[i], -1.0f, 1.0f) * 32767.0f);
			if (s_audio_batch_callback)
				s_audio_batch_callback(s_audio_s16.data(), frames);
			else
			{
				for (u32 i = 0; i < frames; i++)
					s_audio_callback(s_audio_s16[i * 2], s_audio_s16[i * 2 + 1]);
			}
		}
	}
}

void ModernHostSetRumble(unsigned port, float strong, float weak)
{
	if (!s_rumble_interface.set_rumble_state || port >= s_controller_devices.size())
		return;
	s_rumble_interface.set_rumble_state(port, RETRO_RUMBLE_STRONG,
		static_cast<uint16_t>(std::clamp(strong, 0.0f, 1.0f) * 65535.0f));
	s_rumble_interface.set_rumble_state(port, RETRO_RUMBLE_WEAK,
		static_cast<uint16_t>(std::clamp(weak, 0.0f, 1.0f) * 65535.0f));
}

extern "C"
{
	RETRO_API unsigned retro_api_version(void)
	{
		return RETRO_API_VERSION;
	}

	RETRO_API void retro_set_environment(retro_environment_t callback)
	{
		s_environment_callback = callback;
		if (s_environment_callback)
		{
			retro_log_callback logging{};
			if (s_environment_callback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
				s_log_callback = logging.log;
			bool support_no_game = false;
			s_environment_callback(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &support_no_game);
			unsigned pixel_format = RETRO_PIXEL_FORMAT_XRGB8888;
			s_environment_callback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixel_format);
			s_environment_callback(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(s_core_variables));
			s_environment_callback(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, const_cast<retro_controller_info*>(s_controller_info));
			s_environment_callback(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &s_rumble_interface);
		}
	}

	RETRO_API void retro_set_video_refresh(retro_video_refresh_t callback)
	{
		s_video_callback = callback;
		ModernHostSetFrameCaptureEnabled(callback != nullptr);
	}

	RETRO_API void retro_set_audio_sample(retro_audio_sample_t callback)
	{
		s_audio_callback = callback;
	}

	RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback)
	{
		s_audio_batch_callback = callback;
	}

	RETRO_API void retro_set_input_poll(retro_input_poll_t callback)
	{
		s_input_poll_callback = callback;
	}

	RETRO_API void retro_set_input_state(retro_input_state_t callback)
	{
		s_input_state_callback = callback;
	}

	RETRO_API void retro_init(void)
	{
		InitializeRuntime();
	}

	RETRO_API void retro_deinit(void)
	{
		ShutdownGame();
		s_runtime_ready = false;
		s_environment_callback = nullptr;
		s_video_callback = nullptr;
		s_audio_callback = nullptr;
		s_audio_batch_callback = nullptr;
		s_input_poll_callback = nullptr;
		s_input_state_callback = nullptr;
		s_log_callback = nullptr;
	}

	RETRO_API void retro_get_system_info(struct retro_system_info* info)
	{
		if (!info)
			return;
		std::memset(info, 0, sizeof(*info));
		info->library_name = "LRPS2 Modern";
		info->library_version = "PCSX2 2.8.1 CATui";
		info->valid_extensions = "elf|iso|ciso|cue|bin|gz|chd|cso|zso|mdf|nrg|dump|img|m3u|isz";
		info->need_fullpath = true;
		info->block_extract = false;
	}

	RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info)
	{
		if (!info)
			return;
		std::memset(info, 0, sizeof(*info));
		info->geometry.base_width = 640;
		info->geometry.base_height = 448;
		info->geometry.max_width = 8192;
		info->geometry.max_height = 8192;
		info->geometry.aspect_ratio = 4.0f / 3.0f;
		info->timing.fps = 59.94;
		info->timing.sample_rate = 48000.0;
	}

	RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
	{
		if (port < s_controller_devices.size())
			s_controller_devices[port] = device;
	}

	RETRO_API void retro_reset(void)
	{
		if (s_game_loaded)
			VMManager::Reset();
	}

	RETRO_API void retro_run(void)
	{
		bool options_changed = false;
		if (s_game_loaded && s_environment_callback &&
			s_environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_changed) && options_changed)
		{
			ApplyCoreOptions();
			VMManager::ApplySettings();
		}
		if (s_input_poll_callback)
			s_input_poll_callback();
		UpdateControllers();
		if (s_game_loaded && VMManager::GetState() == VMState::Running)
			VMManager::Execute();
		DrainAudio();
		if (s_video_callback)
		{
			unsigned width = 0;
			unsigned height = 0;
			if (ModernHostTakeFrame(&s_video_frame, &width, &height) && !s_video_frame.empty())
				s_video_callback(s_video_frame.data(), width, height, width * sizeof(u32));
			else
				s_video_callback(nullptr, 0, 0, 0);
		}
	}

	RETRO_API size_t retro_serialize_size(void)
	{
		return 0;
	}

	RETRO_API bool retro_serialize(void*, size_t)
	{
		return false;
	}

	RETRO_API bool retro_unserialize(const void*, size_t)
	{
		return false;
	}

	RETRO_API void retro_cheat_reset(void)
	{
	}

	RETRO_API void retro_cheat_set(unsigned, bool, const char*)
	{
	}

	RETRO_API bool retro_load_game(const struct retro_game_info* game)
	{
		s_last_error.clear();
		if (!game || !game->path || !game->path[0])
			return Fail("retro_load_game received an empty content path");
		if (!InitializeRuntime())
			return false;
		ShutdownGame();
		if (!VMManager::Internal::CPUThreadInitialize())
			return Fail("CPU thread initialization failed");
		s_cpu_thread_ready = true;
		VMManager::ApplySettings();

		VMBootParameters parameters;
		parameters.filename = game->path;
		parameters.fast_boot = s_fast_boot;
		Error boot_error;
		const VMBootResult boot_result = VMManager::Initialize(parameters, &boot_error);
		if (boot_result != VMBootResult::StartupSuccess)
		{
			ShutdownGame();
			return Fail("VM initialization failed with VMBootResult=" +
				std::to_string(static_cast<int>(boot_result)) + ", reason=" +
				(boot_error.GetDescription().empty() ? "unspecified" : boot_error.GetDescription()) +
				", content=" + game->path);
		}
		VMManager::SetState(VMState::Running);
		// The frontend owns cadence for libretro. Avoid spending the 16.68 ms frame
		// budget inside PCSX2 and then adding CPU readback plus Godot texture upload
		// after that sleep; one unrestricted VSync per retro_run lets the frontend
		// account for the complete emulation-and-presentation cost.
		VMManager::SetLimiterMode(LimiterModeType::Unlimited);
		s_game_loaded = true;
		return true;
	}

	RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t)
	{
		return false;
	}

	RETRO_API void retro_unload_game(void)
	{
		ShutdownGame();
	}

	RETRO_API unsigned retro_get_region(void)
	{
		return RETRO_REGION_NTSC;
	}

	RETRO_API void* retro_get_memory_data(unsigned id)
	{
		return id == RETRO_MEMORY_SYSTEM_RAM && s_game_loaded && eeMem ? eeMem->Main : nullptr;
	}

	RETRO_API size_t retro_get_memory_size(unsigned id)
	{
		return id == RETRO_MEMORY_SYSTEM_RAM && s_game_loaded && eeMem
			? static_cast<size_t>(Ps2MemSize::ExposedRam) : 0;
	}

	RETRO_API double retro_get_game_fps(void)
	{
		return s_game_loaded ? static_cast<double>(PerformanceMetrics::GetInternalFPS()) : 0.0;
	}

	RETRO_API double retro_get_emulation_fps(void)
	{
		return s_game_loaded ? static_cast<double>(PerformanceMetrics::GetFPS()) : 0.0;
	}

	RETRO_API uint64_t retro_get_emulated_frame_count(void)
	{
		return s_game_loaded ? static_cast<uint64_t>(PerformanceMetrics::GetFrameNumber()) : 0;
	}

	RETRO_API const char* retro_get_last_error(void)
	{
		return s_last_error.c_str();
	}
}
