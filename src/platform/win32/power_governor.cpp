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

PowerState PowerGovernor::GetCurrentState(bool check_maximized) {
    // Priority 1: Workstation Locked → Sleep (Immediate 0% CPU)
    if (m_is_locked) {
        return PowerState::Sleeping;
    }

    uint64_t now = GetTickCount64();
    // Throttle Win32 window queries to once every 250ms
    if (now - m_last_check_tick < 250) {
        return m_cached_state;
    }
    m_last_check_tick = now;

    // Priority 2: Fullscreen 3D Game / App Running → Pause
    if (IsFullscreenAppRunning()) {
        m_cached_state = PowerState::Paused;
        return m_cached_state;
    }

    // Priority 3: Desktop Occluded by Maximized Window → Occluded
    if (check_maximized && IsDesktopOccluded()) {
        m_cached_state = PowerState::Occluded;
        return m_cached_state;
    }

    // Priority 4: On Battery → Reduced FPS
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

bool PowerGovernor::IsFullscreenAppRunning() {
    // Method 1: Check for true fullscreen 3D D3D games via Windows API
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN) {
            return true;
        }
    }

    // Method 2: Check foreground window
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return false;

    // Desktop/shell/our app focused -> NEVER pause
    wchar_t cls[256];
    if (GetClassNameW(fg, cls, 256)) {
        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
            wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
            wcscmp(cls, L"LiteWallpaper_SettingsClass") == 0 ||
            wcscmp(cls, L"LiteWallpaper_Daemon") == 0) {
            return false;
        }
    }

    if (IsIconic(fg) || !IsWindowVisible(fg)) return false;

    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return false;
    }

    HMONITOR hmon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hmon, &mi)) return false;

    RECT rc;
    if (!GetWindowRect(fg, &rc)) return false;

    // A true fullscreen app covers the ENTIRE monitor (rcMonitor, including taskbar)
    if (rc.left <= mi.rcMonitor.left && rc.top <= mi.rcMonitor.top &&
        rc.right >= mi.rcMonitor.right && rc.bottom >= mi.rcMonitor.bottom) {
        return true;
    }

    return false;
}

bool PowerGovernor::IsOnBattery() {
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        return sps.ACLineStatus == 0; // 0 = Offline/Battery, 1 = Online/AC
    }
    return false;
}

bool PowerGovernor::IsDesktopOccluded() {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return false;

    // Desktop / shell / our settings or daemon window focused -> NEVER occluded
    wchar_t cls[256];
    if (GetClassNameW(fg, cls, 256)) {
        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
            wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
            wcscmp(cls, L"LiteWallpaper_SettingsClass") == 0 ||
            wcscmp(cls, L"LiteWallpaper_Daemon") == 0) {
            return false;
        }
    }

    if (IsIconic(fg) || !IsWindowVisible(fg)) return false;

    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return false;
    }

    // Method 1: Check standard Win32 maximized style or zoomed state
    if (IsZoomed(fg) || (GetWindowLongPtr(fg, GWL_STYLE) & WS_MAXIMIZE) != 0) {
        return true;
    }

    // Method 2: Check if window covers the monitor's work area
    HMONITOR hmon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hmon, &mi)) return false;

    RECT rc;
    if (!GetWindowRect(fg, &rc)) return false;

    // A maximized or desktop-filling window covers the entire work area (rcWork)
    // Tolerance of 8px handles invisible DWM resize borders
    if (rc.left <= mi.rcWork.left + 8 && rc.top <= mi.rcWork.top + 8 &&
        rc.right >= mi.rcWork.right - 8 && rc.bottom >= mi.rcWork.bottom - 8) {
        return true;
    }

    return false;
}

void PowerGovernor::Shutdown() {
    if (m_hwnd) {
        WTSUnRegisterSessionNotification(m_hwnd);
        m_hwnd = nullptr;
    }
}

} // namespace litewp
