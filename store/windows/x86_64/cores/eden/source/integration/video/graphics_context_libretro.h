#pragma once

#include "core/frontend/graphics_context.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Libretro {

class LibretroGraphicsContext final : public Core::Frontend::GraphicsContext {
public:
    LibretroGraphicsContext();
    ~LibretroGraphicsContext() override;

    void SwapBuffers() override;
    void MakeCurrent() override;
    void DoneCurrent() override;

private:
#ifdef _WIN32
    HWND hwnd{NULL};
    HDC hdc{NULL};
    HGLRC hglrc{NULL};
    HDC saved_prev_hdc{NULL};
    HGLRC saved_prev_hglrc{NULL};
    bool is_gl{false};
    bool made_current{false};
#endif
};

} // namespace Libretro
