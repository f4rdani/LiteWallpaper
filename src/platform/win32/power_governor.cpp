#include "power_governor.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <sstream>
#include <iomanip>

namespace litewp {

static std::string FormatWindowInfo(HWND hwnd, const char* prefix = "") {
    if (!hwnd || !IsWindow(hwnd)) return "None";

    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);

    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 256);

    RECT rc = {};
    GetWindowRect(hwnd, &rc);

    char utf8Cls[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, cls, -1, utf8Cls, sizeof(utf8Cls), NULL, NULL);

    char utf8Title[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, title, -1, utf8Title, sizeof(utf8Title), NULL, NULL);

    std::ostringstream oss;
    if (prefix && prefix[0] != '\0') {
        oss << prefix << ": ";
    }
    oss << "HWND=0x" << std::hex << reinterpret_cast<uintptr_t>(hwnd) << std::dec
        << " | Class='" << utf8Cls << "'"
        << " | Title='" << utf8Title << "'"
        << " | Rect=(" << rc.left << "," << rc.top << "," << (rc.right - rc.left) << "x" << (rc.bottom - rc.top) << ")";
    return oss.str();
}

static bool IsRealAppWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }

    // Skip cloaked windows (virtual desktops / Metro background)
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return false;
    }

    // Check extended styles: skip transparent, tool windows, or un-activatable surfaces
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TRANSPARENT) != 0 || (exStyle & WS_EX_TOOLWINDOW) != 0 || (exStyle & WS_EX_NOACTIVATE) != 0) {
        return false;
    }

    // Filter out known Windows system/shell background classes
    wchar_t cls[256] = {};
    if (GetClassNameW(hwnd, cls, 256) > 0) {
        if (wcscmp(cls, L"Progman") == 0 ||
            wcscmp(cls, L"WorkerW") == 0 ||
            wcscmp(cls, L"Shell_TrayWnd") == 0 ||
            wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
            wcscmp(cls, L"Shell_LightDismissOverlay") == 0 ||
            wcscmp(cls, L"EdgeUiInputTopWndClass") == 0 ||
            wcscmp(cls, L"EdgeUiInputWndClass") == 0 ||
            wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0 ||
            wcscmp(cls, L"DesktopWindowContentBridge") == 0 ||
            wcscmp(cls, L"Dwm") == 0 ||
            wcscmp(cls, L"DirectUIHWND") == 0 ||
            wcscmp(cls, L"tooltips_class32") == 0 ||
            wcscmp(cls, L"SysShadow") == 0 ||
            wcscmp(cls, L"LiteWallpaper_SettingsClass") == 0 ||
            wcscmp(cls, L"LiteWallpaper_Daemon") == 0) {
            return false;
        }
    }

    RECT rc = {};
    if (!GetWindowRect(hwnd, &rc)) return false;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 100 || h < 100) return false;

    return true;
}

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
    m_last_trigger_info = "Desktop is visible and active";
    return true;
}

PowerState PowerGovernor::GetCurrentState(bool check_maximized) {
    // Priority 1: Workstation Locked → Sleep (Immediate 0% CPU)
    if (m_is_locked) {
        m_last_trigger_info = "Workstation Locked (Session Sleep)";
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
        m_last_trigger_info = "Running on Battery (Throttled FPS)";
        return m_cached_state;
    }

    m_cached_state = PowerState::Active;
    m_last_trigger_info = "Desktop is visible and active";
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
            m_last_trigger_info = "3D Fullscreen D3D Game Running (QUNS_RUNNING_D3D_FULL_SCREEN)";
            return true;
        }
    }

    // Method 2: Check active foreground application window
    HWND fg = GetForegroundWindow();
    if (IsRealAppWindow(fg)) {
        HMONITOR hmon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hmon, &mi)) {
            RECT rc;
            if (GetWindowRect(fg, &rc)) {
                // A true fullscreen app covers the ENTIRE monitor (rcMonitor, including taskbar)
                if (rc.left <= mi.rcMonitor.left && rc.top <= mi.rcMonitor.top &&
                    rc.right >= mi.rcMonitor.right && rc.bottom >= mi.rcMonitor.bottom) {
                    m_last_trigger_info = FormatWindowInfo(fg, "Fullscreen Foreground App");
                    return true;
                }
            }
        }
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
    // 1. Fast path: Check active foreground window
    HWND fg = GetForegroundWindow();
    if (IsRealAppWindow(fg)) {
        if (IsZoomed(fg) || (GetWindowLongPtr(fg, GWL_STYLE) & WS_MAXIMIZE) != 0) {
            m_last_trigger_info = FormatWindowInfo(fg, "Maximized Foreground Window");
            return true;
        }

        HMONITOR hmon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hmon, &mi)) {
            RECT rc;
            if (GetWindowRect(fg, &rc)) {
                if (rc.left <= mi.rcWork.left + 8 && rc.top <= mi.rcWork.top + 8 &&
                    rc.right >= mi.rcWork.right - 8 && rc.bottom >= mi.rcWork.bottom - 8) {
                    m_last_trigger_info = FormatWindowInfo(fg, "Workarea-Filling Foreground Window");
                    return true;
                }
            }
        }
    }

    // 2. Deep scan: If foreground is a small dialog/popup, check if ANY real app window
    // in the visible Z-order stack is maximized covering the desktop work area.
    struct OcclusionResult {
        bool occluded = false;
        HWND trigger_hwnd = nullptr;
    } result;

    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        // Stop scanning immediately when reaching the desktop shell layer
        wchar_t cls[256] = {};
        if (GetClassNameW(hwnd, cls, 256) > 0) {
            if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) {
                return FALSE; // Reached desktop background, stop scanning!
            }
        }

        if (!IsRealAppWindow(hwnd)) {
            return TRUE; // Skip non-app / system helper windows
        }

        // Check if this real application window is Maximized
        if (IsZoomed(hwnd) || (GetWindowLongPtr(hwnd, GWL_STYLE) & WS_MAXIMIZE) != 0) {
            auto* res = reinterpret_cast<OcclusionResult*>(lparam);
            res->occluded = true;
            res->trigger_hwnd = hwnd;
            return FALSE; // Found maximized window covering desktop, stop!
        }

        // Check if window covers the monitor's work area
        HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hmon, &mi)) {
            RECT rc;
            if (GetWindowRect(hwnd, &rc)) {
                if (rc.left <= mi.rcWork.left + 8 && rc.top <= mi.rcWork.top + 8 &&
                    rc.right >= mi.rcWork.right - 8 && rc.bottom >= mi.rcWork.bottom - 8) {
                    auto* res = reinterpret_cast<OcclusionResult*>(lparam);
                    res->occluded = true;
                    res->trigger_hwnd = hwnd;
                    return FALSE;
                }
            }
        }

        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));

    if (result.occluded && result.trigger_hwnd) {
        m_last_trigger_info = FormatWindowInfo(result.trigger_hwnd, "Maximized Background Window");
        return true;
    }

    m_last_trigger_info = "Desktop is visible and active";
    return false;
}

void PowerGovernor::Shutdown() {
    if (m_hwnd) {
        WTSUnRegisterSessionNotification(m_hwnd);
        m_hwnd = nullptr;
    }
}

} // namespace litewp
