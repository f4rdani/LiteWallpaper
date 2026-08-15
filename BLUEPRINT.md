# 🖼️ LiteWallpaper — Complete Implementation Blueprint

> **Dokumen ini adalah panduan lengkap untuk AI agent yang akan mengimplementasikan proyek LiteWallpaper dari nol.**
> Setiap file, function signature, API call, dan keputusan teknis sudah didefinisikan secara eksplisit.
> Agent tidak perlu melakukan research tambahan — semua informasi ada di sini.

---

## 📋 Project Summary

| Item | Value |
|:---|:---|
| **Nama** | LiteWallpaper |
| **Deskripsi** | Lightweight animated video wallpaper engine untuk desktop & lock screen |
| **Lisensi** | GPL v3 |
| **Bahasa** | C++ (C++20 standard) |
| **Target OS** | Windows 7 SP1+ (prioritas), Linux, macOS (nanti) |
| **Fitur Utama** | Video wallpaper (loop), audio (muted default), multi-monitor, lock screen snapshot |
| **Constraint Performa** | ≤ 2% CPU, ≤ 45 MB RAM |
| **UI Framework** | Dear ImGui (untuk Settings app terpisah) |
| **Distribusi** | Portable (.zip, tanpa installer) |
| **Online Gallery** | Tidak ada — hanya file lokal |
| **Live2D** | Tidak untuk sekarang — fokus video dulu |
| **Project Root** | `c:\laragon\laragon\otherproject\litewallpaper` |

---

## 📁 Struktur File Lengkap Yang Harus Dibuat

```
litewallpaper/
├── CMakeLists.txt                          # Root CMake build file
├── vcpkg.json                              # vcpkg dependency manifest
├── LICENSE                                 # GPL v3 full text
├── README.md                               # Project documentation
│
├── src/
│   ├── main.cpp                            # Daemon entry point
│   │
│   ├── core/
│   │   ├── config.h                        # Configuration manager header
│   │   ├── config.cpp                      # Configuration manager implementation
│   │   ├── playback_clock.h                # Adaptive frame-rate clock header
│   │   ├── playback_clock.cpp              # Adaptive frame-rate clock implementation
│   │   ├── ipc_server.h                    # IPC server header
│   │   ├── ipc_server.cpp                  # IPC server implementation (Named Pipe)
│   │   └── ring_buffer.h                   # Lock-free ring buffer (header-only)
│   │
│   ├── decoder/
│   │   ├── video_decoder.h                 # Abstract decoder interface
│   │   ├── ffmpeg_hw_decoder.h             # FFmpeg HW decoder header
│   │   ├── ffmpeg_hw_decoder.cpp           # FFmpeg HW decoder implementation
│   │   └── audio_player.h / audio_player.cpp  # WASAPI audio output (optional)
│   │
│   ├── platform/
│   │   └── win32/
│   │       ├── desktop_injector.h          # WorkerW injection header
│   │       ├── desktop_injector.cpp        # WorkerW injection implementation
│   │       ├── d3d11_presenter.h           # D3D11 swap chain & rendering header
│   │       ├── d3d11_presenter.cpp         # D3D11 swap chain & rendering implementation
│   │       ├── power_governor.h            # Auto-pause heuristics header
│   │       ├── power_governor.cpp          # Auto-pause heuristics implementation
│   │       ├── lockscreen_manager.h        # Lock screen static frame header
│   │       ├── lockscreen_manager.cpp      # Lock screen static frame implementation
│   │       ├── tray_icon.h                 # System tray icon header
│   │       └── tray_icon.cpp               # System tray icon implementation
│   │
│   └── ui/
│       ├── settings_app.h                  # Dear ImGui settings app header
│       └── settings_app.cpp                # Dear ImGui settings app implementation
│
├── shaders/
│   ├── nv12_to_rgb.hlsl                    # NV12 → RGB pixel shader (video frame)
│   └── fullscreen_quad.hlsl                # Fullscreen vertex shader
│
├── assets/
│   ├── icon.ico                            # App icon (harus buat/generate)
│   └── default_config.json                 # Default configuration template
│
└── third_party/
    └── README.md                           # Instructions for fetching FFmpeg & imgui
```

---

## 🔧 LANGKAH 1: Build System Setup

### 1.1 `CMakeLists.txt` (Root)

```cmake
cmake_minimum_required(VERSION 3.20)
project(LiteWallpaper VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# === Performance flags ===
if(MSVC)
    # Disable C++ exceptions and RTTI to reduce memory footprint
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    add_compile_options(/O2 /Oi /Gy /GL /GR- /std:c++20)
    add_link_options(/LTCG /OPT:REF /OPT:ICF)
else()
    add_compile_options(-O3 -flto -fno-exceptions -fno-rtti -std=c++20)
endif()

# === Dependencies via vcpkg ===
find_package(nlohmann_json CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

# === FFmpeg (manual find) ===
# FFmpeg harus diinstall terpisah atau di-bundle di third_party/ffmpeg
# Agent harus download FFmpeg development libraries (shared/static)
# Untuk Windows: https://github.com/BtbN/FFmpeg-Builds/releases
# Pilih: ffmpeg-n7.1-latest-win64-gpl-shared-7.1.zip (atau versi terbaru)
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg root directory")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_DIR}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_DIR}/lib")

# === Daemon executable ===
add_executable(litewp_daemon WIN32
    src/main.cpp
    src/core/config.cpp
    src/core/playback_clock.cpp
    src/core/ipc_server.cpp
    src/decoder/ffmpeg_hw_decoder.cpp
    src/decoder/audio_player.cpp
    src/platform/win32/desktop_injector.cpp
    src/platform/win32/d3d11_presenter.cpp
    src/platform/win32/power_governor.cpp
    src/platform/win32/lockscreen_manager.cpp
    src/platform/win32/tray_icon.cpp
)

target_include_directories(litewp_daemon PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIR}
)

target_link_directories(litewp_daemon PRIVATE ${FFMPEG_LIB_DIR})

target_link_libraries(litewp_daemon PRIVATE
    nlohmann_json::nlohmann_json
    mimalloc
    # FFmpeg libs
    avcodec avformat avutil swscale swresample
    # Windows system libs
    d3d11 dxgi d3dcompiler
    dwmapi user32 gdi32 shell32 ole32 uuid
    winmm  # for audio timing
)

# === Settings UI executable ===
add_executable(litewp_settings WIN32
    src/ui/settings_app.cpp
    src/core/config.cpp
    src/core/ipc_server.cpp  # IPC client mode
)

target_include_directories(litewp_settings PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(litewp_settings PRIVATE
    nlohmann_json::nlohmann_json
    imgui::imgui
    d3d11 dxgi d3dcompiler
    user32 gdi32 shell32 ole32 dwmapi
)
```

### 1.2 `vcpkg.json`

```json
{
  "name": "litewallpaper",
  "version": "1.0.0",
  "dependencies": [
    "nlohmann-json",
    "mimalloc",
    {
      "name": "imgui",
      "features": ["docking-experimental", "win32-binding", "dx11-binding"]
    }
  ]
}
```

### 1.3 Setup Instructions untuk Agent

1. **Install vcpkg** (jika belum ada):
   ```powershell
   git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   ```

2. **Install dependencies**:
   ```powershell
   cd c:\laragon\laragon\otherproject\litewallpaper
   C:\vcpkg\vcpkg install nlohmann-json:x64-windows mimalloc:x64-windows imgui[docking-experimental,win32-binding,dx11-binding]:x64-windows
   ```

3. **Download FFmpeg dev libraries**:
   ```powershell
   # Download dari https://github.com/BtbN/FFmpeg-Builds/releases
   # Extract ke third_party/ffmpeg/ sehingga structurenya:
   # third_party/ffmpeg/include/libavcodec/avcodec.h
   # third_party/ffmpeg/lib/avcodec.lib
   # third_party/ffmpeg/bin/avcodec-61.dll (atau versi terbaru)
   ```

4. **Build**:
   ```powershell
   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
   cmake --build build --config Release
   ```

---

## 🔧 LANGKAH 2: Core Components

### 2.1 `src/core/config.h`

```cpp
#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace litewp {

struct MonitorWallpaper {
    std::string monitor_id;      // Windows monitor device path
    std::string video_path;      // Absolute path to video file
    float volume = 0.0f;         // Audio volume (0.0 = muted, 1.0 = max)
    bool audio_enabled = false;  // Master audio toggle
};

struct AppConfig {
    std::vector<MonitorWallpaper> wallpapers;
    int target_fps = 30;         // Default 30 FPS to save power
    int idle_fps = 15;           // FPS when no interaction
    bool pause_on_fullscreen = true;
    bool pause_on_battery = false; // true = pause, false = reduce FPS
    int battery_fps = 15;
    bool pause_on_lock = true;
    bool update_lockscreen = true; // Capture frame for lock screen
    bool run_on_startup = false;
    std::string config_path;     // Path to this config file
};

class Config {
public:
    // Load from %APPDATA%/LiteWallpaper/config.json
    // If file doesn't exist, create with defaults
    bool Load();
    
    // Save current config to file
    bool Save();
    
    // Get/Set config
    AppConfig& Get();
    const AppConfig& Get() const;
    
    // Get config file path
    static std::string GetConfigDir();
    static std::string GetConfigFilePath();

private:
    AppConfig m_config;
};

// JSON serialization (implement using nlohmann MACRO or manual to_json/from_json)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MonitorWallpaper, monitor_id, video_path, volume, audio_enabled)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppConfig, wallpapers, target_fps, idle_fps, 
    pause_on_fullscreen, pause_on_battery, battery_fps, pause_on_lock,
    update_lockscreen, run_on_startup)

} // namespace litewp
```

**Implementasi `config.cpp`:**
- `GetConfigDir()`: Return `%APPDATA%\LiteWallpaper\` menggunakan `SHGetFolderPathW(CSIDL_APPDATA)` + `\LiteWallpaper\`
- `GetConfigFilePath()`: Return `GetConfigDir() + "config.json"`
- `Load()`: Baca file JSON, parse dengan `nlohmann::json`, isi `m_config`. Jika file tidak ada, buat directory dan tulis default config.
- `Save()`: Serialize `m_config` ke JSON, tulis ke file.

---

### 2.2 `src/core/playback_clock.h`

```cpp
#pragma once
#include <cstdint>

namespace litewp {

// Adaptive frame-rate clock yang menghitung kapan frame berikutnya harus di-render
class PlaybackClock {
public:
    void SetTargetFPS(int fps);     // Set target frame rate
    int  GetTargetFPS() const;
    
    // Call setiap iteration main loop.
    // Return true jika sudah waktunya render frame baru.
    // Return false jika belum waktunya (caller harus Sleep/yield).
    bool ShouldRenderFrame();
    
    // Get berapa ms yang harus di-Sleep sampai frame berikutnya
    // Gunakan ini untuk WaitForSingleObject / SleepEx agar CPU idle
    uint32_t GetSleepDurationMs() const;
    
    // Reset clock (panggil saat resume dari pause)
    void Reset();

private:
    int m_target_fps = 30;
    int64_t m_frame_interval_us = 33333; // microseconds per frame (1/30s)
    int64_t m_last_frame_time = 0;       // timestamp in microseconds
    
    // Gunakan QueryPerformanceCounter/QueryPerformanceFrequency di Windows
    static int64_t GetCurrentTimeMicros();
};

} // namespace litewp
```

**Implementasi detail:**
- `GetCurrentTimeMicros()`: Gunakan `QueryPerformanceCounter` + `QueryPerformanceFrequency` untuk precision timing.
- `ShouldRenderFrame()`: Hitung elapsed = now - last_frame. Jika elapsed >= frame_interval, set last_frame = now, return true.
- `GetSleepDurationMs()`: Return `max(0, (frame_interval - elapsed) / 1000)`. Ini digunakan oleh main loop untuk `Sleep()` agar CPU tidak spin-wait.

---

### 2.3 `src/core/ring_buffer.h` (Header-Only)

```cpp
#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>

namespace litewp {

// Lock-free single-producer single-consumer ring buffer untuk compressed video packets
// Kapasitas: 4 slots, masing-masing max 256 KB (compressed H.264/HEVC frame ~30-150 KB)
template<size_t SlotSize = 262144, size_t SlotCount = 4>
class RingBuffer {
public:
    struct Packet {
        uint8_t data[SlotSize];
        size_t  size = 0;
        int64_t pts = 0;   // presentation timestamp
        bool    is_key = false;
    };

    // Producer: tulis packet ke buffer. Return false jika buffer penuh.
    bool Push(const uint8_t* data, size_t size, int64_t pts, bool is_key) {
        size_t write = m_write.load(std::memory_order_relaxed);
        size_t next = (write + 1) % SlotCount;
        if (next == m_read.load(std::memory_order_acquire)) return false; // full
        
        auto& slot = m_slots[write];
        std::memcpy(slot.data, data, (size < SlotSize) ? size : SlotSize);
        slot.size = size;
        slot.pts = pts;
        slot.is_key = is_key;
        
        m_write.store(next, std::memory_order_release);
        return true;
    }

    // Consumer: baca packet dari buffer. Return nullptr jika buffer kosong.
    const Packet* Peek() const {
        size_t read = m_read.load(std::memory_order_relaxed);
        if (read == m_write.load(std::memory_order_acquire)) return nullptr; // empty
        return &m_slots[read];
    }

    // Consumer: hapus packet yang sudah di-consume
    void Pop() {
        size_t read = m_read.load(std::memory_order_relaxed);
        m_read.store((read + 1) % SlotCount, std::memory_order_release);
    }

    bool IsEmpty() const {
        return m_read.load(std::memory_order_acquire) == m_write.load(std::memory_order_acquire);
    }

    bool IsFull() const {
        size_t next = (m_write.load(std::memory_order_acquire) + 1) % SlotCount;
        return next == m_read.load(std::memory_order_acquire);
    }

    void Clear() {
        m_read.store(0, std::memory_order_release);
        m_write.store(0, std::memory_order_release);
    }

private:
    std::array<Packet, SlotCount> m_slots;
    std::atomic<size_t> m_read{0};
    std::atomic<size_t> m_write{0};
};

} // namespace litewp
```

---

### 2.4 `src/core/ipc_server.h` & `ipc_server.cpp`

**Purpose:** Daemon ↔ Settings UI berkomunikasi via Named Pipe. Daemon = server. Settings UI = client.

```cpp
#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>

namespace litewp {

// Pipe name: \\.\pipe\LiteWallpaper
constexpr const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\LiteWallpaper";

// JSON-RPC style commands yang didukung:
// Request:  {"cmd": "set_wallpaper", "monitor": "...", "path": "C:/video.mp4"}
// Request:  {"cmd": "get_status"}
// Request:  {"cmd": "pause"}
// Request:  {"cmd": "resume"}
// Request:  {"cmd": "set_volume", "volume": 0.5}
// Request:  {"cmd": "set_fps", "fps": 30}
// Request:  {"cmd": "reload_config"}
// Response: {"ok": true, "data": {...}}

using IpcCallback = std::function<std::string(const std::string& request_json)>;

class IpcServer {
public:
    // Start listening di background thread
    // callback dipanggil setiap ada request masuk, harus return response JSON string
    void Start(IpcCallback callback);
    void Stop();
    bool IsRunning() const;

private:
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    IpcCallback m_callback;
    
    void ServerLoop();
};

// Client-side (dipakai oleh Settings UI)
class IpcClient {
public:
    // Connect ke daemon pipe
    bool Connect();
    void Disconnect();
    bool IsConnected() const;
    
    // Send request, wait for response (synchronous)
    std::string SendRequest(const std::string& request_json);

private:
    void* m_pipe_handle = nullptr; // HANDLE
};

} // namespace litewp
```

**Implementasi detail (ipc_server.cpp):**

**Server:**
1. `CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0, NULL)`
2. Loop: `ConnectNamedPipe()` → `ReadFile()` → parse JSON → call `m_callback` → `WriteFile(response)` → `DisconnectNamedPipe()` → kembali ke `ConnectNamedPipe()`
3. Untuk stop: set `m_running = false`, lalu `CancelIoEx` atau buat dummy connection ke pipe.

**Client:**
1. `CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL)`
2. `WriteFile(request)` → `ReadFile(response)` → return response string

---

## 🔧 LANGKAH 3: Video Decoder (FFmpeg Hardware Accelerated)

### 3.1 `src/decoder/video_decoder.h` (Abstract Interface)

```cpp
#pragma once
#include <cstdint>

struct ID3D11Texture2D;   // Forward declare
struct ID3D11Device;       // Forward declare

namespace litewp {

struct VideoFrame {
    ID3D11Texture2D* texture = nullptr;  // GPU texture (NV12 format)
    int texture_index = 0;               // Array index dalam texture array
    int64_t pts = 0;                     // Presentation timestamp (microseconds)
    int width = 0;
    int height = 0;
};

struct VideoInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double duration_seconds = 0.0;
    bool has_audio = false;
    std::string codec_name;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    
    // Open video file. d3d_device dipakai untuk hardware decoding.
    virtual bool Open(const char* path, ID3D11Device* d3d_device) = 0;
    
    // Decode frame berikutnya. Return true jika ada frame baru.
    // frame.texture berisi decoded NV12 texture di GPU VRAM.
    virtual bool DecodeNextFrame(VideoFrame& frame) = 0;
    
    // Seek ke awal video (untuk seamless looping)
    virtual void SeekToStart() = 0;
    
    // Get video info
    virtual VideoInfo GetInfo() const = 0;
    
    // Has audio stream?
    virtual bool HasAudio() const = 0;
    
    // Decode next audio samples (interleaved float, stereo)
    // Return jumlah samples yang di-decode. 0 = tidak ada audio baru.
    virtual int DecodeAudioSamples(float* buffer, int max_samples) = 0;
    
    virtual void Close() = 0;
};

} // namespace litewp
```

### 3.2 `src/decoder/ffmpeg_hw_decoder.h` & `ffmpeg_hw_decoder.cpp`

**Ini adalah komponen paling kritikal. Berikut detail implementasinya:**

**Headers yang dibutuhkan:**
```cpp
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#include <d3d11.h>
```

**Class definition:**
```cpp
class FFmpegHWDecoder : public IVideoDecoder {
public:
    bool Open(const char* path, ID3D11Device* d3d_device) override;
    bool DecodeNextFrame(VideoFrame& frame) override;
    void SeekToStart() override;
    VideoInfo GetInfo() const override;
    bool HasAudio() const override;
    int DecodeAudioSamples(float* buffer, int max_samples) override;
    void Close() override;

private:
    AVFormatContext* m_fmt_ctx = nullptr;
    AVCodecContext* m_video_codec_ctx = nullptr;
    AVCodecContext* m_audio_codec_ctx = nullptr;
    AVBufferRef* m_hw_device_ctx = nullptr;
    AVFrame* m_hw_frame = nullptr;
    AVPacket* m_packet = nullptr;
    SwrContext* m_swr_ctx = nullptr;  // Audio resampler
    
    int m_video_stream_idx = -1;
    int m_audio_stream_idx = -1;
    
    ID3D11Device* m_d3d_device = nullptr;
    
    bool InitHWDecoder(ID3D11Device* device);
};
```

**Langkah implementasi `Open()`:**
1. `avformat_open_input(&m_fmt_ctx, path, NULL, NULL)`
2. `avformat_find_stream_info(m_fmt_ctx, NULL)`
3. Cari video stream: loop `m_fmt_ctx->streams[]`, cari `AVMEDIA_TYPE_VIDEO` → simpan index ke `m_video_stream_idx`
4. Cari audio stream (opsional): cari `AVMEDIA_TYPE_AUDIO` → simpan ke `m_audio_stream_idx`
5. **Setup D3D11VA hardware context:**
   ```cpp
   // Buat AVBufferRef untuk D3D11VA device
   AVBufferRef* hw_device_ctx = nullptr;
   
   // Method: Buat hw device context yang menggunakan D3D11 device kita
   av_hwdevice_ctx_alloc(&hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA);
   AVHWDeviceContext* device_ctx = (AVHWDeviceContext*)hw_device_ctx->data;
   AVD3D11VADeviceContext* d3d11_ctx = (AVD3D11VADeviceContext*)device_ctx->hwctx;
   
   // Set D3D11 device kita ke FFmpeg
   d3d11_ctx->device = d3d_device;
   d3d_device->AddRef(); // FFmpeg akan Release saat cleanup
   
   av_hwdevice_ctx_init(hw_device_ctx);
   m_hw_device_ctx = hw_device_ctx;
   ```
6. **Setup video codec:**
   ```cpp
   const AVCodec* decoder = avcodec_find_decoder(m_fmt_ctx->streams[m_video_stream_idx]->codecpar->codec_id);
   m_video_codec_ctx = avcodec_alloc_context3(decoder);
   avcodec_parameters_to_context(m_video_codec_ctx, m_fmt_ctx->streams[m_video_stream_idx]->codecpar);
   
   // Assign hardware device context
   m_video_codec_ctx->hw_device_ctx = av_buffer_ref(m_hw_device_ctx);
   
   // Set format callback untuk hardware frames
   m_video_codec_ctx->get_format = [](AVCodecContext* ctx, const AVPixelFormat* pix_fmts) -> AVPixelFormat {
       for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
           if (*p == AV_PIX_FMT_D3D11) return *p;
       }
       return AV_PIX_FMT_NONE; // Fallback: software decode
   };
   
   avcodec_open2(m_video_codec_ctx, decoder, NULL);
   ```
7. **Setup audio codec (jika ada):**
   ```cpp
   if (m_audio_stream_idx >= 0) {
       const AVCodec* audio_dec = avcodec_find_decoder(m_fmt_ctx->streams[m_audio_stream_idx]->codecpar->codec_id);
       m_audio_codec_ctx = avcodec_alloc_context3(audio_dec);
       avcodec_parameters_to_context(m_audio_codec_ctx, m_fmt_ctx->streams[m_audio_stream_idx]->codecpar);
       avcodec_open2(m_audio_codec_ctx, audio_dec, NULL);
       
       // Setup SwrContext untuk resample ke float stereo 44100Hz
       m_swr_ctx = swr_alloc_set_opts(NULL,
           AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, 44100,           // output
           m_audio_codec_ctx->ch_layout, m_audio_codec_ctx->sample_fmt, m_audio_codec_ctx->sample_rate, // input
           0, NULL);
       swr_init(m_swr_ctx);
   }
   ```
8. Allocate: `m_hw_frame = av_frame_alloc(); m_packet = av_packet_alloc();`

**Langkah implementasi `DecodeNextFrame()`:**
1. Loop `av_read_frame(m_fmt_ctx, m_packet)` sampai dapat video packet
2. `avcodec_send_packet(m_video_codec_ctx, m_packet)` → `avcodec_receive_frame(m_video_codec_ctx, m_hw_frame)`
3. **Extract D3D11 texture dari decoded frame (ZERO COPY!):**
   ```cpp
   // m_hw_frame->data[0] = ID3D11Texture2D* (NV12 format, di GPU VRAM)
   // m_hw_frame->data[1] = intptr_t array_index
   frame.texture = (ID3D11Texture2D*)m_hw_frame->data[0];
   frame.texture_index = (int)(intptr_t)m_hw_frame->data[1];
   frame.width = m_hw_frame->width;
   frame.height = m_hw_frame->height;
   frame.pts = m_hw_frame->pts;
   ```
4. Jika `av_read_frame` return `AVERROR_EOF`, panggil `SeekToStart()` untuk looping.

**Langkah implementasi `SeekToStart()`:**
```cpp
av_seek_frame(m_fmt_ctx, m_video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
avcodec_flush_buffers(m_video_codec_ctx);
if (m_audio_codec_ctx) avcodec_flush_buffers(m_audio_codec_ctx);
```

**Langkah implementasi `Close()`:**
- `av_frame_free`, `av_packet_free`, `avcodec_free_context`, `avformat_close_input`, `av_buffer_unref`, `swr_free` — cleanup semua resource.

---

### 3.3 `src/decoder/audio_player.h` & `audio_player.cpp`

**Purpose:** Output audio via WASAPI (Windows Audio Session API) — support Win7+.

```cpp
#pragma once
#include <atomic>
#include <thread>

namespace litewp {

class AudioPlayer {
public:
    // Initialize WASAPI shared-mode audio output (44100 Hz, stereo, float32)
    bool Init();
    
    // Push audio samples ke internal buffer (called by decoder thread)
    void PushSamples(const float* data, int num_samples);
    
    // Set volume (0.0 to 1.0)
    void SetVolume(float volume);
    float GetVolume() const;
    
    // Mute/unmute
    void SetMuted(bool muted);
    bool IsMuted() const;
    
    void Stop();

private:
    std::atomic<float> m_volume{0.0f};
    std::atomic<bool> m_muted{true};  // Muted by default!
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    
    // Internal: WASAPI handles (IAudioClient*, IAudioRenderClient*, etc.)
    // Implement using COM interfaces
    void AudioThread();
};

} // namespace litewp
```

**Implementasi WASAPI:**
1. `CoInitializeEx(NULL, COINIT_MULTITHREADED)`
2. `CoCreateInstance(CLSID_MMDeviceEnumerator)` → `IMMDeviceEnumerator`
3. `enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)`
4. `device->Activate(IID_IAudioClient, ...)` → `IAudioClient`
5. `audioClient->GetMixFormat(&waveFormat)` — gunakan format system default
6. `audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, ...)`
7. `audioClient->GetService(IID_IAudioRenderClient, ...)` → `IAudioRenderClient`
8. Loop thread: `renderClient->GetBuffer()` → copy samples dengan volume scaling → `ReleaseBuffer()`
9. Volume scaling: `sample * m_volume * (m_muted ? 0.0f : 1.0f)`

> **PENTING:** Jika `m_muted == true && m_volume == 0`, JANGAN initialize WASAPI sama sekali untuk menghemat RAM (~8-15 MB). Hanya init saat user unmute.

---

## 🔧 LANGKAH 4: Platform Layer — Windows

### 4.1 `src/platform/win32/desktop_injector.h` & `desktop_injector.cpp`

**Ini adalah teknik inti: menyisipkan window di belakang desktop icons.**

```cpp
#pragma once
#include <windows.h>
#include <vector>

namespace litewp {

struct MonitorInfo {
    HMONITOR handle;
    RECT rect;             // Monitor rectangle dalam virtual screen coords
    std::wstring device_id; // Monitor device path (untuk IDesktopWallpaper compatibility)
    bool is_primary;
};

class DesktopInjector {
public:
    // Dapatkan WorkerW window handle dan attach render window
    // renderHwnd = window yang akan dijadikan wallpaper
    bool Attach(HWND renderHwnd);
    
    // Lepaskan window dari desktop
    void Detach();
    
    // Re-attach (dipanggil saat Explorer restart)
    bool Reattach(HWND renderHwnd);
    
    // Cek apakah masih valid
    bool IsAttached() const;
    
    // Get WorkerW handle
    HWND GetWorkerW() const;
    
    // Enumerate monitors
    static std::vector<MonitorInfo> EnumerateMonitors();
    
    // Register untuk Explorer restart notification
    void RegisterExplorerRestart(HWND messageHwnd);

private:
    HWND m_workerw = nullptr;
    HWND m_render_hwnd = nullptr;
    UINT m_taskbar_restart_msg = 0;
    
    // Internal: find WorkerW
    static HWND FindDesktopWorkerW();
};

} // namespace litewp
```

**Implementasi `FindDesktopWorkerW()`** (KRITIKAL — harus persis seperti ini):
```cpp
HWND DesktopInjector::FindDesktopWorkerW() {
    // 1. Temukan Progman
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;

    // 2. Kirim undocumented message 0x052C ke Progman
    //    Ini memaksa Explorer membuat WorkerW di belakang desktop icons
    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &result);

    // 3. Cari WorkerW yang berada di belakang SHELLDLL_DefView
    HWND workerw = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        HWND defview = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defview != nullptr) {
            // WorkerW target ada SETELAH window yang mengandung SHELLDLL_DefView
            HWND* pResult = reinterpret_cast<HWND*>(lparam);
            *pResult = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&workerw));

    return workerw;
}
```

**Implementasi `Attach()`:**
```cpp
bool DesktopInjector::Attach(HWND renderHwnd) {
    m_workerw = FindDesktopWorkerW();
    if (!m_workerw) return false;
    
    m_render_hwnd = renderHwnd;
    
    // Hapus border, caption — jadikan child window
    LONG_PTR style = GetWindowLongPtrW(renderHwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_CHILD;
    SetWindowLongPtrW(renderHwnd, GWL_STYLE, style);
    
    // Set WorkerW sebagai parent
    SetParent(renderHwnd, m_workerw);
    
    // Resize ke ukuran virtual screen (semua monitor)
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    
    SetWindowPos(renderHwnd, nullptr, x, y, cx, cy,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    
    return true;
}
```

**Implementasi `EnumerateMonitors()`:**
```cpp
std::vector<MonitorInfo> DesktopInjector::EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hmon, HDC, LPRECT, LPARAM lparam) -> BOOL {
        auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hmon, &mi);
        
        MonitorInfo info;
        info.handle = hmon;
        info.rect = mi.rcMonitor;
        info.device_id = mi.szDevice;
        info.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        list->push_back(info);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}
```

**Explorer restart recovery:**
```cpp
void DesktopInjector::RegisterExplorerRestart(HWND messageHwnd) {
    // Register pesan "TaskbarCreated" — dikirim saat explorer.exe restart
    m_taskbar_restart_msg = RegisterWindowMessageW(L"TaskbarCreated");
    // Di WndProc, cek: if (msg == m_taskbar_restart_msg) { Reattach(...); }
}
```

---

### 4.2 `src/platform/win32/d3d11_presenter.h` & `d3d11_presenter.cpp`

**Purpose:** Membuat D3D11 device, swap chain, dan me-render video frames (NV12 textures) ke window.

```cpp
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>  // ComPtr

using Microsoft::WRL::ComPtr;

namespace litewp {

class D3D11Presenter {
public:
    // Initialize D3D11 device dan swap chain pada HWND target
    bool Init(HWND hwnd, int width, int height);
    
    // Render NV12 texture ke swap chain (dipanggil per frame)
    // nv12_texture: decoded video frame dari FFmpeg (NV12 format di GPU)
    // array_index: texture array index (dari AVFrame->data[1])
    void RenderFrame(ID3D11Texture2D* nv12_texture, int array_index);
    
    // Present frame ke layar
    void Present();
    
    // Resize swap chain (saat monitor resolution berubah)
    void Resize(int width, int height);
    
    // Get D3D11 device (dipakai oleh FFmpeg HW decoder)
    ID3D11Device* GetDevice() const;
    ID3D11DeviceContext* GetContext() const;
    
    void Cleanup();

private:
    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<IDXGISwapChain1>        m_swapchain;  // Gunakan IDXGISwapChain (bukan 1) untuk Win7
    ComPtr<ID3D11RenderTargetView> m_rtv;
    
    // NV12 → RGB conversion resources
    ComPtr<ID3D11PixelShader>      m_nv12_ps;
    ComPtr<ID3D11VertexShader>     m_fullscreen_vs;
    ComPtr<ID3D11SamplerState>     m_sampler;
    ComPtr<ID3D11ShaderResourceView> m_srv_y;   // Y plane view
    ComPtr<ID3D11ShaderResourceView> m_srv_uv;  // UV plane view
    
    int m_width = 0;
    int m_height = 0;
    
    bool CreateShaders();
    bool CreateSwapChain(HWND hwnd, int width, int height);
};

} // namespace litewp
```

**Implementasi `Init()`:**
```cpp
bool D3D11Presenter::Init(HWND hwnd, int width, int height) {
    m_width = width;
    m_height = height;
    
    // 1. Buat D3D11 device
    //    Feature level 11_0 untuk Win7 compatibility (dengan Platform Update)
    //    Feature level 9_3 fallback untuk GPU sangat tua
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
    };
    
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT; // PENTING: untuk D3D11VA
    #ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
    #endif
    
    D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        featureLevels, _countof(featureLevels), D3D11_SDK_VERSION,
        &m_device, nullptr, &m_context
    );
    
    // 2. Enable multithread protection (FFmpeg decode di thread lain)
    ComPtr<ID3D10Multithread> mt;
    m_device.As(&mt);
    mt->SetMultithreadProtected(TRUE);
    
    // 3. Buat swap chain
    CreateSwapChain(hwnd, width, height);
    
    // 4. Compile dan buat shaders
    CreateShaders();
    
    // 5. Buat sampler state
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT; // Bilinear filter
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    m_device->CreateSamplerState(&sd, &m_sampler);
    
    return true;
}
```

**Implementasi `CreateSwapChain()`:**
```cpp
bool D3D11Presenter::CreateSwapChain(HWND hwnd, int width, int height) {
    // Untuk Win7 compatibility: gunakan IDXGISwapChain (bukan DXGI 1.2 IDXGISwapChain1)
    ComPtr<IDXGIDevice> dxgiDevice;
    m_device.As(&dxgiDevice);
    
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    
    ComPtr<IDXGIFactory> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));
    
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate = {0, 0};  // Tidak lock ke refresh rate monitor
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc = {1, 0};
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; // Win7 compatible!
    //   Pada Win10+ bisa upgrade ke DXGI_SWAP_EFFECT_FLIP_DISCARD
    
    ComPtr<IDXGISwapChain> swapchain;
    factory->CreateSwapChain(m_device.Get(), &scd, &swapchain);
    swapchain.As(&m_swapchain);
    
    // Buat render target view
    ComPtr<ID3D11Texture2D> backbuffer;
    m_swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    m_device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_rtv);
    
    return true;
}
```

**Implementasi `RenderFrame()`:**
```cpp
void D3D11Presenter::RenderFrame(ID3D11Texture2D* nv12_texture, int array_index) {
    // 1. Buat SRV (Shader Resource View) untuk Y dan UV plane dari NV12 texture
    D3D11_TEXTURE2D_DESC texDesc;
    nv12_texture->GetDesc(&texDesc);
    
    // Y plane (luminance): DXGI_FORMAT_R8_UNORM, full resolution
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
    srvDescY.Format = DXGI_FORMAT_R8_UNORM;
    srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDescY.Texture2DArray.FirstArraySlice = array_index;
    srvDescY.Texture2DArray.ArraySize = 1;
    srvDescY.Texture2DArray.MipLevels = 1;
    m_device->CreateShaderResourceView(nv12_texture, &srvDescY, &m_srv_y);
    
    // UV plane (chrominance): DXGI_FORMAT_R8G8_UNORM, half resolution
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = {};
    srvDescUV.Format = DXGI_FORMAT_R8G8_UNORM;
    srvDescUV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDescUV.Texture2DArray.FirstArraySlice = array_index;
    srvDescUV.Texture2DArray.ArraySize = 1;
    srvDescUV.Texture2DArray.MipLevels = 1;
    m_device->CreateShaderResourceView(nv12_texture, &srvDescUV, &m_srv_uv);
    
    // 2. Set render target
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    
    // 3. Set viewport
    D3D11_VIEWPORT vp = {0, 0, (float)m_width, (float)m_height, 0, 1};
    m_context->RSSetViewports(1, &vp);
    
    // 4. Bind shaders dan textures
    m_context->VSSetShader(m_fullscreen_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_nv12_ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { m_srv_y.Get(), m_srv_uv.Get() };
    m_context->PSSetShaderResources(0, 2, srvs);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    
    // 5. Draw fullscreen quad (3 vertices, no vertex buffer needed)
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);
    m_context->Draw(3, 0);  // Fullscreen triangle trick (lihat shader di bawah)
    
    // 6. Cleanup SRVs untuk frame ini
    ID3D11ShaderResourceView* nullsrvs[] = { nullptr, nullptr };
    m_context->PSSetShaderResources(0, 2, nullsrvs);
}

void D3D11Presenter::Present() {
    m_swapchain->Present(1, 0); // VSync ON (1). Gunakan 0 untuk no-VSync.
}
```

---

### 4.3 Shader Files

#### `shaders/fullscreen_quad.hlsl` (Vertex Shader)

```hlsl
// Fullscreen triangle trick — no vertex buffer needed!
// Draw 3 vertices yang membentuk triangle besar menutupi seluruh screen
struct VS_OUT {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VS_OUT main(uint vertexID : SV_VertexID) {
    VS_OUT o;
    // Generate fullscreen triangle dari vertex ID (0, 1, 2)
    o.uv = float2((vertexID << 1) & 2, vertexID & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
```

#### `shaders/nv12_to_rgb.hlsl` (Pixel Shader)

```hlsl
// Konversi NV12 (YUV 4:2:0) ke RGB
// Y plane: texture0 (R8_UNORM, full res)
// UV plane: texture1 (R8G8_UNORM, half res)

Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState      samp  : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float y = texY.Sample(samp, uv);
    float2 uv_val = texUV.Sample(samp, uv);
    
    // BT.709 YUV to RGB conversion (standard untuk HD video)
    float u = uv_val.x - 0.5;
    float v = uv_val.y - 0.5;
    
    float r = y + 1.5748 * v;
    float g = y - 0.1873 * u - 0.4681 * v;
    float b = y + 1.8556 * u;
    
    return float4(saturate(float3(r, g, b)), 1.0);
}
```

> **CATATAN untuk Agent:** Shader ini harus di-compile saat build menggunakan `D3DCompileFromFile()` atau di-compile offline dengan `fxc.exe` dan di-embed sebagai byte array.
> Cara termudah: compile inline saat `CreateShaders()` menggunakan `D3DCompile()` dari `d3dcompiler.h`.

---

### 4.4 `src/platform/win32/power_governor.h` & `power_governor.cpp`

```cpp
#pragma once
#include <windows.h>

namespace litewp {

enum class PowerState {
    Active,         // Desktop visible, no fullscreen app → render at target FPS
    Reduced,        // On battery → render at reduced FPS
    Paused,         // Fullscreen app/game detected → stop rendering completely
    Sleeping,       // Workstation locked → stop everything (0% CPU)
};

class PowerGovernor {
public:
    // Initialize: register session notifications
    bool Init(HWND messageHwnd);
    
    // Poll current state (call setiap ~500ms dari main loop)
    PowerState GetCurrentState();
    
    // Handle WM_WTSSESSION_CHANGE message dari WndProc
    void HandleSessionChange(WPARAM wParam);
    
    // Handle WM_POWERBROADCAST
    void HandlePowerChange(WPARAM wParam);
    
    void Shutdown();

private:
    HWND m_hwnd = nullptr;
    bool m_is_locked = false;
    bool m_on_battery = false;
    
    bool IsFullscreenAppRunning();
    bool IsDesktopOccluded();
    bool IsOnBattery();
};

} // namespace litewp
```

**Implementasi detail:**

```cpp
bool PowerGovernor::Init(HWND messageHwnd) {
    m_hwnd = messageHwnd;
    // Register untuk session lock/unlock notifications
    WTSRegisterSessionNotification(messageHwnd, NOTIFY_FOR_THIS_SESSION);
    return true;
}

PowerState PowerGovernor::GetCurrentState() {
    // Priority 1: Locked → Sleep
    if (m_is_locked) return PowerState::Sleeping;
    
    // Priority 2: Fullscreen app → Pause
    if (IsFullscreenAppRunning()) return PowerState::Paused;
    
    // Priority 3: Desktop occluded (banyak window menutupi) → Pause
    if (IsDesktopOccluded()) return PowerState::Paused;
    
    // Priority 4: Battery → Reduced FPS
    if (IsOnBattery()) return PowerState::Reduced;
    
    return PowerState::Active;
}

void PowerGovernor::HandleSessionChange(WPARAM wParam) {
    if (wParam == WTS_SESSION_LOCK)   m_is_locked = true;
    if (wParam == WTS_SESSION_UNLOCK) m_is_locked = false;
}

bool PowerGovernor::IsFullscreenAppRunning() {
    // Method 1: SHQueryUserNotificationState (paling reliable)
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
            state == QUNS_BUSY ||
            state == QUNS_PRESENTATION_MODE) {
            return true;
        }
    }
    
    // Method 2: Cek foreground window menutupi seluruh monitor
    HWND fg = GetForegroundWindow();
    if (fg && fg != GetDesktopWindow()) {
        RECT fgRect;
        GetWindowRect(fg, &fgRect);
        HMONITOR hmon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(hmon, &mi);
        RECT monRect = mi.rcMonitor;
        if (fgRect.left <= monRect.left && fgRect.top <= monRect.top &&
            fgRect.right >= monRect.right && fgRect.bottom >= monRect.bottom) {
            return true;
        }
    }
    return false;
}

bool PowerGovernor::IsOnBattery() {
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        return sps.ACLineStatus == 0; // 0 = battery, 1 = AC
    }
    return false;
}

bool PowerGovernor::IsDesktopOccluded() {
    // Hitung berapa persen desktop yang tertutup window lain
    // Simplified: jika foreground window > 90% of any monitor → occluded
    // Implementasi lebih detail bisa menggunakan GetWindowRect + IntersectRect
    // Untuk v1.0, cukup return false (fitur opsional)
    return false;
}
```

---

### 4.5 `src/platform/win32/lockscreen_manager.h` & `lockscreen_manager.cpp`

**Purpose:** Saat user tekan Win+L, capture frame terakhir video dan set sebagai static lock screen image.

```cpp
#pragma once
#include <d3d11.h>
#include <string>

namespace litewp {

class LockScreenManager {
public:
    // Capture current frame dari D3D11 texture, save sebagai JPEG,
    // lalu set sebagai lock screen image
    bool CaptureAndSetLockScreen(ID3D11Device* device, ID3D11DeviceContext* ctx,
                                  ID3D11Texture2D* currentFrame, int arrayIndex);
    
    // Set static image file sebagai lock screen (Win10/11 via WinRT API)
    bool SetLockScreenImage(const std::wstring& imagePath);
    
    // Win7 fallback: copy image ke oobe folder + set registry
    bool SetLockScreenImageWin7(const std::wstring& imagePath);
    
private:
    std::wstring GetTempImagePath(); // %APPDATA%/LiteWallpaper/lockscreen_capture.jpg
};

} // namespace litewp
```

**Implementasi:**

1. **`CaptureAndSetLockScreen()`:**
   - Buat staging texture (`D3D11_USAGE_STAGING, CPU_ACCESS_READ`)
   - `CopySubresourceRegion()` dari NV12 texture ke staging texture
   - `Map()` staging texture untuk baca pixel data dari CPU
   - Convert NV12 → RGB → JPEG menggunakan `stb_image_write.h` (header-only library)
   - Save ke `%APPDATA%/LiteWallpaper/lockscreen_capture.jpg`
   - Call `SetLockScreenImage()` atau `SetLockScreenImageWin7()`

2. **`SetLockScreenImage()` (Win10/11):**
   - Gunakan C++/WinRT: `winrt::Windows::System::UserProfile::UserProfilePersonalizationSettings`
   - Atau alternatif tanpa WinRT: Gunakan `IDesktopWallpaper` COM interface
   - Atau paling sederhana: `SystemParametersInfoW(SPI_SETDESKWALLPAPER, ...)` — ini hanya set desktop wallpaper, bukan lock screen
   - **Rekomendasi pragmatis**: Untuk v1.0, gunakan shell command:
     ```cpp
     // Menggunakan PowerShell untuk set lock screen (Win10+)
     std::wstring cmd = L"powershell -Command \"Add-Type -TypeDefinition '" 
        L"using Windows.System.UserProfile; "
        L"[UserProfilePersonalizationSettings]::Current.TrySetLockScreenImageAsync(...)'"
        L"\"";
     ```
   - **Atau lebih sederhana**: Copy file ke `%WINDIR%\Web\Screen\` dan update registry `LockScreenImage`

3. **`SetLockScreenImageWin7()`:**
   ```cpp
   // 1. Copy JPEG ke C:\Windows\System32\oobe\info\backgrounds\backgroundDefault.jpg
   //    File HARUS < 256 KB!
   // 2. Set registry:
   //    HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\LogonUI\Background
   //    OEMBackground = 1 (DWORD)
   // CATATAN: Butuh admin privilege!
   ```

> **CATATAN PENTING untuk Agent:** Lock screen feature adalah nice-to-have. Jika terlalu kompleks, buat sebagai fitur opsional yang bisa di-enable/disable di config. Fokus utama adalah video wallpaper di desktop.

---

### 4.6 `src/platform/win32/tray_icon.h` & `tray_icon.cpp`

```cpp
#pragma once
#include <windows.h>

namespace litewp {

// Callback saat user klik menu item di tray
enum class TrayAction {
    PauseResume,
    OpenSettings,
    ChangeWallpaper,
    MuteUnmute,
    Exit
};

using TrayCallback = void(*)(TrayAction action);

class TrayIcon {
public:
    // Buat tray icon. hwnd = window untuk menerima messages.
    bool Create(HWND hwnd, TrayCallback callback);
    
    // Update icon tooltip text (e.g. "LiteWallpaper - Playing video.mp4")
    void SetTooltip(const wchar_t* text);
    
    // Handle WM_APP message dari WndProc (tray icon events)
    void HandleMessage(WPARAM wParam, LPARAM lParam);
    
    void Destroy();

private:
    NOTIFYICONDATAW m_nid = {};
    TrayCallback m_callback = nullptr;
    HMENU m_menu = nullptr;
    
    void ShowContextMenu(HWND hwnd);
};

} // namespace litewp
```

**Implementasi:**
```cpp
bool TrayIcon::Create(HWND hwnd, TrayCallback callback) {
    m_callback = callback;
    
    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = hwnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    m_nid.uCallbackMessage = WM_APP + 1;
    m_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
    // Fallback: m_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(m_nid.szTip, L"LiteWallpaper");
    
    Shell_NotifyIconW(NIM_ADD, &m_nid);
    
    // Buat context menu
    m_menu = CreatePopupMenu();
    AppendMenuW(m_menu, MF_STRING, 1, L"⏯ Pause/Resume");
    AppendMenuW(m_menu, MF_STRING, 2, L"🔇 Mute/Unmute");
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m_menu, MF_STRING, 3, L"📁 Change Wallpaper...");
    AppendMenuW(m_menu, MF_STRING, 4, L"⚙ Settings");
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m_menu, MF_STRING, 5, L"❌ Exit");
    
    return true;
}

void TrayIcon::HandleMessage(WPARAM wParam, LPARAM lParam) {
    if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
        ShowContextMenu(m_nid.hWnd);
    }
}

void TrayIcon::ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // Wajib! Agar menu hilang saat klik di luar
    int cmd = TrackPopupMenu(m_menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0); // Wajib setelah TrackPopupMenu
    
    switch (cmd) {
        case 1: m_callback(TrayAction::PauseResume); break;
        case 2: m_callback(TrayAction::MuteUnmute); break;
        case 3: m_callback(TrayAction::ChangeWallpaper); break;
        case 4: m_callback(TrayAction::OpenSettings); break;
        case 5: m_callback(TrayAction::Exit); break;
    }
}
```

---

## 🔧 LANGKAH 5: Main Daemon Entry Point

### `src/main.cpp`

```cpp
// LiteWallpaper Daemon — Background wallpaper engine
// Ini adalah main entry point yang menjalankan semuanya

#include <windows.h>
#include <mimalloc.h>          // Custom allocator
#include "core/config.h"
#include "core/playback_clock.h"
#include "core/ipc_server.h"
#include "decoder/ffmpeg_hw_decoder.h"
#include "decoder/audio_player.h"
#include "platform/win32/desktop_injector.h"
#include "platform/win32/d3d11_presenter.h"
#include "platform/win32/power_governor.h"
#include "platform/win32/lockscreen_manager.h"
#include "platform/win32/tray_icon.h"

// Override global allocator with mimalloc
// mi_version(); // Link mimalloc

using namespace litewp;

// === Global state ===
static Config           g_config;
static PlaybackClock    g_clock;
static DesktopInjector  g_injector;
static D3D11Presenter   g_presenter;
static FFmpegHWDecoder  g_decoder;
static AudioPlayer      g_audio;
static PowerGovernor    g_governor;
static LockScreenManager g_lockscreen;
static TrayIcon         g_tray;
static IpcServer        g_ipc;

static bool g_running = true;
static bool g_paused = false;
static HWND g_main_hwnd = nullptr;

// Forward declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void OnTrayAction(TrayAction action);
std::string OnIpcRequest(const std::string& json);

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // 1. Load config
    g_config.Load();
    auto& cfg = g_config.Get();
    
    // 2. Buat invisible message-only window untuk menerima system messages
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"LiteWallpaper_Daemon";
    RegisterClassExW(&wc);
    
    // Buat window VISIBLE (nanti di-attach ke WorkerW)
    // Ukuran = virtual screen (semua monitor)
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    
    g_main_hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, // Tidak muncul di taskbar
        L"LiteWallpaper_Daemon", L"LiteWallpaper",
        WS_POPUP, // Borderless
        vx, vy, vw, vh,
        nullptr, nullptr, hInstance, nullptr
    );
    
    // 3. Initialize D3D11
    g_presenter.Init(g_main_hwnd, vw, vh);
    
    // 4. Inject window ke desktop (belakang icons)
    g_injector.Attach(g_main_hwnd);
    g_injector.RegisterExplorerRestart(g_main_hwnd);
    
    // 5. Open video file (dari config, wallpaper pertama)
    if (!cfg.wallpapers.empty()) {
        g_decoder.Open(cfg.wallpapers[0].video_path.c_str(), g_presenter.GetDevice());
    }
    
    // 6. Setup audio (lazy init — hanya init jika user unmute)
    // g_audio akan di-init nanti saat user unmute
    
    // 7. Setup power governor
    g_governor.Init(g_main_hwnd);
    
    // 8. Setup tray icon
    g_tray.Create(g_main_hwnd, OnTrayAction);
    
    // 9. Setup playback clock
    g_clock.SetTargetFPS(cfg.target_fps);
    
    // 10. Start IPC server (background thread)
    g_ipc.Start(OnIpcRequest);
    
    // 11. Main loop
    MSG msg;
    while (g_running) {
        // Process Windows messages (non-blocking)
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        
        // Check power state
        PowerState power = g_governor.GetCurrentState();
        
        if (power == PowerState::Sleeping || power == PowerState::Paused || g_paused) {
            // Tidur! Gunakan MsgWaitForMultipleObjects untuk hemat CPU
            // Bangun hanya jika ada Windows message
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
            continue;
        }
        
        // Adjust FPS based on power state
        if (power == PowerState::Reduced) {
            g_clock.SetTargetFPS(cfg.battery_fps);
        } else {
            g_clock.SetTargetFPS(cfg.target_fps);
        }
        
        // Frame timing
        if (g_clock.ShouldRenderFrame()) {
            // Decode & render video frame
            VideoFrame frame;
            if (g_decoder.DecodeNextFrame(frame)) {
                g_presenter.RenderFrame(frame.texture, frame.texture_index);
                g_presenter.Present();
            }
        } else {
            // Sleep sampai frame berikutnya
            DWORD sleepMs = g_clock.GetSleepDurationMs();
            if (sleepMs > 0) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, sleepMs, QS_ALLINPUT);
            }
        }
    }
    
    // Cleanup
    g_ipc.Stop();
    g_tray.Destroy();
    g_governor.Shutdown();
    g_decoder.Close();
    g_audio.Stop();
    g_presenter.Cleanup();
    g_injector.Detach();
    
    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_WTSSESSION_CHANGE:
            g_governor.HandleSessionChange(wParam);
            // Capture frame untuk lock screen saat locking
            if (wParam == WTS_SESSION_LOCK && g_config.Get().update_lockscreen) {
                // TODO: capture current frame dan set lock screen
            }
            return 0;
            
        case WM_POWERBROADCAST:
            g_governor.HandlePowerChange(wParam);
            return TRUE;
            
        case WM_DISPLAYCHANGE:
            // Monitor resolution berubah → resize
            {
                int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                g_presenter.Resize(vw, vh);
                g_injector.Reattach(hwnd);
            }
            return 0;
            
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    
    // Handle tray icon messages
    if (msg == WM_APP + 1) {
        g_tray.HandleMessage(wParam, lParam);
        return 0;
    }
    
    // Handle Explorer restart
    // (cek apakah msg == RegisterWindowMessage("TaskbarCreated"))
    // Jika ya: g_injector.Reattach(hwnd);
    
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OnTrayAction(TrayAction action) {
    switch (action) {
        case TrayAction::PauseResume:
            g_paused = !g_paused;
            break;
        case TrayAction::MuteUnmute:
            g_audio.SetMuted(!g_audio.IsMuted());
            if (!g_audio.IsMuted() && !g_audio.IsRunning()) {
                g_audio.Init(); // Lazy init audio saat pertama kali unmute
            }
            break;
        case TrayAction::OpenSettings:
            // Launch settings app sebagai proses terpisah
            ShellExecuteW(nullptr, L"open", L"litewp_settings.exe", nullptr, nullptr, SW_SHOW);
            break;
        case TrayAction::ChangeWallpaper: {
            // Open file dialog (di thread terpisah agar tidak block)
            // Gunakan GetOpenFileName atau IFileDialog
            // Setelah dapat file, update config dan reload video
            break;
        }
        case TrayAction::Exit:
            g_running = false;
            PostQuitMessage(0);
            break;
    }
}

std::string OnIpcRequest(const std::string& json) {
    // Parse JSON request, execute command, return JSON response
    // Contoh:
    // {"cmd":"set_wallpaper","path":"C:/video.mp4","monitor":"\\\\?\\DISPLAY1"}
    // → g_decoder.Close(); g_decoder.Open(path, g_presenter.GetDevice());
    // → return {"ok":true}
    
    // {"cmd":"get_status"}
    // → return {"ok":true,"playing":true,"paused":false,"fps":30,"ram_mb":25}
    
    // {"cmd":"pause"} → g_paused = true
    // {"cmd":"resume"} → g_paused = false
    // {"cmd":"set_volume","volume":0.5} → g_audio.SetVolume(0.5f)
    // {"cmd":"set_fps","fps":60} → g_clock.SetTargetFPS(60)
    // {"cmd":"reload_config"} → g_config.Load(); apply settings
    
    return R"({"ok":true})";
}
```

---

## 🔧 LANGKAH 6: Settings UI (Dear ImGui)

### `src/ui/settings_app.cpp`

**Purpose:** Aplikasi terpisah yang berkomunikasi dengan daemon via IPC. Hanya berjalan saat user membukanya.

**Fitur yang harus diimplementasi:**
1. **Gallery tab**: Browse folder → tampilkan thumbnail video files → klik untuk set sebagai wallpaper
2. **Settings tab**: FPS target, pause behavior, audio volume, auto-start
3. **Monitors tab**: Tampilkan layout monitor → assign wallpaper per monitor
4. **Performance tab**: Real-time CPU/RAM usage dari daemon (via IPC `get_status`)

**Arsitektur:**
- Window menggunakan Win32 API (CreateWindowEx) dengan D3D11 swap chain
- Rendering menggunakan Dear ImGui D3D11 backend
- IPC ke daemon menggunakan `IpcClient`
- Saat window ditutup, proses terminate (tidak consume RAM)

**Skeleton implementasi:**
```cpp
#include <windows.h>
#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include "core/ipc_server.h" // IpcClient
#include "core/config.h"

// Agent harus implement:
// 1. Buat Win32 window + D3D11 device/swapchain
// 2. Init Dear ImGui (ImGui::CreateContext, ImGui_ImplWin32_Init, ImGui_ImplDX11_Init)
// 3. Main loop:
//    a. Poll messages
//    b. ImGui_ImplDX11_NewFrame()
//    c. ImGui_ImplWin32_NewFrame()
//    d. ImGui::NewFrame()
//    e. Render UI panels (lihat di bawah)
//    f. ImGui::Render()
//    g. ImGui_ImplDX11_RenderDrawData()
//    h. SwapChain->Present()
// 4. Cleanup saat close

// UI Panels:
void RenderGalleryPanel(IpcClient& ipc) {
    ImGui::Begin("Wallpaper Gallery");
    // List semua .mp4, .webm, .mkv, .avi files di folder tertentu
    // Tampilkan sebagai grid thumbnails
    // Klik thumbnail → IPC: {"cmd":"set_wallpaper","path":"..."}
    ImGui::End();
}

void RenderSettingsPanel(IpcClient& ipc, Config& config) {
    ImGui::Begin("Settings");
    ImGui::SliderInt("Target FPS", &config.Get().target_fps, 15, 60);
    ImGui::SliderInt("Battery FPS", &config.Get().battery_fps, 10, 30);
    ImGui::Checkbox("Pause on Fullscreen App", &config.Get().pause_on_fullscreen);
    ImGui::Checkbox("Pause on Battery", &config.Get().pause_on_battery);
    ImGui::Checkbox("Update Lock Screen", &config.Get().update_lockscreen);
    ImGui::Checkbox("Run on Startup", &config.Get().run_on_startup);
    
    // Audio
    static float volume = 0.0f;
    ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f);
    // IPC: {"cmd":"set_volume","volume":0.5}
    
    if (ImGui::Button("Save Settings")) {
        config.Save();
        // IPC: {"cmd":"reload_config"}
    }
    ImGui::End();
}

void RenderPerformancePanel(IpcClient& ipc) {
    ImGui::Begin("Performance");
    // IPC: {"cmd":"get_status"} → parse response
    // Tampilkan: CPU %, RAM MB, FPS, power state
    // Gunakan ImGui::PlotLines untuk graph
    ImGui::End();
}
```

> **CATATAN untuk Agent:** Dear ImGui setup dengan D3D11 sudah sangat well-documented. Lihat official ImGui examples: `examples/example_win32_directx11/`. Copy dan modifikasi dari sana.

---

## 🔧 LANGKAH 7: Supporting Files

### 7.1 `LICENSE`
Copy full text GPL v3 dari https://www.gnu.org/licenses/gpl-3.0.txt

### 7.2 `README.md`
```markdown
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
```
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## License
GPL v3
```

### 7.3 `assets/default_config.json`
```json
{
    "wallpapers": [],
    "target_fps": 30,
    "idle_fps": 15,
    "pause_on_fullscreen": true,
    "pause_on_battery": false,
    "battery_fps": 15,
    "pause_on_lock": true,
    "update_lockscreen": true,
    "run_on_startup": false
}
```

---

## ⚠️ Catatan Penting untuk Implementing Agent

### Prioritas Implementasi (Urutan Kerja)
1. **CMakeLists.txt + vcpkg.json** — pastikan build system jalan dulu
2. **D3D11Presenter** — pastikan bisa buat window + render warna solid
3. **DesktopInjector** — pastikan window muncul di belakang desktop icons
4. **FFmpegHWDecoder** — pastikan bisa decode dan tampilkan video frame
5. **PlaybackClock** — pastikan frame rate terkontrol
6. **PowerGovernor** — auto-pause saat fullscreen/lock
7. **TrayIcon** — system tray dengan context menu
8. **Config** — load/save settings
9. **IpcServer** — komunikasi daemon ↔ settings
10. **AudioPlayer** — WASAPI audio output
11. **LockScreenManager** — capture frame untuk lock screen
12. **Settings UI (ImGui)** — terakhir, karena daemon harus jalan dulu

### Error Handling
- Semua fungsi yang bisa fail harus return bool atau error code
- Log errors ke file: `%APPDATA%/LiteWallpaper/litewp.log`
- Jangan crash! Jika video file corrupt, tampilkan warna hitam dan log error.

### Memory Rules
- Gunakan `mimalloc` sebagai global allocator
- JANGAN allocate uncompressed video frames di CPU RAM (gunakan GPU VRAM via D3D11VA)
- Audio buffers: max 44100 * 2 * 4 * 0.1 = ~35 KB (100ms buffer)
- Packet queue: max 4 * 256 KB = 1 MB

### Win7 Compatibility Gotchas
- JANGAN gunakan `DXGI_SWAP_EFFECT_FLIP_DISCARD` — itu Win10+ only. Gunakan `DXGI_SWAP_EFFECT_DISCARD`.
- JANGAN gunakan C++/WinRT (`winrt::` namespace) — itu Win10+ only.
- JANGAN gunakan `IDXGISwapChain1` — gunakan `IDXGISwapChain` (versi original).
- JANGAN gunakan `CreateDXGIFactory2` — gunakan `CreateDXGIFactory1` atau `CreateDXGIFactory`.
- `D3D11CreateDevice` dengan `D3D_FEATURE_LEVEL_11_0` memerlukan Platform Update pada Win7 SP1.
- `SHQueryUserNotificationState` tersedia sejak Vista — aman.
- `WTSRegisterSessionNotification` tersedia sejak XP — aman.

### Testing
- Test dengan video 1080p H.264 (format paling umum)
- Verifikasi RAM < 45 MB di Task Manager
- Verifikasi CPU < 2% saat video playing
- Test pause/resume saat buka game fullscreen
- Test lock/unlock workstation
- Test multi-monitor jika tersedia

### Multi-Monitor Implementation Detail
- `EnumDisplayMonitors` untuk enumerate semua monitor
- Buat 1 window besar yang cover seluruh virtual screen (semua monitor)
- Saat render, gunakan viewport per monitor:
  ```cpp
  for (auto& monitor : monitors) {
      // Set viewport ke area monitor ini
      D3D11_VIEWPORT vp = {
          (float)(monitor.rect.left - vx),  // Offset dari virtual screen origin
          (float)(monitor.rect.top - vy),
          (float)(monitor.rect.right - monitor.rect.left),
          (float)(monitor.rect.bottom - monitor.rect.top),
          0, 1
      };
      context->RSSetViewports(1, &vp);
      // Render video frame untuk monitor ini
      // (bisa video yang berbeda per monitor sesuai config)
  }
  ```
