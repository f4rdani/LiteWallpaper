#include "desktop_injector.h"

namespace litewp {

DesktopInjector::DesktopInjector() = default;

DesktopInjector::~DesktopInjector() {
    Detach();
}

HWND DesktopInjector::FindDesktopWorkerW() {
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

    // 6. Fallback: attach directly to Progman (desktop icons hidden case)
    return progman;
}

bool DesktopInjector::Attach(HWND renderHwnd) {
    if (!renderHwnd) return false;

    m_workerw = FindDesktopWorkerW();
    if (!m_workerw) return false;

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
        // Restore window to a normal hidden top-level window so it stops
        // covering the desktop wallpaper.
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
    return (parent == m_workerw);
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

void DesktopInjector::RegisterExplorerRestart(HWND /*messageHwnd*/) {
    m_taskbar_restart_msg = RegisterWindowMessageW(L"TaskbarCreated");
}

UINT DesktopInjector::GetTaskbarRestartMsg() const {
    return m_taskbar_restart_msg;
}

} // namespace litewp
