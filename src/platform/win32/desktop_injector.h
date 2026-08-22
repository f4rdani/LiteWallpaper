#pragma once
#include <windows.h>
#include <vector>
#include <string>

namespace litewp {

struct MonitorInfo {
    HMONITOR handle = nullptr;
    RECT rect = {0, 0, 0, 0};     // Monitor rectangle in virtual screen coordinates
    std::wstring device_id;        // Monitor device path
    bool is_primary = false;
};

class DesktopInjector {
public:
    DesktopInjector();
    ~DesktopInjector();

    // Find WorkerW handle and attach render window behind desktop icons
    bool Attach(HWND renderHwnd);
    
    // Detach window from desktop
    void Detach();
    
    // Re-attach (called on Explorer restart or monitor layout change)
    bool Reattach(HWND renderHwnd);
    
    // Check if currently attached
    bool IsAttached() const;
    
    // Check if the render window is still a child of a valid WorkerW
    bool IsAttachedValid() const;
    
    // Check if attached to genuine WorkerW (not fallback Progman)
    bool IsTrueWorkerW() const;

    // Get WorkerW handle
    HWND GetWorkerW() const;
    
    // Enumerate active display monitors
    static std::vector<MonitorInfo> EnumerateMonitors();
    
    // Register for Explorer restart notification ("TaskbarCreated")
    void RegisterExplorerRestart(HWND targetHwnd);

    UINT GetTaskbarRestartMsg() const;

private:
    HWND m_workerw = nullptr;
    HWND m_render_hwnd = nullptr;
    HWND m_msg_receiver = nullptr;
    UINT m_taskbar_restart_msg = 0;
    bool m_is_fallback = false;
    
    static HWND FindDesktopWorkerW(bool* out_is_fallback = nullptr);
    static bool IsWorkerWClass(HWND hwnd);
    static LRESULT CALLBACK MsgReceiverWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace litewp
