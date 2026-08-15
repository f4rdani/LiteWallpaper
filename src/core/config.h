#pragma once
#include <string>
#include <vector>
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
    int target_fps = 30;         // Default 30 FPS to save power
    int idle_fps = 15;           // FPS when no interaction
    bool pause_on_fullscreen = true;
    bool pause_on_battery = false; // true = pause, false = reduce FPS
    int battery_fps = 15;
    bool pause_on_lock = true;
    bool update_lockscreen = true; // Capture frame for lock screen
    bool run_on_startup = false;
    std::string config_path;     // Path to this config file
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
        {"target_fps", c.target_fps},
        {"idle_fps", c.idle_fps},
        {"pause_on_fullscreen", c.pause_on_fullscreen},
        {"pause_on_battery", c.pause_on_battery},
        {"battery_fps", c.battery_fps},
        {"pause_on_lock", c.pause_on_lock},
        {"update_lockscreen", c.update_lockscreen},
        {"run_on_startup", c.run_on_startup}
    };
}

inline void from_json(const nlohmann::json& j, AppConfig& c) {
    if (j.contains("wallpapers")) j.at("wallpapers").get_to(c.wallpapers);
    if (j.contains("target_fps")) j.at("target_fps").get_to(c.target_fps);
    if (j.contains("idle_fps")) j.at("idle_fps").get_to(c.idle_fps);
    if (j.contains("pause_on_fullscreen")) j.at("pause_on_fullscreen").get_to(c.pause_on_fullscreen);
    if (j.contains("pause_on_battery")) j.at("pause_on_battery").get_to(c.pause_on_battery);
    if (j.contains("battery_fps")) j.at("battery_fps").get_to(c.battery_fps);
    if (j.contains("pause_on_lock")) j.at("pause_on_lock").get_to(c.pause_on_lock);
    if (j.contains("update_lockscreen")) j.at("update_lockscreen").get_to(c.update_lockscreen);
    if (j.contains("run_on_startup")) j.at("run_on_startup").get_to(c.run_on_startup);
}

} // namespace litewp
