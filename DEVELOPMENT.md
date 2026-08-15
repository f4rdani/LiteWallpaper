# 🛠️ LiteWallpaper — Developer & Architecture Guide

Welcome to the **LiteWallpaper** codebase! This guide is written for developers and contributors who want to understand the internal architecture, set up the development environment, build from source, and extend the engine.

---

## 📑 Table of Contents
1. [Core Philosophy & Constraints](#-core-philosophy--constraints)
2. [High-Level Architecture](#-high-level-architecture)
3. [Component Breakdown](#-component-breakdown)
4. [Prerequisites & Environment Setup](#-prerequisites--environment-setup)
5. [Building from Source](#-building-from-source)
6. [Packaging Portable Distribution](#-packaging-portable-distribution)
7. [IPC Protocol Specification](#-ipc-protocol-specification)
8. [Critical Engineering Rules (Performance & Compatibility)](#-critical-engineering-rules)
9. [Future Roadmap & Porting Guide](#-future-roadmap--porting-guide)

---

## 🎯 Core Philosophy & Constraints

LiteWallpaper is engineered with extreme performance constraints that differentiate it from other wallpaper engines:

| Constraint | Target / Rule | Rationale |
|:---|:---|:---|
| **CPU Usage** | **< 1.0%** (0.0% when gaming/locked) | No background CPU polling, zero software video decoding |
| **RAM Usage** | **< 30 MB** (typically 12–18 MB) | Decoupled UI; video frames stay 100% in GPU VRAM |
| **Compatibility** | **Windows 7 SP1+ → Windows 11** | DXGI 1.0/1.1 DISCARD swapchain, no C++/WinRT requirements |
| **Process Model** | **Dual-Process Architecture** | UI engine never stays loaded in background |

---

## 🏗️ High-Level Architecture

LiteWallpaper is split into two independent executables communicating over Windows Named Pipes:

```
┌─────────────────────────────────────────────────────────────┐
│                 litewp_daemon.exe (Resident)                │
│  - Memory: ~12 MB - 25 MB RAM | CPU: < 0.7%                 │
│  - Injects into desktop canvas (WorkerW 0x052C)             │
│  - Zero-Copy D3D11VA Hardware Video Decoding                │
│  - Low-overhead WASAPI Audio Streamer                       │
│  - Power Governor (Auto-pauses on game / lock / battery)    │
│  - Tray Icon & Named Pipe IPC Server                        │
└──────────────────────────────▲──────────────────────────────┘
                               │ IPC (JSON-RPC over Named Pipe)
                               │ \\.\pipe\LiteWallpaper
┌──────────────────────────────▼──────────────────────────────┐
│                litewp_settings.exe (On-Demand)              │
│  - Memory: 0 MB when closed | Runs only when opened         │
│  - Dear ImGui (Docking) + DirectX 11 backend                │
│  - Real-time performance graphing (FPS, CPU, RAM)           │
│  - Video file picker, volume, and power sliders             │
└─────────────────────────────────────────────────────────────┘
```

---

## 📂 Component Breakdown

```
src/
├── main.cpp                         # Daemon main loop & message dispatcher
│
├── core/                            # Platform-agnostic core logic
│   ├── config.h / .cpp              # JSON configuration load/save (nlohmann_json)
│   ├── playback_clock.h / .cpp      # QPC-driven adaptive frame pacing clock
│   ├── ipc_server.h / .cpp          # Windows Named Pipe server & client
│   └── ring_buffer.h                # Lock-free SPSC packet ring buffer
│
├── decoder/                         # Media playback & hardware acceleration
│   ├── video_decoder.h              # Abstract video decoder interface
│   ├── ffmpeg_hw_decoder.h / .cpp   # FFmpeg D3D11VA zero-copy hardware decoder
│   └── audio_player.h / .cpp        # Low-latency WASAPI shared audio output
│
├── platform/win32/                  # Windows OS integration layer
│   ├── desktop_injector.h / .cpp    # WorkerW desktop canvas injector (0x052C)
│   ├── d3d11_presenter.h / .cpp     # Direct3D 11 device, swapchain & NV12 shaders
│   ├── power_governor.h / .cpp      # Auto-pause heuristics (fullscreen/lock/battery)
│   ├── lockscreen_manager.h / .cpp  # Lock screen snapshot capture & staging
│   └── tray_icon.h / .cpp           # Shell_NotifyIconW & popup menu
│
├── ui/                              # User Interface
│   ├── settings_app.h / .cpp        # Dear ImGui settings application
│
└── shaders/                         # Embedded HLSL Shader files
    ├── fullscreen_quad.hlsl         # 3-vertex fullscreen triangle vertex shader
    └── nv12_to_rgb.hlsl             # BT.709 NV12 (Y + UV) to RGB pixel shader
```

---

## 🧰 Prerequisites & Environment Setup

### Required Tools
1. **Operating System**: Windows 10/11 (64-bit) for development (binaries run on Windows 7 SP1+).
2. **Compiler**: Visual Studio 2022 (MSVC v143 toolset) with *Desktop development with C++*.
3. **CMake**: Version 3.20 or newer.
4. **vcpkg**: Microsoft C++ package manager.
5. **FFmpeg Developer Shared Libraries (64-bit)**:
   - Header files (`include/libavcodec`, `include/libavformat`, etc.)
   - Import libraries (`lib/avcodec.lib`, `lib/avformat.lib`, etc.)
   - Runtime DLLs (`bin/avcodec-*.dll`, etc.)

---

## 🔨 Building from Source

### Step 1: Install Dependencies via vcpkg
In the project root directory (manifest mode will read `vcpkg.json` automatically):

```powershell
# If using a local vcpkg instance (e.g. C:\tools\vcpkg):
C:\tools\vcpkg\vcpkg.exe install --triplet x64-windows
```

### Step 2: Set Up FFmpeg Developer Files
Ensure FFmpeg 64-bit shared developer files are extracted in `third_party/ffmpeg`:
```
third_party/ffmpeg/
├── include/
│   ├── libavcodec/
│   ├── libavformat/
│   ├── libavutil/
│   ├── libswresample/
│   └── libswscale/
├── lib/
│   ├── avcodec.lib
│   ├── avformat.lib
│   ├── avutil.lib
│   ├── swresample.lib
│   └── swscale.lib
└── bin/
    ├── avcodec-*.dll
    ├── avformat-*.dll
    ├── avutil-*.dll
    ├── swresample-*.dll
    └── swscale-*.dll
```

### Step 3: Configure and Build
```powershell
# Configure CMake with vcpkg toolchain
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="C:/tools/vcpkg/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows

# Build in Release configuration
cmake --build build --config Release
```

Output executables will be located in:
- `build/Release/litewp_daemon.exe`
- `build/Release/litewp_settings.exe`

---

## 📦 Packaging Portable Distribution

To bundle executables, runtime DLLs, assets, and shaders for release:

```powershell
$dist = "dist/LiteWallpaper-Portable"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
New-Item -ItemType Directory -Force -Path "$dist/assets" | Out-Null

Copy-Item "build/Release/litewp_daemon.exe" -Destination $dist
Copy-Item "build/Release/litewp_settings.exe" -Destination $dist
Copy-Item "third_party/ffmpeg/bin/*.dll" -Destination $dist
Copy-Item "build/vcpkg_installed/x64-windows/bin/*.dll" -Destination $dist
Copy-Item "assets/*" -Destination "$dist/assets"
Copy-Item "LICENSE" -Destination $dist
Copy-Item "README.md" -Destination $dist

# Compress to Zip
Compress-Archive -Path "$dist/*" -DestinationPath "dist/LiteWallpaper-Portable-win64.zip" -Force
```

---

## 🔌 IPC Protocol Specification

The daemon and settings app communicate via Windows Named Pipe:
- **Pipe Address**: `\\.\pipe\LiteWallpaper`
- **Format**: JSON-RPC over message pipe (`PIPE_TYPE_MESSAGE`)

### Supported Commands

#### 1. Set Wallpaper
```json
// Request
{ "cmd": "set_wallpaper", "path": "C:/path/to/video.mp4", "monitor": "\\\\.\\DISPLAY1" }

// Response
{ "ok": true }
```

#### 2. Get Engine Status
```json
// Request
{ "cmd": "get_status" }

// Response
{
  "ok": true,
  "playing": true,
  "paused": false,
  "fps": 30,
  "ram_mb": 14,
  "power_state": "Active",
  "wallpaper": "C:/path/to/video.mp4",
  "volume": 0.5,
  "muted": true
}
```

#### 3. Playback Controls
```json
{ "cmd": "pause" }
{ "cmd": "resume" }
{ "cmd": "set_fps", "fps": 60 }
{ "cmd": "set_volume", "volume": 0.8 }
{ "cmd": "reload_config" }
```

---

## ⚠️ Critical Engineering Rules

When modifying or contributing code to LiteWallpaper, **always follow these rules**:

### 1. Zero-Copy Video Pipeline
- **NEVER** decode video into host CPU RAM buffers. Decoded frames from `FFmpegHWDecoder` **must** remain inside `ID3D11Texture2D` in GPU VRAM (NV12 format).
- Use `Texture2DArray` HLSL sampling in pixel shaders (`shaders/nv12_to_rgb.hlsl`).

### 2. Concurrency & Thread Safety
- The media decoder (`g_decoder`) is shared between the main render loop and background IPC/dialog worker threads.
- Always protect decoder operations (`Close()`, `Open()`, `DecodeNextFrame()`, `DecodeAudioSamples()`) using `g_decoder_mutex`.
- D3D11 multithread protection (`ID3D10Multithread::SetMultithreadProtected(TRUE)`) must remain enabled.

### 3. Windows 7 Compatibility
- **DO NOT** use `DXGI_SWAP_EFFECT_FLIP_DISCARD` or `IDXGISwapChain1` (Windows 10+ only). Always use `IDXGISwapChain` and `DXGI_SWAP_EFFECT_DISCARD`.
- **DO NOT** use `winrt::` / C++/WinRT in core background components.
- Keep C++ exceptions disabled (`/GR-`, exception-free JSON parsing via `json::parse(..., nullptr, false)`).

### 4. Audio Pacing & Buffering
- `AudioPlayer` uses shared-mode WASAPI.
- When muted (`m_muted == true`), WASAPI streams are kept idle to conserve memory and CPU wakeups.

---

## 🗺️ Future Roadmap & Porting Guide

1. **Linux Port**:
   - Replace `WorkerW` injector with `wlr-layer-shell` protocol for Wayland (`ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND`) and `_NET_WM_WINDOW_TYPE_DESKTOP` for X11.
   - Replace D3D11VA with FFmpeg `VA-API` / `VDPAU`.
2. **macOS Port**:
   - Attach borderless `NSWindow` with `level = kCGDesktopWindowLevel - 1`.
   - Use `VideoToolbox` hardware acceleration and Metal render pipelines.
3. **Live2D / 2D Mesh Engine**:
   - Optional modular plugin architecture for rendering Cubism Core / Inochi2D puppet models with sub-step physics limits.

---

## 📄 License
This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**. See [LICENSE](LICENSE) for details.
