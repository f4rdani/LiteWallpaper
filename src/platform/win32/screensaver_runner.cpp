#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "screensaver_runner.h"
#include "d3d11_presenter.h"
#include "core/config.h"
#include "core/playback_clock.h"
#include "core/win_utils.h"
#include "decoder/ffmpeg_hw_decoder.h"
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace miniwp {

namespace {
POINT g_scr_mouse_init = { 0, 0 };
bool  g_scr_mouse_init_set = false;

LRESULT CALLBACK ScreensaverWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            GetCursorPos(&g_scr_mouse_init);
            g_scr_mouse_init_set = true;
            return 0;
        case WM_SETCURSOR:
            SetCursor(nullptr);
            return TRUE;
        case WM_MOUSEMOVE: {
            if (!g_scr_mouse_init_set) {
                GetCursorPos(&g_scr_mouse_init);
                g_scr_mouse_init_set = true;
                return 0;
            }
            POINT pt;
            GetCursorPos(&pt);
            if (std::abs(pt.x - g_scr_mouse_init.x) > 15 || std::abs(pt.y - g_scr_mouse_init.y) > 15) {
                PostQuitMessage(0);
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

int RunScreensaverMode(HINSTANCE hInstance) {
    g_scr_mouse_init_set = false;

    Config config;
    config.Load();
    const auto& cfg = config.Get();

    std::string video_path = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
    std::error_code ec;
    if (video_path.empty() || !std::filesystem::exists(PathFromUtf8(video_path), ec)) {
        return 0;
    }

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = ScreensaverWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"MiniWallpaper_Screensaver";
    wc.hCursor = nullptr;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        wc.lpszClassName,
        L"MiniWallpaper Screensaver",
        WS_POPUP | WS_VISIBLE,
        vx, vy, vw, vh,
        nullptr, nullptr, hInstance, nullptr
    );
    if (!hwnd) {
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    ShowCursor(FALSE);

    D3D11Presenter presenter;
    if (!presenter.Init(hwnd, vw, vh, cfg.gpu_device_index)) {
        ShowCursor(TRUE);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    FFmpegHWDecoder decoder;
    decoder.SetForceSoftware(cfg.gpu_device_index == -1);
    if (!decoder.Open(video_path.c_str(), presenter.GetDevice(), vw, vh)) {
        ShowCursor(TRUE);
        presenter.Cleanup();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    PlaybackClock clock;
    clock.SetTargetFPS((cfg.target_fps > 0) ? cfg.target_fps : 60);

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    VideoFrame frame;
    MSG msg = {};
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        if (clock.ShouldRenderFrame()) {
            if (decoder.DecodeNextFrame(frame) && frame.texture) {
                presenter.RenderFrame(frame.texture, frame.texture_index, cfg.scaling_mode, {}, 0.0f, frame.width, frame.height);
                presenter.Present(1);
            } else {
                decoder.SeekToStart();
                clock.Reset();
            }
        } else {
            uint32_t sleep_ms = clock.GetSleepDurationMs();
            DWORD wait_ms = (sleep_ms > 0) ? sleep_ms : 1;
            if (hTimer) {
                LARGE_INTEGER due = {};
                due.QuadPart = -static_cast<LONGLONG>(wait_ms) * 10000LL;
                if (SetWaitableTimer(hTimer, &due, 0, nullptr, nullptr, FALSE)) {
                    MsgWaitForMultipleObjects(1, &hTimer, FALSE, wait_ms + 2, QS_ALLINPUT);
                } else {
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, wait_ms, QS_ALLINPUT);
                }
            } else {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, wait_ms, QS_ALLINPUT);
            }
        }
    }

    if (hTimer) CloseHandle(hTimer);
    ShowCursor(TRUE);
    presenter.ReleaseVideoResources();
    decoder.Close();
    presenter.Cleanup();
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}

} // namespace miniwp
