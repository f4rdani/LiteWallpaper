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

    // Method 2: Fast check foreground window
    HWND fg = GetForegroundWindow();
    if (fg && IsWindow(fg) && !IsIconic(fg) && IsWindowVisible(fg)) {
        wchar_t cls[256] = {};
        if (GetClassNameW(fg, cls, 256)) {
            if (wcscmp(cls, L"LiteWallpaper_SettingsClass") != 0 &&
                wcscmp(cls, L"LiteWallpaper_Daemon") != 0) {
                int cloaked = 0;
                if (!SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked == 0) {
                    HMONITOR hmon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi = { sizeof(mi) };
                    if (GetMonitorInfoW(hmon, &mi)) {
                        RECT rc;
                        if (GetWindowRect(fg, &rc)) {
                            if (rc.left <= mi.rcMonitor.left && rc.top <= mi.rcMonitor.top &&
                                rc.right >= mi.rcMonitor.right && rc.bottom >= mi.rcMonitor.bottom) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    // Method 3: Deep Z-Order scan - check if ANY visible top-level window covers the entire monitor
    bool fullscreen = false;
    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

        int cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
            return TRUE;
        }

        wchar_t cls[256] = {};
        if (GetClassNameW(hwnd, cls, 256) > 0) {
            // Reached desktop layer, stop enumeration
            if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) return FALSE;
            // Skip shell trays, tooltips, settings UI, and daemon
            if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
                wcscmp(cls, L"LiteWallpaper_SettingsClass") == 0 ||
                wcscmp(cls, L"LiteWallpaper_Daemon") == 0 ||
                wcscmp(cls, L"tooltips_class32") == 0) {
                return TRUE;
            }
        }

        HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hmon, &mi)) {
            RECT rc;
            if (GetWindowRect(hwnd, &rc)) {
                if (rc.left <= mi.rcMonitor.left && rc.top <= mi.rcMonitor.top &&
                    rc.right >= mi.rcMonitor.right && rc.bottom >= mi.rcMonitor.bottom) {
                    *reinterpret_cast<bool*>(lparam) = true;
                    return FALSE; // Found fullscreen app, stop searching!
                }
            }
        }

        return TRUE;
    }, reinterpret_cast<LPARAM>(&fullscreen));

    return fullscreen;
}

bool PowerGovernor::IsOnBattery() {
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        return sps.ACLineStatus == 0; // 0 = Offline/Battery, 1 = Online/AC
    }
    return false;
}

bool PowerGovernor::IsDesktopOccluded() {
    // Method 1: Fast check foreground window
    HWND fg = GetForegroundWindow();
    if (fg && IsWindow(fg) && !IsIconic(fg) && IsWindowVisible(fg)) {
        wchar_t cls[256] = {};
        if (GetClassNameW(fg, cls, 256)) {
            if (wcscmp(cls, L"LiteWallpaper_SettingsClass") != 0 &&
                wcscmp(cls, L"LiteWallpaper_Daemon") != 0) {
                int cloaked = 0;
                if (!SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked == 0) {
                    if (IsZoomed(fg) || (GetWindowLongPtr(fg, GWL_STYLE) & WS_MAXIMIZE) != 0) {
                        return true;
                    }
                    HMONITOR hmon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi = { sizeof(mi) };
                    if (GetMonitorInfoW(hmon, &mi)) {
                        RECT rc;
                        if (GetWindowRect(fg, &rc)) {
                            if (rc.left <= mi.rcWork.left + 8 && rc.top <= mi.rcWork.top + 8 &&
                                rc.right >= mi.rcWork.right - 8 && rc.bottom >= mi.rcWork.bottom - 8) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    // Method 2: Deep Z-Order scan - check if ANY visible top-level window is maximized or covers the work area
    // (Handles small dialogs/popups focused on top of a maximized browser/editor/game)
    bool occluded = false;
    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

        int cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
            return TRUE;
        }

        wchar_t cls[256] = {};
        if (GetClassNameW(hwnd, cls, 256) > 0) {
            // Reached desktop layer, stop enumeration
            if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) return FALSE;
            // Skip shell trays, tooltips, settings UI, and daemon
            if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
                wcscmp(cls, L"LiteWallpaper_SettingsClass") == 0 ||
                wcscmp(cls, L"LiteWallpaper_Daemon") == 0 ||
                wcscmp(cls, L"tooltips_class32") == 0) {
                return TRUE;
            }
        }

        // Check if window is maximized (IsZoomed or WS_MAXIMIZE style)
        if (IsZoomed(hwnd) || (GetWindowLongPtr(hwnd, GWL_STYLE) & WS_MAXIMIZE) != 0) {
            *reinterpret_cast<bool*>(lparam) = true;
            return FALSE; // Found a maximized window covering the desktop, stop searching!
        }

        // Check if window rect covers monitor work area (e.g. borderless maximized)
        HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hmon, &mi)) {
            RECT rc;
            if (GetWindowRect(hwnd, &rc)) {
                if (rc.left <= mi.rcWork.left + 8 && rc.top <= mi.rcWork.top + 8 &&
                    rc.right >= mi.rcWork.right - 8 && rc.bottom >= mi.rcWork.bottom - 8) {
                    *reinterpret_cast<bool*>(lparam) = true;
                    return FALSE;
                }
            }
        }

        return TRUE;
    }, reinterpret_cast<LPARAM>(&occluded));

    return occluded;
}

void PowerGovernor::Shutdown() {
    if (m_hwnd) {
        WTSUnRegisterSessionNotification(m_hwnd);
        m_hwnd = nullptr;
    }
}

} // namespace litewp
