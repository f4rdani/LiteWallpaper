#include "desktop_injector.h"
#include <cwchar>

namespace miniwp {

static UINT g_taskbar_created_msg = 0;

DesktopInjector::DesktopInjector() = default;

DesktopInjector::~DesktopInjector() {
    Detach();
    if (m_msg_receiver) {
        DestroyWindow(m_msg_receiver);
        m_msg_receiver = nullptr;
    }
}

HWND DesktopInjector::FindDesktopWorkerW() {
    HWND progman = FindWindowW(L"Progman", L"Program Manager");
    if (!progman) progman = FindWindowW(L"Progman", nullptr);
    if (!progman) progman = GetShellWindow();
    if (!progman) {
        EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
            wchar_t cls[64] = {};
            if (GetClassNameW(hwnd, cls, 64) > 0 && wcscmp(cls, L"Progman") == 0) {
                *reinterpret_cast<HWND*>(lparam) = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&progman));
    }

    if (!progman) return nullptr;

    // Spawn WorkerW behind desktop icons via undocumented shell message 0x052C
    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, 0x052C, 0x0000000D, 0, SMTO_NORMAL, 1000, &result);
    SendMessageTimeoutW(progman, 0x052C, 0x0000000D, 1, SMTO_NORMAL, 1000, &result);

    // Strategy A (Win10/11 classic): top-level WorkerW sibling after the window hosting SHELLDLL_DefView
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

    // Strategy B (Win11 24H2+): Child WorkerW directly under Progman
    EnumChildWindows(progman, [](HWND hwnd, LPARAM lparam) -> BOOL {
        wchar_t cls[64] = {};
        if (GetClassNameW(hwnd, cls, 64) > 0 && wcscmp(cls, L"WorkerW") == 0) {
            if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) == nullptr) {
                *reinterpret_cast<HWND*>(lparam) = hwnd;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&workerw));
    if (workerw) return workerw;

    // Fallback: attach directly to Progman
    return progman;
}

bool DesktopInjector::Attach(HWND renderHwnd, bool showImmediately) {
    if (!renderHwnd) return false;

    m_workerw = FindDesktopWorkerW();
    if (!m_workerw) {
        ShowWindow(renderHwnd, SW_HIDE);
        return false;
    }

    m_render_hwnd = renderHwnd;

    LONG_PTR style = GetWindowLongPtrW(renderHwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_POPUP);
    style |= WS_CHILD;
    SetWindowLongPtrW(renderHwnd, GWL_STYLE, style);

    SetParent(renderHwnd, m_workerw);

    RECT rc;
    int w = 0, h = 0;
    if (GetClientRect(m_workerw, &rc) && rc.right > 0 && rc.bottom > 0) {
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    } else {
        w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }

    UINT swpFlags = SWP_NOACTIVATE | (showImmediately ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    SetWindowPos(renderHwnd, HWND_BOTTOM, 0, 0, w, h, swpFlags);
    if (showImmediately) {
        ShowWindow(renderHwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(renderHwnd);
    }
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
    m_workerw = nullptr;
}

bool DesktopInjector::Reattach(HWND renderHwnd, bool showImmediately) {
    Detach();
    return Attach(renderHwnd, showImmediately);
}

bool DesktopInjector::IsAttached() const noexcept {
    return m_workerw != nullptr && m_render_hwnd != nullptr;
}

bool DesktopInjector::IsAttachedValid() const {
    if (!IsAttached()) return false;
    if (!IsWindow(m_workerw) || !IsWindow(m_render_hwnd)) return false;
    return GetParent(m_render_hwnd) == m_workerw;
}

LRESULT CALLBACK DesktopInjector::MsgReceiverWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_taskbar_created_msg && msg == g_taskbar_created_msg) {
        HWND target = reinterpret_cast<HWND>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (target && IsWindow(target)) {
            // Forward TaskbarCreated to the render child window so BOTH tray icon and WorkerW reattach!
            PostMessageW(target, g_taskbar_created_msg, 0, 0);
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
        wc.lpszClassName = L"MiniWallpaper_MsgReceiver";
        RegisterClassExW(&wc);

        // Zero-DWM-surface hidden top-level window (receives shell broadcast messages)
        m_msg_receiver = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"MiniWallpaper_MsgReceiver",
            L"",
            WS_POPUP,
            0, 0, 0, 0,
            nullptr, nullptr, wc.hInstance, nullptr
        );
    }

    if (m_msg_receiver) {
        SetWindowLongPtrW(m_msg_receiver, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(targetHwnd));
    }
}

} // namespace miniwp
