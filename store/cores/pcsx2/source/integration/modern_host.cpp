// Minimal libretro host implementation for the modern PCSX2 engine. The
// signatures intentionally come from the 2.8.1 headers instead of the old
// LRPS2 host; callbacks gain real frontend behavior as each subsystem lands.

#include "pcsx2/Achievements.h"
#include "pcsx2/GS.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GS/Renderers/Common/GSTexture.h"
#include "common/ProgressCallback.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
	std::atomic<bool> s_capture_enabled{false};
	std::atomic<u32> s_internal_render_scale{1};
	// 0 means the complete internal-resolution image; 1/2 cap the image copied
	// back to the libretro frontend while leaving GS rendering untouched.
	std::atomic<u32> s_capture_output_scale{1};
	std::mutex s_frame_mutex;
	struct PublishedFrame
	{
		std::vector<u32> pixels;
		u32 width = 0;
		u32 height = 0;
	};
	static constexpr size_t PUBLISHED_FRAME_CAPACITY = 3;
	std::array<PublishedFrame, PUBLISHED_FRAME_CAPACITY> s_published_frames;
	size_t s_published_head = 0;
	size_t s_published_count = 0;

	struct CaptureSlot
	{
		GSTexture* target = nullptr;
		std::unique_ptr<GSDownloadTexture> download;
		bool pending = false;
	};

	// Keep enough GPU distance between CopyFromTexture and Map/Flush for the
	// standalone render queue to finish naturally. Two slots regularly catch the
	// RX 580 one frame too early and turn an otherwise asynchronous readback into
	// a 20-30 ms CPU stall; three preserves full cadence with one extra frame of
	// bounded presentation latency.
	std::array<CaptureSlot, 3> s_capture_slots;
	u32 s_capture_width = 0;
	u32 s_capture_height = 0;
	bool s_capture_direct = false;
	size_t s_capture_slot = 0;

	void ResetCaptureSlots()
	{
		for (CaptureSlot& slot : s_capture_slots)
		{
			slot.download.reset();
			if (slot.target && g_gs_device)
				g_gs_device->Recycle(slot.target);
			slot.target = nullptr;
			slot.pending = false;
		}
		s_capture_width = 0;
		s_capture_height = 0;
		s_capture_direct = false;
		s_capture_slot = 0;
	}

	bool EnsureCaptureSlots(u32 width, u32 height, bool direct)
	{
		if (s_capture_width == width && s_capture_height == height && s_capture_direct == direct &&
			s_capture_slots[0].download && s_capture_slots[1].download)
		{
			return true;
		}

		ResetCaptureSlots();
		for (CaptureSlot& slot : s_capture_slots)
		{
			if (!direct)
				slot.target = g_gs_device->CreateRenderTarget(width, height, GSTexture::Format::Color, false);
			slot.download = g_gs_device->CreateDownloadTexture(width, height, GSTexture::Format::Color);
			if ((!direct && !slot.target) || !slot.download)
			{
				ResetCaptureSlots();
				return false;
			}
		}
		s_capture_width = width;
		s_capture_height = height;
		s_capture_direct = direct;
		return true;
	}

	void PublishCaptureSlot(CaptureSlot& slot)
	{
		if (!slot.pending)
			return;

		const GSVector4i rc(0, 0, s_capture_width, s_capture_height);
		slot.download->Flush();
		if (slot.download->Map(rc))
		{
			std::lock_guard lock(s_frame_mutex);
			if (s_published_count == PUBLISHED_FRAME_CAPACITY)
			{
				s_published_head = (s_published_head + 1) % PUBLISHED_FRAME_CAPACITY;
				--s_published_count;
			}
			const size_t tail = (s_published_head + s_published_count) % PUBLISHED_FRAME_CAPACITY;
			PublishedFrame& frame = s_published_frames[tail];
			frame.pixels.resize(static_cast<size_t>(s_capture_width) * s_capture_height);
			const u8* source = slot.download->GetMapPointer();
			for (u32 row = 0; row < s_capture_height; row++)
			{
				const u8* src = source + static_cast<size_t>(row) * slot.download->GetMapPitch();
				u32* dst = frame.pixels.data() + static_cast<size_t>(row) * s_capture_width;
				const u32* src32 = reinterpret_cast<const u32*>(src);
				for (u32 column = 0; column < s_capture_width; column++)
				{
					const u32 rgba = src32[column];
					dst[column] = ((rgba & 0x000000FFu) << 16) |
						(rgba & 0x0000FF00u) | ((rgba & 0x00FF0000u) >> 16);
				}
			}
			frame.width = s_capture_width;
			frame.height = s_capture_height;
			++s_published_count;
			slot.download->Unmap();
		}
		slot.pending = false;
	}
}

void ModernHostSetFrameCaptureEnabled(bool enabled)
{
	s_capture_enabled.store(enabled, std::memory_order_release);
}

void ModernHostSetCaptureResolution(u32 internal_scale, u32 output_scale)
{
	s_internal_render_scale.store(std::max(1u, internal_scale), std::memory_order_release);
	s_capture_output_scale.store(output_scale, std::memory_order_release);
}

void ModernHostResetFrameCapture()
{
	ResetCaptureSlots();
	std::lock_guard lock(s_frame_mutex);
	for (PublishedFrame& frame : s_published_frames)
	{
		frame.pixels.clear();
		frame.width = 0;
		frame.height = 0;
	}
	s_published_head = 0;
	s_published_count = 0;
}

bool ModernHostTakeFrame(std::vector<u32>* pixels, unsigned* width, unsigned* height)
{
	std::lock_guard lock(s_frame_mutex);
	if (s_published_count == 0)
		return false;
	PublishedFrame& frame = s_published_frames[s_published_head];
	pixels->swap(frame.pixels);
	*width = frame.width;
	*height = frame.height;
	frame.width = 0;
	frame.height = 0;
	s_published_head = (s_published_head + 1) % PUBLISHED_FRAME_CAPACITY;
	--s_published_count;
	return true;
}

void Host::CommitBaseSettingChanges() {}
void Host::LoadSettings(SettingsInterface&, std::unique_lock<std::mutex>&) {}
void Host::CheckForSettingsChanges(const Pcsx2Config&) {}
bool Host::RequestResetSettings(bool, bool, bool, bool, bool) { return false; }
void Host::SetDefaultUISettings(SettingsInterface&) {}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	std::fprintf(stderr, "[LRPS2 modern] %.*s: %.*s\n", static_cast<int>(title.size()), title.data(),
		static_cast<int>(message.size()), message.data());
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	std::fprintf(stderr, "[LRPS2 modern error] %.*s: %.*s\n", static_cast<int>(title.size()), title.data(),
		static_cast<int>(message.size()), message.data());
}
void Host::OpenURL(const std::string_view) {}
bool Host::InBatchMode() { return true; }
bool Host::InNoGUIMode() { return true; }
bool Host::CopyTextToClipboard(const std::string_view) { return false; }
std::string Host::GetTextFromClipboard() { return {}; }
void Host::BeginTextInput() {}
void Host::EndTextInput() {}
std::optional<WindowInfo> Host::GetTopLevelWindowInfo() { return std::nullopt; }
void Host::OnInputDeviceConnected(const std::string_view, const std::string_view) {}
void Host::OnInputDeviceDisconnected(const InputBindingKey, const std::string_view) {}
void Host::SetMouseMode(bool, bool) {}
void Host::SetMouseLock(bool) {}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool)
{
	// PCSX2 treats an empty optional as a host failure. A populated Surfaceless
	// WindowInfo is the supported offscreen path used by the GS runner.
	return WindowInfo{};
}
void Host::ReleaseRenderWindow()
{
}
void Host::BeginPresentFrame()
{
	if (!s_capture_enabled.load(std::memory_order_acquire))
		return;

	GSTexture* const current = g_gs_device ? g_gs_device->GetCurrent() : nullptr;
	if (!current)
		return;
	const u32 source_width = static_cast<u32>(current->GetWidth());
	const u32 source_height = static_cast<u32>(current->GetHeight());
	const u32 internal_scale = s_internal_render_scale.load(std::memory_order_acquire);
	const u32 requested_output_scale = s_capture_output_scale.load(std::memory_order_acquire);
	const u32 output_scale = requested_output_scale == 0 ? internal_scale :
		std::min(requested_output_scale, internal_scale);
	const u32 width = std::max(1u, static_cast<u32>((static_cast<u64>(source_width) * output_scale + internal_scale / 2) / internal_scale));
	const u32 height = std::max(1u, static_cast<u32>((static_cast<u64>(source_height) * output_scale + internal_scale / 2) / internal_scale));
	const bool same_size = width == source_width && height == source_height;
	const bool direct = same_size && GSGetCurrentRenderer() != GSRendererType::SW &&
		current->GetFormat() == GSTexture::Format::Color && !g_gs_device->UsesLowerLeftOrigin();
	if (!EnsureCaptureSlots(width, height, direct))
		return;

	// completed it, avoiding the screenshot path's same-frame synchronization.
	// Read the oldest staging resource first. By now the GPU has normally
	// completed it, avoiding the screenshot path's same-frame synchronization.
	// completed it, avoiding the screenshot path's same-frame synchronization.
	CaptureSlot& slot = s_capture_slots[s_capture_slot];
	PublishCaptureSlot(slot);

	const GSVector4i capture_rc(0, 0, width, height);
	if (direct)
	{
		slot.download->CopyFromTexture(capture_rc, current, capture_rc, 0);
	}
	else
	{
		g_gs_device->StretchRect(current, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), slot.target,
			GSVector4(capture_rc), ShaderConvert::TRANSPARENCY_FILTER, Biln);
		slot.download->CopyFromTexture(capture_rc, slot.target, capture_rc, 0);
	}
	slot.pending = true;
	s_capture_slot = (s_capture_slot + 1) % s_capture_slots.size();
}
void Host::RequestResizeHostDisplay(s32, s32) {}
bool Host::IsFullscreen() { return false; }
void Host::SetFullscreen(bool) {}
void Host::OnCaptureStarted(const std::string&) {}
void Host::OnCaptureStopped() {}

void Host::OnVMStarting() {}
void Host::OnVMStarted() {}
void Host::OnVMDestroyed() {}
void Host::OnVMPaused() {}
void Host::OnVMResumed() {}
void Host::OnGameChanged(const std::string&, const std::string&, const std::string&,
	const std::string&, u32, u32) {}
void Host::OnPerformanceMetricsUpdated() {}
void Host::OnSaveStateLoading(const std::string_view) {}
void Host::OnSaveStateLoaded(const std::string_view, bool) {}
void Host::OnSaveStateSaved(const std::string_view) {}

void Host::RunOnCPUThread(std::function<void()> function, bool)
{
	if (function)
		function();
}

void Host::RunOnGSThread(std::function<void()> function)
{
	if (function)
		function();
}

void Host::RefreshGameListAsync(bool) {}
void Host::CancelGameListRefresh() {}
namespace Host
{
	void OnGameListFavoriteChanged(const std::string&) {}
}
void Host::RequestExitApplication(bool) {}
void Host::RequestExitBigPicture() {}
void Host::RequestVMShutdown(bool, bool, bool) {}
void Host::PumpMessagesOnCPUThread() {}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	if (msg.empty())
		return 0;
	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char*, const char* msg, const char*, int count)
{
	TinyString count_string = TinyString::from_format("{}", count);
	std::string result(msg);
	for (;;)
	{
		const std::string::size_type position = result.find("%n");
		if (position == std::string::npos)
			break;
		result.replace(position, 2, count_string.view());
	}
	return result;
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason) {}
void Host::OnAchievementsLoginSuccess(const char*, u32, u32, u32) {}
void Host::OnAchievementsRefreshed() {}
void Host::OnAchievementsHardcoreModeChanged(bool) {}
bool Host::LocaleCircleConfirm() { return false; }
bool Host::ShouldPreferHostFileSelector() { return false; }

void Host::OpenHostFileSelectorAsync(std::string_view, bool, FileSelectorCallback callback,
	FileSelectorFilters, std::string_view)
{
	if (callback)
		callback({});
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int result = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (result != 0)
		return result;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

#include "pcsx2/GS/GSCapture.h"
#include "common/Threading.h"

namespace GSCapture
{
	bool BeginCapture(float, GSVector2i, float, std::string) { return false; }
	bool DeliverVideoFrame(GSTexture*) { return false; }
	void DeliverAudioPacket(const float*) {}
	void EndCapture() {}
	bool IsCapturing() { return false; }
	bool IsCapturingVideo() { return false; }
	bool IsCapturingAudio() { return false; }
	TinyString GetElapsedTime() { return {}; }
	const Threading::ThreadHandle& GetEncoderThreadHandle()
	{
		static const Threading::ThreadHandle s_handle;
		return s_handle;
	}
	GSVector2i GetSize() { return {}; }
	std::string GetNextCaptureFileName() { return {}; }
	void Flush() {}
	void FlushAudioOnly() {}
	CodecList GetVideoCodecList(const char*) { return {}; }
	CodecList GetAudioCodecList(const char*) { return {}; }
	FormatList GetVideoFormatList(const char*) { return {}; }
} // namespace GSCapture

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include "common/RedtapeWilCom.h"
#include <objidl.h>
#include <oleauto.h>
#include <dxcapi.h>

namespace
{
	struct NullDxcCompiler3 final : IDxcCompiler3
	{
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppvObject) override
		{
			if (ppvObject)
				*ppvObject = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
		ULONG STDMETHODCALLTYPE Release() override { return 1; }
		HRESULT STDMETHODCALLTYPE Compile(const DxcBuffer*, LPCWSTR*, UINT32, IDxcIncludeHandler*, REFIID, LPVOID* ppResult) override
		{
			if (ppResult)
				*ppResult = nullptr;
			return E_FAIL;
		}
		HRESULT STDMETHODCALLTYPE Disassemble(const DxcBuffer*, REFIID, LPVOID* ppResult) override
		{
			if (ppResult)
				*ppResult = nullptr;
			return E_FAIL;
		}
	};
} // namespace

extern "C" HRESULT __stdcall DxcCreateInstance(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = nullptr;
	if (HMODULE dxcompiler = LoadLibraryW(L"dxcompiler.dll"))
	{
		if (auto fn = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxcompiler, "DxcCreateInstance")))
		{
			if (SUCCEEDED(fn(rclsid, riid, ppv)) && *ppv)
				return S_OK;
		}
	}
	static NullDxcCompiler3 s_null_compiler;
	*ppv = static_cast<IDxcCompiler3*>(&s_null_compiler);
	return E_NOINTERFACE;
}
#endif

