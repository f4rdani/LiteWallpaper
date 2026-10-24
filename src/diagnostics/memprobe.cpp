#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "memprobe.h"
#include "core/config.h"
#include "core/engine_controller.h"
#include "core/engine_state.h"
#include "core/logger.h"
#include "core/win_utils.h"
#include "ui/settings_app.h"
#include "ui/thumbnail_manager.h"

#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace miniwp {

namespace {

struct MemProbeSample {
    size_t private_ws_kb = 0;   // Task Manager "Memory" (active private working set)
    size_t ws_kb = 0;           // Total working set (includes shared pages)
    size_t private_commit_kb = 0;
    size_t peak_ws_kb = 0;
    size_t threads = 0;
    DWORD page_faults = 0;
};

bool ProbeEnumerateWorkingSet(std::vector<PSAPI_WORKING_SET_BLOCK>& out) {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return false;
    }

    size_t entry_capacity = pmc.WorkingSetSize / 4096 + 2048;
    static std::vector<BYTE> buffer;
    for (int attempt = 0; attempt < 8; ++attempt) {
        size_t needed = sizeof(PSAPI_WORKING_SET_INFORMATION) + entry_capacity * sizeof(PSAPI_WORKING_SET_BLOCK);
        if (buffer.size() < needed) buffer.resize(needed);
        auto* ws = reinterpret_cast<PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
        if (QueryWorkingSet(GetCurrentProcess(), ws, static_cast<DWORD>(buffer.size()))) {
            out.assign(ws->WorkingSetInfo, ws->WorkingSetInfo + ws->NumberOfEntries);
            return true;
        }
        if (GetLastError() != ERROR_BAD_LENGTH) return false;
        entry_capacity = static_cast<size_t>(ws->NumberOfEntries) + 2048;
    }
    return false;
}

size_t ProbeQueryPrivateWorkingSetKB() {
    static std::vector<PSAPI_WORKING_SET_BLOCK> pages;
    if (!ProbeEnumerateWorkingSet(pages)) return 0;
    size_t private_pages = 0;
    for (const auto& p : pages) {
        if (!p.Shared) private_pages++;
    }
    return private_pages * 4;
}

size_t ProbeCountThreads() {
    size_t count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) count++;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return count;
}

MemProbeSample ProbeSampleMemory() {
    MemProbeSample s;
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        s.ws_kb = pmc.WorkingSetSize / 1024;
        s.private_commit_kb = pmc.PrivateUsage / 1024;
        s.peak_ws_kb = pmc.PeakWorkingSetSize / 1024;
        s.page_faults = pmc.PageFaultCount;
    }
    s.private_ws_kb = ProbeQueryPrivateWorkingSetKB();
    s.threads = ProbeCountThreads();
    return s;
}

void ProbeLog(std::ofstream& out, const std::string& line) {
    out << line << "\n";
    out.flush();
    Logger::Info("[memprobe] ", line);
}

void ProbeLogTopModules(std::ofstream& out) {
    struct ModRange {
        size_t base;
        size_t end;
        std::string name;
        size_t priv_kb = 0;
        size_t shared_kb = 0;
    };
    std::vector<ModRange> mods;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = {};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                std::string name = WideToUtf8(me.szModule);
                size_t base = reinterpret_cast<size_t>(me.modBaseAddr);
                mods.push_back({ base, base + me.modBaseSize, std::move(name), 0, 0 });
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    std::sort(mods.begin(), mods.end(), [](const ModRange& a, const ModRange& b) { return a.base < b.base; });

    std::vector<PSAPI_WORKING_SET_BLOCK> pages;
    if (!ProbeEnumerateWorkingSet(pages)) return;

    size_t heap_priv_kb = 0, heap_shared_kb = 0;
    for (const auto& p : pages) {
        const bool is_private = !p.Shared;
        const size_t addr = static_cast<size_t>(p.VirtualPage) << 12;
        auto it = std::upper_bound(mods.begin(), mods.end(), addr,
                                   [](size_t v, const ModRange& m) { return v < m.base; });
        if (it != mods.begin()) {
            ModRange& m = *(it - 1);
            if (addr < m.end) {
                if (is_private) m.priv_kb += 4; else m.shared_kb += 4;
                continue;
            }
        }
        if (is_private) heap_priv_kb += 4; else heap_shared_kb += 4;
    }

    std::sort(mods.begin(), mods.end(), [](const ModRange& a, const ModRange& b) {
        return (a.priv_kb + a.shared_kb) > (b.priv_kb + b.shared_kb);
    });
    char buf[256];
    snprintf(buf, sizeof(buf), "resident pages by module (private / shared) - top 8 of %zu modules:", mods.size());
    ProbeLog(out, buf);
    for (size_t i = 0; i < mods.size() && i < 8; ++i) {
        snprintf(buf, sizeof(buf), "  %-30s %7.1f MB priv / %7.1f MB shared",
                 mods[i].name.c_str(), mods[i].priv_kb / 1024.0, mods[i].shared_kb / 1024.0);
        ProbeLog(out, buf);
    }
    snprintf(buf, sizeof(buf), "  %-30s %7.1f MB priv / %7.1f MB shared", "(heap/anon/driver)",
             heap_priv_kb / 1024.0, heap_shared_kb / 1024.0);
    ProbeLog(out, buf);
}

void ProbeLogSample(std::ofstream& out, const char* phase, int second) {
    MemProbeSample s = ProbeSampleMemory();
    char buf[300];
    snprintf(buf, sizeof(buf),
             "phase=%s t=%ds privateWS=%.1fMB ws=%.1fMB privateCommit=%.1fMB peakWS=%.1fMB threads=%zu faults=%lu",
             phase, second,
             s.private_ws_kb / 1024.0, s.ws_kb / 1024.0, s.private_commit_kb / 1024.0,
             s.peak_ws_kb / 1024.0, s.threads, static_cast<unsigned long>(s.page_faults));
    ProbeLog(out, buf);
}

void ProbeLogTrimmed(std::ofstream& out, const char* phase, const MemProbeHooks& hooks) {
    hooks.trim_working_set();
    Sleep(1200);
    ProbeLogSample(out, phase, 0);
}

void ProbeRunSeconds(int seconds, bool render_panel, std::ofstream& out, const char* phase, const MemProbeHooks& hooks) {
    uint64_t start = GetTickCount64();
    uint64_t last_ui_tick_ms = 0;
    int last_second = -1;
    while (static_cast<int>((GetTickCount64() - start) / 1000) < seconds) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        EngineController::DrainMainThreadTasks();

        bool ui_active = render_panel && SettingsUI::IsOpen();
        if (ui_active) {
            HWND uiHwnd = SettingsUI::GetHwnd();
            if (uiHwnd && !IsIconic(uiHwnd)) {
                uint64_t nowMs = GetTickCount64();
                uint32_t uiBudgetMs = SettingsUI::GetPreferredWaitMs();
                if (nowMs - last_ui_tick_ms >= uiBudgetMs) {
                    last_ui_tick_ms = nowMs;
                    SettingsUI::RenderFrame();
                }
            }
        }

        hooks.tick_wallpaper(ui_active);

        int elapsed = static_cast<int>((GetTickCount64() - start) / 1000);
        if (elapsed != last_second) {
            last_second = elapsed;
            ProbeLogSample(out, phase, elapsed);
        }
    }
}

} // namespace

int RunMemoryProbe(HINSTANCE hInstance, int seconds_per_phase, const MemProbeHooks& hooks) {
    fs::path reportPath = GetExecutableDir() / L"memprobe.txt";
    std::ofstream out(reportPath, std::ios::trunc);
    if (!out.is_open()) return 1;

    g_config.Load();
    auto& cfg = g_config.Get();

    {
        MemProbeSample startSample = ProbeSampleMemory();
        char buf[192];
        snprintf(buf, sizeof(buf), "baseline (process start) privateWS=%.1fMB ws=%.1fMB threads=%zu",
                 startSample.private_ws_kb / 1024.0, startSample.ws_kb / 1024.0, startSample.threads);
        ProbeLog(out, buf);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"MiniWallpaper_MemProbe";
    RegisterClassExW(&wc);

    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HWND probeHwnd = CreateWindowExW(
        0, wc.lpszClassName, L"MiniWallpaper mem probe", WS_POPUP,
        0, 0, vw, vh, nullptr, nullptr, hInstance, nullptr
    );
    if (!probeHwnd) {
        ProbeLog(out, "FATAL: probe window creation failed");
        return 1;
    }

    if (!hooks.init_engine(probeHwnd, vw, vh, cfg.gpu_device_index)) {
        ProbeLog(out, "FATAL: presenter init failed");
        DestroyWindow(probeHwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    ProbeLog(out, "adapter: " + hooks.get_adapter_summary());
    ProbeLog(out, "swapchain target: " + std::to_string(vw) + "x" + std::to_string(vh));

    std::string video = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
    if (!video.empty()) {
        bool ok = hooks.open_video(video);
        MemProbeVideoStats stats = hooks.get_video_stats();
        char buf[512];
        snprintf(buf, sizeof(buf), "video: %s (%dx%d @ %.1f fps, hw=%s, open=%s)",
                 video.c_str(), stats.width, stats.height, stats.fps,
                 stats.hw_accelerated ? "yes" : "no", ok ? "yes" : "no");
        ProbeLog(out, buf);
    } else {
        ProbeLog(out, "video: NONE configured - measuring idle engine only");
    }

    ThumbnailManager::Instance(); // Verify zero idle worker thread until work is actually queued

    ProbeLog(out, "=== phase 1 / 6: BACKGROUND (panel closed, video playing) ===");
    ProbeRunSeconds(seconds_per_phase, false, out, "background", hooks);
    ProbeLogTopModules(out);
    ProbeLogTrimmed(out, "background-trimmed", hooks);

    ProbeLog(out, "=== phase 2 / 6: SETTINGS PANEL OPEN (idle browsing) ===");
    SettingsUI::Open(hInstance);
    ProbeRunSeconds(seconds_per_phase, true, out, "panel-open", hooks);
    ProbeLogTopModules(out);
    ProbeLogTrimmed(out, "panel-open-trimmed", hooks);
    {
        int tw = 0, th = 0;
        SettingsUI::GetFontAtlasSize(tw, th);
        char buf[160];
        snprintf(buf, sizeof(buf), "font atlas: %dx%d (%d KB RGBA, CPU+GPU)", tw, th, (tw * th * 4) / 1024);
        ProbeLog(out, buf);
    }

    ProbeLog(out, "=== phase 3 / 6: PANEL CLOSED AGAIN ===");
    SettingsUI::Close();
    ProbeRunSeconds(seconds_per_phase, false, out, "panel-closed", hooks);
    ProbeLogTrimmed(out, "panel-closed-trimmed", hooks);

    ProbeLog(out, "=== phase 4 / 6: OPEN/CLOSE x5 (leak check) ===");
    for (int cycle = 0; cycle < 5; ++cycle) {
        SettingsUI::Open(hInstance);
        ProbeRunSeconds(1, true, out, "cycle-open", hooks);
        SettingsUI::Close();
        ProbeRunSeconds(1, false, out, "cycle-closed", hooks);
        ProbeLogTrimmed(out, "cycle-closed-trimmed", hooks);
    }

    ProbeLog(out, "=== phase 5 / 6: DEEP SUSPEND, PANEL CLOSED (decoder released) ===");
    {
        MemProbeVideoStats before = hooks.get_video_stats();
        double probe_pos = before.duration_seconds > 1.0 ? before.duration_seconds * 0.4 : 4.0;
        g_shared_engine_state.current_time_sec.store(probe_pos);
        hooks.enter_deep_suspend();
        MemProbeVideoStats after = hooks.get_video_stats();
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "deep suspend: saved position=%.1fs, frame pts_before=%lld, decoder after release: %dx%d open=%s",
                 probe_pos, static_cast<long long>(before.current_pts), after.width, after.height,
                 (after.width > 0) ? "yes" : "no (released)");
        ProbeLog(out, buf);
    }
    ProbeRunSeconds(seconds_per_phase, false, out, "deep-suspend", hooks);
    ProbeLogTopModules(out);
    ProbeLogTrimmed(out, "deep-suspend-trimmed", hooks);

    ProbeLog(out, "=== phase 6 / 6: DEEP SUSPEND + SETTINGS PANEL OPEN ===");
    SettingsUI::Open(hInstance);
    ProbeRunSeconds(seconds_per_phase, true, out, "deep-suspend-panel", hooks);
    ProbeLogTrimmed(out, "deep-suspend-panel-trimmed", hooks);
    SettingsUI::Close();

    ProbeLog(out, "=== resume from deep suspend (reopen decoder + seek back) ===");
    uint64_t frames_before_resume = hooks.get_frames_rendered();
    {
        hooks.leave_deep_suspend();
        hooks.render_one_frame_now();
        MemProbeVideoStats resumed = hooks.get_video_stats();
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "resume: decoder %dx%d open=%s, resumed frame pts=%lld",
                 resumed.width, resumed.height, (resumed.width > 0) ? "yes" : "no",
                 static_cast<long long>(resumed.current_pts));
        ProbeLog(out, buf);
    }
    ProbeRunSeconds(seconds_per_phase, false, out, "resumed", hooks);
    ProbeLogTrimmed(out, "resumed-trimmed", hooks);
    {
        char buf[192];
        snprintf(buf, sizeof(buf), "resume check: %llu frames rendered during the resumed phase",
                 static_cast<unsigned long long>(hooks.get_frames_rendered() - frames_before_resume));
        ProbeLog(out, buf);
    }

    {
        MemProbeSample s = ProbeSampleMemory();
        char buf[300];
        snprintf(buf, sizeof(buf),
                 "FINAL: privateWS=%.1fMB ws=%.1fMB privateCommit=%.1fMB peakWS=%.1fMB threads=%zu faults=%lu",
                 s.private_ws_kb / 1024.0, s.ws_kb / 1024.0, s.private_commit_kb / 1024.0,
                 s.peak_ws_kb / 1024.0, s.threads, static_cast<unsigned long>(s.page_faults));
        ProbeLog(out, buf);
    }

    hooks.shutdown_engine();
    DestroyWindow(probeHwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);

    ProbeLog(out, "done - report written to memprobe.txt");
    return 0;
}

} // namespace miniwp
