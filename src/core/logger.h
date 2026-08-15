#pragma once
#include <windows.h>
#include <string>
#include <sstream>
#include <fstream>
#include <mutex>
#include <cstdio>
#include <shlobj.h>

namespace litewp {

// Minimal diagnostic logger. Appends timestamped lines to
// %APPDATA%\LiteWallpaper\engine.log and mirrors them to the debugger.
class Logger {
public:
    static Logger& Instance() {
        static Logger s_instance;
        return s_instance;
    }

    void Log(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        OutputDebugStringA(message.c_str());
        OutputDebugStringA("\n");

        SYSTEMTIME st;
        GetLocalTime(&st);
        char stamp[64];
        snprintf(stamp, sizeof(stamp), "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        if (m_file) {
            m_file << stamp << message << "\n";
            m_file.flush();
        }
    }

    // Convenience: build a message from parts using operator<<
    template <typename... Args>
    static void Info(Args&&... args) {
        std::ostringstream oss;
        (oss << ... << std::forward<Args>(args));
        Instance().Log(oss.str());
    }

private:
    Logger() {
        wchar_t appDataPath[MAX_PATH];
        std::wstring path;
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath))) {
            path = std::wstring(appDataPath) + L"\\LiteWallpaper";
            CreateDirectoryW(path.c_str(), nullptr);
            path += L"\\engine.log";
        } else {
            path = L"engine.log";
        }
        m_file.open(path, std::ios::out | std::ios::app);
    }

    ~Logger() {
        if (m_file) m_file.close();
    }

    std::mutex m_mutex;
    std::ofstream m_file;
};

} // namespace litewp