#include "power_governor.h"
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
    // Priority 1: Workstation Locked → Sleep (Immediate 0% CPU, no throttling)
    if (m_is_locked) {
        return PowerState::Sleeping;
    }

    uint64_t now = GetTickCount64();
    // Throttle heavy Win32 window queries to once every 500ms
    if (now - m_last_check_tick < 500) {
        return m_cached_state;
    }
    m_last_check_tick = now;

    // Priority 2: Fullscreen App / Game Running → Pause
    if (IsFullscreenAppRunning()) {
        m_cached_state = PowerState::Paused;
        return m_cached_state;
    }

    // Priority 3: Desktop Occluded → Pause
    if (IsDesktopOccluded()) {
        m_cached_state = PowerState::Paused;
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
    // Method 1: SHQueryUserNotificationState (Windows Vista+)
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
            state == QUNS_BUSY ||
            state == QUNS_PRESENTATION_MODE) {
            return true;
        }
    }

    // Method 2: Foreground Window Bounds Check
    HWND fg = GetForegroundWindow();
    if (fg && fg != GetDesktopWindow() && fg != GetShellWindow()) {
        wchar_t className[256];
        if (GetClassNameW(fg, className, 256)) {
            if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0) {
                return false;
            }
        }

        RECT fgRect;
        if (GetWindowRect(fg, &fgRect)) {
            HMONITOR hmon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(hmon, &mi)) {
                RECT monRect = mi.rcMonitor;
                if (fgRect.left <= monRect.left && fgRect.top <= monRect.top &&
                    fgRect.right >= monRect.right && fgRect.bottom >= monRect.bottom) {
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
    return false;
}

void PowerGovernor::Shutdown() {
    if (m_hwnd) {
        WTSUnRegisterSessionNotification(m_hwnd);
        m_hwnd = nullptr;
    }
}

} // namespace litewp
