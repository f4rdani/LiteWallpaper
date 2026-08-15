# Third-Party Dependencies

## FFmpeg Development Libraries
LiteWallpaper uses FFmpeg (avcodec, avformat, avutil, swscale, swresample) for hardware-accelerated video decoding (D3D11VA).

### Download Instructions:
1. Download Windows 64-bit shared developer builds (e.g. from https://github.com/BtbN/FFmpeg-Builds/releases or https://ffmpeg.org).
2. Extract the files into `third_party/ffmpeg` such that:
   - `third_party/ffmpeg/include/libavcodec/avcodec.h`
   - `third_party/ffmpeg/include/libavformat/avformat.h`
   - `third_party/ffmpeg/include/libavutil/avutil.h`
   - `third_party/ffmpeg/include/libswscale/swscale.h`
   - `third_party/ffmpeg/include/libswresample/swresample.h`
   - `third_party/ffmpeg/lib/avcodec.lib`
   - `third_party/ffmpeg/lib/avformat.lib`
   - `third_party/ffmpeg/lib/avutil.lib`
   - `third_party/ffmpeg/lib/swscale.lib`
   - `third_party/ffmpeg/lib/swresample.lib`
   - `third_party/ffmpeg/bin/*.dll`

## vcpkg Dependencies
The remaining dependencies are managed via `vcpkg.json`:
- `nlohmann-json`
- `mimalloc`
- `imgui[docking-experimental,win32-binding,dx11-binding]`
