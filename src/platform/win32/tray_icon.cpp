#include "tray_icon.h"

namespace litewp {

TrayIcon::TrayIcon() = default;

TrayIcon::~TrayIcon() {
    Destroy();
}

bool TrayIcon::Create(HWND hwnd, TrayCallback callback) {
    m_callback = callback;
    
    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = hwnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    m_nid.uCallbackMessage = WM_APP_TRAYICON;
    m_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!m_nid.hIcon) {
        m_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
    }
    if (!m_nid.hIcon) {
        m_nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION
    }
    wcscpy_s(m_nid.szTip, L"LiteWallpaper");
    
    Shell_NotifyIconW(NIM_ADD, &m_nid);
    
    // Create Context Menu
    m_menu = CreatePopupMenu();
    AppendMenuW(m_menu, MF_STRING, 4, L"Control Panel");
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m_menu, MF_STRING, 1, L"Play / Pause");
    AppendMenuW(m_menu, MF_STRING, 2, L"Mute / Unmute");
    AppendMenuW(m_menu, MF_STRING, 3, L"Change Wallpaper...");
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m_menu, MF_STRING, 5, L"Exit");
    
    return true;
}

void TrayIcon::SetTooltip(const wchar_t* text) {
    if (text) {
        wcsncpy_s(m_nid.szTip, text, _countof(m_nid.szTip) - 1);
        Shell_NotifyIconW(NIM_MODIFY, &m_nid);
    }
}

void TrayIcon::HandleMessage(WPARAM /*wParam*/, LPARAM lParam) {
    UINT msg = LOWORD(lParam);
    if (msg == WM_RBUTTONUP || msg == WM_CONTEXTMENU) {
        ShowContextMenu(m_nid.hWnd);
    } else if (msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK) {
        if (m_callback) {
            m_callback(TrayAction::OpenSettings);
        }
    }
}

void TrayIcon::ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    if (m_menu) {
        SetMenuDefaultItem(m_menu, 4, FALSE);
    }
    int cmd = TrackPopupMenu(m_menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    
    if (!m_callback) return;

    switch (cmd) {
        case 1: m_callback(TrayAction::PauseResume); break;
        case 2: m_callback(TrayAction::MuteUnmute); break;
        case 3: m_callback(TrayAction::ChangeWallpaper); break;
        case 4: m_callback(TrayAction::OpenSettings); break;
        case 5: m_callback(TrayAction::Exit); break;
        default: break;
    }
}

void TrayIcon::Destroy() {
    if (m_nid.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_nid.hWnd = nullptr;
    }
    if (m_menu) {
        DestroyMenu(m_menu);
        m_menu = nullptr;
    }
}

} // namespace litewp
