#pragma once
#include <windows.h>

namespace litewp {

class SettingsUI {
public:
    // Open the Settings window on-demand (allocates D3D11 swapchain & ImGui)
    static bool Open(HINSTANCE hInstance);

    // Render one frame if Settings window is active
    static void RenderFrame();

    // Check if the Settings window is currently open
    static bool IsOpen();

    // Hide the Settings window to tray
    static void Close();

    // Shutdown and destroy the Settings window & resources on app exit
    static void Shutdown();

    // Get HWND of Settings window
    static HWND GetHwnd();
};

} // namespace litewp
