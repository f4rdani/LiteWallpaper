#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "win_utils.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>

namespace fs = std::filesystem;

namespace miniwp {

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

fs::path PathFromUtf8(std::string_view utf8) {
    if (utf8.empty()) return {};
    return fs::path(Utf8ToWide(utf8));
}

std::string PathToUtf8(const fs::path& path) {
    if (path.empty()) return {};
    return WideToUtf8(path.wstring());
}

uint64_t Fnv1a64(std::string_view data) noexcept {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : data) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string Fnv1a64Hex(std::string_view data) {
    char buf[17] = {};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(Fnv1a64(data)));
    return std::string(buf, 16);
}

std::string NormalizePathKey(std::string_view path) {
    if (path.empty()) return {};
    std::wstring w = Utf8ToWide(path);
    for (wchar_t& ch : w) {
        if (ch == L'/') ch = L'\\';
    }
    if (!w.empty()) {
        CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
    }
    while (w.size() > 3 && w.back() == L'\\') {
        w.pop_back();
    }
    return WideToUtf8(w);
}

bool PathsEqualNormalized(std::string_view a, std::string_view b) {
    if (a == b) return true;
    if (a.empty() || b.empty()) return false;
    return NormalizePathKey(a) == NormalizePathKey(b);
}

bool IsOptimizedCachePath(std::string_view path) noexcept {
    if (path.empty()) return false;
    return path.find("\\MiniWallpaper\\optimized\\") != std::string_view::npos ||
           path.find("/MiniWallpaper/optimized/") != std::string_view::npos ||
           path.find("\\optimized\\") != std::string_view::npos ||
           path.find("/optimized/") != std::string_view::npos;
}

const std::wstring& GetAppDataDirWide() {
    static const std::wstring s_dir = []() -> std::wstring {
        wchar_t appDataPath[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath))) {
            std::wstring dir = std::wstring(appDataPath) + L"\\MiniWallpaper";
            CreateDirectoryW(dir.c_str(), nullptr);
            return dir;
        }
        return L".";
    }();
    return s_dir;
}

const std::string& GetAppDataDirUtf8() {
    static const std::string s_dir_utf8 = WideToUtf8(GetAppDataDirWide());
    return s_dir_utf8;
}

std::string GetAppDataSubDirUtf8(std::wstring_view sub_dir) {
    const std::wstring& root = GetAppDataDirWide();
    std::wstring full = root;
    if (!sub_dir.empty()) {
        if (full.back() != L'\\' && sub_dir.front() != L'\\') full += L'\\';
        full.append(sub_dir);
    }
    std::error_code ec;
    fs::create_directories(fs::path(full), ec);
    return WideToUtf8(full);
}

fs::path GetExecutableDir() {
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0) return fs::current_path();
    return fs::path(std::wstring_view(exePath, len)).parent_path();
}

} // namespace miniwp
