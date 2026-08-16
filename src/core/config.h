#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace litewp {

struct MonitorWallpaper {
    std::string monitor_id;      // Windows monitor device path
    std::string video_path;      // Absolute path to video file
    float volume = 0.0f;         // Audio volume (0.0 = muted, 1.0 = max)
    bool audio_enabled = false;  // Master audio toggle
};

struct AppConfig {
    std::vector<MonitorWallpaper> wallpapers;
    std::vector<std::string> gallery_history; // Persistent gallery of used video files
    int scaling_mode = 0;        // 0 = Auto / Aspect Fill (Cover), 1 = Aspect Fit (Letterbox), 2 = Stretch
    int target_fps = 30;         // Display cap. Video always plays at native speed;
                                // if target < video fps, frames are skipped (not slowed).
    int battery_fps = 15;        // Frame rate limit when running on battery power
    int gpu_device_index = 0;    // -1 = CPU (Software), 0 = GPU 1 (Primary), 1 = GPU 2, etc.
    bool pause_on_fullscreen = true;
    bool pause_on_battery = false; // true = pause, false = reduce FPS
    bool pause_on_lock = true;
    bool update_lockscreen = true; // Capture frame for lock screen
    bool auto_downscale_highres = true; // Auto-downscale 4K+ videos to display resolution for 75% GPU/VRAM savings
    bool prompt_downscale = true;       // Prompt before optimizing when dropping high-res video
    bool run_on_startup = false;
    std::string config_path;     // Path to this config file

    void AddToGallery(const std::string& path) {
        if (path.empty()) return;
        if (path.find("\\LiteWallpaper\\optimized\\") != std::string::npos ||
            path.find("/LiteWallpaper/optimized/") != std::string::npos ||
            path.find("\\optimized\\") != std::string::npos ||
            path.find("/optimized/") != std::string::npos) {
            return;
        }
        auto it = std::find(gallery_history.begin(), gallery_history.end(), path);
        if (it != gallery_history.end()) {
            gallery_history.erase(it);
        }
        gallery_history.insert(gallery_history.begin(), path);
        if (gallery_history.size() > 50) {
            gallery_history.resize(50);
        }
    }

    void RemoveFromGallery(const std::string& path) {
        if (path.empty()) return;
        auto normalize = [](std::string s) {
            std::replace(s.begin(), s.end(), '/', '\\');
            for (auto& c : s) c = (char)::tolower(c);
            return s;
        };
        std::string target = normalize(path);
        auto it = std::remove_if(gallery_history.begin(), gallery_history.end(), [&](const std::string& item) {
            return normalize(item) == target || item == path;
        });
        gallery_history.erase(it, gallery_history.end());
    }
};

class Config {
public:
    // Load from %APPDATA%/LiteWallpaper/config.json
    // If file doesn't exist, create with defaults
    bool Load();
    
    // Save current config to file
    bool Save();
    
    // Get/Set config
    AppConfig& Get();
    const AppConfig& Get() const;
    
    // Get config file path
    static std::string GetConfigDir();
    static std::string GetConfigFilePath();

private:
    AppConfig m_config;
};

// JSON serialization
inline void to_json(nlohmann::json& j, const MonitorWallpaper& m) {
    j = nlohmann::json{
        {"monitor_id", m.monitor_id},
        {"video_path", m.video_path},
        {"volume", m.volume},
        {"audio_enabled", m.audio_enabled}
    };
}

inline void from_json(const nlohmann::json& j, MonitorWallpaper& m) {
    if (j.contains("monitor_id")) j.at("monitor_id").get_to(m.monitor_id);
    if (j.contains("video_path")) j.at("video_path").get_to(m.video_path);
    if (j.contains("volume")) j.at("volume").get_to(m.volume);
    if (j.contains("audio_enabled")) j.at("audio_enabled").get_to(m.audio_enabled);
}

inline void to_json(nlohmann::json& j, const AppConfig& c) {
    j = nlohmann::json{
        {"wallpapers", c.wallpapers},
        {"gallery_history", c.gallery_history},
        {"scaling_mode", c.scaling_mode},
        {"target_fps", c.target_fps},
        {"gpu_device_index", c.gpu_device_index},
        {"pause_on_fullscreen", c.pause_on_fullscreen},
        {"pause_on_battery", c.pause_on_battery},
        {"battery_fps", c.battery_fps},
        {"pause_on_lock", c.pause_on_lock},
        {"update_lockscreen", c.update_lockscreen},
        {"auto_downscale_highres", c.auto_downscale_highres},
        {"prompt_downscale", c.prompt_downscale},
        {"run_on_startup", c.run_on_startup}
    };
}

inline void from_json(const nlohmann::json& j, AppConfig& c) {
    if (j.contains("wallpapers")) j.at("wallpapers").get_to(c.wallpapers);
    if (j.contains("gallery_history")) j.at("gallery_history").get_to(c.gallery_history);
    if (j.contains("scaling_mode")) j.at("scaling_mode").get_to(c.scaling_mode);
    if (j.contains("target_fps")) j.at("target_fps").get_to(c.target_fps);
    if (j.contains("gpu_device_index")) j.at("gpu_device_index").get_to(c.gpu_device_index);
    if (j.contains("pause_on_fullscreen")) j.at("pause_on_fullscreen").get_to(c.pause_on_fullscreen);
    if (j.contains("pause_on_battery")) j.at("pause_on_battery").get_to(c.pause_on_battery);
    if (j.contains("battery_fps")) j.at("battery_fps").get_to(c.battery_fps);
    if (j.contains("pause_on_lock")) j.at("pause_on_lock").get_to(c.pause_on_lock);
    if (j.contains("update_lockscreen")) j.at("update_lockscreen").get_to(c.update_lockscreen);
    if (j.contains("auto_downscale_highres")) j.at("auto_downscale_highres").get_to(c.auto_downscale_highres);
    if (j.contains("prompt_downscale")) j.at("prompt_downscale").get_to(c.prompt_downscale);
    if (j.contains("run_on_startup")) j.at("run_on_startup").get_to(c.run_on_startup);
}

extern Config g_config;

} // namespace litewp
