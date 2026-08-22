#include "desktop_injector.h"
#include <iostream>

#define WM_APP_ATTACH (WM_APP + 11)

namespace litewp {

static UINT g_taskbar_created_msg = 0;

DesktopInjector::DesktopInjector() = default;

DesktopInjector::~DesktopInjector() {
    Detach();
    if (m_msg_receiver) {
        DestroyWindow(m_msg_receiver);
        m_msg_receiver = nullptr;
    }
}

bool DesktopInjector::IsWorkerWClass(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, 64) == 0) return false;
    return (wcscmp(cls, L"WorkerW") == 0);
}

HWND DesktopInjector::FindDesktopWorkerW(bool* out_is_fallback) {
    if (out_is_fallback) *out_is_fallback = false;

    // 1. Find Progman window
    HWND progman = FindWindowW(L"Progman", L"Program Manager");
    if (!progman) {
        progman = FindWindowW(L"Progman", nullptr);
    }
    if (!progman) {
        progman = GetShellWindow();
    }
    if (!progman) return nullptr;

    // 2. Send message 0x052C to Progman to spawn WorkerW behind desktop icons
    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, 0x052C, 0x0000000D, 0x00000001, SMTO_NORMAL, 1000, &result);
    SendMessageTimeoutW(progman, 0x052C, 0x0000000D, 0x00000000, SMTO_NORMAL, 1000, &result);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);

    // 3. Strategy A (classic, Win10/11): top-level WorkerW that is the sibling following
    //    the window containing SHELLDLL_DefView (behind desktop icons).
    HWND workerw = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        HWND defview = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defview != nullptr) {
            HWND nextWorker = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr);
            if (nextWorker != nullptr) {
                *reinterpret_cast<HWND*>(lparam) = nextWorker;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&workerw));
    if (workerw) return workerw;

    // 4. Strategy B (Win11 24H2+): the background WorkerW is a CHILD of Progman,
    //    while SHELLDLL_DefView is also a direct child of Progman. The wallpaper layer
    //    is the bottom-most Progman child WorkerW that does NOT contain the icons.
    HWND defviewInProgman = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defviewInProgman) {
        HWND bgWorkerW = nullptr;
        EnumChildWindows(progman, [](HWND hwnd, LPARAM lparam) -> BOOL {
            wchar_t cls[256];
            if (GetClassNameW(hwnd, cls, 256) == 0 || wcscmp(cls, L"WorkerW") != 0) {
                return TRUE;
            }
            // Skip any WorkerW that contains the icons layer
            if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr) {
                return TRUE;
            }
            // Keep the LAST (bottom-most) WorkerW child of Progman: that is the
            // background wallpaper layer closest to the desktop.
            *reinterpret_cast<HWND*>(lparam) = hwnd;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&bgWorkerW));
        if (bgWorkerW) return bgWorkerW;
    }

    // 5. Strategy C: Any top-level WorkerW that does NOT contain icons
    HWND standaloneWorker = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        wchar_t cls[256];
        if (GetClassNameW(hwnd, cls, 256) && wcscmp(cls, L"WorkerW") == 0) {
            if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) == nullptr) {
                *reinterpret_cast<HWND*>(lparam) = hwnd;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&standaloneWorker));
    if (standaloneWorker) return standaloneWorker;

    // 6. Fallback: attach directly to Progman (desktop icons hidden case or early boot)
    if (out_is_fallback) *out_is_fallback = true;
    return progman;
}

bool DesktopInjector::Attach(HWND renderHwnd) {
    if (!renderHwnd) return false;

    bool is_fallback = false;
    m_workerw = FindDesktopWorkerW(&is_fallback);
    if (!m_workerw) return false;

    m_is_fallback = is_fallback;
    m_render_hwnd = renderHwnd;

    // Remove window decorations and make it a child window
    LONG_PTR style = GetWindowLongPtrW(renderHwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_POPUP);
    style |= WS_CHILD;
    SetWindowLongPtrW(renderHwnd, GWL_STYLE, style);

    // Set WorkerW as parent
    SetParent(renderHwnd, m_workerw);

    // Ensure WorkerW is shown and visible
    ShowWindow(m_workerw, SW_SHOW);
    UpdateWindow(m_workerw);

    // Resize child to cover the parent WorkerW client area
    RECT rc;
    int w = 0, h = 0;
    if (GetClientRect(m_workerw, &rc) && rc.right > 0 && rc.bottom > 0) {
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    } else {
        w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }

    SetWindowPos(
        renderHwnd,
        HWND_BOTTOM,
        0, 0, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW
    );

    ShowWindow(renderHwnd, SW_SHOW);
    UpdateWindow(renderHwnd);
    return true;
}

void DesktopInjector::Detach() {
    if (m_render_hwnd) {
        ShowWindow(m_render_hwnd, SW_HIDE);
        LONG_PTR style = GetWindowLongPtrW(m_render_hwnd, GWL_STYLE);
        style &= ~WS_CHILD;
        style |= WS_POPUP;
        SetWindowLongPtrW(m_render_hwnd, GWL_STYLE, style);
        SetParent(m_render_hwnd, nullptr);
        m_render_hwnd = nullptr;
    }
    if (m_workerw) {
        InvalidateRect(m_workerw, nullptr, TRUE);
        UpdateWindow(m_workerw);
    }
    m_workerw = nullptr;
    m_is_fallback = false;
}

bool DesktopInjector::Reattach(HWND renderHwnd) {
    Detach();
    return Attach(renderHwnd);
}

bool DesktopInjector::IsAttached() const {
    return m_workerw != nullptr && m_render_hwnd != nullptr;
}

bool DesktopInjector::IsAttachedValid() const {
    if (!IsAttached()) return false;
    if (!IsWindow(m_workerw)) return false;
    if (!IsWindow(m_render_hwnd)) return false;
    HWND parent = GetParent(m_render_hwnd);
    if (parent != m_workerw) return false;

    // If currently attached to fallback Progman, check if true WorkerW has spawned
    if (m_is_fallback) {
        bool still_fallback = false;
        HWND trueWorker = FindDesktopWorkerW(&still_fallback);
        if (!still_fallback && trueWorker && trueWorker != m_workerw) {
            return false; // Force reattach to upgrade to genuine WorkerW!
        }
    }

    return true;
}

bool DesktopInjector::IsTrueWorkerW() const {
    return IsAttached() && !m_is_fallback && IsWorkerWClass(m_workerw);
}

HWND DesktopInjector::GetWorkerW() const {
    return m_workerw;
}

std::vector<MonitorInfo> DesktopInjector::EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hmon, HDC, LPRECT, LPARAM lparam) -> BOOL {
        auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hmon, &mi)) {
            MonitorInfo info;
            info.handle = hmon;
            info.rect = mi.rcMonitor;
            info.device_id = mi.szDevice;
            info.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            list->push_back(info);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

LRESULT CALLBACK DesktopInjector::MsgReceiverWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_taskbar_created_msg && msg == g_taskbar_created_msg) {
        HWND target = reinterpret_cast<HWND>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (target && IsWindow(target)) {
            PostMessageW(target, WM_APP_ATTACH, 0, 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void DesktopInjector::RegisterExplorerRestart(HWND targetHwnd) {
    m_taskbar_restart_msg = RegisterWindowMessageW(L"TaskbarCreated");
    g_taskbar_created_msg = m_taskbar_restart_msg;

    if (!m_msg_receiver) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = MsgReceiverWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LiteWallpaper_MsgReceiver";
        RegisterClassExW(&wc);

        // Top-level hidden window (WS_POPUP) to reliably receive HWND_BROADCAST TaskbarCreated
        m_msg_receiver = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"LiteWallpaper_MsgReceiver",
            L"",
            WS_POPUP,
            0, 0, 0, 0,
            nullptr, nullptr, wc.hInstance, nullptr
        );

        if (m_msg_receiver) {
            SetWindowLongPtrW(m_msg_receiver, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(targetHwnd));
        }
    }
}

UINT DesktopInjector::GetTaskbarRestartMsg() const {
    return m_taskbar_restart_msg;
}

} // namespace litewp
