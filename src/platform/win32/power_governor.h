#pragma once
#include <windows.h>
#include <wtsapi32.h>
#include <d3d11.h>
#include <cstdint>
#include <string>

namespace litewp {

enum class PowerState {
    Active,         // Desktop visible, no fullscreen app → render at target FPS
    Reduced,        // On battery → render at reduced FPS
    Paused,         // Fullscreen app/game detected → stop rendering completely
    Occluded,       // Maximized window / desktop covered → stop rendering completely (0% CPU/GPU)
    Sleeping,       // Workstation locked → stop everything (0% CPU)
    ResourceHeavy,  // High system RAM/VRAM load (Gaming in windowed mode) → deep sleep
};

class PowerGovernor {
public:
    PowerGovernor();
    ~PowerGovernor();

    // Initialize session notifications
    bool Init(HWND messageHwnd);
    
    // Poll current state (throttled internally to conserve CPU)
    PowerState GetCurrentState(bool check_maximized = true,
                               bool check_resources = true,
                               int ram_threshold = 80,
                               int vram_threshold = 80,
                               ID3D11Device* d3d_device = nullptr);
    
    // Detailed telemetry
    std::string GetLastTriggerInfo() const { return m_last_trigger_info; }
    int GetSystemRamPercent() const { return m_last_ram_pct; }
    int GetGpuVramPercent() const { return m_last_vram_pct; }
    bool IsInResourceSleep() const { return m_in_resource_sleep; }

    // Handle WM_WTSSESSION_CHANGE message from WndProc
    void HandleSessionChange(WPARAM wParam);
    
    // Handle WM_POWERBROADCAST
    void HandlePowerChange(WPARAM wParam);
    
    void Shutdown();

    // Query helper functions
    static int QuerySystemRamPercent();
    static int QueryGpuVramPercent(ID3D11Device* d3d_device);

private:
    HWND m_hwnd = nullptr;
    bool m_is_locked = false;
    bool m_on_battery = false;
    
    PowerState  m_cached_state = PowerState::Active;
    uint64_t    m_last_check_tick = 0;
    uint64_t    m_last_resource_check_tick = 0;
    std::string m_last_trigger_info = "Desktop is active";
    
    int  m_last_ram_pct = 0;
    int  m_last_vram_pct = 0;
    int  m_over_threshold_counter = 0;
    bool m_in_resource_sleep = false;

    bool IsFullscreenAppRunning();
    bool IsDesktopOccluded();
    bool IsOnBattery();
};

} // namespace litewp
