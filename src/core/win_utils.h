#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace miniwp {

// UTF-8 <-> UTF-16 conversion helpers (Win32 Wide API safe)
std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);

// Unicode-safe std::filesystem::path conversions on Windows
// (Avoids MSVC's default ANSI codepage interpretation of std::string paths)
std::filesystem::path PathFromUtf8(std::string_view utf8);
std::string PathToUtf8(const std::filesystem::path& path);

// Deterministic 64-bit FNV-1a hash (compatible with existing disk cache filenames)
uint64_t Fnv1a64(std::string_view data) noexcept;
std::string Fnv1a64Hex(std::string_view data);

// Path normalization & comparison helpers
std::string NormalizePathKey(std::string_view path);
bool PathsEqualNormalized(std::string_view a, std::string_view b);
bool IsOptimizedCachePath(std::string_view path) noexcept;

// Cached application directories (%APPDATA%\MiniWallpaper and executable folder)
const std::wstring& GetAppDataDirWide();
const std::string& GetAppDataDirUtf8();
std::string GetAppDataSubDirUtf8(std::wstring_view sub_dir);
std::filesystem::path GetExecutableDir();

} // namespace miniwp
