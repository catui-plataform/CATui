# CATUI's adapter is opt-in and isolated from the normal shadPS4 executable.
add_library(catui_shadps4_libretro_bridge STATIC
    catui_libretro_bridge.cpp
    catui_libretro_bridge.h
)
target_include_directories(catui_shadps4_libretro_bridge PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(catui_shadps4_libretro_bridge PUBLIC cxx_std_20)

add_library(shadps4_libretro SHARED
    catui_libretro_api.cpp
    catui_libretro_api.h
    catui_shadps4_backend.cpp
)
target_link_libraries(shadps4_libretro PRIVATE catui_shadps4_libretro_bridge)
target_include_directories(shadps4_libretro PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(shadps4_libretro PUBLIC cxx_std_20)
target_compile_definitions(shadps4_libretro PRIVATE CATUI_SHADPS4_REAL_BACKEND)

if(WIN32)
    target_link_libraries(shadps4_libretro PRIVATE winmm)
    # The emulator DLL is loaded only by a disposable, hidden process. The
    # frontend sees the small proxy under the historical registered filename.
    set_target_properties(shadps4_libretro PROPERTIES OUTPUT_NAME shadps4_libretro_engine)
    add_library(shadps4_libretro_proxy SHARED catui_ps4_proxy.cpp catui_ps4_ipc.h)
    set_target_properties(shadps4_libretro_proxy PROPERTIES OUTPUT_NAME shadps4_libretro)
    target_include_directories(shadps4_libretro_proxy PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(shadps4_libretro_proxy PRIVATE cxx_std_20)

    add_executable(catui_shadps4_host catui_ps4_host.cpp catui_ps4_ipc.h)
    target_include_directories(catui_shadps4_host PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(catui_shadps4_host PRIVATE cxx_std_20)
    target_link_libraries(catui_shadps4_host PRIVATE shadps4_libretro winmm)
endif()

option(ENABLE_CATUI_LIBRETRO_SMOKE "Build the synthetic Libretro ABI smoke test" OFF)
if(ENABLE_CATUI_LIBRETRO_SMOKE)
    add_executable(catui_shadps4_libretro_smoke
        catui_libretro_smoke.cpp
        catui_libretro_api.cpp
        catui_libretro_bridge.cpp
    )
    target_include_directories(catui_shadps4_libretro_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(catui_shadps4_libretro_smoke PRIVATE cxx_std_20)

    add_executable(catui_shadps4_libretro_real_probe catui_real_probe.cpp)
    target_include_directories(catui_shadps4_libretro_real_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(catui_shadps4_libretro_real_probe PRIVATE ${CMAKE_SOURCE_DIR}/externals/stb)
    target_compile_features(catui_shadps4_libretro_real_probe PRIVATE cxx_std_20)
    target_link_libraries(catui_shadps4_libretro_real_probe PRIVATE shadps4_libretro $<$<PLATFORM_ID:Windows>:winmm>)

    if(WIN32)
        add_executable(catui_shadps4_proxy_lifecycle_probe catui_real_probe.cpp)
        target_include_directories(catui_shadps4_proxy_lifecycle_probe PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/externals/stb)
        target_compile_features(catui_shadps4_proxy_lifecycle_probe PRIVATE cxx_std_20)
        target_link_libraries(catui_shadps4_proxy_lifecycle_probe PRIVATE shadps4_libretro_proxy winmm)
    endif()

    add_executable(catui_shadps4_libretro_benchmark catui_benchmark.cpp)
    target_include_directories(catui_shadps4_libretro_benchmark PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(catui_shadps4_libretro_benchmark PRIVATE ${CMAKE_SOURCE_DIR}/externals/stb)
    target_compile_features(catui_shadps4_libretro_benchmark PRIVATE cxx_std_20)
    target_link_libraries(catui_shadps4_libretro_benchmark PRIVATE shadps4_libretro $<$<PLATFORM_ID:Windows>:winmm>)
endif()
