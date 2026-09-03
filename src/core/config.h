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
    std::vector<int> target_displays; // Target displays to show wallpaper on (empty = all displays)
    bool pause_on_fullscreen = true;
    bool pause_on_maximized = true; // Auto-pause when an app is maximized / covers the desktop (0% CPU/GPU)
    bool pause_on_battery = false; // true = pause, false = reduce FPS
    bool pause_on_lock = true;
    bool pause_on_resource_heavy = true;   // Auto-sleep wallpaper when system RAM or VRAM exceeds threshold (Gaming/Heavy load)
    int resource_ram_threshold_pct = 80;   // RAM threshold percentage (default 80%)
    int resource_vram_threshold_pct = 80;  // VRAM threshold percentage (default 80%)
    bool update_lockscreen = true; // Capture frame for lock screen
    bool auto_downscale_highres = true; // Auto-downscale 4K+ videos to display resolution for 75% GPU/VRAM savings
    bool prompt_downscale = true;       // Prompt before optimizing when dropping high-res video
    int optimizer_crop_mode = 0;        // 0 = Aspect Fit (Proportional), 1 = Aspect Fill (Center Crop to Full Screen)
    bool run_on_startup = false;
    int startup_priority = 1;           // 0 = Normal (Registry Only), 1 = High Priority (Dual Path: Registry + Startup Shortcut)
    bool auto_smooth_loop = true;       // Seamless crossfade transition on video loop boundary
    float smooth_loop_duration = 0.8f;  // Crossfade transition duration in seconds (0.2s - 2.5s)
    int loop_preset = 0;                // 0=Cinematic Speed Ramp, 1=Smoothstep S-Curve, 2=Gentle Flow, 3=Instant Snap, 4=Custom
    bool loop_speed_ramp = true;        // Dynamic time-warp deceleration during loop boundary
    float loop_min_speed = 0.75f;       // Minimum playback speed at seam (0.5x - 0.95x)
    int loop_easing_curve = 1;          // 0=Linear, 1=Smoothstep, 2=Sine, 3=Smootherstep
    std::string config_path;     // Path to this config file

    bool IsDisplayEnabled(int idx) const {
        if (target_displays.empty()) return true;
        return std::find(target_displays.begin(), target_displays.end(), idx) != target_displays.end();
    }

    void SetDisplayEnabled(int idx, bool enabled) {
        auto it = std::find(target_displays.begin(), target_displays.end(), idx);
        if (enabled && it == target_displays.end()) {
            target_displays.push_back(idx);
        } else if (!enabled && it != target_displays.end()) {
            target_displays.erase(it);
        }
    }

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
        {"target_displays", c.target_displays},
        {"pause_on_fullscreen", c.pause_on_fullscreen},
        {"pause_on_maximized", c.pause_on_maximized},
        {"pause_on_battery", c.pause_on_battery},
        {"battery_fps", c.battery_fps},
        {"pause_on_lock", c.pause_on_lock},
        {"pause_on_resource_heavy", c.pause_on_resource_heavy},
        {"resource_ram_threshold_pct", c.resource_ram_threshold_pct},
        {"resource_vram_threshold_pct", c.resource_vram_threshold_pct},
        {"update_lockscreen", c.update_lockscreen},
        {"auto_downscale_highres", c.auto_downscale_highres},
        {"prompt_downscale", c.prompt_downscale},
        {"optimizer_crop_mode", c.optimizer_crop_mode},
        {"run_on_startup", c.run_on_startup},
        {"startup_priority", c.startup_priority},
        {"auto_smooth_loop", c.auto_smooth_loop},
        {"smooth_loop_duration", c.smooth_loop_duration},
        {"loop_preset", c.loop_preset},
        {"loop_speed_ramp", c.loop_speed_ramp},
        {"loop_min_speed", c.loop_min_speed},
        {"loop_easing_curve", c.loop_easing_curve}
    };
}

inline void from_json(const nlohmann::json& j, AppConfig& c) {
    if (j.contains("wallpapers")) j.at("wallpapers").get_to(c.wallpapers);
    if (j.contains("gallery_history")) j.at("gallery_history").get_to(c.gallery_history);
    if (j.contains("scaling_mode")) j.at("scaling_mode").get_to(c.scaling_mode);
    if (j.contains("target_fps")) j.at("target_fps").get_to(c.target_fps);
    if (j.contains("gpu_device_index")) j.at("gpu_device_index").get_to(c.gpu_device_index);
    if (j.contains("target_displays")) j.at("target_displays").get_to(c.target_displays);
    if (j.contains("pause_on_fullscreen")) j.at("pause_on_fullscreen").get_to(c.pause_on_fullscreen);
    if (j.contains("pause_on_maximized")) j.at("pause_on_maximized").get_to(c.pause_on_maximized);
    if (j.contains("pause_on_battery")) j.at("pause_on_battery").get_to(c.pause_on_battery);
    if (j.contains("battery_fps")) j.at("battery_fps").get_to(c.battery_fps);
    if (j.contains("pause_on_lock")) j.at("pause_on_lock").get_to(c.pause_on_lock);
    if (j.contains("pause_on_resource_heavy")) j.at("pause_on_resource_heavy").get_to(c.pause_on_resource_heavy);
    if (j.contains("resource_ram_threshold_pct")) j.at("resource_ram_threshold_pct").get_to(c.resource_ram_threshold_pct);
    if (j.contains("resource_vram_threshold_pct")) j.at("resource_vram_threshold_pct").get_to(c.resource_vram_threshold_pct);
    if (j.contains("update_lockscreen")) j.at("update_lockscreen").get_to(c.update_lockscreen);
    if (j.contains("auto_downscale_highres")) j.at("auto_downscale_highres").get_to(c.auto_downscale_highres);
    if (j.contains("prompt_downscale")) j.at("prompt_downscale").get_to(c.prompt_downscale);
    if (j.contains("optimizer_crop_mode")) j.at("optimizer_crop_mode").get_to(c.optimizer_crop_mode);
    if (j.contains("run_on_startup")) j.at("run_on_startup").get_to(c.run_on_startup);
    if (j.contains("startup_priority")) j.at("startup_priority").get_to(c.startup_priority);
    if (j.contains("auto_smooth_loop")) j.at("auto_smooth_loop").get_to(c.auto_smooth_loop);
    if (j.contains("smooth_loop_duration")) j.at("smooth_loop_duration").get_to(c.smooth_loop_duration);
    if (j.contains("loop_preset")) j.at("loop_preset").get_to(c.loop_preset);
    if (j.contains("loop_speed_ramp")) j.at("loop_speed_ramp").get_to(c.loop_speed_ramp);
    if (j.contains("loop_min_speed")) j.at("loop_min_speed").get_to(c.loop_min_speed);
    if (j.contains("loop_easing_curve")) j.at("loop_easing_curve").get_to(c.loop_easing_curve);
}

extern Config g_config;

} // namespace litewp
