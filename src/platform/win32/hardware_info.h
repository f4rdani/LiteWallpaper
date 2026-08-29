#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace litewp {

struct CpuInfo {
    std::string model_name;
    int logical_cores = 0;
    int physical_cores = 0;
};

struct GpuInfo {
    std::string name;
    std::string vendor;
    size_t dedicated_vram_mb = 0;
    size_t shared_vram_mb = 0;
    bool is_discrete = false;
    bool is_active_device = false;
};

struct DisplayInfo {
    std::string name;
    std::string device_id;
    int width = 0;
    int height = 0;
    int refresh_rate = 0;
    int pos_x = 0;
    int pos_y = 0;
    bool is_primary = false;
};

struct SystemHardwareInfo {
    CpuInfo cpu;
    std::vector<GpuInfo> gpus;
    std::vector<DisplayInfo> displays;
};

class HardwareDetector {
public:
    static SystemHardwareInfo QuerySystemInfo();
    static CpuInfo GetCpuInfo();
    static std::vector<GpuInfo> GetGpuList();
    static std::vector<DisplayInfo> GetDisplayList();
};

class WindowsAutostart {
public:
    static bool IsEnabled();
    static bool SetEnabled(bool enable);
    static void AutoRepairIfMoved();
};

} // namespace litewp
