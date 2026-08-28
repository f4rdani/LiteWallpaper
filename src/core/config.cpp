#include "config.h"
#include "video_optimizer.h"
#include "ui/thumbnail_manager.h"
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace litewp {

Config g_config;

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size_needed);
    return wstr;
}

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size_needed, NULL, NULL);
    return str;
}

std::string Config::GetConfigDir() {
    wchar_t appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
        std::wstring dir = std::wstring(appDataPath) + L"\\LiteWallpaper";
        CreateDirectoryW(dir.c_str(), NULL);
        return WideToUtf8(dir);
    }
    return ".";
}

std::string Config::GetConfigFilePath() {
    return GetConfigDir() + "\\config.json";
}

bool Config::Load() {
    m_config.config_path = GetConfigFilePath();
    std::wstring wpath = Utf8ToWide(m_config.config_path);

    std::ifstream file(wpath);
    if (!file.is_open()) {
        // File does not exist, initialize defaults and save
        m_config.target_fps = 30;
        m_config.pause_on_fullscreen = true;
        m_config.pause_on_battery = false;
        m_config.battery_fps = 15;
        m_config.pause_on_lock = true;
        m_config.update_lockscreen = true;
        m_config.run_on_startup = false;
        m_config.auto_smooth_loop = true;
        m_config.smooth_loop_duration = 0.8f;
        return Save();
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    auto j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return false;
    }

    m_config = j.get<AppConfig>();
    m_config.config_path = GetConfigFilePath();

    // Clean up empty strings and internal cache paths from gallery history
    auto is_invalid = [](const std::string& s) {
        if (s.empty()) return true;
        if (s.find("\\LiteWallpaper\\optimized\\") != std::string::npos ||
            s.find("/LiteWallpaper/optimized/") != std::string::npos ||
            s.find("\\optimized\\") != std::string::npos ||
            s.find("/optimized/") != std::string::npos) {
            return true;
        }
        return false;
    };
    auto it = std::remove_if(m_config.gallery_history.begin(), m_config.gallery_history.end(), is_invalid);
    m_config.gallery_history.erase(it, m_config.gallery_history.end());

    // Clean up any orphaned cache files that don't belong to current gallery or active wallpaper
    std::vector<std::string> all_active = m_config.gallery_history;
    for (const auto& w : m_config.wallpapers) {
        if (!w.video_path.empty()) all_active.push_back(w.video_path);
    }
    VideoOptimizer::CleanOrphanCaches(all_active);
    ThumbnailManager::CleanOrphanThumbnails(all_active);

    return true;
}

bool Config::Save() {
    m_config.config_path = GetConfigFilePath();
    std::wstring wpath = Utf8ToWide(m_config.config_path);

    // Clean up empty strings and internal cache paths before saving
    auto is_invalid = [](const std::string& s) {
        if (s.empty()) return true;
        if (s.find("\\LiteWallpaper\\optimized\\") != std::string::npos ||
            s.find("/LiteWallpaper/optimized/") != std::string::npos ||
            s.find("\\optimized\\") != std::string::npos ||
            s.find("/optimized/") != std::string::npos) {
            return true;
        }
        return false;
    };
    auto it = std::remove_if(m_config.gallery_history.begin(), m_config.gallery_history.end(), is_invalid);
    m_config.gallery_history.erase(it, m_config.gallery_history.end());

    std::ofstream file(wpath);
    if (!file.is_open()) {
        return false;
    }

    nlohmann::json j = m_config;
    file << j.dump(4);
    return true;
}

AppConfig& Config::Get() {
    return m_config;
}

const AppConfig& Config::Get() const {
    return m_config;
}

} // namespace litewp
