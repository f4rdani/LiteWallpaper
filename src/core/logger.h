#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "win_utils.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace miniwp {

// Lightweight diagnostic logger with bounded rotation (max 512 KB -> engine.old.log).
class Logger {
public:
    static constexpr uintmax_t MAX_LOG_BYTES = 512 * 1024;

    static Logger& Instance() {
        static Logger s_instance;
        return s_instance;
    }

    void Log(const std::string& message, bool force_flush = false) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (IsDebuggerPresent()) {
            std::string dbg = message + "\n";
            OutputDebugStringA(dbg.c_str());
        }

        if (!m_file.is_open()) return;

        SYSTEMTIME st;
        GetLocalTime(&st);
        char stamp[32];
        int stamp_len = std::snprintf(
            stamp, sizeof(stamp), "[%02u:%02u:%02u.%03u] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds
        );

        m_file.write(stamp, stamp_len > 0 ? stamp_len : 0);
        m_file.write(message.data(), static_cast<std::streamsize>(message.size()));
        m_file.put('\n');
        m_bytes_written += static_cast<size_t>((stamp_len > 0 ? stamp_len : 0)) + message.size() + 1;

        if (force_flush || message.find("FATAL") != std::string::npos || message.find("Error") != std::string::npos || message.find("failed") != std::string::npos) {
            m_file.flush();
        }

        if (m_bytes_written >= MAX_LOG_BYTES) {
            RotateLocked();
        }
    }

    void Flush() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file.is_open()) m_file.flush();
    }

    template <typename... Args>
    static void Info(Args&&... args) {
        std::ostringstream oss;
        (oss << ... << std::forward<Args>(args));
        Instance().Log(oss.str(), false);
    }

    template <typename... Args>
    static void Error(Args&&... args) {
        std::ostringstream oss;
        (oss << ... << std::forward<Args>(args));
        Instance().Log(oss.str(), true);
    }

private:
    Logger() {
        m_log_path = std::filesystem::path(GetAppDataDirWide()) / L"engine.log";
        m_old_path = std::filesystem::path(GetAppDataDirWide()) / L"engine.old.log";

        std::error_code ec;
        if (std::filesystem::exists(m_log_path, ec)) {
            uintmax_t sz = std::filesystem::file_size(m_log_path, ec);
            if (!ec && sz >= MAX_LOG_BYTES) {
                std::filesystem::remove(m_old_path, ec);
                std::filesystem::rename(m_log_path, m_old_path, ec);
                sz = 0;
            }
            m_bytes_written = ec ? 0 : static_cast<size_t>(sz);
        }

        m_file.open(m_log_path, std::ios::out | std::ios::app);
    }

    ~Logger() {
        if (m_file.is_open()) {
            m_file.flush();
            m_file.close();
        }
    }

    void RotateLocked() {
        if (m_file.is_open()) {
            m_file.flush();
            m_file.close();
        }
        std::error_code ec;
        std::filesystem::remove(m_old_path, ec);
        std::filesystem::rename(m_log_path, m_old_path, ec);
        m_bytes_written = 0;
        m_file.open(m_log_path, std::ios::out | std::ios::trunc);
    }

    std::mutex m_mutex;
    std::ofstream m_file;
    std::filesystem::path m_log_path;
    std::filesystem::path m_old_path;
    size_t m_bytes_written = 0;
};

} // namespace miniwp
