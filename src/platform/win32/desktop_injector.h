#pragma once
#include <windows.h>

namespace miniwp {

class DesktopInjector {
public:
    DesktopInjector();
    ~DesktopInjector();

    // Find WorkerW handle behind desktop icons and attach render window
    bool Attach(HWND renderHwnd, bool showImmediately = true);

    // Detach window from desktop
    void Detach();

    // Re-attach (called on Explorer restart or monitor layout change)
    bool Reattach(HWND renderHwnd, bool showImmediately = true);

    bool IsAttached() const noexcept;
    bool IsAttachedValid() const;
    HWND GetWorkerW() const noexcept { return m_workerw; }

    // Register hidden top-level receiver for Explorer restart ("TaskbarCreated") broadcast
    void RegisterExplorerRestart(HWND targetHwnd);
    UINT GetTaskbarRestartMsg() const noexcept { return m_taskbar_restart_msg; }

private:
    HWND m_workerw = nullptr;
    HWND m_render_hwnd = nullptr;
    HWND m_msg_receiver = nullptr;
    UINT m_taskbar_restart_msg = 0;

    static HWND FindDesktopWorkerW();
    static LRESULT CALLBACK MsgReceiverWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace miniwp
