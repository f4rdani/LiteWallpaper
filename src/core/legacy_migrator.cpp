#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "legacy_migrator.h"
#include "logger.h"
#include "win_utils.h"
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace miniwp {

namespace {

void FixLegacyRegSzPath(HKEY rootKey, const wchar_t* subKey, const wchar_t* valueName,
                        const std::wstring& oldRoot, const std::wstring& newRoot) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(rootKey, subKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return;

    wchar_t val[1024] = {};
    DWORD size = sizeof(val);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(val), &size) == ERROR_SUCCESS && type == REG_SZ) {
        std::wstring value(val);
        size_t pos = value.find(oldRoot);
        if (pos != std::wstring::npos) {
            std::wstring fixed = value.substr(0, pos) + newRoot + value.substr(pos + oldRoot.size());
            if (GetFileAttributesW(fixed.c_str()) != INVALID_FILE_ATTRIBUTES) {
                RegSetValueExW(hKey, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(fixed.c_str()),
                               static_cast<DWORD>((fixed.length() + 1) * sizeof(wchar_t)));
            }
        }
    }
    RegCloseKey(hKey);
}

void MigrateLegacyAppData(const wchar_t* appDataPath) {
    fs::path legacyDir = fs::path(appDataPath) / L"LiteWallpaper";
    fs::path newDir = fs::path(appDataPath) / L"MiniWallpaper";

    std::error_code ec;
    if (fs::exists(newDir, ec)) return;
    if (!fs::exists(legacyDir, ec)) return;

    fs::rename(legacyDir, newDir, ec);
    if (ec) {
        ec.clear();
        fs::copy(legacyDir, newDir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            Logger::Error("Legacy data migration failed: ", ec.message());
            return;
        }
        std::error_code removeEc;
        fs::remove_all(legacyDir, removeEc);
    }
    Logger::Info("Migrated legacy LiteWallpaper appdata to MiniWallpaper");
}

void MigrateLegacyRegistryPaths(const wchar_t* appDataPath) {
    std::wstring oldRoot = std::wstring(appDataPath) + L"\\LiteWallpaper";
    std::wstring newRoot = std::wstring(appDataPath) + L"\\MiniWallpaper";

    FixLegacyRegSzPath(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"Wallpaper", oldRoot, newRoot);

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        wchar_t val[1024] = {};
        DWORD size = sizeof(val);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"SCRNSAVE.EXE", nullptr, &type, reinterpret_cast<LPBYTE>(val), &size) == ERROR_SUCCESS && type == REG_SZ) {
            std::wstring value(val);
            size_t pos = value.find(L"LiteWallpaper.scr");
            if (pos != std::wstring::npos) {
                std::wstring fixed = value.substr(0, pos) + L"MiniWallpaper.scr" + value.substr(pos + wcslen(L"LiteWallpaper.scr"));
                if (GetFileAttributesW(fixed.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    RegSetValueExW(hKey, L"SCRNSAVE.EXE", 0, REG_SZ, reinterpret_cast<const BYTE*>(fixed.c_str()),
                                   static_cast<DWORD>((fixed.length() + 1) * sizeof(wchar_t)));
                }
            }
        }
        RegCloseKey(hKey);
    }

    const wchar_t* creativeKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Lock Screen\\Creative";
    FixLegacyRegSzPath(HKEY_CURRENT_USER, creativeKey, L"LockScreenImage", oldRoot, newRoot);
    FixLegacyRegSzPath(HKEY_CURRENT_USER, creativeKey, L"LandscapeAssetPath", oldRoot, newRoot);
    FixLegacyRegSzPath(HKEY_CURRENT_USER, creativeKey, L"PortraitAssetPath", oldRoot, newRoot);

    const wchar_t* cspKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\PersonalizationCSP";
    FixLegacyRegSzPath(HKEY_CURRENT_USER, cspKey, L"LockScreenImagePath", oldRoot, newRoot);
    FixLegacyRegSzPath(HKEY_CURRENT_USER, cspKey, L"LockScreenImageUrl", oldRoot, newRoot);
}

void CleanupLegacyStartupDelayOverride() {
    HKEY hSerializeKey = nullptr;
    if (RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize",
        0,
        KEY_QUERY_VALUE | KEY_SET_VALUE,
        &hSerializeKey
    ) == ERROR_SUCCESS) {
        DWORD val = 1;
        DWORD size = sizeof(val);
        DWORD type = 0;
        if (RegQueryValueExW(hSerializeKey, L"StartupDelayInMSec", nullptr, &type, reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS &&
            type == REG_DWORD && val == 0) {
            RegDeleteValueW(hSerializeKey, L"StartupDelayInMSec");
        }
        RegCloseKey(hSerializeKey);
    }
}

} // namespace

void LegacyMigrator::RunOnce() {
    wchar_t appDataPath[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath))) return;

    std::wstring appDir = std::wstring(appDataPath) + L"\\MiniWallpaper";
    std::wstring sentinelPath = appDir + L"\\.migrated_v2";
    if (GetFileAttributesW(sentinelPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MigrateLegacyAppData(appDataPath);
        MigrateLegacyRegistryPaths(appDataPath);

        CreateDirectoryW(appDir.c_str(), nullptr);
        HANDLE hFile = CreateFileW(sentinelPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
        }
    }

    // One-time cleanup of legacy Explorer\Serialize\StartupDelayInMSec = 0 override
    std::wstring startupCleanSentinel = appDir + L"\\.migrated_v2_startup_clean";
    if (GetFileAttributesW(startupCleanSentinel.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CleanupLegacyStartupDelayOverride();
        CreateDirectoryW(appDir.c_str(), nullptr);
        HANDLE hClean = CreateFileW(startupCleanSentinel.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (hClean != INVALID_HANDLE_VALUE) {
            CloseHandle(hClean);
        }
    }
}

} // namespace miniwp
