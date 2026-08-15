# 🖼️ LiteWallpaper

Lightweight animated video wallpaper engine for Windows.

## Features
- 🎬 Video wallpaper (MP4, WebM, MKV, AVI)
- 🖥️ Multi-monitor support
- 🔇 Audio support (muted by default)
- 🔒 Lock screen snapshot
- ⚡ Ultra-lightweight: < 30 MB RAM, < 1% CPU
- 🎮 Auto-pause during games
- 🔋 Battery-aware FPS throttling

## System Requirements
- Windows 7 SP1 or later (with Platform Update KB2670838)
- DirectX 11 compatible GPU
- 64-bit processor

## Usage
1. Extract the .zip file
2. Run `litewp_daemon.exe`
3. Right-click the tray icon to change wallpaper
4. Run `litewp_settings.exe` for advanced settings

## Building from Source

### Prerequisites
- Visual Studio 2022 / MSVC (C++20 support)
- CMake 3.20+
- vcpkg package manager
- FFmpeg 64-bit shared developer libraries

### Build Commands
```powershell
# 1. Install dependencies via vcpkg
vcpkg install nlohmann-json:x64-windows mimalloc:x64-windows imgui[docking-experimental,win32-binding,dx11-binding]:x64-windows

# 2. Extract FFmpeg dev libraries into third_party/ffmpeg
# third_party/ffmpeg/include/libavcodec/avcodec.h
# third_party/ffmpeg/lib/avcodec.lib

# 3. Configure and build
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## License
GPL v3
