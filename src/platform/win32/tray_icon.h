#pragma once
#include <windows.h>

#define WM_APP_TRAYICON (WM_APP + 1)

namespace litewp {

// Callback action triggered from system tray menu
enum class TrayAction {
    PauseResume,
    OpenSettings,
    ChangeWallpaper,
    MuteUnmute,
    Exit
};

using TrayCallback = void(*)(TrayAction action);

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    // Create system tray icon. hwnd = window to receive messages.
    bool Create(HWND hwnd, TrayCallback callback);
    
    // Update tooltip text
    void SetTooltip(const wchar_t* text);
    
    // Handle WM_APP + 1 messages from WndProc
    void HandleMessage(WPARAM wParam, LPARAM lParam);
    
    void Destroy();

private:
    NOTIFYICONDATAW m_nid = {};
    TrayCallback m_callback = nullptr;
    HMENU m_menu = nullptr;
    
    void ShowContextMenu(HWND hwnd);
};

} // namespace litewp
