#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "hardware_info.h"
#include "core/win_utils.h"
#include <windows.h>
#include <shlobj.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace miniwp {

static std::string TrimString(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

CpuInfo HardwareDetector::GetCpuInfo() {
    CpuInfo info;

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

    SYSTEM_INFO sysInfo = {};
    GetSystemInfo(&sysInfo);
    info.logical_cores = (sysInfo.dwNumberOfProcessors > 0) ? static_cast<int>(sysInfo.dwNumberOfProcessors) : 1;

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

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
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

            gpu.is_active_device = gpus.empty();
            gpus.push_back(std::move(gpu));
        }
    }

    return gpus;
}

struct MonitorEnumData {
    std::vector<DisplayInfo>* list;
};

static BOOL CALLBACK MonitorEnumCallback(HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData) {
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
        display.refresh_rate = EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
            ? static_cast<int>(dm.dmDisplayFrequency)
            : 60;

        DISPLAY_DEVICEW dd = {};
        dd.cb = sizeof(dd);
        if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0)) {
            display.name = TrimString(WideToUtf8(dd.DeviceString));
        }

        if (display.name.empty() || display.name == "Default Monitor") {
            display.name = display.is_primary ? "Primary Display Monitor" : "Secondary Display Monitor";
        }

        data->list->push_back(std::move(display));
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
        return std::wstring(path) + L"\\MiniWallpaper.lnk";
    }
    return {};
}

static void RemoveLegacyStartupShortcut() {
    std::wstring shortcutPath = GetStartupShortcutPath();
    if (!shortcutPath.empty() && GetFileAttributesW(shortcutPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(shortcutPath.c_str());
    }
}

bool WindowsAutostart::IsEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t val[MAX_PATH] = {};
    DWORD size = sizeof(val);
    DWORD type = 0;
    LSTATUS res = RegQueryValueExW(hKey, L"MiniWallpaper", nullptr, &type, reinterpret_cast<LPBYTE>(val), &size);
    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS);
}

bool WindowsAutostart::SetEnabled(bool enable, int /*priority*/) {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeStr(exePath);

    RemoveLegacyStartupShortcut();

    if (enable) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
            std::wstring cmd = L"\"" + exeStr + L"\" --startup";
            RegSetValueExW(hKey, L"MiniWallpaper", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), static_cast<DWORD>((cmd.length() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        HKEY hApprovedRun = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
                            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hApprovedRun, nullptr) == ERROR_SUCCESS) {
            BYTE enabledBytes[12] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            RegSetValueExW(hApprovedRun, L"MiniWallpaper", 0, REG_BINARY, enabledBytes, sizeof(enabledBytes));
            RegCloseKey(hApprovedRun);
        }
    } else {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValueW(hKey, L"MiniWallpaper");
            RegCloseKey(hKey);
        }

        HKEY hApprovedRun = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", 0, KEY_SET_VALUE, &hApprovedRun) == ERROR_SUCCESS) {
            RegDeleteValueW(hApprovedRun, L"MiniWallpaper");
            RegCloseKey(hApprovedRun);
        }
    }

    return true;
}

void WindowsAutostart::AutoRepairIfMoved(int priority) {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring currentExe(exePath);

    RemoveLegacyStartupShortcut();

    bool needsRepair = false;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        wchar_t val[MAX_PATH * 2] = {};
        DWORD size = sizeof(val);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"MiniWallpaper", nullptr, &type, reinterpret_cast<LPBYTE>(val), &size) == ERROR_SUCCESS) {
            std::wstring regVal(val);
            if (regVal.find(currentExe) == std::wstring::npos) {
                needsRepair = true;
            }
        }
        RegCloseKey(hKey);
    } else {
        needsRepair = true;
    }

    if (needsRepair) {
        SetEnabled(true, priority);
    }
}

void WindowsAutostart::MigrateLegacy() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        wchar_t legacyVal[MAX_PATH * 2] = {};
        DWORD legacySize = sizeof(legacyVal);
        DWORD type = 0;
        bool legacySet = (RegQueryValueExW(hKey, L"LiteWallpaper", nullptr, &type, reinterpret_cast<LPBYTE>(legacyVal), &legacySize) == ERROR_SUCCESS);

        if (legacySet) {
            wchar_t newVal[MAX_PATH * 2] = {};
            DWORD newSize = sizeof(newVal);
            bool newSet = (RegQueryValueExW(hKey, L"MiniWallpaper", nullptr, &type, reinterpret_cast<LPBYTE>(newVal), &newSize) == ERROR_SUCCESS);

            if (!newSet) {
                wchar_t exePath[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" --startup";
                RegSetValueExW(hKey, L"MiniWallpaper", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), static_cast<DWORD>((cmd.length() + 1) * sizeof(wchar_t)));
            }
            RegDeleteValueW(hKey, L"LiteWallpaper");
        }
        RegCloseKey(hKey);
    }

    HKEY hApprovedRun = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hApprovedRun) == ERROR_SUCCESS) {
        BYTE legacyBytes[12] = {};
        DWORD legacySize = sizeof(legacyBytes);
        DWORD type = 0;
        bool legacySet = (RegQueryValueExW(hApprovedRun, L"LiteWallpaper", nullptr, &type, legacyBytes, &legacySize) == ERROR_SUCCESS);

        if (legacySet) {
            BYTE newBytes[12] = {};
            DWORD newSize = sizeof(newBytes);
            bool newSet = (RegQueryValueExW(hApprovedRun, L"MiniWallpaper", nullptr, &type, newBytes, &newSize) == ERROR_SUCCESS);
            if (!newSet) {
                RegSetValueExW(hApprovedRun, L"MiniWallpaper", 0, REG_BINARY, legacyBytes, legacySize);
            }
            RegDeleteValueW(hApprovedRun, L"LiteWallpaper");
        }
        RegCloseKey(hApprovedRun);
    }

    wchar_t startupPath[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, startupPath))) {
        std::wstring legacyShortcut = std::wstring(startupPath) + L"\\LiteWallpaper.lnk";
        DeleteFileW(legacyShortcut.c_str());
    }
}

} // namespace miniwp
