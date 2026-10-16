# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(eden_libretro MODULE
    libretro.h
    libretro_core.h
    libretro_core.cpp
    environment.h
    environment.cpp
    emu_window/emu_window_libretro.h
    emu_window/emu_window_libretro.cpp
    video/graphics_context_libretro.h
    video/graphics_context_libretro.cpp
    video/opengl_bridge.h
    video/opengl_bridge.cpp
    video/vulkan_bridge.h
    video/vulkan_bridge.cpp
    video/vma.cpp
    audio/libretro_sink.h
    audio/libretro_sink.cpp
    input/libretro_driver.h
    input/libretro_driver.cpp
    logging/libretro_log.h
    logging/libretro_log.cpp
)

set_target_properties(eden_libretro PROPERTIES
    PREFIX ""
    OUTPUT_NAME "eden_libretro"
)

if (NOT WIN32)
    target_link_options(eden_libretro PRIVATE "-Wl,-Bsymbolic")
else()
    if (MSVC)
        target_link_options(eden_libretro PRIVATE "/MAP")
    endif()
endif()

target_include_directories(eden_libretro PRIVATE
    .
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(eden_libretro PRIVATE
    audio_core
    common
    core
    frontend_common
    hid_core
    input_common
    video_core
    ${PLATFORM_LIBRARIES}
    Threads::Threads
    $<$<PLATFORM_ID:Windows>:opengl32>
)

create_target_directory_groups(eden_libretro)
