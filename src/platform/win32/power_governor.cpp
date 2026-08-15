#include "power_governor.h"
#include <dwmapi.h>
#include <shellapi.h>

namespace litewp {

PowerGovernor::PowerGovernor() = default;

PowerGovernor::~PowerGovernor() {
    Shutdown();
}

bool PowerGovernor::Init(HWND messageHwnd) {
    m_hwnd = messageHwnd;
    if (m_hwnd) {
        WTSRegisterSessionNotification(messageHwnd, NOTIFY_FOR_THIS_SESSION);
    }
    m_on_battery = IsOnBattery();
    m_last_check_tick = GetTickCount64();
    m_cached_state = PowerState::Active;
    return true;
}

PowerState PowerGovernor::GetCurrentState() {
    // Priority 1: Workstation Locked → Sleep (Immediate 0% CPU)
    if (m_is_locked) {
        return PowerState::Sleeping;
    }

    uint64_t now = GetTickCount64();
    // Throttle Win32 window queries to once every 500ms
    if (now - m_last_check_tick < 500) {
        return m_cached_state;
    }
    m_last_check_tick = now;

    // Priority 2: Fullscreen 3D Game / App Running → Pause
    if (IsFullscreenAppRunning()) {
        m_cached_state = PowerState::Paused;
        return m_cached_state;
    }

    // Priority 3: On Battery → Reduced FPS
    if (IsOnBattery()) {
        m_cached_state = PowerState::Reduced;
        return m_cached_state;
    }

    m_cached_state = PowerState::Active;
    return m_cached_state;
}

void PowerGovernor::HandleSessionChange(WPARAM wParam) {
    if (wParam == WTS_SESSION_LOCK) {
        m_is_locked = true;
    } else if (wParam == WTS_SESSION_UNLOCK) {
        m_is_locked = false;
        m_last_check_tick = 0; // Force immediate re-evaluation on unlock
    }
}

void PowerGovernor::HandlePowerChange(WPARAM wParam) {
    if (wParam == PBT_APMPOWERSTATUSCHANGE) {
        m_on_battery = IsOnBattery();
        m_last_check_tick = 0;
    } else if (wParam == PBT_APMSUSPEND) {
        m_is_locked = true;
    } else if (wParam == PBT_APMRESUMEAUTOMATIC) {
        m_is_locked = false;
        m_last_check_tick = 0;
    }
}

namespace {
struct FullscreenCheckCtx {
    bool any_fullscreen = false;
};

BOOL CALLBACK EnumFullscreenWindowProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<FullscreenCheckCtx*>(lParam);
    if (ctx->any_fullscreen) {
        return FALSE; // Early exit once one is found
    }

    // Only visible, non-minimized top-level windows can occlude the desktop.
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
    }

    // Skip windows that DWM is currently cloaking (e.g. app startup splash
    // screens, non-visible UWP/Settings windows). They report WS_VISIBLE but
    // are not actually rendered on screen.
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return TRUE;
    }

    // Skip layered windows that are fully transparent (alpha == 0), such as
    // the NVIDIA GeForce Overlay. They cover the whole monitor but are
    // invisible, so they never actually occlude the desktop.
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_LAYERED) != 0) {
        DWORD colorKey = 0;
        BYTE alpha = 0;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hwnd, &colorKey, &alpha, &flags) && alpha == 0) {
            return TRUE;
        }
    }

    // Skip shell and our own windows.
    wchar_t className[256];
    if (GetClassNameW(hwnd, className, 256)) {
        if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0 ||
            wcscmp(className, L"Shell_TrayWnd") == 0 || wcscmp(className, L"LiteWallpaper_SettingsClass") == 0 ||
            wcscmp(className, L"LiteWallpaper_Daemon") == 0) {
            return TRUE;
        }
    }

    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) {
        return TRUE;
    }

    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hmon, &mi)) {
        return TRUE;
    }

    // A window only occludes the desktop when it is TRULY fullscreen: a
    // frameless (no caption) window covering the full monitor, e.g. a game,
    // F11 video/terminal, or a media player. Plain maximized windows (Edge,
    // Explorer, terminals with a title bar) are NORMAL usage — the user still
    // reaches the desktop via Win+D / 3-finger swipe and Alt-Tab, so the
    // wallpaper must keep running behind them.
    RECT work = mi.rcWork;
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) != 0 || (style & WS_CHILD) != 0) {
        return TRUE;
    }
    if (rc.left <= work.left + 2 && rc.top <= work.top + 2 &&
        rc.right >= work.right - 2 && rc.bottom >= work.bottom - 2) {
        ctx->any_fullscreen = true;
        return FALSE;
    }
    return TRUE;
}
} // namespace

bool PowerGovernor::IsFullscreenAppRunning() {
    // Method 1: Check for true fullscreen 3D D3D games
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN) {
            return true;
        }
    }

    // Method 2: The desktop is considered "occluded" when ANY visible
    // top-level window covers the entire working area of its monitor —
    // maximized windows and borderless fullscreen apps alike. Unlike checking
    // only the foreground window, this stays suspended when a small non-
    // fullscreen window is brought on top of a fullscreen one, because the
    // fullscreen window behind it still hides the wallpaper.
    FullscreenCheckCtx ctx;
    EnumWindows(EnumFullscreenWindowProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.any_fullscreen;
}

bool PowerGovernor::IsOnBattery() {
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        return sps.ACLineStatus == 0; // 0 = Offline/Battery, 1 = Online/AC
    }
    return false;
}

bool PowerGovernor::IsDesktopOccluded() {
    return false;
}

void PowerGovernor::Shutdown() {
    if (m_hwnd) {
        WTSUnRegisterSessionNotification(m_hwnd);
        m_hwnd = nullptr;
    }
}

} // namespace litewp
