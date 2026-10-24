#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "power_governor.h"
#include "core/win_utils.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace miniwp {

namespace {

std::atomic<bool> s_window_topology_dirty{true};

void CALLBACK GovernorWinEventProc(
    HWINEVENTHOOK /*hWinEventHook*/,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD /*idEventThread*/,
    DWORD /*dwmsEventTime*/
) {
    if (event >= EVENT_SYSTEM_FOREGROUND && event <= EVENT_SYSTEM_MINIMIZEEND) {
        if (event == EVENT_SYSTEM_FOREGROUND ||
            event == EVENT_SYSTEM_MOVESIZEEND ||
            event == EVENT_SYSTEM_MINIMIZESTART ||
            event == EVENT_SYSTEM_MINIMIZEEND) {
            s_window_topology_dirty.store(true, std::memory_order_relaxed);
        }
        return;
    }

    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) {
        return;
    }
    if (event == EVENT_OBJECT_DESTROY ||
        event == EVENT_OBJECT_SHOW ||
        event == EVENT_OBJECT_HIDE ||
        event == EVENT_OBJECT_LOCATIONCHANGE) {
        if (event != EVENT_OBJECT_DESTROY && (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0) {
            return;
        }
        s_window_topology_dirty.store(true, std::memory_order_relaxed);
    }
}

} // namespace

static std::string FormatWindowInfo(HWND hwnd, const char* prefix = "") {
    if (!hwnd || !IsWindow(hwnd)) return "None";

    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);

    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 256);

    RECT rc = {};
    GetWindowRect(hwnd, &rc);

    std::string utf8Cls = WideToUtf8(cls);
    std::string utf8Title = WideToUtf8(title);

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

    // Check cheap in-process window styles and bounds before cross-process DWM queries
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TRANSPARENT) != 0 || (exStyle & WS_EX_TOOLWINDOW) != 0 || (exStyle & WS_EX_NOACTIVATE) != 0) {
        return false;
    }

    RECT rc = {};
    if (!GetWindowRect(hwnd, &rc)) return false;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 100 || h < 100) return false;

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
            wcscmp(cls, L"MiniWallpaper_SettingsClass") == 0 ||
            wcscmp(cls, L"MiniWallpaper_Daemon") == 0) {
            return false;
        }
    }

    // Skip cloaked windows (virtual desktops / suspended UWP background hosts)
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return false;
    }

    return true;
}

PowerGovernor::PowerGovernor() = default;

PowerGovernor::~PowerGovernor() {
    Shutdown();
}

bool PowerGovernor::Init(HWND messageHwnd) {
    Shutdown();
    m_hwnd = messageHwnd;
    if (m_hwnd) {
        WTSRegisterSessionNotification(messageHwnd, NOTIFY_FOR_THIS_SESSION);
    }
    const DWORD hookFlags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    m_hook_sys = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND,
        nullptr, GovernorWinEventProc, 0, 0, hookFlags
    );
    m_hook_obj = SetWinEventHook(
        EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE,
        nullptr, GovernorWinEventProc, 0, 0, hookFlags
    );

    m_on_battery = IsOnBattery();
    m_last_check_tick = 0;
    s_window_topology_dirty.store(true, std::memory_order_relaxed);
    m_cached_state = PowerState::Active;
    m_last_trigger_info = "Desktop is visible and active";
    return true;
}

PowerState PowerGovernor::GetCurrentState(bool check_maximized, bool check_resources, int ram_threshold, int vram_threshold, ID3D11Device* d3d_device) {
    // Priority 1: Workstation Locked -> Sleep (Immediate 0% CPU)
    if (m_is_locked) {
        m_last_trigger_info = "Workstation Locked (Session Sleep)";
        return PowerState::Sleeping;
    }

    uint64_t now = GetTickCount64();

    // Resource Pressure Check (Throttled to once every 1000ms = 1s for zero CPU overhead)
    if (check_resources && (now - m_last_resource_check_tick >= 1000)) {
        m_last_resource_check_tick = now;
        m_last_ram_pct = QuerySystemRamPercent();
        m_last_vram_pct = QueryGpuVramPercent(d3d_device);

        if (!m_in_resource_sleep) {
            bool over_ram = (ram_threshold > 0 && m_last_ram_pct >= ram_threshold);
            bool over_vram = (vram_threshold > 0 && m_last_vram_pct >= vram_threshold);
            if (over_ram || over_vram) {
                m_over_threshold_counter++;
                if (m_over_threshold_counter >= 2) { // 2 consecutive seconds over threshold
                    m_in_resource_sleep = true;
                }
            } else {
                m_over_threshold_counter = 0;
            }
        } else {
            // Hysteresis of 8% to prevent rapid flapping/toggling
            int recover_ram = (ram_threshold > 10) ? (ram_threshold - 8) : ram_threshold;
            int recover_vram = (vram_threshold > 10) ? (vram_threshold - 8) : vram_threshold;
            bool safe_ram = (m_last_ram_pct <= recover_ram);
            bool safe_vram = (m_last_vram_pct <= recover_vram);
            if (safe_ram && safe_vram) {
                m_in_resource_sleep = false;
                m_over_threshold_counter = 0;
            }
        }
    }

    // Priority 2: Resource Heavy Sleep (Gaming in Windowed Mode / High RAM or VRAM load)
    if (check_resources && m_in_resource_sleep) {
        std::ostringstream oss;
        oss << "High system load [RAM: " << m_last_ram_pct << "% >= " << ram_threshold << "%";
        if (m_last_vram_pct > 0) {
            oss << ", VRAM: " << m_last_vram_pct << "% >= " << vram_threshold << "%";
        }
        oss << "]";
        m_last_trigger_info = oss.str();
        m_cached_state = PowerState::ResourceHeavy;
        return m_cached_state;
    }

    if (check_maximized != m_last_check_maximized) {
        m_last_check_maximized = check_maximized;
        s_window_topology_dirty.store(true, std::memory_order_relaxed);
    }

    // Event-driven evaluation: only re-scan window Z-order when WinEventHook marks topology dirty
    // (debounced to 120ms during active drags) or on a low-frequency 3000ms safety fallback.
    if (now - m_last_check_tick < 120) {
        return m_cached_state;
    }
    const bool topology_changed = s_window_topology_dirty.exchange(false, std::memory_order_relaxed);
    const bool safety_fallback_due = (now - m_last_check_tick >= 3000);
    if (!topology_changed && !safety_fallback_due) {
        return m_cached_state;
    }
    m_last_check_tick = now;

    // Priority 3: Fullscreen 3D Game / App Running -> Pause
    if (IsFullscreenAppRunning()) {
        m_cached_state = PowerState::Paused;
        return m_cached_state;
    }

    // Priority 4: Desktop Occluded by Maximized Window -> Occluded
    if (check_maximized && IsDesktopOccluded()) {
        m_cached_state = PowerState::Occluded;
        return m_cached_state;
    }

    // Priority 5: On Battery -> Reduced FPS
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
        s_window_topology_dirty.store(true, std::memory_order_relaxed);
    }
}

void PowerGovernor::HandlePowerChange(WPARAM wParam) {
    if (wParam == PBT_APMPOWERSTATUSCHANGE) {
        m_on_battery = IsOnBattery();
        m_last_check_tick = 0;
        s_window_topology_dirty.store(true, std::memory_order_relaxed);
    } else if (wParam == PBT_APMSUSPEND) {
        m_is_locked = true;
    } else if (wParam == PBT_APMRESUMEAUTOMATIC) {
        m_is_locked = false;
        m_last_check_tick = 0;
        s_window_topology_dirty.store(true, std::memory_order_relaxed);
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
        if (IsZoomed(fg) || (GetWindowLongPtrW(fg, GWL_STYLE) & WS_MAXIMIZE) != 0) {
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
        // Fast skip for the ~95% hidden helper/COM/IME top-level windows
        if (!IsWindowVisible(hwnd)) {
            return TRUE;
        }

        // Stop scanning immediately when reaching the visible desktop shell layer
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
        if (IsZoomed(hwnd) || (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_MAXIMIZE) != 0) {
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
    if (m_hook_sys) {
        UnhookWinEvent(m_hook_sys);
        m_hook_sys = nullptr;
    }
    if (m_hook_obj) {
        UnhookWinEvent(m_hook_obj);
        m_hook_obj = nullptr;
    }
    if (m_hwnd) {
        WTSUnRegisterSessionNotification(m_hwnd);
        m_hwnd = nullptr;
    }
}

int PowerGovernor::QuerySystemRamPercent() {
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        return static_cast<int>(mem.dwMemoryLoad);
    }
    return 0;
}

int PowerGovernor::QueryGpuVramPercent(ID3D11Device* d3d_device) {
    if (!d3d_device) return 0;
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3d_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
        return 0;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) {
        return 0;
    }
    ComPtr<IDXGIAdapter3> adapter3;
    if (SUCCEEDED(adapter.As(&adapter3))) {
        DXGI_ADAPTER_DESC desc = {};
        adapter->GetDesc(&desc);
        DXGI_QUERY_VIDEO_MEMORY_INFO memInfo = {};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) {
            if (desc.DedicatedVideoMemory > 0 && memInfo.Budget > 0) {
                double totalDedicated = static_cast<double>(desc.DedicatedVideoMemory);
                double availableBudget = static_cast<double>(memInfo.Budget);
                double pressure = (1.0 - (availableBudget / totalDedicated)) * 100.0;
                if (pressure < 0.0) pressure = 0.0;
                if (pressure > 100.0) pressure = 100.0;
                return static_cast<int>(pressure);
            }
        }
    }
    return 0;
}

} // namespace miniwp
