// SPDX-License-Identifier: GPL-2.0-or-later
#include "libretro.h"
#include "transport.h"
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>
namespace {
retro_environment_t env{};
retro_video_refresh_t video_cb{};
retro_audio_sample_batch_t audio_cb{};
retro_audio_sample_t audio_one{};
retro_input_poll_t input_poll{};
retro_input_state_t input_state{};
retro_rumble_interface rumble{};
retro_log_printf_t log_cb{};
HANDLE map{};
PROCESS_INFORMATION host{};
CatuiProxy::Shared* shared{};
LONG last_serial{};
std::vector<std::uint8_t> pixels;
std::vector<std::int16_t> samples;
std::string error;
unsigned sample_rate = 48000;
ULONGLONG last_tick{};
LONG fps_serial{};
double measured_fps{};
bool connected[CatuiProxy::Ports]{true, false, false, false};
constexpr retro_variable options[] = {
    {"rpcs3_renderer", "Renderer (restart); Vulkan|OpenGL"},
    {"rpcs3_shader_mode", "Shader mode (restart); Async Recompiler (multi-threaded)|Async Recompiler with Shader Interpreter|Legacy Recompiler (single-threaded)"},
    {"rpcs3_write_color_buffers", "Write color buffers (restart); disabled|enabled"},
    {nullptr, nullptr}};
std::filesystem::path module_dir() {
    HMODULE mod{}; wchar_t path[32768]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&retro_init), &mod);
    GetModuleFileNameW(mod, path, 32768);
    return std::filesystem::path(path).parent_path();
}
void close_host() {
    if (shared) CatuiProxy::write(&shared->stop, 1);
    if (host.hProcess) {
        if (WaitForSingleObject(host.hProcess, 10000) == WAIT_TIMEOUT) {
            TerminateProcess(host.hProcess, 1); WaitForSingleObject(host.hProcess, 2000);
        }
        CloseHandle(host.hProcess); CloseHandle(host.hThread); host = {};
    }
    if (shared) { UnmapViewOfFile(shared); shared = nullptr; }
    if (map) { CloseHandle(map); map = nullptr; }
    pixels.clear(); samples.clear(); last_serial = 0; measured_fps = 0;
}
void report_error() {
    if (log_cb) log_cb(RETRO_LOG_ERROR, "%s\n", error.c_str());
    if (env) { retro_message m{error.c_str(), 600}; env(RETRO_ENVIRONMENT_SET_MESSAGE, &m); }
}
}
extern "C" {
RETRO_API void retro_set_environment(retro_environment_t cb) { env = cb; if (env) env(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(options)); }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_one = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state = cb; }
RETRO_API unsigned retro_api_version() { return RETRO_API_VERSION; }
RETRO_API void retro_get_system_info(retro_system_info* i) { *i = {"RPCS3 CATUI Proxy", "1.0-readback", "elf|self|bin|iso", true, false}; }
RETRO_API void retro_get_system_av_info(retro_system_av_info* i) { *i = {{1280,720,CatuiProxy::MaxWidth,CatuiProxy::MaxHeight,16.0f/9.0f},{60.0,static_cast<double>(sample_rate)}}; }
RETRO_API void retro_init() { if (env) { auto f = RETRO_PIXEL_FORMAT_XRGB8888; env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f); retro_log_callback logger{}; if (env(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logger)) log_cb = logger.log; } }
RETRO_API void retro_deinit() { close_host(); }
RETRO_API void retro_unload_game() { close_host(); }
RETRO_API bool retro_load_game(const retro_game_info* game) {
    close_host(); error.clear(); sample_rate = 48000;
    const auto executable = module_dir() / L"rpcs3.exe";
    if (!game || !game->path || !std::filesystem::exists(game->path)) { error = "PS3 executable is missing"; report_error(); return false; }
    if (!std::filesystem::exists(executable)) { error = "CATUI-patched rpcs3.exe must be beside the proxy DLL"; report_error(); return false; }
    const std::wstring name = L"Local\\CATUI_RPCS3_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(CatuiProxy::Shared), name.c_str());
    if (map) shared = static_cast<CatuiProxy::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CatuiProxy::Shared)));
    if (!shared) { error = "Cannot map PS3 transport"; close_host(); report_error(); return false; }
    new (shared) CatuiProxy::Shared{};
    const char* save{}; if (env) env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save);
    const auto state_dir = save && *save ? std::filesystem::u8path(save) / "RPCS3" : module_dir() / "catui-state";
    std::error_code ec; std::filesystem::create_directories(state_dir, ec);
    const auto config = state_dir / "catui-proxy.yml";
    retro_variable renderer{"rpcs3_renderer", nullptr}; if (env) env(RETRO_ENVIRONMENT_GET_VARIABLE, &renderer);
    const bool use_vulkan = renderer.value && std::string(renderer.value) == "Vulkan";
    retro_variable shader{"rpcs3_shader_mode", nullptr}; if (env) env(RETRO_ENVIRONMENT_GET_VARIABLE, &shader);
    std::string shader_mode = shader.value ? shader.value : "Async Recompiler (multi-threaded)";
    retro_variable color_buffers{"rpcs3_write_color_buffers", nullptr}; if (env) env(RETRO_ENVIRONMENT_GET_VARIABLE, &color_buffers);
    if (shader_mode != "Async Recompiler (multi-threaded)" && shader_mode != "Async Recompiler with Shader Interpreter" && shader_mode != "Legacy Recompiler (single-threaded)") shader_mode = "Async Recompiler (multi-threaded)";
    std::ofstream cfg(config);
    cfg << "Video:\n  Renderer: " << (use_vulkan ? "Vulkan" : "OpenGL")
        << "\n  Resolution: 1280x720\n  Frame limit: Auto\n  Record With Overlays: true\n  Write Color Buffers: "
        << ((color_buffers.value && std::string(color_buffers.value) == "enabled") ? "true" : "false")
        << "\n  Shader Mode: " << shader_mode
        << "\nAudio:\n  Audio Format: Stereo\n  Convert to 16 bit: true\nMiscellaneous:\n  Start games in fullscreen mode: false\n  Exit RPCS3 when process finishes: true\n";
    cfg.close();
    // Build a private environment block, preserving all inherited variables.
    wchar_t* inherited = GetEnvironmentStringsW(); std::vector<wchar_t> environment;
    for (const wchar_t* p = inherited; p && *p; p += wcslen(p) + 1) {
        if (_wcsnicmp(p, L"CATUI_RPCS3_IPC=", 16)) environment.insert(environment.end(), p, p + wcslen(p) + 1);
    }
    if (inherited) FreeEnvironmentStringsW(inherited);
    const std::wstring setting = L"CATUI_RPCS3_IPC=" + name;
    environment.insert(environment.end(), setting.begin(), setting.end()); environment.push_back(0); environment.push_back(0);
    std::wstring command = L"\"" + executable.wstring() + L"\" --no-gui --config \"" + config.wstring() + L"\" \"" + std::filesystem::u8path(game->path).wstring() + L"\"";
    STARTUPINFOW start{}; start.cb = sizeof(start); start.dwFlags = STARTF_USESHOWWINDOW; start.wShowWindow = SW_HIDE;
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        environment.data(), executable.parent_path().c_str(), &start, &host)) {
        error = "Cannot launch RPCS3 host"; close_host(); report_error(); return false;
    }
    if (env) env(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble);
    const ULONGLONG deadline = GetTickCount64() + 600000;
    while (GetTickCount64() < deadline) {
        if (CatuiProxy::read(&shared->ready) == 1) { last_tick = GetTickCount64(); fps_serial = 0; return true; }
        if (CatuiProxy::read(&shared->ready) == -1) break;
        if (WaitForSingleObject(host.hProcess, 0) != WAIT_TIMEOUT) break;
        Sleep(5);
    }
    error = shared->error[0] ? shared->error : "RPCS3 host did not publish a game frame; check RPCS3.log and firmware";
    close_host(); report_error(); return false;
}
RETRO_API void retro_run() {
    if (!shared) return;
    if (WaitForSingleObject(host.hProcess, 0) != WAIT_TIMEOUT) {
        DWORD code = 0;
        GetExitCodeProcess(host.hProcess, &code);
        if (shared->error[0] || code != 0) {
            error = shared->error[0] ? shared->error : "RPCS3 host stopped with exit code " + std::to_string(code);
            report_error();
        } else {
            error.clear();
        }
        if (env) env(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);
        return;
    }
    if (input_poll) input_poll();
    if (input_state) {
        for (unsigned port=0;port<CatuiProxy::Ports;++port) {
            CatuiProxy::write(&shared->connected[port], connected[port]);
            for (unsigned b=0;b<16;++b) CatuiProxy::write(&shared->input_buttons[port][b], input_state(port,RETRO_DEVICE_JOYPAD,0,b));
            for (unsigned a=0;a<4;++a) CatuiProxy::write(&shared->input_axes[port][a],input_state(port,RETRO_DEVICE_ANALOG,a/2,a%2));
        }
    }
    const LONG serial = CatuiProxy::read(&shared->frame_serial);
    const LONG slot = CatuiProxy::read(&shared->published_slot);
    if (video_cb && serial != last_serial && slot >= 0 && slot < 2 && !InterlockedCompareExchange(&shared->slot_lock[slot],1,0)) {
        const unsigned w=shared->width[slot],h=shared->height[slot],pitch=shared->pitch[slot];
        if (w && h && w<=CatuiProxy::MaxWidth && h<=CatuiProxy::MaxHeight && pitch==w*4) {
            pixels.assign(shared->pixels[slot],shared->pixels[slot]+pitch*h); last_serial=serial;
        }
        CatuiProxy::write(&shared->slot_lock[slot],0);
        if (last_serial==serial) video_cb(pixels.data(),w,h,pitch);
    }
    const unsigned rate=CatuiProxy::read(&shared->audio_rate);
    if (rate >=8000 && rate<=192000 && rate!=sample_rate) { sample_rate=rate;retro_system_av_info av{};retro_get_system_av_info(&av);if(env)env(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO,&av); }
    const unsigned r=static_cast<unsigned>(CatuiProxy::read(&shared->audio_read)),w=static_cast<unsigned>(CatuiProxy::read(&shared->audio_write));
    const unsigned count=std::min(w-r,4096u);samples.resize(count*2);
    for(unsigned i=0;i<count;++i)for(unsigned c=0;c<2;++c)samples[i*2+c]=shared->audio[((r+i)%CatuiProxy::AudioFrames)*2+c];
    std::size_t consumed=count;
    if(count && audio_cb)consumed=std::min(audio_cb(samples.data(),count),static_cast<std::size_t>(count));
    else if(audio_one)for(unsigned i=0;i<count;++i)audio_one(samples[i*2],samples[i*2+1]);
    CatuiProxy::write(&shared->audio_read,static_cast<LONG>(r+consumed));
    if(rumble.set_rumble_state) for(unsigned port=0;port<CatuiProxy::Ports;++port) {
        rumble.set_rumble_state(port,RETRO_RUMBLE_STRONG,CatuiProxy::read(&shared->rumble_strong[port]));
        rumble.set_rumble_state(port,RETRO_RUMBLE_WEAK,CatuiProxy::read(&shared->rumble_weak[port]));
    }
    const ULONGLONG now=GetTickCount64();
    if(now-last_tick>=500){measured_fps=static_cast<unsigned>(serial-fps_serial)*1000.0/(now-last_tick);fps_serial=serial;last_tick=now;}
}
RETRO_API void retro_set_controller_port_device(unsigned port,unsigned device) {
    if(port<CatuiProxy::Ports) {
        connected[port] = (device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD;
        if(shared) CatuiProxy::write(&shared->connected[port], connected[port]);
    }
}
RETRO_API void retro_reset() {}
RETRO_API std::size_t retro_serialize_size(){return 0;}
RETRO_API bool retro_serialize(void*,std::size_t){return false;}
RETRO_API bool retro_unserialize(const void*,std::size_t){return false;}
RETRO_API void retro_cheat_reset(){}
RETRO_API void retro_cheat_set(unsigned,bool,const char*){}
RETRO_API bool retro_load_game_special(unsigned,const retro_game_info*,std::size_t){return false;}
RETRO_API unsigned retro_get_region(){return RETRO_REGION_NTSC;}
RETRO_API void* retro_get_memory_data(unsigned){return nullptr;}
RETRO_API std::size_t retro_get_memory_size(unsigned){return 0;}
__declspec(dllexport) void retro_set_paused(bool p){if(shared)CatuiProxy::write(&shared->paused,p);}
__declspec(dllexport) bool retro_get_paused(){return shared && CatuiProxy::read(&shared->paused);}
__declspec(dllexport) double retro_get_game_fps(){return measured_fps;}
__declspec(dllexport) std::uint32_t retro_get_process_id(){return host.hProcess ? host.dwProcessId : 0;}
__declspec(dllexport) const char* retro_get_last_error(){return error.c_str();}
}
