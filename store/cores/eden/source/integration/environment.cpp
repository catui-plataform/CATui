// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eden_libretro/environment.h"

#include <vector>
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"

namespace Libretro {

namespace {

bool DirectoryHasEntries(const std::filesystem::path& path) {
    std::error_code ec;
    const auto begin = std::filesystem::directory_iterator(path, ec);
    return !ec && begin != std::filesystem::directory_iterator{};
}

bool HasInstalledFirmware(const std::filesystem::path& nand_dir) {
    return DirectoryHasEntries(nand_dir / "system" / "Contents" / "registered");
}

} // namespace

void Environment::SetEnvironmentCallback(retro_environment_t cb) {
    env_cb = cb;
}

retro_environment_t Environment::GetEnvironmentCallback() {
    return env_cb;
}

void Environment::InitializeDirectories() {
    if (!env_cb) {
        return;
    }

    const char* sys_dir_str = nullptr;
    if (env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir_str) && sys_dir_str) {
        system_dir = std::filesystem::path(sys_dir_str);
    } else {
        system_dir = std::filesystem::current_path();
    }

    const char* save_dir_str = nullptr;
    if (env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir_str) && save_dir_str) {
        save_dir = std::filesystem::path(save_dir_str);
    } else {
        save_dir = system_dir;
    }

    const auto ensure_dir = [](const std::filesystem::path& p) -> std::filesystem::path {
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
    };

    using namespace Common::FS;
	const auto eden_system_dir = ensure_dir(system_dir / "eden");
    const auto canonical_nand = ensure_dir(eden_system_dir / "nand");
    const auto legacy_nand = system_dir.parent_path() / "saves" / "eden" / "nand";
    const auto selected_nand = !HasInstalledFirmware(canonical_nand) && HasInstalledFirmware(legacy_nand)
                                   ? legacy_nand
                                   : canonical_nand;

    SetEdenPath(EdenPath::EdenDir, eden_system_dir);
    // Firmware/NAND is machine-wide console data and must not move when the
    // active CATui profile changes. Existing installs that used user://saves
    // keep working, while new installs use user://system/eden/nand.
    SetEdenPath(EdenPath::NANDDir, ensure_dir(selected_nand));
    SetEdenPath(EdenPath::SDMCDir, ensure_dir(save_dir / "eden" / "sdmc"));
    SetEdenPath(EdenPath::SaveDir, ensure_dir(save_dir / "eden" / "save"));
	const char* game_mod_dir_str = nullptr;
	constexpr unsigned CatuiGetGameModDirectory = 0x20000u | 0x100u;
	const bool has_game_mod_dir = env_cb(CatuiGetGameModDirectory, &game_mod_dir_str) &&
	                              game_mod_dir_str && game_mod_dir_str[0] != '\0';
	SetEdenPath(EdenPath::LoadDir,
	            ensure_dir(has_game_mod_dir ? std::filesystem::path(game_mod_dir_str)
	                                        : eden_system_dir / "load"));
    SetEdenPath(EdenPath::DumpDir, ensure_dir(eden_system_dir / "dump"));
    SetEdenPath(EdenPath::CacheDir, ensure_dir(eden_system_dir / "cache"));
    SetEdenPath(EdenPath::ShaderDir, ensure_dir(eden_system_dir / "shader"));
    SetEdenPath(EdenPath::ConfigDir, ensure_dir(eden_system_dir / "config"));
    SetEdenPath(EdenPath::LogDir, ensure_dir(eden_system_dir / "log"));

    const auto sys_eden_keys = ensure_dir(eden_system_dir / "keys");
    const auto sys_keys = system_dir / "keys";
    const auto sys_direct = system_dir;
    const auto appdata_keys = GetEdenPath(EdenPath::KeysDir);

    if (std::filesystem::exists(sys_eden_keys / "prod.keys")) {
        SetEdenPath(EdenPath::KeysDir, sys_eden_keys);
    } else if (std::filesystem::exists(sys_keys / "prod.keys")) {
        SetEdenPath(EdenPath::KeysDir, sys_keys);
    } else if (std::filesystem::exists(sys_direct / "prod.keys")) {
        SetEdenPath(EdenPath::KeysDir, sys_direct);
    } else if (std::filesystem::exists(appdata_keys / "prod.keys")) {
        std::error_code ec;
        std::filesystem::copy_file(appdata_keys / "prod.keys", sys_eden_keys / "prod.keys", std::filesystem::copy_options::overwrite_existing, ec);
        SetEdenPath(EdenPath::KeysDir, sys_eden_keys);
    } else {
        SetEdenPath(EdenPath::KeysDir, sys_eden_keys);
    }

    LOG_INFO(Frontend, "CATui Eden paths: system='{}', keys='{}', nand='{}', saves='{}'",
             PathToUTF8String(eden_system_dir), PathToUTF8String(GetEdenPath(EdenPath::KeysDir)),
             PathToUTF8String(GetEdenPath(EdenPath::NANDDir)),
             PathToUTF8String(GetEdenPath(EdenPath::SaveDir)));
    if (selected_nand == legacy_nand) {
        LOG_WARNING(Frontend,
                    "Using legacy CATui firmware directory '{}'; new installs should use '{}'",
                    PathToUTF8String(legacy_nand), PathToUTF8String(canonical_nand));
    }

    CreateEdenPaths();
}

const std::filesystem::path& Environment::GetSystemDir() {
    return system_dir;
}

const std::filesystem::path& Environment::GetSaveDir() {
    return save_dir;
}

static struct retro_core_option_v2_category option_categories[] = {
    {"graphics", "Graphics / Video", "Configurações de renderização gráfica e resolução."},
    {"system", "System / Console", "Configurações de região, idioma e comportamento do sistema."},
    {"audio", "Audio", "Configurações de saída de som e volume."},
    {"performance", "Performance & CPU", "Opções de multithreading e precisão de CPU."},
    {nullptr, nullptr, nullptr}
};

static struct retro_core_option_v2_definition option_definitions[] = {
    {
        "eden_renderer_backend",
        "Graphics API",
        "Graphics API Renderer Backend",
        "Seleciona a API de renderização gráfica do emulador (Vulkan ou OpenGL).",
        nullptr,
        "graphics",
        {
            {"vulkan", "Vulkan (Recommended)"},
            {"opengl", "OpenGL (Core Profile)"},
            {nullptr, nullptr}
        },
        "vulkan"
    },
    {
        "eden_docked_mode",
        "Console Mode",
        "Console Mode (Docked / Handheld)",
        "Alterna entre modo TV (Docked - 1080p) e modo Portátil (Handheld - 720p).",
        nullptr,
        "graphics",
        {
            {"docked", "Docked (1080p)"},
            {"handheld", "Handheld (720p)"},
            {nullptr, nullptr}
        },
        "handheld"
    },
    {
        "eden_resolution_scale",
        "Resolution Scale",
        "Internal Resolution Scale",
        "Multiplicador de resolução interna de renderização 3D.",
        nullptr,
        "graphics",
        {
            {"0.5", "0.5x (360p / 540p)"},
            {"0.75", "0.75x (540p / 810p)"},
            {"1.0", "1.0x Native (720p / 1080p)"},
            {"1.5", "1.5x (1080p / 1620p)"},
            {"2.0", "2.0x (1440p / 2160p 4K)"},
            {"3.0", "3.0x (2160p / 3240p)"},
            {"4.0", "4.0x (2880p / 4320p 8K)"},
            {nullptr, nullptr}
        },
        "1.0"
    },
    {
        "eden_scaling_filter",
        "Window Adapting Filter",
        "Window Adapting Scaling Filter",
        "Filtro de escala de imagem para tela cheia / janela.",
        nullptr,
        "graphics",
        {
            {"bilinear", "Bilinear"},
            {"bicubic", "Bicubic"},
            {"gaussian", "Gaussian"},
            {"scale_force", "ScaleForce"},
            {"fsr", "AMD FidelityFX Super Resolution (FSR)"},
            {"nearest", "Nearest Neighbor"},
            {nullptr, nullptr}
        },
        "bilinear"
    },
    {
        "eden_anti_aliasing",
        "Anti-Aliasing Method",
        "Anti-Aliasing Post-Processing",
        "Método de suavização de bordas pós-processamento.",
        nullptr,
        "graphics",
        {
            {"none", "None (Off)"},
            {"fxaa", "FXAA"},
            {"smaa", "SMAA"},
            {nullptr, nullptr}
        },
        "none"
    },
    {
        "eden_astc_decoding",
        "ASTC Textures Decoding Method",
        "ASTC Textures Decoding Method",
        "Decodificação de compressão de textura ASTC.",
        nullptr,
        "graphics",
        {
            {"gpu", "GPU Hardware"},
            {"cpu_async", "CPU Asynchronous"},
            {"cpu_sync", "CPU Synchronous"},
            {nullptr, nullptr}
        },
        "gpu"
    },
    {
        "eden_gpu_accuracy",
        "GPU Accuracy",
        "GPU Emulation Accuracy",
        "Equilibra compatibilidade gráfica e desempenho. Balanced corresponde ao padrão do Eden desktop.",
        nullptr,
        "graphics",
        {
            {"performance", "Performance (Fast)"},
            {"balanced", "Balanced (Normal / Recommended)"},
            {"accurate", "Accurate (High)"},
            {nullptr, nullptr}
        },
        "balanced"
    },
    {
        "eden_async_gpu",
        "Asynchronous GPU Emulation",
        "Use Asynchronous GPU Emulation",
        "Uses a dedicated CPU thread for GPU emulation. Keep enabled for normal gameplay.",
        nullptr,
        "performance",
        {
            {"enabled", "Enabled (Recommended)"},
            {"disabled", "Disabled"},
            {nullptr, nullptr}
        },
        "enabled"
    },
    {
        "eden_shader_cache",
        "Persistent Shader Cache",
        "Use Persistent Pipeline Cache",
        "Stores compiled shaders so later launches stutter less.",
        nullptr,
        "performance",
        {
            {"enabled", "Enabled (Recommended)"},
            {"disabled", "Disabled"},
            {nullptr, nullptr}
        },
        "enabled"
    },
    {
        "eden_vulkan_pipeline_cache",
        "Vulkan Pipeline Cache",
        "Use Vulkan Driver Pipeline Cache",
        "Uses the GPU driver's pipeline cache to reduce shader compilation stalls.",
        nullptr,
        "performance",
        {
            {"enabled", "Enabled (Recommended)"},
            {"disabled", "Disabled"},
            {nullptr, nullptr}
        },
        "enabled"
    },
    {
        "eden_async_shaders",
        "Asynchronous Shaders",
        "Asynchronous Shader Compilation",
        "Compiles new shaders in the background to reduce gameplay stutter.",
        nullptr,
        "performance",
        {
            {"enabled", "Enabled (Smoother)"},
            {"disabled", "Disabled (More Accurate)"},
            {nullptr, nullptr}
        },
        "enabled"
    },
    {
        "eden_async_presentation",
        "Asynchronous Presentation",
        "Asynchronous Vulkan Presentation",
        "Moves presentation to a separate CPU thread for steadier frame delivery.",
        nullptr,
        "performance",
        {
            {"enabled", "Enabled (Recommended)"},
            {"disabled", "Disabled"},
            {nullptr, nullptr}
        },
        "enabled"
    },
    {
        "eden_frame_pacing",
        "Frame Pacing",
        "Vulkan Frame Pacing Target",
        "Selects the pacing target. Auto follows the game's native frame rate.",
        nullptr,
        "performance",
        {
            {"auto", "Auto (Recommended)"},
            {"30", "30 FPS"},
            {"60", "60 FPS"},
            {"90", "90 FPS"},
            {"120", "120 FPS"},
            {nullptr, nullptr}
        },
        "auto"
    },
    {
        "eden_vsync_mode",
        "VSync Mode",
        "Vulkan Presentation Mode",
        "FIFO is the safest smooth default; relaxed and immediate modes may tear.",
        nullptr,
        "graphics",
        {
            {"fifo", "FIFO (Smooth)"},
            {"fifo_relaxed", "FIFO Relaxed"},
            {"mailbox", "Mailbox"},
            {"immediate", "Immediate"},
            {nullptr, nullptr}
        },
        "fifo"
    },
    {
        "eden_vram_usage",
        "VRAM Usage",
        "VRAM Usage Strategy",
        "Aggressive can improve performance on GPUs with enough VRAM; Conservative is safer.",
        nullptr,
        "performance",
        {
            {"conservative", "Conservative (Recommended)"},
            {"aggressive", "Aggressive"},
            {nullptr, nullptr}
        },
        "conservative"
    },
    {
        "eden_nvdec",
        "Video Decoding",
        "NVDEC Emulation",
        "GPU video decoding normally provides the best performance.",
        nullptr,
        "performance",
        {
            {"gpu", "GPU (Recommended)"},
            {"cpu", "CPU"},
            {"off", "Off (Black Videos)"},
            {nullptr, nullptr}
        },
        "gpu"
    },
    {
        "eden_astc_recompression",
        "ASTC Recompression",
        "ASTC Texture Recompression",
        "BC formats save VRAM at a quality and conversion cost. Uncompressed is fastest when VRAM allows.",
        nullptr,
        "graphics",
        {
            {"uncompressed", "Uncompressed (Recommended)"},
            {"bc1", "BC1 (Lowest VRAM)"},
            {"bc3", "BC3 (Balanced VRAM)"},
            {nullptr, nullptr}
        },
        "uncompressed"
    },
    {
        "eden_dma_accuracy",
        "DMA Accuracy",
        "DMA Read Accuracy",
        "Default follows GPU accuracy. Unsafe is faster; Safe favors compatibility.",
        nullptr,
        "performance",
        {
            {"default", "Default (Recommended)"},
            {"unsafe", "Unsafe (Faster)"},
            {"safe", "Safe (Stable)"},
            {nullptr, nullptr}
        },
        "default"
    },
    {
        "eden_gpu_fence",
        "GPU Fence Behavior",
        "GPU Fence Synchronization",
        "Default follows GPU accuracy. Faster modes can reduce synchronization overhead.",
        nullptr,
        "performance",
        {
            {"default", "Default (Recommended)"},
            {"immediate", "Immediate (Fastest)"},
            {"balanced", "Balanced"},
            {"accurate", "Accurate"},
            {"strict", "Strict"},
            {nullptr, nullptr}
        },
        "default"
    },
    {
        "eden_reactive_flushing",
        "Reactive Flushing",
        "Enable Reactive Flushing",
        "Improves memory synchronization accuracy with low overhead on desktop GPUs.",
        nullptr,
        "performance",
        {
            {"enabled", "Enabled (Recommended)"},
            {"disabled", "Disabled"},
            {nullptr, nullptr}
        },
        "enabled"
    },
    {
        "eden_barrier_feedback",
        "Barrier Feedback Loops",
        "Enable Barrier Feedback Loops",
        "Improves rendering of transparency effects in affected games.",
        nullptr,
        "performance",
        {
            {"enabled", "Enabled (Recommended)"},
            {"disabled", "Disabled"},
            {nullptr, nullptr}
        },
        "enabled"
    },
    {
        "eden_region",
        "System Region",
        "System Region",
        "Região do console emulado.",
        nullptr,
        "system",
        {
            {"usa", "USA / North America"},
            {"europe", "Europe"},
            {"japan", "Japan"},
            {"australia", "Australia"},
            {"china", "China"},
            {"korea", "Korea"},
            {"taiwan", "Taiwan"},
            {nullptr, nullptr}
        },
        "usa"
    },
    {
        "eden_language",
        "System Language",
        "System Language",
        "Idioma do sistema operacional da Switch.",
        nullptr,
        "system",
        {
            {"english", "English (American)"},
            {"british_english", "English (British)"},
            {"japanese", "Japanese"},
            {"french", "French"},
            {"german", "German"},
            {"latin_american_spanish", "Spanish (Latin American)"},
            {"spanish", "Spanish (Spain)"},
            {"italian", "Italian"},
            {"portuguese", "Portuguese (Portugal)"},
            {"brazilian_portuguese", "Portuguese (Brazilian)"},
            {"russian", "Russian"},
            {"korean", "Korean"},
            {"traditional_chinese", "Chinese (Traditional)"},
            {"simplified_chinese", "Chinese (Simplified)"},
            {nullptr, nullptr}
        },
        "english"
    },
    {
        "eden_multicore",
        "Multicore CPU Emulation",
        "Multicore CPU Emulation",
        "Habilita a execução paralela dos 4 núcleos da CPU do Switch.",
        nullptr,
        "performance",
        {
            {"enabled", "Enabled (Recommended)"},
            {"disabled", "Disabled (Single Core)"},
            {nullptr, nullptr}
        },
        "enabled"
    },
    {
        "eden_cpu_accuracy",
        "CPU Accuracy",
        "CPU Emulation Accuracy",
        "Nível de precisão do JIT Dynarmic.",
        nullptr,
        "performance",
        {
            {"auto", "Auto"},
            {"accurate", "Accurate"},
            {"unsafe", "Unsafe (Fastest)"},
            {nullptr, nullptr}
        },
        "auto"
    },
    {
        "eden_audio_volume",
        "Audio Volume",
        "Audio Output Volume",
        "Volume de saída do áudio emulado.",
        nullptr,
        "audio",
        {
            {"100", "100%"},
            {"90", "90%"},
            {"80", "80%"},
            {"70", "70%"},
            {"60", "60%"},
            {"50", "50%"},
            {"0", "Mute (0%)"},
            {nullptr, nullptr}
        },
        "100"
    },
    {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr}
};

static struct retro_core_options_v2 core_options_v2 = {
    option_categories,
    option_definitions
};

static const struct retro_variable legacy_variables[] = {
    {"eden_renderer_backend", "Graphics API; vulkan|opengl"},
    {"eden_docked_mode", "Console Mode; docked|handheld"},
    {"eden_resolution_scale", "Internal Resolution Scale; 1.0|0.5|0.75|1.5|2.0|3.0|4.0"},
    {"eden_scaling_filter", "Scaling Filter; bilinear|bicubic|gaussian|scale_force|fsr|nearest"},
    {"eden_anti_aliasing", "Anti-Aliasing; none|fxaa|smaa"},
    {"eden_astc_decoding", "ASTC Decoding; gpu|cpu_async|cpu_sync"},
    {"eden_gpu_accuracy", "GPU Accuracy; balanced|performance|accurate"},
    {"eden_async_gpu", "Asynchronous GPU Emulation; enabled|disabled"},
    {"eden_shader_cache", "Persistent Shader Cache; enabled|disabled"},
    {"eden_vulkan_pipeline_cache", "Vulkan Pipeline Cache; enabled|disabled"},
    {"eden_async_shaders", "Asynchronous Shaders; enabled|disabled"},
    {"eden_async_presentation", "Asynchronous Presentation; enabled|disabled"},
    {"eden_frame_pacing", "Frame Pacing; auto|30|60|90|120"},
    {"eden_vsync_mode", "VSync Mode; fifo|fifo_relaxed|mailbox|immediate"},
    {"eden_vram_usage", "VRAM Usage; conservative|aggressive"},
    {"eden_nvdec", "Video Decoding; gpu|cpu|off"},
    {"eden_astc_recompression", "ASTC Recompression; uncompressed|bc1|bc3"},
    {"eden_dma_accuracy", "DMA Accuracy; default|unsafe|safe"},
    {"eden_gpu_fence", "GPU Fence Behavior; default|immediate|balanced|accurate|strict"},
    {"eden_reactive_flushing", "Reactive Flushing; enabled|disabled"},
    {"eden_barrier_feedback", "Barrier Feedback Loops; enabled|disabled"},
    {"eden_region", "System Region; usa|europe|japan|australia|china|korea|taiwan"},
    {"eden_language", "System Language; english|portuguese_br|spanish|french|german|italian|japanese"},
    {"eden_multicore", "Multicore CPU; enabled|disabled"},
    {"eden_cpu_accuracy", "CPU Accuracy; auto|accurate|unsafe"},
    {"eden_audio_volume", "Audio Volume; 100|90|80|70|60|50|0"},
    {nullptr, nullptr}
};

void Environment::RegisterCoreOptions() {
    if (!env_cb) {
        return;
    }

    unsigned version = 0;
    if (env_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
        env_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &core_options_v2);
    } else {
        env_cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(legacy_variables));
    }
}

std::string Environment::GetVariable(const char* key) {
    if (!env_cb || !key) {
        return "";
    }
    struct retro_variable var { key, nullptr };
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        return std::string(var.value);
    }
    return "";
}

bool Environment::IsVariableUpdated() {
    if (!env_cb) {
        return false;
    }
    bool updated = false;
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated)) {
        return updated;
    }
    return false;
}

void Environment::UpdateVariables() {
    // Renderer Backend
    const auto renderer_str = GetVariable("eden_renderer_backend");
    if (!renderer_str.empty()) {
        if (renderer_str == "opengl") {
            Settings::values.renderer_backend.SetValue(Settings::RendererBackend::OpenGL_GLSL);
        } else {
            Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
        }
    }

    // Docked mode
    const auto docked_str = GetVariable("eden_docked_mode");
    if (!docked_str.empty()) {
        Settings::values.use_docked_mode.SetValue(
            docked_str == "docked" ? Settings::ConsoleMode::Docked : Settings::ConsoleMode::Handheld);
    } else {
        Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
    }

    // Resolution scale
    const auto res_scale_str = GetVariable("eden_resolution_scale");
    if (!res_scale_str.empty()) {
        if (res_scale_str == "0.5") {
            Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res1_2X);
        } else if (res_scale_str == "0.75") {
            Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res3_4X);
        } else if (res_scale_str == "1.0") {
            Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res1X);
        } else if (res_scale_str == "1.5") {
            Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res3_2X);
        } else if (res_scale_str == "2.0") {
            Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res2X);
        } else if (res_scale_str == "3.0") {
            Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res3X);
        } else if (res_scale_str == "4.0") {
            Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res4X);
        }
    }

    // Scaling filter
    const auto scaling_str = GetVariable("eden_scaling_filter");
    if (!scaling_str.empty()) {
        if (scaling_str == "nearest") {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::NearestNeighbor);
        } else if (scaling_str == "bicubic") {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Bicubic);
        } else if (scaling_str == "gaussian") {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Gaussian);
        } else if (scaling_str == "scale_force") {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::ScaleForce);
        } else if (scaling_str == "fsr") {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Fsr);
        } else {
            Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Bilinear);
        }
    }

    // Anti-Aliasing
    const auto aa_str = GetVariable("eden_anti_aliasing");
    if (!aa_str.empty()) {
        if (aa_str == "fxaa") {
            Settings::values.anti_aliasing.SetValue(Settings::AntiAliasing::Fxaa);
        } else if (aa_str == "smaa") {
            Settings::values.anti_aliasing.SetValue(Settings::AntiAliasing::Smaa);
        } else {
            Settings::values.anti_aliasing.SetValue(Settings::AntiAliasing::None);
        }
    }

    // ASTC
    const auto astc_str = GetVariable("eden_astc_decoding");
    if (!astc_str.empty()) {
        if (astc_str == "cpu_async") {
            Settings::values.astc_recompression.SetValue(Settings::AstcRecompression::Uncompressed);
            Settings::values.accelerate_astc.SetValue(Settings::AstcDecodeMode::CpuAsynchronous);
        } else if (astc_str == "cpu_sync") {
            Settings::values.astc_recompression.SetValue(Settings::AstcRecompression::Uncompressed);
            Settings::values.accelerate_astc.SetValue(Settings::AstcDecodeMode::Cpu);
        } else {
            Settings::values.accelerate_astc.SetValue(Settings::AstcDecodeMode::Gpu);
        }
    }

    // GPU accuracy. Eden desktop's default Normal profile is Low; using
    // High stalls the CPU emulation thread on fence waits during cache flushes.
    const auto gpu_accuracy_str = GetVariable("eden_gpu_accuracy");
    if (!gpu_accuracy_str.empty()) {
        if (gpu_accuracy_str == "accurate") {
            Settings::values.gpu_accuracy.SetValue(Settings::GpuAccuracy::High);
        } else {
            Settings::values.gpu_accuracy.SetValue(Settings::GpuAccuracy::Low);
        }
    } else {
        Settings::values.gpu_accuracy.SetValue(Settings::GpuAccuracy::Low);
    }
    Settings::UpdateGPUAccuracy();

    const auto enabled = [](const std::string& value) { return value == "enabled"; };

    const auto async_gpu_str = GetVariable("eden_async_gpu");
    if (!async_gpu_str.empty()) {
        Settings::values.use_asynchronous_gpu_emulation.SetValue(enabled(async_gpu_str));
    }

    const auto shader_cache_str = GetVariable("eden_shader_cache");
    if (!shader_cache_str.empty()) {
        Settings::values.use_disk_shader_cache.SetValue(enabled(shader_cache_str));
    }

    const auto vulkan_cache_str = GetVariable("eden_vulkan_pipeline_cache");
    if (!vulkan_cache_str.empty()) {
        Settings::values.use_vulkan_driver_pipeline_cache.SetValue(enabled(vulkan_cache_str));
    }

    const auto async_shaders_str = GetVariable("eden_async_shaders");
    if (!async_shaders_str.empty()) {
        Settings::values.use_asynchronous_shaders.SetValue(enabled(async_shaders_str));
    }

    const auto async_presentation_str = GetVariable("eden_async_presentation");
    if (!async_presentation_str.empty()) {
        Settings::values.async_presentation.SetValue(enabled(async_presentation_str));
    }

    const auto frame_pacing_str = GetVariable("eden_frame_pacing");
    if (!frame_pacing_str.empty()) {
        auto frame_pacing = Settings::FramePacingMode::Target_Auto;
        if (frame_pacing_str == "30") frame_pacing = Settings::FramePacingMode::Target_30;
        else if (frame_pacing_str == "60") frame_pacing = Settings::FramePacingMode::Target_60;
        else if (frame_pacing_str == "90") frame_pacing = Settings::FramePacingMode::Target_90;
        else if (frame_pacing_str == "120") frame_pacing = Settings::FramePacingMode::Target_120;
        Settings::values.frame_pacing_mode.SetValue(frame_pacing);
    }

    const auto vsync_str = GetVariable("eden_vsync_mode");
    if (!vsync_str.empty()) {
        auto vsync = Settings::VSyncMode::Fifo;
        if (vsync_str == "immediate") vsync = Settings::VSyncMode::Immediate;
        else if (vsync_str == "mailbox") vsync = Settings::VSyncMode::Mailbox;
        else if (vsync_str == "fifo_relaxed") vsync = Settings::VSyncMode::FifoRelaxed;
        Settings::values.vsync_mode.SetValue(vsync);
    }

    const auto vram_str = GetVariable("eden_vram_usage");
    if (!vram_str.empty()) {
        Settings::values.vram_usage_mode.SetValue(
            vram_str == "aggressive" ? Settings::VramUsageMode::Aggressive
                                     : Settings::VramUsageMode::Conservative);
    }

    const auto nvdec_str = GetVariable("eden_nvdec");
    if (!nvdec_str.empty()) {
        auto nvdec = Settings::NvdecEmulation::Gpu;
        if (nvdec_str == "cpu") nvdec = Settings::NvdecEmulation::Cpu;
        else if (nvdec_str == "off") nvdec = Settings::NvdecEmulation::Off;
        Settings::values.nvdec_emulation.SetValue(nvdec);
    }

    const auto recompression_str = GetVariable("eden_astc_recompression");
    if (!recompression_str.empty()) {
        auto recompression = Settings::AstcRecompression::Uncompressed;
        if (recompression_str == "bc1") recompression = Settings::AstcRecompression::Bc1;
        else if (recompression_str == "bc3") recompression = Settings::AstcRecompression::Bc3;
        Settings::values.astc_recompression.SetValue(recompression);
    }

    const auto dma_str = GetVariable("eden_dma_accuracy");
    if (!dma_str.empty()) {
        auto dma = Settings::DmaAccuracy::Default;
        if (dma_str == "unsafe") dma = Settings::DmaAccuracy::Unsafe;
        else if (dma_str == "safe") dma = Settings::DmaAccuracy::Safe;
        Settings::values.dma_accuracy.SetValue(dma);
    } else {
        Settings::values.dma_accuracy.SetValue(Settings::DmaAccuracy::Default);
    }

    const auto fence_str = GetVariable("eden_gpu_fence");
    if (!fence_str.empty()) {
        auto fence = Settings::GpuFenceBehavior::Default;
        if (fence_str == "immediate") fence = Settings::GpuFenceBehavior::Immediate;
        else if (fence_str == "balanced") fence = Settings::GpuFenceBehavior::Balanced;
        else if (fence_str == "accurate") fence = Settings::GpuFenceBehavior::Accurate;
        else if (fence_str == "strict") fence = Settings::GpuFenceBehavior::Strict;
        Settings::values.gpu_fence_behavior.SetValue(fence);
    } else {
        Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Default);
    }

    const auto reactive_str = GetVariable("eden_reactive_flushing");
    if (!reactive_str.empty()) {
        Settings::values.use_reactive_flushing.SetValue(enabled(reactive_str));
    }

    const auto barrier_str = GetVariable("eden_barrier_feedback");
    if (!barrier_str.empty()) {
        Settings::values.barrier_feedback_loops.SetValue(enabled(barrier_str));
    }

    // Multicore
    const auto multicore_str = GetVariable("eden_multicore");
    if (!multicore_str.empty()) {
        Settings::values.use_multi_core.SetValue(multicore_str == "enabled");
    }

    const auto cpu_accuracy_str = GetVariable("eden_cpu_accuracy");
    if (!cpu_accuracy_str.empty()) {
        auto accuracy = Settings::CpuAccuracy::Auto;
        if (cpu_accuracy_str == "accurate") accuracy = Settings::CpuAccuracy::Accurate;
        else if (cpu_accuracy_str == "unsafe") accuracy = Settings::CpuAccuracy::Unsafe;
        Settings::values.cpu_accuracy.SetValue(accuracy);
    } else {
        Settings::values.cpu_accuracy.SetValue(Settings::CpuAccuracy::Auto);
    }
    // CRITICAL STABILITY FIX: Never disable the global monitor even in Unsafe CPU mode!
    // Disabling the global monitor breaks ARM64 LDXR/STXR atomic synchronization across
    // multicore threads, causing random heap/IPC/refcount corruption and 0xc0000005 crashes.
    Settings::values.cpuopt_unsafe_ignore_global_monitor.SetValue(false);

    const auto region_str = GetVariable("eden_region");
    if (!region_str.empty()) {
        auto region = Settings::Region::Usa;
        if (region_str == "japan") region = Settings::Region::Japan;
        else if (region_str == "europe") region = Settings::Region::Europe;
        else if (region_str == "australia") region = Settings::Region::Australia;
        else if (region_str == "china") region = Settings::Region::China;
        else if (region_str == "korea") region = Settings::Region::Korea;
        else if (region_str == "taiwan") region = Settings::Region::Taiwan;
        Settings::values.region_index.SetValue(region);
    }

    const auto language_str = GetVariable("eden_language");
    if (!language_str.empty()) {
        auto language = Settings::Language::EnglishAmerican;
        if (language_str == "british_english") language = Settings::Language::EnglishBritish;
        else if (language_str == "japanese") language = Settings::Language::Japanese;
        else if (language_str == "french") language = Settings::Language::French;
        else if (language_str == "german") language = Settings::Language::German;
        else if (language_str == "latin_american_spanish") language = Settings::Language::SpanishLatin;
        else if (language_str == "spanish") language = Settings::Language::Spanish;
        else if (language_str == "italian") language = Settings::Language::Italian;
        else if (language_str == "portuguese") language = Settings::Language::Portuguese;
        else if (language_str == "brazilian_portuguese") language = Settings::Language::PortugueseBrazilian;
        else if (language_str == "russian") language = Settings::Language::Russian;
        else if (language_str == "korean") language = Settings::Language::Korean;
        else if (language_str == "traditional_chinese") language = Settings::Language::ChineseTraditional;
        else if (language_str == "simplified_chinese") language = Settings::Language::ChineseSimplified;
        Settings::values.language_index.SetValue(language);
    }

    // Audio volume
    const auto vol_str = GetVariable("eden_audio_volume");
    if (!vol_str.empty()) {
        try {
            int vol_val = std::stoi(vol_str);
            Settings::values.volume.SetValue(static_cast<u8>(std::clamp(vol_val, 0, 100)));
        } catch (...) {}
    }
}

void Environment::RegisterInputDescriptors() {
    if (!env_cb) {
        return;
    }

    static const struct retro_input_descriptor descriptors[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A (Right)"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B (Bottom)"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X (Top)"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y (Left)"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Minus (-)"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Plus (+)"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "ZL"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "ZR"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Left Stick Click"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Right Stick Click"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Stick X"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y"},
        {0, 0, 0, 0, nullptr}
    };

    env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, const_cast<struct retro_input_descriptor*>(descriptors));
}

} // namespace Libretro
