cmake_minimum_required(VERSION 3.20)
project(LiteWallpaper VERSION 1.0.3 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# === Performance flags ===
if(MSVC)
    # Optimize for size and performance, disable RTTI
    add_compile_options(/O2 /Oi /Gy /GL /GR- /EHsc /std:c++20 /W3 /utf-8)
    add_link_options(/LTCG /OPT:REF /OPT:ICF)
else()
    add_compile_options(-O3 -flto -fno-exceptions -fno-rtti -std=c++20)
endif()

# === Dependencies via vcpkg ===
find_package(nlohmann_json CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

# === FFmpeg (manual find or bundled) ===
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg root directory")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_DIR}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_DIR}/lib")

# === Unified Single Executable ===
add_executable(LiteWallpaper WIN32
    src/main.cpp
    src/core/config.cpp
    src/core/playback_clock.cpp
    src/core/ipc_server.cpp
    src/core/engine_state.cpp
    src/core/video_optimizer.cpp
    src/decoder/ffmpeg_hw_decoder.cpp
    src/decoder/audio_player.cpp
    src/platform/win32/desktop_injector.cpp
    src/platform/win32/d3d11_presenter.cpp
    src/platform/win32/power_governor.cpp
    src/platform/win32/lockscreen_manager.cpp
    src/platform/win32/tray_icon.cpp
    src/platform/win32/hardware_info.cpp
    src/ui/settings_app.cpp
    src/ui/thumbnail_manager.cpp
    src/app.rc
)

target_include_directories(LiteWallpaper PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${FFMPEG_INCLUDE_DIR}
)

target_link_directories(LiteWallpaper PRIVATE ${FFMPEG_LIB_DIR})

target_link_libraries(LiteWallpaper PRIVATE
    nlohmann_json::nlohmann_json
    mimalloc
    imgui::imgui
    # FFmpeg libs
    avcodec avformat avutil swscale swresample
    # Windows system libs
    d3d11 dxgi d3dcompiler
    dwmapi user32 gdi32 shell32 ole32 uuid
    winmm wtsapi32 psapi comdlg32 advapi32 imm32
)
