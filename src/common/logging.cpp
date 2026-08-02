#include "logging.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace hq::logging {
namespace {

std::mutex g_mutex;
std::filesystem::path g_path;

[[nodiscard]] const char* LevelName(Level level) {
    switch (level) {
    case Level::Info: return "INFO";
    case Level::Warning: return "WARN";
    case Level::Error: return "ERROR";
    }
    return "INFO";
}

[[nodiscard]] std::filesystem::path ResolveLogPath() {
    wchar_t override_path[32768]{};
    const DWORD override_length = GetEnvironmentVariableW(
        L"HQ_OVERLAY_LOG", override_path, static_cast<DWORD>(std::size(override_path)));
    if (override_length > 0 && override_length < std::size(override_path)) {
        return std::filesystem::path(override_path);
    }
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    std::filesystem::path directory;
    if (length > 0 && length < std::size(buffer)) {
        directory = std::filesystem::path(buffer) / L"asta.hq-launcher" / L"logs";
    } else {
        directory = std::filesystem::temp_directory_path() / L"asta.hq-launcher" / L"logs";
    }
    std::wostringstream name;
    name << L"hq_overlay_" << GetCurrentProcessId() << L".log";
    return directory / name.str();
}

}  // namespace

void Initialize() {
    std::scoped_lock lock(g_mutex);
    if (!g_path.empty()) return;
    g_path = ResolveLogPath();
    std::error_code error;
    std::filesystem::create_directories(g_path.parent_path(), error);
}

void Write(Level level, std::string_view message) {
    std::scoped_lock lock(g_mutex);
    if (g_path.empty()) {
        g_path = ResolveLogPath();
        std::error_code error;
        std::filesystem::create_directories(g_path.parent_path(), error);
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::ostringstream line;
    line << '[' << std::setfill('0') << std::setw(4) << now.wYear << '-' << std::setw(2) << now.wMonth << '-'
         << std::setw(2) << now.wDay << ' ' << std::setw(2) << now.wHour << ':' << std::setw(2) << now.wMinute
         << ':' << std::setw(2) << now.wSecond << '.' << std::setw(3) << now.wMilliseconds << "] ["
         << LevelName(level) << "] " << message << '\n';
    const std::string text = line.str();
    OutputDebugStringA(text.c_str());
    std::ofstream file(g_path, std::ios::binary | std::ios::app);
    if (file) file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::filesystem::path CurrentPath() {
    std::scoped_lock lock(g_mutex);
    return g_path;
}

}  // namespace hq::logging
