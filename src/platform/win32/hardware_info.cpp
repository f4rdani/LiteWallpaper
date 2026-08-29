#include "hardware_info.h"
#include <windows.h>
#include <shlobj.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <sstream>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;

namespace litewp {

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size_needed, NULL, NULL);
    return str;
}

static std::string TrimString(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

CpuInfo HardwareDetector::GetCpuInfo() {
    CpuInfo info;

    // 1. Get CPU Brand String from Registry
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t brandBuf[256] = {};
        DWORD bufSize = sizeof(brandBuf);
        DWORD type = REG_SZ;
        if (RegQueryValueExW(hKey, L"ProcessorNameString", nullptr, &type, reinterpret_cast<LPBYTE>(brandBuf), &bufSize) == ERROR_SUCCESS) {
            info.model_name = TrimString(WideToUtf8(brandBuf));
        }
        RegCloseKey(hKey);
    }

    if (info.model_name.empty()) {
        info.model_name = "Generic x64 Processor";
    }

    // 2. Query Logical Processors
    SYSTEM_INFO sysInfo = {};
    GetSystemInfo(&sysInfo);
    info.logical_cores = (sysInfo.dwNumberOfProcessors > 0) ? sysInfo.dwNumberOfProcessors : 1;

    // 3. Query Physical Cores using GetLogicalProcessorInformation
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (len > 0) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (GetLogicalProcessorInformation(buffer.data(), &len)) {
            int physical = 0;
            for (const auto& proc : buffer) {
                if (proc.Relationship == RelationProcessorCore) {
                    physical++;
                }
            }
            if (physical > 0) {
                info.physical_cores = physical;
            }
        }
    }
    if (info.physical_cores <= 0) {
        info.physical_cores = info.logical_cores;
    }

    return info;
}

std::vector<GpuInfo> HardwareDetector::GetGpuList() {
    std::vector<GpuInfo> gpus;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
        return gpus;
    }

    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            // Skip Microsoft Basic Render Driver software adapter
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                GpuInfo gpu;
                gpu.name = TrimString(WideToUtf8(desc.Description));
                gpu.dedicated_vram_mb = desc.DedicatedVideoMemory / (1024 * 1024);
                gpu.shared_vram_mb = desc.SharedSystemMemory / (1024 * 1024);

                if (desc.VendorId == 0x10DE) {
                    gpu.vendor = "NVIDIA";
                    gpu.is_discrete = true;
                } else if (desc.VendorId == 0x1002) {
                    gpu.vendor = "AMD";
                    gpu.is_discrete = (gpu.dedicated_vram_mb > 512);
                } else if (desc.VendorId == 0x8086) {
                    gpu.vendor = "Intel";
                    gpu.is_discrete = (gpu.dedicated_vram_mb > 2048);
                } else {
                    gpu.vendor = "Other";
                    gpu.is_discrete = (gpu.dedicated_vram_mb > 1024);
                }

                gpu.is_active_device = gpus.empty(); // First enumerated hardware adapter is default active
                gpus.push_back(gpu);
            }
        }
        adapter->Release();
    }

    return gpus;
}

struct MonitorEnumData {
    std::vector<DisplayInfo>* list;
};

static BOOL CALLBACK MonitorEnumCallback(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    auto* data = reinterpret_cast<MonitorEnumData*>(dwData);
    if (!data || !data->list) return TRUE;

    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMonitor, &mi)) {
        DisplayInfo display;
        display.width = mi.rcMonitor.right - mi.rcMonitor.left;
        display.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
        display.pos_x = mi.rcMonitor.left;
        display.pos_y = mi.rcMonitor.top;
        display.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        display.device_id = WideToUtf8(mi.szDevice);

        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            display.refresh_rate = dm.dmDisplayFrequency;
        } else {
            display.refresh_rate = 60;
        }

        DISPLAY_DEVICEW dd = {};
        dd.cb = sizeof(dd);
        if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0)) {
            display.name = TrimString(WideToUtf8(dd.DeviceString));
        }

        if (display.name.empty() || display.name == "Default Monitor") {
            display.name = display.is_primary ? "Primary Display Monitor" : "Secondary Display Monitor";
        }

        data->list->push_back(display);
    }
    return TRUE;
}

std::vector<DisplayInfo> HardwareDetector::GetDisplayList() {
    std::vector<DisplayInfo> displays;
    MonitorEnumData data = { &displays };
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumCallback, reinterpret_cast<LPARAM>(&data));
    return displays;
}

SystemHardwareInfo HardwareDetector::QuerySystemInfo() {
    SystemHardwareInfo sys;
    sys.cpu = GetCpuInfo();
    sys.gpus = GetGpuList();
    sys.displays = GetDisplayList();
    return sys;
}

static std::wstring GetStartupShortcutPath() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, path))) {
        return std::wstring(path) + L"\\LiteWallpaper.lnk";
    }
    return L"";
}

static bool RemoveLegacyStartupShortcut() {
    std::wstring shortcutPath = GetStartupShortcutPath();
    if (!shortcutPath.empty() && GetFileAttributesW(shortcutPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(shortcutPath.c_str());
    }
    return true;
}

bool WindowsAutostart::IsEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t val[MAX_PATH] = {};
    DWORD size = sizeof(val);
    DWORD type = 0;
    LSTATUS res = RegQueryValueExW(hKey, L"LiteWallpaper", nullptr, &type, reinterpret_cast<LPBYTE>(val), &size);
    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS);
}

bool WindowsAutostart::SetEnabled(bool enable) {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeStr(exePath);

    // Always clean up any legacy/duplicate .lnk shortcut in shell:startup folder
    RemoveLegacyStartupShortcut();

    if (enable) {
        // 1. Set standard Registry HKCU\Run entry with quoted path and --startup flag
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
            std::wstring cmd = L"\"" + exeStr + L"\" --startup";
            RegSetValueExW(hKey, L"LiteWallpaper", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), static_cast<DWORD>((cmd.length() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        // 2. Set explicitly ENABLED in StartupApproved\Run (0x02 status) to prevent Task Manager disabling
        HKEY hApprovedRun = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hApprovedRun, nullptr) == ERROR_SUCCESS) {
            BYTE enabledBytes[12] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            RegSetValueExW(hApprovedRun, L"LiteWallpaper", 0, REG_BINARY, enabledBytes, sizeof(enabledBytes));
            RegCloseKey(hApprovedRun);
        }

        // 3. Disable Windows Explorer 10-second startup delay policy for fast instant boot
        HKEY hSerializeKey = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize",
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hSerializeKey, nullptr) == ERROR_SUCCESS) {
            DWORD delay = 0;
            RegSetValueExW(hSerializeKey, L"StartupDelayInMSec", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&delay), sizeof(delay));
            RegCloseKey(hSerializeKey);
        }
    } else {
        // Remove from Registry HKCU\Run
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValueW(hKey, L"LiteWallpaper");
            RegCloseKey(hKey);
        }

        // Clean up from StartupApproved keys
        HKEY hApprovedRun = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", 0, KEY_SET_VALUE, &hApprovedRun) == ERROR_SUCCESS) {
            RegDeleteValueW(hApprovedRun, L"LiteWallpaper");
            RegCloseKey(hApprovedRun);
        }
    }

    return true;
}

void WindowsAutostart::AutoRepairIfMoved() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring currentExe(exePath);

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        wchar_t val[MAX_PATH * 2] = {};
        DWORD size = sizeof(val);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"LiteWallpaper", nullptr, &type, reinterpret_cast<LPBYTE>(val), &size) == ERROR_SUCCESS) {
            std::wstring regVal(val);
            // If registered command does not match currentExe path, repair it!
            if (regVal.find(currentExe) == std::wstring::npos) {
                RegCloseKey(hKey);
                SetEnabled(true);
                return;
            }
        }
        RegCloseKey(hKey);
    }
}

} // namespace litewp
