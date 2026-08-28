#pragma once
#include <windows.h>
#include <wtsapi32.h>
#include <cstdint>

namespace litewp {

enum class PowerState {
    Active,         // Desktop visible, no fullscreen app → render at target FPS
    Reduced,        // On battery → render at reduced FPS
    Paused,         // Fullscreen app/game detected → stop rendering completely
    Occluded,       // Maximized window / desktop covered → stop rendering completely (0% CPU/GPU)
    Sleeping,       // Workstation locked → stop everything (0% CPU)
};

class PowerGovernor {
public:
    PowerGovernor();
    ~PowerGovernor();

    // Initialize session notifications
    bool Init(HWND messageHwnd);
    
    // Poll current state (throttled internally to conserve CPU)
    PowerState GetCurrentState(bool check_maximized = true);
    
    // Handle WM_WTSSESSION_CHANGE message from WndProc
    void HandleSessionChange(WPARAM wParam);
    
    // Handle WM_POWERBROADCAST
    void HandlePowerChange(WPARAM wParam);
    
    void Shutdown();

private:
    HWND m_hwnd = nullptr;
    bool m_is_locked = false;
    bool m_on_battery = false;
    
    PowerState m_cached_state = PowerState::Active;
    uint64_t   m_last_check_tick = 0;
    
    bool IsFullscreenAppRunning();
    bool IsDesktopOccluded();
    bool IsOnBattery();
};

} // namespace litewp
