// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eden_libretro/libretro_core.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <stop_token>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/string_util.h"
#include "core/frontend/applets/software_keyboard.h"
#include "core/hle/service/am/frontend/applets.h"
#include "common/settings.h"
#include "core/cpu_manager.h"
#include "core/perf_stats.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "eden_libretro/environment.h"
#include "audio_core/audio_core.h"
#include "audio_core/sink/sink_details.h"
#include "eden_libretro/logging/libretro_log.h"
#include "eden_libretro/video/opengl_bridge.h"
#include "eden_libretro/video/vulkan_bridge.h"
#include "frontend_common/settings_generator.h"
#include "frontend_common/content_manager.h"
#include "input_common/input_poller.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_notify.h"

namespace Libretro {

namespace {


using SwkbdResult = Service::AM::Frontend::SwkbdResult;
using SwkbdReplyType = Service::AM::Frontend::SwkbdReplyType;

struct KeyboardBridgeState {
    std::mutex mutex;
    Core::Frontend::KeyboardInitializeParameters parameters;
    Core::Frontend::SoftwareKeyboardApplet::SubmitNormalCallback normal_callback;
    Core::Frontend::SoftwareKeyboardApplet::SubmitInlineCallback inline_callback;
    std::string request_json;
    std::u16string current_text;
    std::uint64_t next_id = 0;
    std::uint64_t pending_id = 0;
    bool inline_mode = false;
    bool text_check = false;
    bool disable_cancel = false;
    u32 min_length = 0;
    u32 max_length = 0;

    void QueueRequest(const std::u16string& message = {}) {
        ++next_id;
        pending_id = next_id;
        request_json = nlohmann::json{
            {"id", pending_id},
            {"mode", text_check ? "text_check" : inline_mode ? "inline" : "normal"},
            {"header", Common::UTF16ToUTF8(parameters.header_text)},
            {"guide", Common::UTF16ToUTF8(parameters.guide_text)},
            {"message", Common::UTF16ToUTF8(message)},
            {"initial_text", Common::UTF16ToUTF8(current_text)},
            {"cursor", parameters.initial_cursor_position},
            {"min_length", min_length},
            {"max_length", max_length},
            {"keyboard_type", static_cast<u32>(parameters.type)},
            {"password", parameters.password_mode ==
                             Service::AM::Frontend::SwkbdPasswordMode::Enabled},
            {"disable_cancel", disable_cancel},
        }.dump();
    }
};

std::mutex keyboard_state_mutex;
std::shared_ptr<KeyboardBridgeState> keyboard_state;

class LibretroSoftwareKeyboard final : public Core::Frontend::SoftwareKeyboardApplet {
public:
    explicit LibretroSoftwareKeyboard(std::shared_ptr<KeyboardBridgeState> state_)
        : state{std::move(state_)} {}

    void Close() const override {
        std::lock_guard lock{state->mutex};
        state->request_json.clear();
        state->pending_id = 0;
        state->normal_callback = {};
        state->inline_callback = {};
    }

    void InitializeKeyboard(bool inline_mode,
                            Core::Frontend::KeyboardInitializeParameters parameters,
                            SubmitNormalCallback normal_callback,
                            SubmitInlineCallback inline_callback) override {
        std::lock_guard lock{state->mutex};
        state->parameters = std::move(parameters);
        state->normal_callback = std::move(normal_callback);
        state->inline_callback = std::move(inline_callback);
        state->current_text = state->parameters.initial_text;
        state->inline_mode = inline_mode;
        state->text_check = false;
        state->request_json.clear();
        state->pending_id = 0;
    }

    void ShowNormalKeyboard() const override {
        std::lock_guard lock{state->mutex};
        state->inline_mode = false;
        state->text_check = false;
        state->min_length = state->parameters.min_text_length;
        state->max_length = state->parameters.max_text_length;
        state->disable_cancel = state->parameters.disable_cancel_button;
        state->QueueRequest();
    }

    void ShowTextCheckDialog(Service::AM::Frontend::SwkbdTextCheckResult result,
                             std::u16string message) const override {
        std::lock_guard lock{state->mutex};
        state->text_check = result == Service::AM::Frontend::SwkbdTextCheckResult::Confirm;
        state->QueueRequest(message);
    }

    void ShowInlineKeyboard(Core::Frontend::InlineAppearParameters appear) const override {
        std::lock_guard lock{state->mutex};
        state->inline_mode = true;
        state->text_check = false;
        state->min_length = appear.min_text_length;
        state->max_length = appear.max_text_length;
        state->disable_cancel = appear.disable_cancel_button;
        state->QueueRequest();
    }

    void HideInlineKeyboard() const override {
        std::lock_guard lock{state->mutex};
        state->request_json.clear();
        state->pending_id = 0;
    }

    void InlineTextChanged(Core::Frontend::InlineTextParameters text) const override {
        SubmitInlineCallback callback;
        {
            std::lock_guard lock{state->mutex};
            state->current_text = text.input_text;
            callback = state->inline_callback;
            if (state->pending_id != 0) {
                state->QueueRequest();
            }
        }
        if (callback) {
            callback(SwkbdReplyType::ChangedString, std::move(text.input_text),
                     text.cursor_position);
        }
    }

    void ExitKeyboard() const override { Close(); }

private:
    std::shared_ptr<KeyboardBridgeState> state;
};

const char* GetSoftwareKeyboardRequest() {
    std::shared_ptr<KeyboardBridgeState> state;
    {
        std::lock_guard lock{keyboard_state_mutex};
        state = keyboard_state;
    }
    if (!state) {
        return nullptr;
    }
    thread_local std::string request;
    std::lock_guard lock{state->mutex};
    request = state->request_json;
    return request.empty() ? nullptr : request.c_str();
}

bool SubmitSoftwareKeyboard(std::uint64_t id, const char* text_utf8, bool accepted) {
    std::shared_ptr<KeyboardBridgeState> state;
    {
        std::lock_guard lock{keyboard_state_mutex};
        state = keyboard_state;
    }
    if (!state || !text_utf8) {
        return false;
    }
    const std::u16string text = Common::UTF8ToUTF16(text_utf8);
    Core::Frontend::SoftwareKeyboardApplet::SubmitNormalCallback normal;
    Core::Frontend::SoftwareKeyboardApplet::SubmitInlineCallback inline_callback;
    bool inline_mode = false;
    bool text_check = false;
    {
        std::lock_guard lock{state->mutex};
        if (id == 0 || id != state->pending_id ||
            (accepted && (text.size() < state->min_length ||
                          (state->max_length > 0 && text.size() > state->max_length))) ||
            (!accepted && state->disable_cancel)) {
            return false;
        }
        inline_mode = state->inline_mode;
        text_check = state->text_check;
        normal = state->normal_callback;
        inline_callback = state->inline_callback;
        state->current_text = text;
        state->pending_id = 0;
        state->request_json.clear();
    }
    if (inline_mode) {
        if (!inline_callback) {
            return false;
        }
        if (accepted) {
            inline_callback(SwkbdReplyType::ChangedString, text, static_cast<s32>(text.size()));
        }
        inline_callback(accepted ? SwkbdReplyType::DecidedEnter : SwkbdReplyType::DecidedCancel,
                        text, static_cast<s32>(text.size()));
        return true;
    }
    if (!normal) {
        return false;
    }
    normal(accepted ? SwkbdResult::Ok : SwkbdResult::Cancel, text, text_check);
    return true;
}

constexpr const char* CatuiDisabledAddonsFile = "catui_disabled_addons.json";
constexpr const char* CatuiContentLabelsFile = "catui_content_labels.json";
std::string installed_content_json = R"({"ok":false,"content":[]})";

bool ParseTitleId(const char* text, u64& value) {
    if (!text || text[0] == '\0') {
        return false;
    }
    const auto end = text + std::strlen(text);
    const auto parsed = std::from_chars(text, end, value, 16);
    return parsed.ec == std::errc{} && parsed.ptr == end && value != 0;
}

std::filesystem::path DisabledAddonsPath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / CatuiDisabledAddonsFile;
}

std::filesystem::path ContentLabelsPath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / CatuiContentLabelsFile;
}

nlohmann::json LoadCatuiContentLabels() {
    std::ifstream input(ContentLabelsPath());
    if (!input) {
        return nlohmann::json::object();
    }
    try {
        auto root = nlohmann::json::parse(input);
        return root.is_object() ? std::move(root) : nlohmann::json::object();
    } catch (const nlohmann::json::exception& exception) {
        LOG_WARNING(Config, "Ignoring invalid CATui content labels file: {}", exception.what());
        return nlohmann::json::object();
    }
}

bool SaveCatuiContentLabels(const nlohmann::json& root) {
    std::ofstream output(ContentLabelsPath(), std::ios::trunc);
    if (!output) {
        return false;
    }
    output << root.dump(2) << '\n';
    return output.good();
}

std::string ContentLabelFromPath(const char* filepath) {
    std::string label{filepath ? filepath : ""};
    if (const auto slash = label.find_last_of("/\\"); slash != std::string::npos) {
        label.erase(0, slash + 1);
    }
    if (const auto extension = label.find_last_of('.'); extension != std::string::npos) {
        label.erase(extension);
    }
    for (auto open = label.find('['); open != std::string::npos; open = label.find('[', open)) {
        const auto close = label.find(']', open + 1);
        if (close == std::string::npos) {
            break;
        }
        label.erase(open, close - open + 1);
    }
    std::replace(label.begin(), label.end(), '_', ' ');
    const auto is_padding = [](const unsigned char character) {
        return std::isspace(character) || character == '-' || character == ':';
    };
    while (!label.empty() && is_padding(static_cast<unsigned char>(label.front()))) {
        label.erase(label.begin());
    }
    while (!label.empty() && is_padding(static_cast<unsigned char>(label.back()))) {
        label.pop_back();
    }
    label.erase(std::unique(label.begin(), label.end(), [](const char left, const char right) {
                    return std::isspace(static_cast<unsigned char>(left)) &&
                           std::isspace(static_cast<unsigned char>(right));
                }),
                label.end());
    return label;
}

void LoadCatuiDisabledAddons() {
    Settings::values.disabled_addons.clear();
    std::ifstream input(DisabledAddonsPath());
    if (!input) {
        return;
    }
    try {
        const auto root = nlohmann::json::parse(input);
        if (!root.is_object()) {
            return;
        }
        for (const auto& [title_text, disabled_json] : root.items()) {
            u64 title_id{};
            if (!ParseTitleId(title_text.c_str(), title_id) || !disabled_json.is_array()) {
                continue;
            }
            std::vector<std::string> disabled;
            for (const auto& value : disabled_json) {
                if (value.is_string()) {
                    disabled.push_back(value.get<std::string>());
                }
            }
            Settings::values.disabled_addons.insert_or_assign(title_id, std::move(disabled));
        }
    } catch (const nlohmann::json::exception& exception) {
        LOG_WARNING(Config, "Ignoring invalid CATui disabled add-ons file: {}", exception.what());
    }
}

bool SaveCatuiDisabledAddons() {
    nlohmann::json root = nlohmann::json::object();
    for (const auto& [title_id, disabled] : Settings::values.disabled_addons) {
        if (!disabled.empty()) {
            root[fmt::format("{:016X}", title_id)] = disabled;
        }
    }
    std::ofstream output(DisabledAddonsPath(), std::ios::trunc);
    if (!output) {
        return false;
    }
    output << root.dump(2) << '\n';
    return output.good();
}

std::string FormatContentVersion(u32 version, const std::string& display_version) {
    if (!display_version.empty()) {
        return display_version.starts_with('v') ? display_version
                                                : fmt::format("v{}", display_version);
    }
    return fmt::format("v{}", version);
}

std::unique_ptr<Core::System> CreateContentSystem(
    const std::shared_ptr<FileSys::RealVfsFilesystem>& vfs) {
    auto content_system = std::make_unique<Core::System>();
    content_system->Initialize();
    content_system->SetFilesystem(vfs);
    content_system->SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    content_system->GetFileSystemController().CreateFactories(*vfs);
    return content_system;
}

void ReportShaderProgress(std::size_t value, std::size_t total) {
    if (total == 0) {
        return;
    }

    const std::string text = "Compiling shaders " + std::to_string(value) + "/" +
                             std::to_string(total);
    const auto percentage = static_cast<int8_t>(
        std::clamp<std::size_t>((value * 100) / total, 0, 100));
    retro_message_ext message{
        .msg = text.c_str(),
        .duration = 1000,
        .priority = 1,
        .level = RETRO_LOG_INFO,
        .target = RETRO_MESSAGE_TARGET_OSD,
        .type = RETRO_MESSAGE_TYPE_PROGRESS,
        .progress = percentage,
    };
    if (auto callback = Environment::GetEnvironmentCallback(); callback) {
        callback(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
    }
}

void ReportRuntimeShaderActivity(int shaders_started, int shaders_in_flight) {
    const bool active = shaders_in_flight > 0;
    const std::string text = active
                                 ? "Compiling runtime shaders " + std::to_string(shaders_started)
                                 : "Runtime shader compilation complete";
    retro_message_ext message{
        .msg = text.c_str(),
        .duration = 1000,
        .priority = 1,
        .level = RETRO_LOG_INFO,
        .target = RETRO_MESSAGE_TARGET_OSD,
        .type = RETRO_MESSAGE_TYPE_PROGRESS,
        .progress = static_cast<int8_t>(active ? 0 : 100),
    };
    if (auto callback = Environment::GetEnvironmentCallback(); callback) {
        callback(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
    }
}

void ReportInstallProgress(std::size_t value, std::size_t total) {
    const auto percentage = static_cast<int8_t>(total == 0 ? 0 :
        std::clamp<std::size_t>((value * 100) / total, 0, 100));
    const std::string text = percentage >= 100 ? "Finishing content installation"
                                               : "Installing update or DLC";
    retro_message_ext message{
        .msg = text.c_str(), .duration = 1000, .priority = 1,
        .level = RETRO_LOG_INFO, .target = RETRO_MESSAGE_TARGET_OSD,
        .type = RETRO_MESSAGE_TYPE_PROGRESS, .progress = percentage,
    };
    if (auto callback = Environment::GetEnvironmentCallback(); callback) {
        callback(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
    }
}

} // namespace

LibretroSession& LibretroSession::GetInstance() {
    static LibretroSession instance;
    return instance;
}

LibretroSession::LibretroSession() = default;
LibretroSession::~LibretroSession() {
    Deinit();
}

void LibretroSession::Init() {
    if (is_initialized) {
        return;
    }

    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(false);

    FrontendCommon::GenerateSettings();
    Environment::InitializeDirectories();
    LoadCatuiDisabledAddons();
    Environment::RegisterCoreOptions();
    Environment::RegisterInputDescriptors();

    Core::Crypto::KeyManager::Instance().ReloadKeys();

    // The libretro driver is the only input backend this frontend needs. Avoid
    // starting Eden's desktop SDL/UDP/USB stack: those process-global workers
    // add long teardown delays and can race the next core instance.
    input_driver = std::make_shared<LibretroDriver>();
    Common::Input::RegisterInputFactory(input_driver->GetEngineName(),
                                        std::make_shared<InputCommon::InputFactory>(input_driver));
    Common::Input::RegisterOutputFactory(input_driver->GetEngineName(),
                                         std::make_shared<InputCommon::OutputFactory>(input_driver));

    Settings::UpdateGPUAccuracy();
    is_initialized = true;
    LOG_INFO(Frontend, "Eden Libretro Core initialized");
}

void LibretroSession::Deinit() {
    if (is_game_loaded) {
        UnloadGame();
    }
    if (is_initialized) {
        Common::Input::UnregisterInputFactory(input_driver->GetEngineName());
        Common::Input::UnregisterOutputFactory(input_driver->GetEngineName());
        input_driver.reset();
        LOG_INFO(Frontend, "Eden Libretro Core deinitialized");
        Common::Log::Stop();
        is_initialized = false;
    }
}

bool LibretroSession::LoadGame(const char* filepath) {
    if (!filepath || strlen(filepath) == 0) {
        LOG_ERROR(Frontend, "Invalid game path provided");
        return false;
    }

    // Libretro frontends serialize lifecycle calls. Unload before taking the
    // per-frame mutex: UnloadGame() takes the same lock and the old ordering
    // deadlocked when a frontend reused the core for a second title.
    if (is_game_loaded) {
        UnloadGame();
    }
    std::lock_guard lock{run_mutex};

    runtime_shader_reporting_armed = false;
    last_runtime_shader_count = 0;
	last_runtime_shader_started = 0;
	runtime_shader_start_base = 0;

    // Apply the frontend's renderer and other core options without overriding
    // the user's per-core CATUI configuration.
    Settings::values.use_auto_stub.SetValue(false);

    // Apply configuration from core options
    Environment::UpdateVariables();

    // Re-verify directories and reload crypto keys
    Environment::InitializeDirectories();
    LoadCatuiDisabledAddons();
    Core::Crypto::KeyManager::Instance().ReloadKeys();

    // Configure Audio Sink for Libretro
    audio_sink = nullptr;
    AudioCore::Sink::SetCustomSinkFactory([this]() -> std::unique_ptr<AudioCore::Sink::Sink> {
        auto sink = std::make_unique<LibretroAudioSink>();
        if (audio_batch_cb) {
            sink->SetAudioCallback(audio_batch_cb);
        }
        if (!audio_sink) {
            audio_sink = sink.get();
        }
        return sink;
    });
    Settings::values.sink_id.SetValue(Settings::AudioEngine::Libretro);

    // Instantiate EmuWindow
    emu_window = std::make_unique<EmuWindow_Libretro>();

    // Request rumble callback from Libretro environment
    retro_rumble_interface rumble_iface{};
    if (Environment::GetEnvironmentCallback() &&
        Environment::GetEnvironmentCallback()(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble_iface) &&
        rumble_iface.set_rumble_state) {
        input_driver->SetRumbleCallback(rumble_iface.set_rumble_state);
        LOG_INFO(Frontend, "Libretro rumble interface registered successfully");
    }

    const bool is_handheld = Settings::values.use_docked_mode.GetValue() == Settings::ConsoleMode::Handheld;

    // Configure default bindings for players and handheld mode
    auto& players = Settings::values.players.GetValue();
    for (std::size_t p = 0; p < players.size(); ++p) {
        auto& player = players[p];
        player.connected = (p == 0 || (is_handheld && p == 8));
        player.controller_type = (is_handheld && p == 8) ? Settings::ControllerType::Handheld
                                                         : Settings::ControllerType::ProController;
        player.vibration_enabled = true;
        player.vibration_strength = 100;

        const auto port_idx = (p == 8) ? 0 : static_cast<int>(p);

        Common::ParamPackage port_param;
        port_param.Set("port", port_idx);

        const auto btn_map = input_driver->GetButtonMappingForDevice(port_param);
        for (const auto& [btn_idx, pkg] : btn_map) {
            if (static_cast<std::size_t>(btn_idx) < player.buttons.size()) {
                player.buttons[btn_idx] = pkg.Serialize();
            }
        }

        const auto analog_map = input_driver->GetAnalogMappingForDevice(port_param);
        for (const auto& [analog_idx, pkg] : analog_map) {
            if (static_cast<std::size_t>(analog_idx) < player.analogs.size()) {
                player.analogs[analog_idx] = pkg.Serialize();
            }
        }
    }

    // Instantiate Core::System
    system = std::make_unique<Core::System>();
    system->Initialize();
    {
        auto bridge = std::make_shared<KeyboardBridgeState>();
        Service::AM::Frontend::FrontendAppletSet applets;
        applets.software_keyboard = std::make_unique<LibretroSoftwareKeyboard>(bridge);
        system->SetFrontendAppletSet(std::move(applets));
        std::lock_guard keyboard_lock{keyboard_state_mutex};
        keyboard_state = std::move(bridge);
    }

    // Setup virtual filesystem
    auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    system->SetFilesystem(vfs);

    auto content_provider_union = std::make_unique<FileSys::ContentProviderUnion>();
    system->SetContentProvider(std::move(content_provider_union));
    manual_content_provider = std::make_unique<FileSys::ManualContentProvider>();
    system->RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                    manual_content_provider.get());

    // Register every NCA contained in the selected NSP/XCI before boot. Eden's
    // desktop and Android frontends both do this; loading only the main program
    // is insufficient for packages whose control/data content is opened later.
    const auto content_file = vfs->OpenFile(filepath, FileSys::OpenMode::Read);
    if (!content_file || !manual_content_provider->AddEntriesFromContainer(content_file)) {
        LOG_WARNING(Frontend, "Could not register all content entries from {}", filepath);
    }

    system->GetFileSystemController().CreateFactories(*system->GetFilesystem());
    system->GetUserChannel().clear();

    // Complete the same pre-load initialization used by Eden's desktop and
    // Android frontends. Core options update the global Settings values above,
    // but System owns derived runtime state and HID devices that must be
    // refreshed before the application process is loaded. Skipping this left
    // some titles with a live GPU/compositor but no usable application frame.
    system->SetShuttingDown(false);
    system->ApplySettings();
    system->HIDCore().ReloadInputDevices();
    Settings::LogSettings();

    // Applet parameters
    Service::AM::FrontendAppletParameters load_parameters{
        .applet_id = Service::AM::AppletId::Application,
        .applet_type = Service::AM::AppletType::Application,
        .launch_type = Service::AM::LaunchType::FrontendInitiated,
        .program_index = 0,
    };

    // Load executable / ROM
    const auto result = system->Load(*emu_window, filepath, load_parameters);
    if (result != Core::SystemResultStatus::Success) {
        LOG_ERROR(Frontend, "Failed to load game: status {}", static_cast<u32>(result));
        system->ShutdownMainProcess();
        system.reset();
        emu_window.reset();
        return false;
    }

    current_fb_width = is_handheld ? 1280 : 1920;
    current_fb_height = is_handheld ? 720 : 1080;
    emu_window->UpdateLayout(current_fb_width, current_fb_height);

    {
        std::lock_guard fb_lock{frame_mutex};
        frame_buffer_front.assign(current_fb_width * current_fb_height, 0);
        frame_buffer_back.assign(current_fb_width * current_fb_height, 0);
        frame_ready = false;
    }

    emu_window->SetFrameDisplayedCallback([this] {
        if (!system) {
            return;
        }
        if (VulkanBridge::GetInstance().IsHardwareRenderActive()) {
            return;
        }

        if (system->Renderer().IsScreenshotPending()) {
            return;
        }

        const Layout::FramebufferLayout layout = Layout::DefaultFrameLayout(current_fb_width, current_fb_height);

        system->Renderer().RequestScreenshot(
            frame_buffer_back.data(),
            [this](bool invert_y) {
                std::lock_guard lk{frame_mutex};
                if (invert_y) {
                    for (u32 y = 0; y < current_fb_height / 2; ++y) {
                        const auto top = frame_buffer_back.begin() +
                                         static_cast<std::ptrdiff_t>(y) * current_fb_width;
                        const auto bottom = frame_buffer_back.begin() +
                                            static_cast<std::ptrdiff_t>(current_fb_height - 1 - y) *
                                                current_fb_width;
                        std::swap_ranges(top, top + current_fb_width, bottom);
                    }
                }
                std::swap(frame_buffer_front, frame_buffer_back);
                frame_ready = true;
            },
            layout
        );
    });

    // Match Eden's desktop startup path: rebuild cached pipelines before the
    // guest starts. This both avoids the large first-scene hitch and exposes
    // real progress to libretro frontends instead of appearing frozen.
    auto& gpu = system->GPU();
    gpu.ObtainContext();
    if (Settings::values.use_disk_shader_cache.GetValue()) {
        bool shader_progress_reported = false;
        std::size_t shader_progress_total = 0;
        system->Renderer().ReadRasterizer()->LoadDiskResources(
            system->GetApplicationProcessProgramID(), std::stop_token{},
            [&shader_progress_reported, &shader_progress_total](
                VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
                if (stage == VideoCore::LoadCallbackStage::Build) {
                    shader_progress_reported = total > 0;
                    shader_progress_total = total;
                    ReportShaderProgress(value, total);
                }
            });
    if (shader_progress_reported) {
        ReportShaderProgress(shader_progress_total, shader_progress_total);
    }
    }
    // Install this after the optional startup cache batch, regardless of the
    // disk-cache setting. Runtime pipelines can be built synchronously inside
    // the emulation thread, so waiting for the next retro_run would report the
    // status only after the visible hitch.
	runtime_shader_start_base = gpu.ShaderNotify().ShadersStarted();
    gpu.ShaderNotify().SetShaderBuildingCallback([this](int shader_count, int shaders_started) {
		const int session_started = std::max(0, shaders_started - runtime_shader_start_base);
        ReportRuntimeShaderActivity(session_started, shader_count);
    });
    // If startup work still has a renderer task in flight, wait for its final
    // zero before treating later tasks as runtime gameplay compilation.
    runtime_shader_reporting_armed = gpu.ShaderNotify().ShadersInFlight() == 0;
    gpu.ReleaseContext();

    // Start GPU and sync barrier
    gpu.Start();
    system->GetCpuManager().OnGpuReady();

    // Start emulation execution
    system->Run();

    // Directly bind audio_sink to the real output sink
    auto* real_sink = static_cast<LibretroAudioSink*>(&system->AudioCore().GetOutputSink());
    if (real_sink) {
        audio_sink = real_sink;
        if (audio_batch_cb) {
            audio_sink->SetAudioCallback(audio_batch_cb);
        }
    }

    is_game_loaded = true;
    current_game_path = filepath;
    LOG_INFO(Frontend, "Game loaded and emulation started successfully: {}", filepath);
    return true;
}

void LibretroSession::UnloadGame() {
    std::lock_guard lock{run_mutex};
    if (!is_game_loaded) {
        return;
    }

    LOG_INFO(Frontend, "Unloading game...");
    {
        std::lock_guard keyboard_lock{keyboard_state_mutex};
        keyboard_state.reset();
    }
    if (system) {
        system->GPU().ShaderNotify().SetShaderBuildingCallback({});
        // Stop scheduling screenshots before tearing the GPU down. A pending
        // readback used to race ShutdownMainProcess and could leave libretro
        // frontends waiting forever while switching titles.
        if (emu_window) {
            emu_window->SetFrameDisplayedCallback({});
        }
        system->Renderer().CancelScreenshot();
        if (emu_window) {
            emu_window->SetOpen(false);
        }
        LOG_INFO(Frontend, "Eden libretro teardown: frame capture detached");
        if (audio_sink) {
            system->SetShuttingDown(true);
            audio_sink->PrepareForShutdown();
            system->AudioCore().Shutdown();
        }
        // Match Eden's Android/desktop shutdown preparation. Input devices and
        // debugger hooks own service-side callbacks that must be detached
        // before Kernel::CloseServices joins the emulated service threads.
        system->HIDCore().UnloadInputDevices();
        system->HIDCore().SetSupportedStyleTag({Core::HID::NpadStyleSet::All});
        system->DetachDebugger();
        LOG_INFO(Frontend, "Eden libretro teardown: frontend services detached");
        // ShutdownMainProcess performs the coordinated CPU/kernel/GPU stop.
        // Pausing immediately beforehand can suspend a service thread that the
        // shutdown sequence must join, producing an intermittent deadlock.
        system->ShutdownMainProcess();
        LOG_INFO(Frontend, "Eden libretro teardown: main process stopped");
        system.reset();
        LOG_INFO(Frontend, "Eden libretro teardown: system released");
    }
    manual_content_provider.reset();

    audio_sink = nullptr;
    AudioCore::Sink::SetCustomSinkFactory(nullptr);
    emu_window.reset();

    is_game_loaded = false;
    current_game_path.clear();
    runtime_shader_reporting_armed = false;
    last_runtime_shader_count = 0;
	last_runtime_shader_started = 0;
	runtime_shader_start_base = 0;
    LOG_INFO(Frontend, "Game unloaded successfully");
}

void LibretroSession::Reset() {
    if (!is_game_loaded || current_game_path.empty()) {
        return;
    }
    const std::string path = current_game_path;
    UnloadGame();
    LoadGame(path.c_str());
}

void LibretroSession::SetPaused(bool paused) {
    std::scoped_lock lock(run_mutex);
    if (!is_game_loaded || !system) return;
    if (paused == system->IsPaused()) return;
    if (paused) system->Pause();
    else system->Run();
}

bool LibretroSession::IsPaused() const {
    std::scoped_lock lock(run_mutex);
    return is_game_loaded && system && system->IsPaused();
}

int LibretroSession::InstallContent(const char* filepath, const char* expected_title_id) {
    if (!is_initialized || is_game_loaded || !filepath || filepath[0] == '\0') {
        return 5;
    }
    std::scoped_lock lock(run_mutex);
    Environment::InitializeDirectories();
    LoadCatuiDisabledAddons();
    Core::Crypto::KeyManager::Instance().ReloadKeys();

    auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    auto install_system = CreateContentSystem(vfs);

    const auto package = ContentManager::InspectNSP(*vfs, filepath);
    if (!package.valid) {
        return 2;
    }
    u64 expected{};
    if (expected_title_id && expected_title_id[0] != '\0') {
        const auto parsed = std::from_chars(expected_title_id,
                                            expected_title_id + std::strlen(expected_title_id),
                                            expected, 16);
        if (parsed.ec != std::errc{} || package.base_title_id != expected) {
            return 4;
        }
    }
    if (package.type == FileSys::TitleType::Application) {
        return 3;
    }
    if (package.type != FileSys::TitleType::Update && package.type != FileSys::TitleType::AOC) {
        return 2;
    }
    ReportInstallProgress(0, 1);
    const auto result = ContentManager::InstallNSP(
        *install_system, *vfs, filepath,
        [](std::size_t total, std::size_t current) {
            ReportInstallProgress(current, total);
            return false;
        });
    ReportInstallProgress(1, 1);
    if ((result == ContentManager::InstallResult::Success ||
         result == ContentManager::InstallResult::Overwrite) &&
        package.type == FileSys::TitleType::AOC) {
        const auto label = ContentLabelFromPath(filepath);
        if (!label.empty()) {
            auto labels = LoadCatuiContentLabels();
            labels[fmt::format("{:016X}", package.title_id)] = label;
            if (!SaveCatuiContentLabels(labels)) {
                LOG_WARNING(Config, "Failed to save CATui label for DLC {:016X}",
                            package.title_id);
            }
        }
    }
    switch (result) {
    case ContentManager::InstallResult::Success: return 0;
    case ContentManager::InstallResult::Overwrite: return 1;
    case ContentManager::InstallResult::BaseInstallAttempted: return 3;
    default: return 2;
    }
}

const char* LibretroSession::ListInstalledContent(const char* expected_title_id) {
    std::scoped_lock lock(run_mutex);
    u64 title_id{};
    if (!is_initialized || is_game_loaded || !ParseTitleId(expected_title_id, title_id)) {
        installed_content_json = R"({"ok":false,"error":"invalid_title_id","content":[]})";
        return installed_content_json.c_str();
    }
    Environment::InitializeDirectories();
    LoadCatuiDisabledAddons();
    Core::Crypto::KeyManager::Instance().ReloadKeys();
    auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    auto content_system = CreateContentSystem(vfs);
    auto* user_nand = content_system->GetFileSystemController().GetUserNANDContents();
    if (!user_nand) {
        installed_content_json = R"({"ok":false,"error":"nand_unavailable","content":[]})";
        return installed_content_json.c_str();
    }

    const auto& disabled = Settings::values.disabled_addons[title_id];
    const auto is_disabled = [&disabled](const std::string& key) {
        return std::find(disabled.begin(), disabled.end(), key) != disabled.end();
    };
    nlohmann::json content = nlohmann::json::array();
    const u64 update_id = title_id | 0x800;
    if (user_nand->HasEntry(update_id, FileSys::ContentRecordType::Program)) {
        const u32 version = user_nand->GetEntryVersion(update_id).value_or(0);
        const auto metadata =
            FileSys::PatchManager::GetMetadataFromBaseOrUpdate(*content_system, title_id);
        const std::string display_version =
            metadata.first ? metadata.first->GetVersionString() : std::string{};
        content.push_back({
            {"key", "Update (NAND)"}, {"type", "update"}, {"name", "Update"},
            {"version", FormatContentVersion(version, display_version)},
            {"numeric_version", version},
            {"enabled", !is_disabled("Update (NAND)")},
        });
    }

    std::vector<u64> dlc_ids;
    for (const auto& entry : user_nand->ListEntriesFilter(
             FileSys::TitleType::AOC, FileSys::ContentRecordType::Data)) {
        if (FileSys::GetBaseTitleID(entry.title_id) == title_id) {
            dlc_ids.push_back(entry.title_id);
        }
    }
    if (!dlc_ids.empty()) {
        std::sort(dlc_ids.begin(), dlc_ids.end());
        nlohmann::json ids = nlohmann::json::array();
        nlohmann::json names = nlohmann::json::array();
        const auto saved_labels = LoadCatuiContentLabels();
        std::string display_name;
        for (const u64 dlc_id : dlc_ids) {
            const auto id = fmt::format("{:016X}", dlc_id);
            ids.push_back(id);
            if (!saved_labels.contains(id) || !saved_labels[id].is_string()) {
                continue;
            }
            const auto label = saved_labels[id].get<std::string>();
            if (label.empty()) {
                continue;
            }
            names.push_back(label);
            if (!display_name.empty()) {
                display_name += ", ";
            }
            display_name += label;
        }
        if (display_name.empty()) {
            display_name = "DLC";
        } else if (names.size() < dlc_ids.size()) {
            display_name += fmt::format(", + {} more DLC", dlc_ids.size() - names.size());
        }
        content.push_back({
            {"key", "DLC"}, {"type", "dlc"}, {"name", display_name}, {"names", names},
            {"version", fmt::format("{} installed", dlc_ids.size())}, {"title_ids", ids},
            {"enabled", !is_disabled("DLC")},
        });
    }
    installed_content_json = nlohmann::json{{"ok", true}, {"content", content}}.dump();
    return installed_content_json.c_str();
}

bool LibretroSession::SetInstalledContentEnabled(const char* expected_title_id,
                                                 const char* content_key, bool enabled) {
    std::scoped_lock lock(run_mutex);
    u64 title_id{};
    if (!is_initialized || is_game_loaded || !ParseTitleId(expected_title_id, title_id) ||
        !content_key) {
        return false;
    }
    const std::string key{content_key};
    if (key != "Update (NAND)" && key != "DLC") {
        return false;
    }
    Environment::InitializeDirectories();
    LoadCatuiDisabledAddons();
    auto& disabled = Settings::values.disabled_addons[title_id];
    const auto existing = std::find(disabled.begin(), disabled.end(), key);
    if (enabled && existing != disabled.end()) {
        disabled.erase(existing);
    } else if (!enabled && existing == disabled.end()) {
        disabled.push_back(key);
    }
    return SaveCatuiDisabledAddons();
}

void LibretroSession::Run() {
    std::lock_guard lock{run_mutex};
    if (!is_game_loaded || !system) {
        return;
    }

    // 1. Dynamic variable updates
    if (Environment::IsVariableUpdated()) {
        Environment::UpdateVariables();
        system->ApplySettings();
    }

    // Poll only the outstanding pipeline count. ShadersBuilding() intentionally
    // retains Eden's two-second historical batch for desktop/Android overlays;
    // mixing that value with the live callback made libretro alternate between
    // small live counts and very large cumulative counts during gameplay.
    const int runtime_shader_count = system->GPU().ShaderNotify().ShadersInFlight();
	const int runtime_shader_started = std::max(
	    0, system->GPU().ShaderNotify().ShadersStarted() - runtime_shader_start_base);
    if (!runtime_shader_reporting_armed) {
        runtime_shader_reporting_armed = runtime_shader_count == 0;
    } else if (runtime_shader_count != last_runtime_shader_count ||
	           runtime_shader_started != last_runtime_shader_started) {
        ReportRuntimeShaderActivity(runtime_shader_started, runtime_shader_count);
        last_runtime_shader_count = runtime_shader_count;
		last_runtime_shader_started = runtime_shader_started;
    }

    // 2. Poll and update inputs
    const bool is_handheld = Settings::values.use_docked_mode.GetValue() == Settings::ConsoleMode::Handheld;
    if (input_driver) {
        input_driver->PollInputs(input_poll_cb, input_state_cb, is_handheld);
    }

    // 3. Flush audio samples (800 frames = 16.6ms @ 48kHz)
    if (audio_sink) {
        audio_sink->FlushAudio(800);
    }

    // 4. Deliver each captured renderer frame exactly once. Frame capture is
    // scheduled by EmuWindow::OnFrameDisplayed above; requesting it here too
    // caused duplicate GPU readbacks. Replaying one stale buffer on every
    // retro_run also made frontends believe a frozen first frame was ready.
    static int debug_frame_counter = 0;
    ++debug_frame_counter;
    emulated_frames.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_perf_sample).count() >= 250) {
        last_perf_sample = now;
        if (system) {
            const auto stats = system->GetAndResetPerfStats();
            cached_game_fps.store(stats.average_game_fps, std::memory_order_relaxed);
            cached_system_fps.store(stats.system_fps, std::memory_order_relaxed);
        }
    }

    if (video_cb) {
        if (VulkanBridge::GetInstance().IsHardwareRenderActive()) {
            video_cb(reinterpret_cast<const void*>(-1), current_fb_width, current_fb_height, 0);
        } else {
            if (!frame_ready.load(std::memory_order_relaxed) && system) {
                system->Renderer().TryRetireScreenshot();
            }
            std::lock_guard fb_lock{frame_mutex};
            if (debug_frame_counter <= 5 || (debug_frame_counter % 60 == 0)) {
                LOG_DEBUG(Frontend, "Run#{}: frame_ready={} front_empty={} fb_size={}x{} front_buf_size={}",
                          debug_frame_counter, frame_ready.load(), frame_buffer_front.empty(),
                          current_fb_width, current_fb_height, frame_buffer_front.size());
            }
            if (frame_ready.exchange(false) && !frame_buffer_front.empty()) {
                video_cb(frame_buffer_front.data(), current_fb_width, current_fb_height, current_fb_width * sizeof(u32));
            }
        }
    }
}

void LibretroSession::SetVideoRefreshCallback(retro_video_refresh_t cb) {
    video_cb = cb;
    VulkanBridge::GetInstance().SetVideoRefreshCallback(cb);
}

void LibretroSession::SetAudioSampleBatchCallback(retro_audio_sample_batch_t cb) {
    audio_batch_cb = cb;
    if (audio_sink) {
        audio_sink->SetAudioCallback(cb);
    }
}

void LibretroSession::SetInputPollCallback(retro_input_poll_t cb) {
    input_poll_cb = cb;
}

void LibretroSession::SetInputStateCallback(retro_input_state_t cb) {
    input_state_cb = cb;
}

void LibretroSession::GetSystemAVInfo(struct retro_system_av_info* info) {
    if (!info) {
        return;
    }
    const bool is_handheld = Settings::values.use_docked_mode.GetValue() == Settings::ConsoleMode::Handheld;
    info->geometry.base_width = is_handheld ? 1280 : 1920;
    info->geometry.base_height = is_handheld ? 720 : 1080;
    info->geometry.max_width = 3840;
    info->geometry.max_height = 2160;
    info->geometry.aspect_ratio = 16.0f / 9.0f;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 48000.0;
}

} // namespace Libretro

// =============================================================================
// Libretro C API Export Functions
// =============================================================================

extern "C" {

RETRO_API int retro_install_content(const char* filepath, const char* expected_title_id) {
    return Libretro::LibretroSession::GetInstance().InstallContent(filepath, expected_title_id);
}

RETRO_API const char* retro_list_installed_content(const char* expected_title_id) {
    return Libretro::LibretroSession::GetInstance().ListInstalledContent(expected_title_id);
}

RETRO_API bool retro_set_installed_content_enabled(const char* expected_title_id,
                                                   const char* content_key, bool enabled) {
    return Libretro::LibretroSession::GetInstance().SetInstalledContentEnabled(
        expected_title_id, content_key, enabled);
}

RETRO_API const char* retro_get_software_keyboard_request(void) {
    return Libretro::GetSoftwareKeyboardRequest();
}

RETRO_API bool retro_submit_software_keyboard(uint64_t id, const char* text, bool accepted) {
    return Libretro::SubmitSoftwareKeyboard(id, text, accepted);
}

RETRO_API void retro_init(void) {
    Libretro::LibretroSession::GetInstance().Init();
}

RETRO_API void retro_deinit(void) {
    Libretro::LibretroSession::GetInstance().Deinit();
}

RETRO_API unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
    if (!info) return;
    info->library_name = "Eden";
    info->library_version = "0.2.0-catui";
    info->valid_extensions = "nca|nro|nso|nsp|xci";
    info->need_fullpath = true;
    info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    Libretro::LibretroSession::GetInstance().GetSystemAVInfo(info);
}

RETRO_API void retro_set_environment(retro_environment_t cb) {
    Libretro::Environment::SetEnvironmentCallback(cb);
    Libretro::Environment::RegisterCoreOptions();
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
    // Eden currently presents through an explicit renderer screenshot. Its
    // experimental Vulkan bridge is not wired to the renderer's VkImage and
    // advertising it produced RETRO_HW_FRAME_BUFFER_VALID without an image.
    Libretro::LibretroLog::Initialize(cb);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) {
    Libretro::LibretroSession::GetInstance().SetVideoRefreshCallback(cb);
}

RETRO_API void retro_set_audio_sample([[maybe_unused]] retro_audio_sample_t cb) {
    // Unused: batch callback is used instead
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    Libretro::LibretroSession::GetInstance().SetAudioSampleBatchCallback(cb);
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb) {
    Libretro::LibretroSession::GetInstance().SetInputPollCallback(cb);
}

RETRO_API void retro_set_input_state(retro_input_state_t cb) {
    Libretro::LibretroSession::GetInstance().SetInputStateCallback(cb);
}

RETRO_API void retro_set_controller_port_device([[maybe_unused]] unsigned port,
                                                [[maybe_unused]] unsigned device) {
}

RETRO_API void retro_reset(void) {
    Libretro::LibretroSession::GetInstance().Reset();
}

RETRO_API void retro_set_paused(bool paused) {
    Libretro::LibretroSession::GetInstance().SetPaused(paused);
}

RETRO_API bool retro_get_paused(void) {
    return Libretro::LibretroSession::GetInstance().IsPaused();
}

RETRO_API void retro_run(void) {
    Libretro::LibretroSession::GetInstance().Run();
}

RETRO_API size_t retro_serialize_size(void) {
    return 0;
}

RETRO_API bool retro_serialize([[maybe_unused]] void *data, [[maybe_unused]] size_t size) {
    return false;
}

RETRO_API bool retro_unserialize([[maybe_unused]] const void *data, [[maybe_unused]] size_t size) {
    return false;
}

RETRO_API void retro_cheat_reset(void) {}

RETRO_API void retro_cheat_set([[maybe_unused]] unsigned index,
                               [[maybe_unused]] bool enabled,
                               [[maybe_unused]] const char *code) {}

RETRO_API bool retro_load_game(const struct retro_game_info *game) {
    if (!game || !game->path) {
        return false;
    }
    return Libretro::LibretroSession::GetInstance().LoadGame(game->path);
}

RETRO_API bool retro_load_game_special([[maybe_unused]] unsigned game_type,
                                       [[maybe_unused]] const struct retro_game_info *info,
                                       [[maybe_unused]] size_t num_info) {
    return false;
}

RETRO_API void retro_unload_game(void) {
    Libretro::LibretroSession::GetInstance().UnloadGame();
}

RETRO_API unsigned retro_get_region(void) {
    return 0;
}

RETRO_API void *retro_get_memory_data([[maybe_unused]] unsigned id) {
    return nullptr;
}

RETRO_API size_t retro_get_memory_size([[maybe_unused]] unsigned id) {
    return 0;
}

RETRO_API double retro_get_game_fps(void) {
    return Libretro::LibretroSession::GetInstance().GetGameFPS();
}

RETRO_API double retro_get_emulation_fps(void) {
    return Libretro::LibretroSession::GetInstance().GetEmulationFPS();
}

RETRO_API uint64_t retro_get_emulated_frame_count(void) {
    return Libretro::LibretroSession::GetInstance().GetEmulatedFrameCount();
}

} // extern "C"
