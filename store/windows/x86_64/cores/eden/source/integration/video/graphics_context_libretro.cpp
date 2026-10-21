#include "eden_libretro/video/graphics_context_libretro.h"

#include <mutex>
#include <glad/glad.h>
#include "common/logging.h"
#include "common/settings.h"

namespace Libretro {

namespace {
#ifdef _WIN32
std::mutex gl_share_mutex;
HGLRC gl_share_root{NULL};
#endif
} // namespace

LibretroGraphicsContext::LibretroGraphicsContext() {
#ifdef _WIN32
    const auto backend = Settings::values.renderer_backend.GetValue();
    if (backend == Settings::RendererBackend::Vulkan ||
        backend == Settings::RendererBackend::Null) {
        return;
    }

    is_gl = true;

    // Save previous host context (e.g. Godot engine UI context)
    HDC host_hdc = wglGetCurrentDC();
    HGLRC host_hglrc = wglGetCurrentContext();

    hwnd = CreateWindowExA(0, "STATIC", "EdenGLContext", WS_POPUP, 0, 0, 16, 16, NULL, NULL,
                           GetModuleHandle(NULL), NULL);
    if (!hwnd) {
        LOG_CRITICAL(Render_OpenGL, "Failed to create hidden HWND for WGL");
        return;
    }

    hdc = GetDC(hwnd);
    if (!hdc) {
        LOG_CRITICAL(Render_OpenGL, "Failed to get DC for hidden HWND");
        return;
    }

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;

    int format = ChoosePixelFormat(hdc, &pfd);
    if (!SetPixelFormat(hdc, format, &pfd)) {
        LOG_CRITICAL(Render_OpenGL, "Failed to SetPixelFormat for WGL");
        return;
    }

    HGLRC temp_hglrc = wglCreateContext(hdc);
    if (!temp_hglrc) {
        LOG_CRITICAL(Render_OpenGL, "Failed to create bootstrap WGL context");
        return;
    }

    // GLAD stores process-global function pointers. Eden creates many shader
    // contexts in parallel, so context creation and GLAD initialization must be
    // serialized or the loader races itself and crashes inside the driver.
    std::lock_guard share_lock{gl_share_mutex};
    wglMakeCurrent(hdc, temp_hglrc);
    if (!gl_share_root) {
        gladLoadGL();
    }

    typedef HGLRC(WINAPI * PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
    auto wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    if (wglCreateContextAttribsARB) {
        int attribs[] = {
            0x2091, 4, // WGL_CONTEXT_MAJOR_VERSION_ARB
            0x2092, 6, // WGL_CONTEXT_MINOR_VERSION_ARB
            // Eden's desktop OpenGL path requests a compatibility profile.
            0x9126,
            0x00000002, // WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
            0};
        HGLRC core_hglrc = wglCreateContextAttribsARB(hdc, gl_share_root, attribs);
        if (core_hglrc) {
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(temp_hglrc);
            hglrc = core_hglrc;
            wglMakeCurrent(hdc, hglrc);
            if (!gl_share_root) {
                gladLoadGL();
            }
        } else {
            hglrc = temp_hglrc;
        }
    } else {
        if (gl_share_root && !wglShareLists(gl_share_root, temp_hglrc)) {
            LOG_CRITICAL(Render_OpenGL, "Failed to share the fallback WGL context");
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(temp_hglrc);
            return;
        }
        hglrc = temp_hglrc;
    }
    if (!gl_share_root) {
        gl_share_root = hglrc;
    }

    // Immediately restore host context so Godot's UI rendering is unaffected
    if (host_hdc && host_hglrc) {
        wglMakeCurrent(host_hdc, host_hglrc);
    } else {
        wglMakeCurrent(NULL, NULL);
    }
#endif
}

LibretroGraphicsContext::~LibretroGraphicsContext() {
#ifdef _WIN32
    DoneCurrent();
    if (hglrc) {
        std::lock_guard share_lock{gl_share_mutex};
        if (gl_share_root == hglrc) {
            gl_share_root = NULL;
        }
        wglDeleteContext(hglrc);
        hglrc = NULL;
    }
    if (hwnd && hdc) {
        ReleaseDC(hwnd, hdc);
        if (IsWindow(hwnd)) {
            DestroyWindow(hwnd);
        }
        hwnd = NULL;
        hdc = NULL;
    }
#endif
}

void LibretroGraphicsContext::SwapBuffers() {
#ifdef _WIN32
    if (hdc && is_gl) {
        ::SwapBuffers(hdc);
    }
#endif
}

void LibretroGraphicsContext::MakeCurrent() {
#ifdef _WIN32
    if (is_gl && hdc && hglrc && !made_current) {
        saved_prev_hdc = wglGetCurrentDC();
        saved_prev_hglrc = wglGetCurrentContext();
        made_current = wglMakeCurrent(hdc, hglrc) == TRUE;
    }
#endif
}

void LibretroGraphicsContext::DoneCurrent() {
#ifdef _WIN32
    if (is_gl && made_current) {
        if (saved_prev_hdc && saved_prev_hglrc) {
            wglMakeCurrent(saved_prev_hdc, saved_prev_hglrc);
        } else {
            wglMakeCurrent(NULL, NULL);
        }
        saved_prev_hdc = NULL;
        saved_prev_hglrc = NULL;
        made_current = false;
    }
#endif
}

} // namespace Libretro
