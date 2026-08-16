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
7. [IPC & Engine Command Protocol](#-ipc--engine-command-protocol)
8. [Critical Engineering Rules](#-critical-engineering-rules)
9. [Hardware Acceleration & Dual-GPU Pipeline](#-hardware-acceleration--dual-gpu-pipeline)

---

## 🎯 Core Philosophy & Constraints

LiteWallpaper is engineered with extreme performance constraints that differentiate it from existing wallpaper engines:

| Constraint | Target / Rule | Rationale |
|:---|:---|:---|
| **CPU Usage** | **< 1.0%** (0.0% when idle/locked) | Adaptive QPC frame pacing; hardware-accelerated video decoding |
| **RAM Usage** | **< 15 MB** (typically 4.4–12 MB) | Native C++20 + mimalloc; zero web runtime overhead (no Electron/Chromium/WPF) |
| **GPU Power Saving**| **Deep Sleep for dGPU (0% NVIDIA load)** | Automatic smart routing to Integrated GPU (Intel UHD / AMD APU) |
| **Compatibility** | **Windows 7 SP1+ → Windows 11** | Direct3D 11 with FLIP_DISCARD on Win10/11 and DISCARD fallback on Win7 |
| **Process Model** | **Unified Single-Executable Architecture** | Zero IPC latency; UI resources freed on hide-to-tray |

---

## 🏗️ High-Level Architecture

LiteWallpaper is built as a unified native binary (`LiteWallpaper.exe`) combining an ultra-low-overhead resident render loop and an on-demand Dear ImGui control panel:

```
┌─────────────────────────────────────────────────────────────┐
│                    LiteWallpaper.exe                        │
├─────────────────────────────────────────────────────────────┤
│  1. Background Desktop Renderer Loop                        │
│     - Memory: ~4.4 MB - 12 MB RAM | CPU: < 1.0%             │
│     - Injects into desktop canvas (WorkerW 0x052C)          │
│     - D3D11VA Hardware Accelerated Video Decoding           │
│     - Low-overhead WASAPI Shared Audio Output               │
│     - Power Governor (Auto-pauses on game / lock / battery) │
│     - System Tray Icon & Local Pipe IPC Server              │
│                                                             │
│  2. Integrated Control Panel (On-Demand)                    │
│     - Dear ImGui (Dark Modern UI) with FontAwesome icons    │
│     - Real-Time Hardware & Telemetry Monitor                │
│     - Fluid Responsive Video Gallery & Multi-Monitor Target │
│     - Built-in 1080p GPU Video Optimizer & Downscaler       │
│     - Instantaneous (0ms) Tray Hide / Restore Lifecycle     │
└─────────────────────────────────────────────────────────────┘
```

---

## 📂 Component Breakdown

```
src/
├── main.cpp                         # Daemon main loop, window class, & message dispatcher
├── app.rc                           # Windows resource script (Embedded multi-res icon)
│
├── core/                            # Platform-agnostic core logic
│   ├── config.h / .cpp              # JSON configuration load/save (nlohmann_json)
│   ├── engine_state.h / .cpp        # Thread-safe atomic shared engine state
│   ├── playback_clock.h / .cpp      # QPC-driven adaptive frame pacing clock
│   ├── ipc_server.h / .cpp          # Windows Named Pipe server & async client
│   ├── ring_buffer.h                # Lock-free SPSC packet ring buffer
│   └── video_optimizer.h / .cpp     # Multi-threaded GPU video pre-scaler / downscaler
│
├── decoder/                         # Media playback & hardware acceleration
│   ├── video_decoder.h              # Abstract video decoder interface
│   ├── ffmpeg_hw_decoder.h / .cpp   # FFmpeg D3D11VA hardware video decoder
│   └── audio_player.h / .cpp        # Low-latency WASAPI shared audio streamer
│
├── platform/win32/                  # Windows OS integration layer
│   ├── desktop_injector.h / .cpp    # WorkerW desktop canvas injector (0x052C)
│   ├── d3d11_presenter.h / .cpp     # Direct3D 11 device, swapchain & NV12 shaders
│   ├── power_governor.h / .cpp      # Auto-pause heuristics (fullscreen/lock/battery)
│   ├── lockscreen_manager.h / .cpp  # Lock screen snapshot capture & staging
│   ├── hardware_info.h / .cpp       # Multi-GPU / multi-monitor hardware detector
│   └── tray_icon.h / .cpp           # Shell_NotifyIconW & tray popup menu
│
├── ui/                              # User Interface
│   ├── settings_app.h / .cpp        # Responsive Dear ImGui settings application
│   └── IconsFontAwesome6.h          # FontAwesome 6 icon glyph definitions
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
   - Header files in `third_party/ffmpeg/include`
   - Import libraries in `third_party/ffmpeg/lib`
   - Runtime DLLs in `third_party/ffmpeg/bin`

---

## 🔨 Building from Source

### Step 1: Install Dependencies via vcpkg
In the project root directory (manifest mode will read `vcpkg.json` automatically):

```powershell
# Using vcpkg instance:
vcpkg install nlohmann-json:x64-windows mimalloc:x64-windows imgui[docking-experimental,win32-binding,dx11-binding]:x64-windows
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
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows

# Build in Release configuration
cmake --build build --config Release
```

Output executable will be located at:
- `build/Release/LiteWallpaper.exe`

---

## 📦 Packaging Portable Distribution

To bundle the executable, runtime DLLs, assets, and shaders for release:

```powershell
$dist = "dist/LiteWallpaper-Portable"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
New-Item -ItemType Directory -Force -Path "$dist/assets" | Out-Null

Copy-Item "build/Release/LiteWallpaper.exe" -Destination $dist
Copy-Item "third_party/ffmpeg/bin/*.dll" -Destination $dist
Copy-Item "build/vcpkg_installed/x64-windows/bin/*.dll" -Destination $dist
Copy-Item "assets/*" -Destination "$dist/assets"
Copy-Item "LICENSE" -Destination $dist
Copy-Item "README.md" -Destination $dist

# Compress to Zip
Compress-Archive -Path "$dist/*" -DestinationPath "dist/LiteWallpaper-Portable-win64.zip" -Force
```

---

## 🔌 IPC & Engine Command Protocol

The engine provides a local Windows Named Pipe interface for single-instance activation and IPC control:
- **Pipe Address**: `\\.\pipe\LiteWallpaper`
- **Format**: JSON-RPC over message pipe (`PIPE_TYPE_MESSAGE`)

### Supported Commands

| Command | Payload | Description |
| :--- | :--- | :--- |
| `open_settings` | `{ "cmd": "open_settings" }` | Restores and brings Control Panel window to front |
| `set_wallpaper` | `{ "cmd": "set_wallpaper", "path": "...", "action": "wallpaper" }` | Applies video as desktop wallpaper |
| `set_render_device` | `{ "cmd": "set_render_device", "gpu_index": 1 }` | Switches rendering between GPU 1, GPU 2, or CPU (-1) |
| `set_target_displays`| `{ "cmd": "set_target_displays", "displays": [0, 1] }` | Directs wallpaper rendering to specific monitors |
| `set_scaling` | `{ "cmd": "set_scaling", "mode": 0 }` | Sets scaling mode (0=Cover, 1=Fit, 2=Stretch) |
| `set_fps` | `{ "cmd": "set_fps", "fps": 30 }` | Sets target render frame rate |
| `set_volume` | `{ "cmd": "set_volume", "volume": 0.5 }` | Adjusts master playback volume |
| `pause` / `resume` / `stop` | `{ "cmd": "pause" }` | Controls playback state and restores native desktop on stop |

---

## ⚡ Hardware Acceleration & Dual-GPU Pipeline

LiteWallpaper employs a specialized DirectX 11 rendering pipeline designed for minimal power and memory consumption:

1. **Integrated GPU (iGPU) Preference**:
   * When multiple GPUs are detected (e.g. Intel UHD Graphics + NVIDIA RTX), the default hardware device routes to the iGPU (`gpu_index = 1`).
   * This allows discrete gaming GPUs to remain in **0% power-saving sleep state** while the desktop wallpaper animates smoothly.
2. **Zero-Copy NV12 D3D11VA Decoding**:
   * Decoded video frames remain as hardware texture surfaces in GPU memory.
   * Pixel shaders perform color space conversion (BT.709 YUV $\rightarrow$ RGB) on the GPU without intermediate CPU memory copying.
3. **Double-Buffered Swapchain**:
   * Uses `DXGI_SWAP_EFFECT_FLIP_DISCARD` on Windows 10/11 and `DXGI_SWAP_EFFECT_DISCARD` on Windows 7/8.

---

## ⚠️ Critical Engineering Rules

When contributing code to LiteWallpaper, **always adhere to these principles**:

1. **Preserve Ultra-Low RAM Footprint**: Never introduce heavy libraries, web runtimes, or unmanaged thread pools into hot render loops.
2. **Thread Safety**: All state shared between the daemon render loop and the UI thread must use `std::atomic` or `g_decoder_mutex` protection.
3. **Non-Destructive UI Hiding**: Control Panel hiding must preserve smooth state without memory leaks.
4. **Desktop Cleanup on Stop**: Detaching the wallpaper must trigger desktop refresh (`SystemParametersInfoW(SPI_SETDESKWALLPAPER, ...)` + `InvalidateRect`) to restore native Windows wallpaper cleanly.

---

## 📄 License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.
