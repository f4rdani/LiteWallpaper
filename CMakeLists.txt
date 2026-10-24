cmake_minimum_required(VERSION 3.20)
project(MiniWallpaper VERSION 2.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# === Performance & Size Optimization Flags ===
if(MSVC)
    # Optimize for speed & compact codegen, disable RTTI, enforce UTF-8 source/execution charset
    add_compile_options(/O2 /Oi /Gy /GL /Gw /GR- /EHsc /std:c++20 /W3 /utf-8)
    add_link_options(/LTCG /OPT:REF /OPT:ICF /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll /DELAYLOAD:D3DCOMPILER_47.dll /DELAYLOAD:COMDLG32.dll)
else()
    add_compile_options(-O3 -flto -fno-exceptions -fno-rtti -std=c++20)
endif()

# === Dependencies via vcpkg ===
find_package(nlohmann_json CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

# === FFmpeg (bundled SDK) ===
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg root directory")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_DIR}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_DIR}/lib")

# === Build-Time HLSL Bytecode Compilation (eliminates runtime d3dcompiler_47.dll) ===
find_program(FXC_EXECUTABLE fxc
    HINTS
        "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
        "$ENV{WindowsSdkVerBinPath}/x64"
)
if(NOT FXC_EXECUTABLE)
    file(GLOB FXC_SDK_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
    if(FXC_SDK_CANDIDATES)
        list(SORT FXC_SDK_CANDIDATES ORDER DESCENDING)
        list(GET FXC_SDK_CANDIDATES 0 FXC_EXECUTABLE)
    endif()
endif()
if(NOT FXC_EXECUTABLE)
    message(FATAL_ERROR "fxc.exe not found in Windows SDK! Install Windows 10/11 SDK.")
endif()
message(STATUS "Using HLSL compiler: ${FXC_EXECUTABLE}")

set(GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_VS_HEADER "${GENERATED_SHADER_DIR}/fullscreen_quad_vs.h")
set(GENERATED_PS_HEADER "${GENERATED_SHADER_DIR}/nv12_to_rgb_ps.h")
set(GENERATED_SHADER_HEADERS "${GENERATED_VS_HEADER}" "${GENERATED_PS_HEADER}")

add_custom_command(
    OUTPUT "${GENERATED_VS_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_SHADER_DIR}"
    COMMAND "${FXC_EXECUTABLE}" /nologo /T vs_4_0 /E main /O3 /Qstrip_reflect /Qstrip_debug /Qstrip_priv /Vn g_vs_bytecode /Fh "${GENERATED_VS_HEADER}" "${CMAKE_SOURCE_DIR}/shaders/fullscreen_quad.hlsl"
    DEPENDS "${CMAKE_SOURCE_DIR}/shaders/fullscreen_quad.hlsl"
    COMMENT "Precompiling HLSL Vertex Shader -> fullscreen_quad_vs.h"
    VERBATIM
)

add_custom_command(
    OUTPUT "${GENERATED_PS_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_SHADER_DIR}"
    COMMAND "${FXC_EXECUTABLE}" /nologo /T ps_4_0 /E main /O3 /Qstrip_reflect /Qstrip_debug /Qstrip_priv /Vn g_ps_bytecode /Fh "${GENERATED_PS_HEADER}" "${CMAKE_SOURCE_DIR}/shaders/nv12_to_rgb.hlsl"
    DEPENDS "${CMAKE_SOURCE_DIR}/shaders/nv12_to_rgb.hlsl"
    COMMENT "Precompiling HLSL Pixel Shader -> nv12_to_rgb_ps.h"
    VERBATIM
)

# === Unified Single Executable ===
add_executable(MiniWallpaper WIN32
    src/main.cpp
    src/core/win_utils.cpp
    src/core/config.cpp
    src/core/legacy_migrator.cpp
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
    src/platform/win32/screensaver_runner.cpp
    src/ui/settings_app.cpp
    src/ui/ui_widgets.cpp
    src/ui/ui_modals.cpp
    src/ui/ui_pages.cpp
    src/ui/thumbnail_manager.cpp
    src/diagnostics/memprobe.cpp
    src/app.rc
    ${GENERATED_SHADER_HEADERS}
)

target_include_directories(MiniWallpaper PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${FFMPEG_INCLUDE_DIR}
    ${GENERATED_SHADER_DIR}
)

# Generate native MSVC short-import .lib files from BtbN FFmpeg .def files so MSVC /DELAYLOAD works cleanly
set(FFMPEG_LINK_LIBS avcodec avformat avutil swscale swresample)
set(FFMPEG_DELAYLOAD_FLAGS "")
if(MSVC)
    get_filename_component(MSVC_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(MSVC_LIB_EXECUTABLE lib HINTS "${MSVC_COMPILER_DIR}" NO_DEFAULT_PATH)
    if(NOT MSVC_LIB_EXECUTABLE)
        find_program(MSVC_LIB_EXECUTABLE lib)
    endif()

    set(GEN_FFMPEG_LIB_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_msvc_libs")
    file(MAKE_DIRECTORY "${GEN_FFMPEG_LIB_DIR}")

    set(GEN_FFMPEG_LIBS "")
    foreach(FF_COMP avcodec avformat avutil swscale swresample)
        file(GLOB FF_DEF_MATCH "${FFMPEG_LIB_DIR}/${FF_COMP}-*.def")
        if(FF_DEF_MATCH AND MSVC_LIB_EXECUTABLE)
            list(GET FF_DEF_MATCH 0 FF_DEF_FILE)
            get_filename_component(FF_DLL_STEM "${FF_DEF_FILE}" NAME_WE)
            set(FF_DLL_FILENAME "${FF_DLL_STEM}.dll")
            set(GEN_DEF_FILE "${GEN_FFMPEG_LIB_DIR}/${FF_DLL_STEM}.def")
            set(GEN_LIB_FILE "${GEN_FFMPEG_LIB_DIR}/${FF_COMP}.lib")

            file(READ "${FF_DEF_FILE}" FF_DEF_CONTENTS)
            if(NOT FF_DEF_CONTENTS MATCHES "^LIBRARY")
                file(WRITE "${GEN_DEF_FILE}" "LIBRARY \"${FF_DLL_FILENAME}\"\n${FF_DEF_CONTENTS}")
            else()
                file(WRITE "${GEN_DEF_FILE}" "${FF_DEF_CONTENTS}")
            endif()

            execute_process(
                COMMAND "${MSVC_LIB_EXECUTABLE}" /nologo /machine:x64 "/def:${GEN_DEF_FILE}" "/out:${GEN_LIB_FILE}"
                RESULT_VARIABLE LIB_GEN_RES
                OUTPUT_QUIET
            )
            if(LIB_GEN_RES EQUAL 0 AND EXISTS "${GEN_LIB_FILE}")
                list(APPEND GEN_FFMPEG_LIBS "${GEN_LIB_FILE}")
                list(APPEND FFMPEG_DELAYLOAD_FLAGS "/DELAYLOAD:${FF_DLL_FILENAME}")
            endif()
        endif()
    endforeach()

    list(LENGTH GEN_FFMPEG_LIBS GEN_FFMPEG_COUNT)
    if(GEN_FFMPEG_COUNT EQUAL 5)
        set(FFMPEG_LINK_LIBS ${GEN_FFMPEG_LIBS})
        message(STATUS "Generated native MSVC delay-load import libs: ${FFMPEG_DELAYLOAD_FLAGS}")
    else()
        set(FFMPEG_DELAYLOAD_FLAGS "")
        target_link_directories(MiniWallpaper PRIVATE ${FFMPEG_LIB_DIR})
    endif()
else()
    target_link_directories(MiniWallpaper PRIVATE ${FFMPEG_LIB_DIR})
endif()

target_link_libraries(MiniWallpaper PRIVATE
    nlohmann_json::nlohmann_json
    mimalloc
    imgui::imgui
    # FFmpeg libs (all delay-loaded on MSVC so --startup loads 0 MB FFmpeg until readiness gate opens,
    # and swscale/swresample stay 100% unloaded during muted hardware playback!)
    ${FFMPEG_LINK_LIBS}
    # Windows system libs (d3d11, dxgi, D3DCOMPILER_47 & COMDLG32 delay-loaded!)
    d3d11 dxgi windowscodecs delayimp
    dwmapi user32 gdi32 shell32 ole32 uuid
    winmm wtsapi32 psapi comdlg32 advapi32 imm32 windowsapp
)

if(MSVC AND FFMPEG_DELAYLOAD_FLAGS)
    target_link_options(MiniWallpaper PRIVATE ${FFMPEG_DELAYLOAD_FLAGS})
endif()

# Automatically generate MiniWallpaper.scr companion screensaver binary
add_custom_command(TARGET MiniWallpaper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:MiniWallpaper>"
    "$<TARGET_FILE_DIR:MiniWallpaper>/MiniWallpaper.scr"
    COMMENT "Creating MiniWallpaper.scr screensaver binary"
)
