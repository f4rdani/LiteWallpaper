#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "config.h"
#include "video_optimizer.h"
#include "ui/thumbnail_manager.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <fstream>
#include <iterator>

namespace miniwp {

Config g_config;

// v2 rebranding repair: rewrite stale absolute paths that still point into the
// legacy %APPDATA%\LiteWallpaper root so an upgraded install keeps playing its
// current wallpaper instead of failing with "file not found".
static bool RepairLegacyBrandedPaths(AppConfig& cfg) {
    wchar_t appDataPath[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath))) return false;

    std::string oldRoot = WideToUtf8(std::wstring(appDataPath) + L"\\LiteWallpaper");
    std::string newRoot = WideToUtf8(std::wstring(appDataPath) + L"\\MiniWallpaper");
    if (oldRoot.empty() || newRoot.empty() || oldRoot == newRoot) return false;

    std::string oldRootFwd = oldRoot;
    std::string newRootFwd = newRoot;
    for (char& c : oldRootFwd) if (c == '\\') c = '/';
    for (char& c : newRootFwd) if (c == '\\') c = '/';

    bool changed = false;
    auto fix = [&](std::string& p) {
        if (p.empty()) return;
        std::string before = p;
        size_t pos = 0;
        while ((pos = p.find(oldRoot, pos)) != std::string::npos) {
            p.replace(pos, oldRoot.size(), newRoot);
            pos += newRoot.size();
        }
        pos = 0;
        while ((pos = p.find(oldRootFwd, pos)) != std::string::npos) {
            p.replace(pos, oldRootFwd.size(), newRootFwd);
            pos += newRootFwd.size();
        }
        if (p != before) changed = true;
    };

    for (auto& w : cfg.wallpapers) fix(w.video_path);
    fix(cfg.desktop_static_video_path);
    fix(cfg.lockscreen_video_path);
    for (auto& g : cfg.gallery_history) fix(g);

    return changed;
}

static void SanitizeGalleryHistory(std::vector<std::string>& history) {
    auto it = std::remove_if(history.begin(), history.end(), [](const std::string& s) {
        return s.empty() || IsOptimizedCachePath(s);
    });
    history.erase(it, history.end());
}

std::string Config::GetConfigDir() {
    return GetAppDataDirUtf8();
}

std::string Config::GetConfigFilePath() {
    return GetConfigDir() + "\\config.json";
}

bool Config::Load() {
    m_config.config_path = GetConfigFilePath();
    std::filesystem::path wpath = PathFromUtf8(m_config.config_path);

    std::ifstream file(wpath, std::ios::binary);
    if (!file.is_open()) {
        m_config = AppConfig{};
        m_config.config_path = GetConfigFilePath();
        return Save();
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return false;
    }

    m_config = j.get<AppConfig>();
    m_config.config_path = GetConfigFilePath();
    m_dirty = false;

    bool paths_repaired = RepairLegacyBrandedPaths(m_config);
    SanitizeGalleryHistory(m_config.gallery_history);

    // Clean up orphaned cache files that don't belong to current gallery or active wallpaper
    std::vector<std::string> all_active = m_config.gallery_history;
    for (const auto& w : m_config.wallpapers) {
        if (!w.video_path.empty()) all_active.push_back(w.video_path);
    }
    if (!m_config.desktop_static_video_path.empty()) all_active.push_back(m_config.desktop_static_video_path);
    if (!m_config.lockscreen_video_path.empty()) all_active.push_back(m_config.lockscreen_video_path);

    VideoOptimizer::CleanOrphanCaches(all_active);
    ThumbnailManager::CleanOrphanThumbnails(all_active);

    if (paths_repaired) {
        Save();
    }

    return true;
}

bool Config::Save() {
    m_config.config_path = GetConfigFilePath();
    SanitizeGalleryHistory(m_config.gallery_history);

    std::filesystem::path target_path = PathFromUtf8(m_config.config_path);
    std::filesystem::path temp_path = target_path;
    temp_path += L".tmp";

    {
        std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }
        nlohmann::json j = m_config;
        std::string payload = j.dump(4);
        file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (!file.good()) {
            return false;
        }
    }

    if (!MoveFileExW(temp_path.c_str(), target_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        // Fallback direct overwrite if atomic rename fails (e.g. antivirus lock)
        std::ofstream direct_file(target_path, std::ios::binary | std::ios::trunc);
        if (!direct_file.is_open()) return false;
        nlohmann::json j = m_config;
        direct_file << j.dump(4);
        std::error_code ec;
        std::filesystem::remove(temp_path, ec);
    }

    m_dirty = false;
    return true;
}

void Config::MarkDirty() noexcept {
    m_dirty = true;
    m_last_dirty_tick_ms = GetTickCount64();
}

bool Config::FlushIfDirty(bool force, uint64_t debounce_ms) {
    if (!m_dirty) return true;
    if (!force) {
        uint64_t elapsed = GetTickCount64() - m_last_dirty_tick_ms;
        if (elapsed < debounce_ms) return true;
    }
    return Save();
}

} // namespace miniwp
