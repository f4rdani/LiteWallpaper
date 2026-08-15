#include "desktop_injector.h"

namespace litewp {

DesktopInjector::DesktopInjector() = default;

DesktopInjector::~DesktopInjector() {
    Detach();
}

HWND DesktopInjector::FindDesktopWorkerW() {
    // 1. Find Progman window
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;

    // 2. Send undocumented message 0x052C to Progman to spawn WorkerW behind icons
    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &result);

    // 3. Find WorkerW behind SHELLDLL_DefView
    HWND workerw = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        HWND defview = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defview != nullptr) {
            HWND* pResult = reinterpret_cast<HWND*>(lparam);
            *pResult = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&workerw));

    return workerw;
}

bool DesktopInjector::Attach(HWND renderHwnd) {
    if (!renderHwnd) return false;

    m_workerw = FindDesktopWorkerW();
    if (!m_workerw) return false;

    m_render_hwnd = renderHwnd;

    // Remove window decorations and make it a child window
    LONG_PTR style = GetWindowLongPtrW(renderHwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_CHILD;
    SetWindowLongPtrW(renderHwnd, GWL_STYLE, style);

    // Set WorkerW as parent
    SetParent(renderHwnd, m_workerw);

    // Resize to cover entire virtual screen across all monitors
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    SetWindowPos(
        renderHwnd,
        nullptr,
        x, y, cx, cy,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW
    );

    return true;
}

void DesktopInjector::Detach() {
    if (m_render_hwnd) {
        SetParent(m_render_hwnd, nullptr);
        m_render_hwnd = nullptr;
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
