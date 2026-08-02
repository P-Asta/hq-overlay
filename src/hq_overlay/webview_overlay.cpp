#include "webview_overlay.hpp"

#include "config.hpp"
#include "lcstats_client.hpp"
#include "logging.hpp"

#include <objbase.h>
#include <objidl.h>
#include <WebView2.h>

#include <dcomp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <wrl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hq::overlay::webview {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr UINT kMessageStop = WM_APP + 0x541;
constexpr UINT kMessageBounds = WM_APP + 0x542;
constexpr UINT kMessageSettings = WM_APP + 0x543;
constexpr UINT kMessageMouse = WM_APP + 0x544;
constexpr UINT kMessageShortcut = WM_APP + 0x545;
constexpr UINT kMessageLcStats = WM_APP + 0x546;
constexpr UINT kFocusHintPollIntervalMs = 250;
constexpr UINT kFocusHintDurationMs = 10000;
constexpr UINT kEmbeddedOverlayResourceId = 101;
constexpr std::uintmax_t kMaximumRpcFileSize = 16U * 1024U * 1024U;
constexpr wchar_t kApplicationHost[] = L"hq-overlay.local";
constexpr wchar_t kApplicationOrigin[] = L"https://hq-overlay.local/";

std::atomic<State> g_state{State::Stopped};
std::atomic_bool g_process_detaching{false};
std::atomic_bool g_settings_open{false};
std::atomic_bool g_controls_enabled{false};
std::atomic_bool g_dialog_active{false};
std::atomic_uint32_t g_settings_hotkey{VK_INSERT};
std::atomic<std::uint8_t> g_settings_hotkey_modifiers{0};
std::atomic<HMODULE> g_overlay_module{nullptr};
std::atomic<HWND> g_game_window{nullptr};
std::atomic<DWORD> g_thread_id{0};
std::atomic<HCURSOR> g_cursor{nullptr};
std::atomic_bool g_accept_lcstats{false};
std::mutex g_thread_mutex;
std::mutex g_shortcuts_mutex;
HANDLE g_thread_handle = nullptr;
std::vector<std::string> g_shortcuts;

[[nodiscard]] std::uint8_t CurrentModifierMask() {
    std::uint8_t modifiers = 0;
    const auto down = [](int virtual_key) {
        return (GetAsyncKeyState(virtual_key) & static_cast<SHORT>(0x8000)) != 0;
    };
    if (down(VK_CONTROL)) modifiers |= hq::config::kHotkeyModifierControl;
    if (down(VK_SHIFT)) modifiers |= hq::config::kHotkeyModifierShift;
    if (down(VK_MENU)) modifiers |= hq::config::kHotkeyModifierAlt;
    if (down(VK_LWIN) || down(VK_RWIN)) modifiers |= hq::config::kHotkeyModifierMeta;
    return modifiers;
}

struct MouseInput {
    COREWEBVIEW2_MOUSE_EVENT_KIND kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
    COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS virtual_keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
    UINT32 mouse_data = 0;
    POINT point{};
};

struct ShortcutInput {
    bool down = false;
    bool control = false;
    bool shift = false;
    bool alt = false;
    bool meta = false;
    std::string key;
    std::string shortcut;
};

struct LcStatsInput {
    hq::lcstats::Update update;
};

[[nodiscard]] std::string HResultText(HRESULT result) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    return text.str();
}

void Log(logging::Level level, std::string_view message) noexcept {
    if (g_process_detaching.load(std::memory_order_acquire)) return;
    try {
        logging::Write(level, message);
    } catch (...) {
    }
}

[[nodiscard]] std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            result.data(), count) != count) {
        return {};
    }
    return result;
}

[[nodiscard]] std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            result.data(), count, nullptr, nullptr) != count) {
        return {};
    }
    return result;
}

[[nodiscard]] bool StartsWithInsensitive(std::wstring_view value, std::wstring_view prefix) {
    if (value.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (towlower(value[index]) != towlower(prefix[index])) return false;
    }
    return true;
}

[[nodiscard]] std::filesystem::path KnownFolder(REFKNOWNFOLDERID folder_id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, nullptr, &raw)) || raw == nullptr) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

[[nodiscard]] std::filesystem::path ModuleDirectory(HMODULE module) {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

[[nodiscard]] bool ReadBinaryFile(const std::filesystem::path& path, std::string& content, std::string& error) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path, filesystem_error)) {
        error = "file does not exist";
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size > kMaximumRpcFileSize) {
        error = "file is too large";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "file could not be opened";
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (!file.eof() && file.fail()) {
        error = "file could not be read";
        return false;
    }
    return true;
}

[[nodiscard]] bool PathComponentEqualsInsensitive(
    const std::filesystem::path& left, const std::filesystem::path& right) {
    const std::wstring left_text = left.native();
    const std::wstring right_text = right.native();
    if (left_text.size() != right_text.size()) return false;
    for (std::size_t index = 0; index < left_text.size(); ++index) {
        if (towlower(left_text[index]) != towlower(right_text[index])) return false;
    }
    return true;
}

[[nodiscard]] bool PathIsWithinRoot(
    const std::filesystem::path& candidate, const std::filesystem::path& root) {
    auto candidate_part = candidate.begin();
    for (auto root_part = root.begin(); root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() ||
            !PathComponentEqualsInsensitive(*candidate_part, *root_part)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsReparsePoint(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

[[nodiscard]] bool ValidateResolvedPath(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    std::filesystem::path& result,
    std::string& error) {
    std::error_code filesystem_error;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, filesystem_error);
    if (filesystem_error || canonical_root.empty()) {
        error = "overlay data root could not be resolved";
        return false;
    }
    if (IsReparsePoint(root)) {
        error = "overlay data root cannot be a reparse point";
        return false;
    }

    std::filesystem::path current = root;
    for (const auto& component : relative) {
        current /= component;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                error = "reparse points are not allowed in overlay data paths";
                return false;
            }
            continue;
        }
        const DWORD win32_error = GetLastError();
        if (win32_error != ERROR_FILE_NOT_FOUND && win32_error != ERROR_PATH_NOT_FOUND) {
            error = "overlay data path attributes could not be read";
            return false;
        }
    }

    const std::filesystem::path candidate = root / relative;
    const std::filesystem::path canonical_candidate =
        std::filesystem::weakly_canonical(candidate, filesystem_error);
    if (filesystem_error || canonical_candidate.empty() ||
        !PathIsWithinRoot(canonical_candidate, canonical_root)) {
        error = "resolved path leaves the allowed overlay data root";
        return false;
    }
    result = candidate;
    return true;
}

[[nodiscard]] bool IsSafeRelativePath(
    std::string_view request,
    std::wstring_view required_extension,
    const std::filesystem::path& root,
    std::filesystem::path& result,
    std::string& error) {
    if (request.empty() || request.size() > 1024 || request.find('\0') != std::string_view::npos) {
        error = "invalid relative path";
        return false;
    }
    const std::wstring wide = Utf8ToWide(request);
    if (wide.empty()) {
        error = "path is not valid UTF-8";
        return false;
    }
    std::filesystem::path relative(wide);
    relative = relative.lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        error = "absolute paths are not allowed";
        return false;
    }
    for (const auto& component : relative) {
        if (component == L".." || component == L".") {
            error = "path traversal is not allowed";
            return false;
        }
    }
    std::wstring extension = relative.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    std::wstring expected(required_extension);
    std::transform(expected.begin(), expected.end(), expected.begin(), towlower);
    if (extension != expected) {
        error = "file extension is not allowed";
        return false;
    }
    return ValidateResolvedPath(root, relative, result, error);
}

[[nodiscard]] std::string ListFiles(const std::filesystem::path& root, std::wstring_view extension) {
    std::vector<std::string> files;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) return {};
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    for (; !error && iterator != end; iterator.increment(error)) {
        if (IsReparsePoint(iterator->path())) {
            const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                iterator.disable_recursion_pending();
            }
            continue;
        }
        if (!iterator->is_regular_file(error)) continue;
        std::wstring actual = iterator->path().extension().wstring();
        std::wstring expected(extension);
        std::transform(actual.begin(), actual.end(), actual.begin(), towlower);
        std::transform(expected.begin(), expected.end(), expected.begin(), towlower);
        if (actual != expected) continue;
        const auto relative = std::filesystem::relative(iterator->path(), root, error);
        if (error) break;
        files.push_back(WideToUtf8(relative.generic_wstring()));
    }
    std::sort(files.begin(), files.end());
    std::ostringstream result;
    for (std::size_t index = 0; index < files.size(); ++index) {
        if (index != 0) result << '\n';
        result << files[index];
    }
    return result.str();
}

[[nodiscard]] bool WriteConfigFile(
    const std::filesystem::path& target, std::string_view json, std::string& error) {
    const auto first = json.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos || (json[first] != '{' && json[first] != '[') ||
        json.size() > kMaximumRpcFileSize) {
        error = "config payload is not a JSON object or array";
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(target.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "config directory could not be created";
        return false;
    }
    std::filesystem::path temporary = target;
    temporary += L".hqtmp." + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "temporary config file could not be opened";
            return false;
        }
        file.write(json.data(), static_cast<std::streamsize>(json.size()));
        file.flush();
        if (!file) {
            file.close();
            DeleteFileW(temporary.c_str());
            error = "temporary config file could not be written";
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        error = "config file could not be replaced";
        return false;
    }
    return true;
}

[[nodiscard]] bool ParseBoolean(std::string_view text, bool& value) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text == "true" || text == "1") {
        value = true;
        return true;
    }
    if (text == "false" || text == "0") {
        value = false;
        return true;
    }
    return false;
}

[[nodiscard]] std::string Trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return std::string(value);
}

[[nodiscard]] bool EqualInsensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

void StoreShortcuts(std::string_view payload) {
    std::vector<std::string> values;
    std::size_t offset = 0;
    while (offset <= payload.size()) {
        const std::size_t newline = payload.find('\n', offset);
        const std::size_t end = newline == std::string_view::npos ? payload.size() : newline;
        std::string value = Trim(payload.substr(offset, end - offset));
        if (!value.empty() && value.size() <= 128 &&
            std::none_of(values.begin(), values.end(), [&](const std::string& existing) {
                return EqualInsensitive(existing, value);
            })) {
            values.push_back(std::move(value));
        }
        if (newline == std::string_view::npos) break;
        offset = newline + 1;
    }
    std::scoped_lock lock(g_shortcuts_mutex);
    g_shortcuts = std::move(values);
}

[[nodiscard]] std::string KeyName(UINT virtual_key, LPARAM lparam) {
    if (virtual_key >= 'A' && virtual_key <= 'Z') return std::string(1, static_cast<char>(virtual_key));
    if (virtual_key >= '0' && virtual_key <= '9') return std::string(1, static_cast<char>(virtual_key));
    if (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9) {
        return "Numpad" + std::to_string(virtual_key - VK_NUMPAD0);
    }
    if (virtual_key >= VK_F1 && virtual_key <= VK_F24) return "F" + std::to_string(virtual_key - VK_F1 + 1);
    switch (virtual_key) {
    case VK_SPACE: return "Space";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_UP: return "ArrowUp";
    case VK_DOWN: return "ArrowDown";
    case VK_LEFT: return "ArrowLeft";
    case VK_RIGHT: return "ArrowRight";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_ESCAPE: return "Escape";
    case VK_BACK: return "Backspace";
    case VK_ADD: return "NumpadAdd";
    case VK_SUBTRACT: return "NumpadSubtract";
    case VK_MULTIPLY: return "NumpadMultiply";
    case VK_DIVIDE: return "NumpadDivide";
    case VK_DECIMAL: return "NumpadDecimal";
    default: break;
    }
    wchar_t name[64]{};
    LONG key_lparam = static_cast<LONG>(lparam);
    if (GetKeyNameTextW(key_lparam, name, static_cast<int>(std::size(name))) <= 0) return {};
    return WideToUtf8(name);
}

[[nodiscard]] bool BuildShortcut(UINT message, WPARAM wparam, LPARAM lparam, ShortcutInput& input) {
    if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN &&
        message != WM_KEYUP && message != WM_SYSKEYUP) {
        return false;
    }
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        (static_cast<std::uintptr_t>(lparam) & (1ULL << 30U)) != 0) {
        return false;
    }
    const UINT key = static_cast<UINT>(wparam);
    if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL || key == VK_SHIFT ||
        key == VK_LSHIFT || key == VK_RSHIFT || key == VK_MENU || key == VK_LMENU ||
        key == VK_RMENU || key == VK_LWIN || key == VK_RWIN) {
        return false;
    }
    input.key = KeyName(key, lparam);
    if (input.key.empty()) return false;
    input.down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    input.control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    input.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    input.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    input.meta = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
    if (input.control) input.shortcut += "Ctrl+";
    if (input.shift) input.shortcut += "Shift+";
    if (input.alt) input.shortcut += "Alt+";
    if (input.meta) input.shortcut += "Meta+";
    input.shortcut += input.key;

    std::scoped_lock lock(g_shortcuts_mutex);
    return std::any_of(g_shortcuts.begin(), g_shortcuts.end(), [&](const std::string& registered) {
        return EqualInsensitive(registered, input.shortcut);
    });
}

[[nodiscard]] std::string JsonEscape(std::string_view input) {
    std::ostringstream result;
    for (const unsigned char value : input) {
        switch (value) {
        case '\\': result << "\\\\"; break;
        case '"': result << "\\\""; break;
        case '\b': result << "\\b"; break;
        case '\f': result << "\\f"; break;
        case '\n': result << "\\n"; break;
        case '\r': result << "\\r"; break;
        case '\t': result << "\\t"; break;
        default:
            if (value < 0x20) {
                constexpr char hex[] = "0123456789ABCDEF";
                result << "\\u00" << hex[value >> 4U] << hex[value & 0x0FU];
            } else {
                result << static_cast<char>(value);
            }
        }
    }
    return result.str();
}

class JsonValidator final {
public:
    explicit JsonValidator(std::string_view input) : input_(input) {}

    [[nodiscard]] bool Parse() {
        SkipWhitespace();
        if (!ParseValue(0)) return false;
        SkipWhitespace();
        return offset_ == input_.size();
    }

private:
    [[nodiscard]] bool ParseValue(unsigned depth) {
        if (depth > 256 || offset_ >= input_.size()) return false;
        switch (input_[offset_]) {
        case '{': return ParseObject(depth + 1);
        case '[': return ParseArray(depth + 1);
        case '"': return ParseString();
        case 't': return ParseLiteral("true");
        case 'f': return ParseLiteral("false");
        case 'n': return ParseLiteral("null");
        default: return ParseNumber();
        }
    }

    [[nodiscard]] bool ParseObject(unsigned depth) {
        ++offset_;
        SkipWhitespace();
        if (Consume('}')) return true;
        for (;;) {
            if (!ParseString()) return false;
            SkipWhitespace();
            if (!Consume(':')) return false;
            SkipWhitespace();
            if (!ParseValue(depth)) return false;
            SkipWhitespace();
            if (Consume('}')) return true;
            if (!Consume(',')) return false;
            SkipWhitespace();
        }
    }

    [[nodiscard]] bool ParseArray(unsigned depth) {
        ++offset_;
        SkipWhitespace();
        if (Consume(']')) return true;
        for (;;) {
            if (!ParseValue(depth)) return false;
            SkipWhitespace();
            if (Consume(']')) return true;
            if (!Consume(',')) return false;
            SkipWhitespace();
        }
    }

    [[nodiscard]] bool ParseString() {
        if (!Consume('"')) return false;
        while (offset_ < input_.size()) {
            const unsigned char value = static_cast<unsigned char>(input_[offset_++]);
            if (value == '"') return true;
            if (value < 0x20) return false;
            if (value != '\\') continue;
            if (offset_ >= input_.size()) return false;
            const char escape = input_[offset_++];
            if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' ||
                escape == 'f' || escape == 'n' || escape == 'r' || escape == 't') {
                continue;
            }
            if (escape != 'u' || offset_ + 4 > input_.size()) return false;
            for (unsigned index = 0; index < 4; ++index) {
                const unsigned char digit = static_cast<unsigned char>(input_[offset_++]);
                if (!std::isxdigit(digit)) return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool ParseLiteral(std::string_view literal) {
        if (input_.substr(offset_, literal.size()) != literal) return false;
        offset_ += literal.size();
        return true;
    }

    [[nodiscard]] bool ParseNumber() {
        const std::size_t start = offset_;
        (void)Consume('-');
        if (Consume('0')) {
            if (offset_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[offset_]))) {
                return false;
            }
        } else {
            if (offset_ >= input_.size() || input_[offset_] < '1' || input_[offset_] > '9') return false;
            do {
                ++offset_;
            } while (offset_ < input_.size() &&
                     std::isdigit(static_cast<unsigned char>(input_[offset_])));
        }
        if (Consume('.')) {
            if (offset_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[offset_]))) {
                return false;
            }
            do {
                ++offset_;
            } while (offset_ < input_.size() &&
                     std::isdigit(static_cast<unsigned char>(input_[offset_])));
        }
        if (offset_ < input_.size() && (input_[offset_] == 'e' || input_[offset_] == 'E')) {
            ++offset_;
            if (offset_ < input_.size() && (input_[offset_] == '+' || input_[offset_] == '-')) ++offset_;
            if (offset_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[offset_]))) {
                return false;
            }
            do {
                ++offset_;
            } while (offset_ < input_.size() &&
                     std::isdigit(static_cast<unsigned char>(input_[offset_])));
        }
        return offset_ > start;
    }

    void SkipWhitespace() {
        while (offset_ < input_.size()) {
            const char value = input_[offset_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') break;
            ++offset_;
        }
    }

    [[nodiscard]] bool Consume(char expected) {
        if (offset_ >= input_.size() || input_[offset_] != expected) return false;
        ++offset_;
        return true;
    }

    std::string_view input_;
    std::size_t offset_ = 0;
};

[[nodiscard]] std::string BuildLcStatsPayload(
    std::string_view raw_json, std::int64_t received_at_unix_ms, bool include_source) {
    const std::int64_t received_at_seconds = std::max<std::int64_t>(0, received_at_unix_ms / 1000);
    std::ostringstream payload;
    payload << '{';
    if (include_source) payload << "\"source\":\"lcstatstracker\",";
    payload << "\"receivedAt\":" << received_at_seconds << ",\"raw\":\""
            << JsonEscape(raw_json) << "\",\"stats\":" << raw_json << '}';
    return payload.str();
}

void QueueLcStatsUpdate(hq::lcstats::Update update) noexcept {
    if (!g_accept_lcstats.load(std::memory_order_acquire)) return;
    const DWORD thread_id = g_thread_id.load(std::memory_order_acquire);
    if (thread_id == 0) return;
    try {
        auto input = std::make_unique<LcStatsInput>();
        input->update = std::move(update);
        if (!g_accept_lcstats.load(std::memory_order_acquire)) return;
        LcStatsInput* raw = input.release();
        if (!PostThreadMessageW(thread_id, kMessageLcStats, 0, reinterpret_cast<LPARAM>(raw))) {
            delete raw;
        }
    } catch (...) {
    }
}

[[nodiscard]] bool LoadEmbeddedHtml(HMODULE module, std::wstring& html) {
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(kEmbeddedOverlayResourceId), RT_RCDATA);
    if (resource == nullptr) return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    if (loaded == nullptr) return false;
    const DWORD size = SizeofResource(module, resource);
    const void* bytes = LockResource(loaded);
    if (bytes == nullptr || size == 0) return false;
    const std::string_view utf8(static_cast<const char*>(bytes), size);
    html = Utf8ToWide(utf8);
    return !html.empty();
}

class WebViewHost final {
public:
    [[nodiscard]] HRESULT Initialize(HMODULE module, HWND window) {
        module_ = module;
        window_ = window;
        if (module_ == nullptr || window_ == nullptr || !IsWindow(window_)) return E_INVALIDARG;

        DWORD owner = 0;
        GetWindowThreadProcessId(window_, &owner);
        if (owner != GetCurrentProcessId()) return E_ACCESSDENIED;

        const auto roaming = KnownFolder(FOLDERID_RoamingAppData);
        const auto local = KnownFolder(FOLDERID_LocalAppData);
        if (roaming.empty() || local.empty()) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        module_root_ = roaming / L"asta.hq-launcher" / L"overlayModule";
        config_root_ = roaming / L"asta.hq-launcher" / L"config" / L"overlay";
        user_data_root_ = local / L"asta.hq-launcher" / L"WebView2Native";
        std::error_code filesystem_error;
        std::filesystem::create_directories(user_data_root_, filesystem_error);
        if (filesystem_error) return HRESULT_FROM_WIN32(filesystem_error.value());

        g_accept_lcstats.store(true, std::memory_order_release);
        if (!lcstats_client_.Start(QueueLcStatsUpdate)) {
            g_accept_lcstats.store(false, std::memory_order_release);
            Log(logging::Level::Warning, "LCStatsTracker SSE worker could not be started");
        } else {
            Log(logging::Level::Info, "LCStatsTracker SSE worker started for localhost:2145");
        }

        HRESULT result = CreateCompositionTree();
        if (FAILED(result)) return result;

        auto completion = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT error_code, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(error_code) || environment == nullptr) {
                    Fail(FAILED(error_code) ? error_code : E_POINTER, "WebView2 environment creation failed");
                    return S_OK;
                }
                environment_ = environment;
                const HRESULT controller_result = CreateController();
                if (FAILED(controller_result)) Fail(controller_result, "Composition controller creation failed");
                return S_OK;
            });
        result = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, user_data_root_.c_str(), nullptr, completion.Get());
        if (FAILED(result)) return result;
        Log(logging::Level::Info, "WebView2 STA environment creation started");
        return S_OK;
    }

    void HandleBounds() {
        if (!controller_ || window_ == nullptr || !IsWindow(window_)) return;
        RECT bounds{};
        if (!GetClientRect(window_, &bounds)) return;
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
        const HRESULT result = controller_->put_Bounds(bounds);
        if (FAILED(result)) {
            Log(logging::Level::Warning, "WebView2 bounds update failed: " + HResultText(result));
            return;
        }
        if (composition_device_) (void)composition_device_->Commit();
    }

    void HandleFocusHint() {
        if (open_hint_shown_ || !webview_ ||
            g_state.load(std::memory_order_acquire) != State::DomReady) {
            return;
        }
        if (settings_open_on_sta_) {
            open_hint_shown_ = true;
            return;
        }
        if (window_ == nullptr || !IsWindow(window_) || GetForegroundWindow() != window_) return;

        open_hint_shown_ = true;
        PostEvent(
            "overlay://open-config-hint",
            "{\"durationMs\":" + std::to_string(kFocusHintDurationMs) + "}");
        Log(logging::Level::Info, "Displayed first-focus overlay settings hotkey hint");
    }

    void HandleSettings(bool open) {
        const bool was_open = settings_open_on_sta_;
        settings_open_on_sta_ = open;
        if (open) {
            open_hint_shown_ = true;
        }
        g_settings_open.store(open, std::memory_order_release);
        g_controls_enabled.store(open, std::memory_order_release);
        if (webview_) {
            PostEvent("overlay://controls-open-changed", open ? "true" : "false");
        }
        if (!controller_) return;
        if (open) {
            (void)controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        } else if (was_open && window_ != nullptr && IsWindow(window_)) {
            const UINT message = RestoreGameFocusMessage();
            if (message != 0) (void)PostMessageW(window_, message, 0, 0);
        }
    }

    void HandleMouse(const MouseInput& input) {
        if (!composition_controller_) return;
        const HRESULT result = composition_controller_->SendMouseInput(
            input.kind, input.virtual_keys, input.mouse_data, input.point);
        if (FAILED(result)) {
            ++mouse_failure_count_;
            if (mouse_failure_count_ <= 8 || (mouse_failure_count_ & (mouse_failure_count_ - 1)) == 0) {
                RECT controller_bounds{};
                RECT client_bounds{};
                if (controller_) (void)controller_->get_Bounds(&controller_bounds);
                if (window_ != nullptr) (void)GetClientRect(window_, &client_bounds);
                std::ostringstream details;
                details << "WebView2 mouse forwarding failed: " << HResultText(result)
                        << "; failure=" << mouse_failure_count_
                        << "; kind=" << static_cast<unsigned>(input.kind)
                        << "; virtualKeys=0x" << std::hex << static_cast<unsigned>(input.virtual_keys)
                        << "; mouseData=0x" << input.mouse_data << std::dec
                        << "; point=" << input.point.x << ',' << input.point.y
                        << "; controllerBounds=" << controller_bounds.left << ',' << controller_bounds.top
                        << '-' << controller_bounds.right << ',' << controller_bounds.bottom
                        << "; clientBounds=" << client_bounds.left << ',' << client_bounds.top
                        << '-' << client_bounds.right << ',' << client_bounds.bottom;
                Log(logging::Level::Warning, details.str());
            }
        }
    }

    void HandleShortcut(const ShortcutInput& input) {
        if (!webview_ || g_state.load(std::memory_order_acquire) != State::DomReady) return;
        const auto existing = std::find_if(down_shortcuts_.begin(), down_shortcuts_.end(),
                                           [&](const std::string& value) {
                                               return EqualInsensitive(value, input.shortcut);
                                           });
        if (input.down) {
            if (existing == down_shortcuts_.end()) down_shortcuts_.push_back(input.shortcut);
        } else if (existing != down_shortcuts_.end()) {
            down_shortcuts_.erase(existing);
        }
        ++shortcut_sequence_;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        std::ostringstream json;
        json << "{\"id\":\"native-" << now << '-' << shortcut_sequence_ << "\",\"type\":\""
             << (input.down ? "keydown" : "keyup") << "\",\"key\":\"" << JsonEscape(input.key)
             << "\",\"shortcut\":\"" << JsonEscape(input.shortcut) << "\",\"ctrlKey\":"
             << (input.control ? "true" : "false") << ",\"shiftKey\":"
             << (input.shift ? "true" : "false") << ",\"altKey\":"
             << (input.alt ? "true" : "false") << ",\"metaKey\":"
             << (input.meta ? "true" : "false")
             << ",\"source\":\"native-webview2\",\"receivedAt\":" << now
             << ",\"sequence\":" << shortcut_sequence_ << ",\"down\":[";
        for (std::size_t index = 0; index < down_shortcuts_.size(); ++index) {
            if (index != 0) json << ',';
            json << '"' << JsonEscape(down_shortcuts_[index]) << '"';
        }
        json << "]}";
        PostEvent("overlay://input-shortcut", json.str());
    }

    void HandleLcStats(hq::lcstats::Update update) {
        std::string raw_json = Trim(update.raw_json);
        if (raw_json.empty() || raw_json.size() > kMaximumRpcFileSize ||
            !JsonValidator(raw_json).Parse() || Utf8ToWide(raw_json).empty()) {
            Log(logging::Level::Warning, "LCStatsTracker SSE payload ignored because it is not valid UTF-8 JSON");
            return;
        }
        latest_lcstats_payload_ = BuildLcStatsPayload(raw_json, update.received_at_unix_ms, true);
        latest_lcstats_event_ = BuildLcStatsPayload(raw_json, update.received_at_unix_ms, true);
        if (g_state.load(std::memory_order_acquire) == State::DomReady) {
            PostEvent("overlay://lcstats-updated", latest_lcstats_event_);
        }
    }

    void Shutdown() noexcept {
        if (shutdown_) return;
        shutdown_ = true;
        g_accept_lcstats.store(false, std::memory_order_release);
        lcstats_client_.Stop();
        if (g_process_detaching.load(std::memory_order_acquire)) {
            lcstats_client_.AbandonForProcessTermination();
        } else if (!lcstats_client_.WaitUntilStopped(std::chrono::milliseconds(2500))) {
            Log(logging::Level::Warning, "LCStatsTracker SSE worker did not stop within 2500 ms");
        }
        try {
            if (webview_ && g_state.load(std::memory_order_acquire) == State::DomReady) {
                PostEvent("overlay://active-changed", "false");
            }
            if (webview_) {
                if (web_message_token_.value != 0) (void)webview_->remove_WebMessageReceived(web_message_token_);
                if (navigation_token_.value != 0) (void)webview_->remove_NavigationStarting(navigation_token_);
                if (new_window_token_.value != 0) (void)webview_->remove_NewWindowRequested(new_window_token_);
                if (process_failed_token_.value != 0) (void)webview_->remove_ProcessFailed(process_failed_token_);
            }
            if (composition_controller_ && cursor_token_.value != 0) {
                (void)composition_controller_->remove_CursorChanged(cursor_token_);
            }
            if (controller_ && accelerator_token_.value != 0) {
                (void)controller_->remove_AcceleratorKeyPressed(accelerator_token_);
            }
            if (controller_) (void)controller_->Close();
            if (composition_controller_) (void)composition_controller_->put_RootVisualTarget(nullptr);
            if (composition_target_) (void)composition_target_->SetRoot(nullptr);
            if (composition_device_) (void)composition_device_->Commit();
        } catch (...) {
        }
        webview_.Reset();
        controller_.Reset();
        composition_controller_.Reset();
        environment_.Reset();
        webview_visual_.Reset();
        root_visual_.Reset();
        composition_target_.Reset();
        composition_device_.Reset();
        g_cursor.store(nullptr, std::memory_order_release);
    }

private:
    [[nodiscard]] HRESULT CreateCompositionTree() {
        HRESULT result = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&composition_device_));
        if (FAILED(result)) return result;
        result = composition_device_->CreateTargetForHwnd(window_, TRUE, &composition_target_);
        if (FAILED(result)) return result;
        result = composition_device_->CreateVisual(&root_visual_);
        if (FAILED(result)) return result;
        result = composition_device_->CreateVisual(&webview_visual_);
        if (FAILED(result)) return result;
        result = root_visual_->AddVisual(webview_visual_.Get(), TRUE, nullptr);
        if (FAILED(result)) return result;
        result = composition_target_->SetRoot(root_visual_.Get());
        if (FAILED(result)) return result;
        return composition_device_->Commit();
    }

    [[nodiscard]] HRESULT CreateController() {
        auto completion = Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
            [this](HRESULT error_code, ICoreWebView2CompositionController* controller) -> HRESULT {
                if (FAILED(error_code) || controller == nullptr) {
                    Fail(FAILED(error_code) ? error_code : E_POINTER, "WebView2 composition controller callback failed");
                    return S_OK;
                }
                const HRESULT result = ConfigureController(controller);
                if (FAILED(result)) Fail(result, "WebView2 controller configuration failed");
                return S_OK;
            });

        ComPtr<ICoreWebView2Environment10> environment10;
        if (SUCCEEDED(environment_.As(&environment10))) {
            ComPtr<ICoreWebView2ControllerOptions> options;
            HRESULT result = environment10->CreateCoreWebView2ControllerOptions(&options);
            if (FAILED(result)) return result;
            ComPtr<ICoreWebView2ControllerOptions3> options3;
            if (SUCCEEDED(options.As(&options3))) {
                constexpr COREWEBVIEW2_COLOR transparent{0, 0, 0, 0};
                result = options3->put_DefaultBackgroundColor(transparent);
                if (FAILED(result)) return result;
            }
            return environment10->CreateCoreWebView2CompositionControllerWithOptions(
                window_, options.Get(), completion.Get());
        }

        ComPtr<ICoreWebView2Environment3> environment3;
        HRESULT result = environment_.As(&environment3);
        if (FAILED(result)) return result;
        return environment3->CreateCoreWebView2CompositionController(window_, completion.Get());
    }

    [[nodiscard]] HRESULT ConfigureController(ICoreWebView2CompositionController* composition_controller) {
        composition_controller_ = composition_controller;
        HRESULT result = composition_controller_.As(&controller_);
        if (FAILED(result)) return result;
        result = composition_controller_->put_RootVisualTarget(webview_visual_.Get());
        if (FAILED(result)) return result;
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(controller_.As(&controller2))) {
            constexpr COREWEBVIEW2_COLOR transparent{0, 0, 0, 0};
            result = controller2->put_DefaultBackgroundColor(transparent);
            if (FAILED(result)) return result;
        }
        result = controller_->get_CoreWebView2(&webview_);
        if (FAILED(result) || !webview_) return FAILED(result) ? result : E_POINTER;

        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
            (void)settings->put_IsScriptEnabled(TRUE);
            (void)settings->put_IsWebMessageEnabled(TRUE);
            (void)settings->put_AreDefaultScriptDialogsEnabled(FALSE);
            (void)settings->put_AreDefaultContextMenusEnabled(FALSE);
            (void)settings->put_IsStatusBarEnabled(FALSE);
            (void)settings->put_IsZoomControlEnabled(FALSE);
        }

        result = RegisterEvents();
        if (FAILED(result)) return result;
        result = ConfigureContent();
        if (FAILED(result)) return result;

        HandleBounds();
        result = controller_->put_IsVisible(TRUE);
        if (FAILED(result)) return result;
        if (composition_device_) {
            result = composition_device_->Commit();
            if (FAILED(result)) return result;
        }
        HandleSettings(g_settings_open.load(std::memory_order_acquire));
        return S_OK;
    }

    [[nodiscard]] HRESULT RegisterEvents() {
        auto accelerator = Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [this](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                if (args == nullptr) return S_OK;
                UINT virtual_key = 0;
                COREWEBVIEW2_KEY_EVENT_KIND kind{};
                if (FAILED(args->get_VirtualKey(&virtual_key)) ||
                    FAILED(args->get_KeyEventKind(&kind)) ||
                    virtual_key != g_settings_hotkey.load(std::memory_order_acquire)) {
                    return S_OK;
                }
                const bool down = kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                                  kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
                if (!down) {
                    if (settings_hotkey_down_) {
                        settings_hotkey_down_ = false;
                        (void)args->put_Handled(TRUE);
                    }
                    return S_OK;
                }
                if (CurrentModifierMask() !=
                    g_settings_hotkey_modifiers.load(std::memory_order_acquire)) {
                    return S_OK;
                }
                (void)args->put_Handled(TRUE);
                COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};
                if (SUCCEEDED(args->get_PhysicalKeyStatus(&status)) && status.WasKeyDown) return S_OK;
                settings_hotkey_down_ = true;
                HandleSettings(!g_settings_open.load(std::memory_order_acquire));
                return S_OK;
            });
        HRESULT result = controller_->add_AcceleratorKeyPressed(accelerator.Get(), &accelerator_token_);
        if (FAILED(result)) return result;

        auto web_message = Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                HandleWebMessage(args);
                return S_OK;
            });
        result = webview_->add_WebMessageReceived(web_message.Get(), &web_message_token_);
        if (FAILED(result)) return result;

        auto navigation = Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR raw_uri = nullptr;
                if (FAILED(args->get_Uri(&raw_uri)) || raw_uri == nullptr) return S_OK;
                const std::wstring uri(raw_uri);
                CoTaskMemFree(raw_uri);
                const bool embedded_initial_data = embedded_document_ && allow_initial_embedded_navigation_ &&
                                                   StartsWithInsensitive(uri, L"data:text/html");
                const bool allowed = StartsWithInsensitive(uri, kApplicationOrigin) ||
                                     (embedded_document_ && StartsWithInsensitive(uri, L"about:blank")) ||
                                     embedded_initial_data;
                if (embedded_initial_data) allow_initial_embedded_navigation_ = false;
                if (!allowed) {
                    (void)args->put_Cancel(TRUE);
                    Log(logging::Level::Warning, "Blocked WebView2 navigation to " + WideToUtf8(uri));
                }
                return S_OK;
            });
        result = webview_->add_NavigationStarting(navigation.Get(), &navigation_token_);
        if (FAILED(result)) return result;

        auto new_window = Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                if (args) (void)args->put_Handled(TRUE);
                return S_OK;
            });
        result = webview_->add_NewWindowRequested(new_window.Get(), &new_window_token_);
        if (FAILED(result)) return result;

        auto process_failed = Callback<ICoreWebView2ProcessFailedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
                Fail(E_FAIL, "WebView2 browser/render process failed");
                return S_OK;
            });
        result = webview_->add_ProcessFailed(process_failed.Get(), &process_failed_token_);
        if (FAILED(result)) return result;

        auto cursor_changed = Callback<ICoreWebView2CursorChangedEventHandler>(
            [](ICoreWebView2CompositionController* sender, IUnknown*) -> HRESULT {
                HCURSOR cursor = nullptr;
                if (sender && SUCCEEDED(sender->get_Cursor(&cursor))) {
                    g_cursor.store(cursor, std::memory_order_release);
                }
                return S_OK;
            });
        return composition_controller_->add_CursorChanged(cursor_changed.Get(), &cursor_token_);
    }

    [[nodiscard]] HRESULT ConfigureContent() {
        ComPtr<ICoreWebView2_3> webview3;
        HRESULT result = webview_.As(&webview3);
        if (FAILED(result)) return result;

        std::error_code filesystem_error;

        // The production shell is RCDATA 101 so hq_overlay.dll is the only
        // deployment artifact. A disk document is accepted solely when an
        // explicit development override is supplied.
        wchar_t override_path[32768]{};
        const DWORD override_length = GetEnvironmentVariableW(
            L"HQ_OVERLAY_HTML", override_path, static_cast<DWORD>(std::size(override_path)));
        if (override_length > 0 && override_length < std::size(override_path)) {
            const std::filesystem::path html_path(override_path);
            if (!std::filesystem::is_regular_file(html_path, filesystem_error)) {
                return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            }
            asset_root_ = html_path.parent_path();
            result = webview3->SetVirtualHostNameToFolderMapping(
                kApplicationHost, asset_root_.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
            if (FAILED(result)) return result;
            const std::wstring uri = std::wstring(kApplicationOrigin) + html_path.filename().wstring();
            Log(logging::Level::Info, "Loading WebView2 development override from " + WideToUtf8(html_path.wstring()));
            return webview_->Navigate(uri.c_str());
        }

        std::wstring embedded_html;
        if (!LoadEmbeddedHtml(module_, embedded_html)) {
            Log(logging::Level::Error, "RCDATA resource 101 is unavailable; WebView2 cannot become ready");
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
        embedded_document_ = true;
        Log(logging::Level::Info, "Loading embedded overlay shell from RCDATA resource 101");
        return webview_->NavigateToString(embedded_html.c_str());
    }

    void HandleWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) {
        if (args == nullptr || !webview_) return;
        LPWSTR raw_source = nullptr;
        if (FAILED(args->get_Source(&raw_source)) || raw_source == nullptr) return;
        const std::wstring source(raw_source);
        CoTaskMemFree(raw_source);
        const bool trusted_embedded_source = embedded_document_ &&
                                             (StartsWithInsensitive(source, L"about:blank") ||
                                              StartsWithInsensitive(source, L"data:text/html"));
        if (!StartsWithInsensitive(source, kApplicationOrigin) && !trusted_embedded_source) {
            Log(logging::Level::Warning, "Rejected WebMessage from untrusted source " + WideToUtf8(source));
            return;
        }

        LPWSTR raw_message = nullptr;
        if (FAILED(args->TryGetWebMessageAsString(&raw_message)) || raw_message == nullptr) return;
        const std::wstring wide_message(raw_message);
        CoTaskMemFree(raw_message);
        const std::string message = WideToUtf8(wide_message);
        if (message.size() > kMaximumRpcFileSize) return;

        const std::size_t newline = message.find('\n');
        const std::string_view header(message.data(), newline == std::string::npos ? message.size() : newline);
        const std::string_view payload = newline == std::string::npos
                                             ? std::string_view{}
                                             : std::string_view(message).substr(newline + 1);
        const std::size_t first_tab = header.find('\t');
        const std::size_t second_tab = first_tab == std::string_view::npos
                                           ? std::string_view::npos
                                           : header.find('\t', first_tab + 1);
        if (first_tab == std::string_view::npos || second_tab == std::string_view::npos ||
            header.substr(0, first_tab) != "HQ1") {
            return;
        }
        const std::string id(header.substr(first_tab + 1, second_tab - first_tab - 1));
        const std::string command(header.substr(second_tab + 1));
        if (id.empty() || id.size() > 80 || command.empty() || command.size() > 80) return;

        std::string response;
        std::string error;
        const bool success = DispatchRpc(command, payload, response, error);
        SendResponse(id, success, success ? response : error);
    }

    [[nodiscard]] bool DispatchRpc(
        const std::string& command,
        std::string_view payload,
        std::string& response,
        std::string& error) {
        if (command == "frontend.ready") {
            if (g_state.load(std::memory_order_acquire) == State::Starting) {
                g_state.store(State::DomReady, std::memory_order_release);
                Log(logging::Level::Info, "WebView2 frontend.ready received; DOM handshake complete");
                PostEvent("overlay://active-changed", "true");
                PostEvent("overlay://controls-open-changed",
                          g_settings_open.load(std::memory_order_acquire) ? "true" : "false");
                if (!latest_lcstats_event_.empty()) {
                    PostEvent("overlay://lcstats-updated", latest_lcstats_event_);
                }
            }
            return true;
        }
        if (command == "frontend.info") {
            Log(logging::Level::Info, "Web frontend: " + std::string(payload));
            return true;
        }
        if (command == "frontend.error") {
            Log(logging::Level::Warning, "Web frontend error: " + std::string(payload));
            return true;
        }
        if (command == "module.list") {
            response = ListFiles(module_root_, L".js");
            return true;
        }
        if (command == "config.list") {
            response = ListFiles(config_root_, L".json");
            return true;
        }
        if (command == "module.read" || command == "config.read") {
            const bool module = command == "module.read";
            std::filesystem::path target;
            if (!IsSafeRelativePath(payload, module ? L".js" : L".json",
                                    module ? module_root_ : config_root_, target, error)) {
                return false;
            }
            return ReadBinaryFile(target, response, error);
        }
        if (command == "config.write") {
            const std::size_t line_end = payload.find('\n');
            if (line_end == std::string_view::npos) {
                error = "config.write requires a relative path and JSON body";
                return false;
            }
            std::string_view relative = payload.substr(0, line_end);
            if (!relative.empty() && relative.back() == '\r') relative.remove_suffix(1);
            std::filesystem::path target;
            if (!IsSafeRelativePath(relative, L".json", config_root_, target, error)) return false;
            if (!WriteConfigFile(target, payload.substr(line_end + 1), error)) return false;
            return true;
        }
        if (command == "folder.open") {
            std::filesystem::path target;
            if (payload == "overlayModule") {
                target = module_root_;
            } else if (payload == "overlayConfig") {
                target = config_root_;
            } else {
                error = "folder key is not allowed";
                return false;
            }
            std::error_code filesystem_error;
            std::filesystem::create_directories(target, filesystem_error);
            if (filesystem_error) {
                error = "folder could not be created";
                return false;
            }
            const auto launched = reinterpret_cast<std::intptr_t>(
                ShellExecuteW(window_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (launched <= 32) {
                error = "folder could not be opened";
                return false;
            }
            return true;
        }
        if (command == "ui.controls" || command == "dialog.active") {
            bool value = false;
            if (!ParseBoolean(payload, value)) {
                error = "boolean payload required";
                return false;
            }
            if (command == "ui.controls") {
                HandleSettings(value);
            } else {
                g_dialog_active.store(value, std::memory_order_release);
            }
            response = value ? "true" : "false";
            return true;
        }
        if (command == "ui.shortcuts") {
            StoreShortcuts(payload);
            response = "true";
            return true;
        }
        if (command == "debug.status") {
            RECT bounds{};
            if (window_ != nullptr) (void)GetClientRect(window_, &bounds);
            std::ostringstream status;
            status << "{\"state\":\"" << StateName() << "\",\"pid\":" << GetCurrentProcessId()
                   << ",\"hwnd\":\"0x" << std::hex << reinterpret_cast<std::uintptr_t>(window_) << std::dec
                   << "\",\"width\":" << (bounds.right - bounds.left)
                   << ",\"height\":" << (bounds.bottom - bounds.top)
                   << ",\"settingsOpen\":"
                   << (g_settings_open.load(std::memory_order_acquire) ? "true" : "false")
                   << ",\"controlsOpen\":"
                   << (g_settings_open.load(std::memory_order_acquire) ? "true" : "false") << '}';
            response = status.str();
            return true;
        }
        if (command == "lcstats.latest") {
            response = latest_lcstats_payload_.empty() ? "null" : latest_lcstats_payload_;
            return true;
        }
        error = "unknown command: " + command;
        return false;
    }

    void SendResponse(std::string_view id, bool success, std::string_view payload) {
        if (!webview_) return;
        std::string message = "HQ1R\t";
        message.append(id);
        message.append(success ? "\tOK\n" : "\tERR\n");
        message.append(payload);
        const std::wstring wide = Utf8ToWide(message);
        if (!wide.empty()) (void)webview_->PostWebMessageAsString(wide.c_str());
    }

    void PostEvent(std::string_view name, std::string_view json_payload) {
        if (!webview_) return;
        std::string message = "HQ1E\t";
        message.append(name);
        message.push_back('\n');
        message.append(json_payload);
        const std::wstring wide = Utf8ToWide(message);
        if (!wide.empty()) (void)webview_->PostWebMessageAsString(wide.c_str());
    }

    void Fail(HRESULT result, std::string_view context) noexcept {
        const State previous = g_state.exchange(State::Failed, std::memory_order_acq_rel);
        if (previous != State::Failed && previous != State::Stopping) {
            Log(logging::Level::Error, std::string(context) + ": " + HResultText(result));
        }
        PostQuitMessage(1);
    }

    HMODULE module_ = nullptr;
    HWND window_ = nullptr;
    bool embedded_document_ = false;
    bool allow_initial_embedded_navigation_ = true;
    bool settings_hotkey_down_ = false;
    bool shutdown_ = false;
    bool settings_open_on_sta_ = false;
    bool open_hint_shown_ = false;
    std::filesystem::path asset_root_;
    std::filesystem::path module_root_;
    std::filesystem::path config_root_;
    std::filesystem::path user_data_root_;
    ComPtr<IDCompositionDevice> composition_device_;
    ComPtr<IDCompositionTarget> composition_target_;
    ComPtr<IDCompositionVisual> root_visual_;
    ComPtr<IDCompositionVisual> webview_visual_;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2CompositionController> composition_controller_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    EventRegistrationToken web_message_token_{};
    EventRegistrationToken navigation_token_{};
    EventRegistrationToken new_window_token_{};
    EventRegistrationToken process_failed_token_{};
    EventRegistrationToken cursor_token_{};
    EventRegistrationToken accelerator_token_{};
    std::uint64_t shortcut_sequence_ = 0;
    std::uint64_t mouse_failure_count_ = 0;
    std::vector<std::string> down_shortcuts_;
    hq::lcstats::Client lcstats_client_;
    std::string latest_lcstats_payload_;
    std::string latest_lcstats_event_;
};

void DeleteOwnedMessagePayload(const MSG& message) noexcept {
    if (message.hwnd != nullptr || message.lParam == 0) return;
    if (message.message == kMessageMouse) {
        delete reinterpret_cast<MouseInput*>(message.lParam);
    } else if (message.message == kMessageShortcut) {
        delete reinterpret_cast<ShortcutInput*>(message.lParam);
    } else if (message.message == kMessageLcStats) {
        delete reinterpret_cast<LcStatsInput*>(message.lParam);
    }
}

void DrainOwnedThreadMessages() noexcept {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DeleteOwnedMessagePayload(message);
}

DWORD WINAPI ThreadMain(void*) {
    MSG queue_probe{};
    PeekMessageW(&queue_probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_thread_id.store(GetCurrentThreadId(), std::memory_order_release);
    if (g_state.load(std::memory_order_acquire) == State::Stopping ||
        g_process_detaching.load(std::memory_order_acquire)) {
        g_thread_id.store(0, std::memory_order_release);
        g_state.store(State::Stopped, std::memory_order_release);
        return 0;
    }

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) {
        g_state.store(State::Failed, std::memory_order_release);
        Log(logging::Level::Error, "WebView2 STA CoInitializeEx failed: " + HResultText(com_result));
        g_thread_id.store(0, std::memory_order_release);
        return 1;
    }

    WebViewHost host;
    const HRESULT initialize_result = host.Initialize(
        g_overlay_module.load(std::memory_order_acquire), g_game_window.load(std::memory_order_acquire));
    if (FAILED(initialize_result)) {
        g_state.store(State::Failed, std::memory_order_release);
        Log(logging::Level::Error, "WebView2 host initialization failed: " + HResultText(initialize_result));
    } else {
        const UINT_PTR focus_hint_timer =
            SetTimer(nullptr, 0, kFocusHintPollIntervalMs, nullptr);
        if (focus_hint_timer == 0) {
            Log(logging::Level::Warning, "First-focus hint timer could not be started");
        }
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.hwnd == nullptr && message.message == kMessageStop) {
                g_accept_lcstats.store(false, std::memory_order_release);
                break;
            }
            if (message.hwnd == nullptr && message.message == kMessageBounds) {
                host.HandleBounds();
                continue;
            }
            if (message.hwnd == nullptr && message.message == WM_TIMER &&
                message.wParam == focus_hint_timer) {
                host.HandleFocusHint();
                continue;
            }
            if (message.hwnd == nullptr && message.message == kMessageSettings) {
                host.HandleSettings(message.wParam != 0);
                continue;
            }
            if (message.hwnd == nullptr && message.message == kMessageMouse) {
                auto* input = reinterpret_cast<MouseInput*>(message.lParam);
                if (input != nullptr) {
                    host.HandleMouse(*input);
                    delete input;
                }
                continue;
            }
            if (message.hwnd == nullptr && message.message == kMessageShortcut) {
                auto* input = reinterpret_cast<ShortcutInput*>(message.lParam);
                if (input != nullptr) {
                    host.HandleShortcut(*input);
                    delete input;
                }
                continue;
            }
            if (message.hwnd == nullptr && message.message == kMessageLcStats) {
                auto input = std::unique_ptr<LcStatsInput>(reinterpret_cast<LcStatsInput*>(message.lParam));
                if (input != nullptr) {
                    try {
                        host.HandleLcStats(std::move(input->update));
                    } catch (...) {
                        Log(logging::Level::Warning, "LCStatsTracker SSE payload processing failed");
                    }
                }
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (focus_hint_timer != 0) (void)KillTimer(nullptr, focus_hint_timer);
    }

    host.Shutdown();
    DrainOwnedThreadMessages();
    CoUninitialize();
    g_thread_id.store(0, std::memory_order_release);
    const State state = g_state.load(std::memory_order_acquire);
    if (state != State::Failed) g_state.store(State::Stopped, std::memory_order_release);
    Log(logging::Level::Info, "WebView2 STA thread stopped");
    return g_state.load(std::memory_order_acquire) == State::Failed ? 1U : 0U;
}

[[nodiscard]] COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS MouseVirtualKeys(WPARAM wparam) {
    UINT keys = 0;
    const UINT win32 = GET_KEYSTATE_WPARAM(wparam);
    if ((win32 & MK_LBUTTON) != 0) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
    if ((win32 & MK_RBUTTON) != 0) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON;
    if ((win32 & MK_SHIFT) != 0) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_SHIFT;
    if ((win32 & MK_CONTROL) != 0) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_CONTROL;
    if ((win32 & MK_MBUTTON) != 0) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON;
    if ((win32 & MK_XBUTTON1) != 0) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON1;
    if ((win32 & MK_XBUTTON2) != 0) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON2;
    return static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(keys);
}

[[nodiscard]] bool MapMouseMessage(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam, MouseInput& input) {
    switch (message) {
    case WM_MOUSEMOVE: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE; break;
    case WM_MOUSELEAVE: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE; break;
    case WM_LBUTTONDOWN: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN; break;
    case WM_LBUTTONUP: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP; break;
    case WM_LBUTTONDBLCLK: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOUBLE_CLICK; break;
    case WM_RBUTTONDOWN: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN; break;
    case WM_RBUTTONUP: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP; break;
    case WM_RBUTTONDBLCLK: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOUBLE_CLICK; break;
    case WM_MBUTTONDOWN: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN; break;
    case WM_MBUTTONUP: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP; break;
    case WM_MBUTTONDBLCLK: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOUBLE_CLICK; break;
    case WM_XBUTTONDOWN: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOWN; break;
    case WM_XBUTTONUP: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP; break;
    case WM_XBUTTONDBLCLK: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOUBLE_CLICK; break;
    case WM_MOUSEWHEEL: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL; break;
    case WM_MOUSEHWHEEL: input.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL; break;
    default: return false;
    }

    if (message == WM_MOUSELEAVE) {
        // WebView2 requires every non-kind argument of a Leave event to be
        // exactly zero. WM_MOUSELEAVE does not carry coordinates in lParam.
        input.virtual_keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
        input.mouse_data = 0;
        input.point = POINT{};
    } else if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        input.virtual_keys = MouseVirtualKeys(wparam);
        input.mouse_data = static_cast<UINT32>(static_cast<std::int32_t>(GET_WHEEL_DELTA_WPARAM(wparam)));
        input.point = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        (void)ScreenToClient(window, &input.point);
    } else {
        input.virtual_keys = MouseVirtualKeys(wparam);
        input.point = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK) {
            input.mouse_data = GET_XBUTTON_WPARAM(wparam);
        }
    }
    return true;
}

}  // namespace

bool Start(HMODULE overlay_module, HWND game_window) noexcept {
    if (overlay_module == nullptr || game_window == nullptr ||
        g_process_detaching.load(std::memory_order_acquire)) {
        return false;
    }
    try {
        std::scoped_lock lock(g_thread_mutex);
        if (g_thread_handle != nullptr && WaitForSingleObject(g_thread_handle, 0) == WAIT_OBJECT_0) {
            CloseHandle(g_thread_handle);
            g_thread_handle = nullptr;
        }
        if (g_thread_handle != nullptr) {
            const State state = g_state.load(std::memory_order_acquire);
            return g_game_window.load(std::memory_order_acquire) == game_window &&
                   (state == State::Starting || state == State::DomReady);
        }
        g_overlay_module.store(overlay_module, std::memory_order_release);
        g_game_window.store(game_window, std::memory_order_release);
        g_state.store(State::Starting, std::memory_order_release);
        g_cursor.store(nullptr, std::memory_order_release);
        g_thread_handle = CreateThread(nullptr, 0, ThreadMain, nullptr, 0, nullptr);
        if (g_thread_handle == nullptr) {
            g_state.store(State::Failed, std::memory_order_release);
            Log(logging::Level::Error, "Could not create WebView2 STA thread");
            return false;
        }
        return true;
    } catch (...) {
        g_state.store(State::Failed, std::memory_order_release);
        return false;
    }
}

void RequestStop() noexcept {
    const State current = g_state.load(std::memory_order_acquire);
    if (current == State::Stopped || current == State::Stopping) return;
    if (current != State::Failed) g_state.store(State::Stopping, std::memory_order_release);
    const DWORD thread_id = g_thread_id.load(std::memory_order_acquire);
    if (thread_id != 0) (void)PostThreadMessageW(thread_id, kMessageStop, 0, 0);
}

void NotifyProcessDetach() noexcept {
    g_process_detaching.store(true, std::memory_order_release);
    g_accept_lcstats.store(false, std::memory_order_release);
    RequestStop();
}

void UpdateBounds() noexcept {
    const DWORD thread_id = g_thread_id.load(std::memory_order_acquire);
    if (thread_id != 0) (void)PostThreadMessageW(thread_id, kMessageBounds, 0, 0);
}

void SetSettingsOpen(bool open) noexcept {
    g_settings_open.store(open, std::memory_order_release);
    const DWORD thread_id = g_thread_id.load(std::memory_order_acquire);
    if (thread_id != 0) (void)PostThreadMessageW(thread_id, kMessageSettings, open ? 1U : 0U, 0);
}

void SetSettingsHotkey(UINT virtual_key, std::uint8_t modifiers) noexcept {
    g_settings_hotkey.store(virtual_key != 0 ? virtual_key : VK_INSERT, std::memory_order_release);
    g_settings_hotkey_modifiers.store(virtual_key != 0 ? modifiers : 0, std::memory_order_release);
}

UINT RestoreGameFocusMessage() noexcept {
    static const UINT message = RegisterWindowMessageW(L"HQOverlay.NativeWebView2.RestoreGameFocus");
    return message;
}

bool ForwardMouseMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (window == nullptr || !IsDomReady()) return false;
    const DWORD thread_id = g_thread_id.load(std::memory_order_acquire);
    if (thread_id == 0) return false;
    try {
        auto input = std::make_unique<MouseInput>();
        if (!MapMouseMessage(window, message, wparam, lparam, *input)) return false;
        MouseInput* raw = input.release();
        if (!PostThreadMessageW(thread_id, kMessageMouse, 0, reinterpret_cast<LPARAM>(raw))) {
            delete raw;
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ForwardShortcutMessage(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (!IsDomReady() || SettingsOpen()) return false;
    const DWORD thread_id = g_thread_id.load(std::memory_order_acquire);
    if (thread_id == 0) return false;
    try {
        auto input = std::make_unique<ShortcutInput>();
        if (!BuildShortcut(message, wparam, lparam, *input)) return false;
        ShortcutInput* raw = input.release();
        if (!PostThreadMessageW(thread_id, kMessageShortcut, 0, reinterpret_cast<LPARAM>(raw))) {
            delete raw;
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

State CurrentState() noexcept {
    return g_state.load(std::memory_order_acquire);
}

bool IsDomReady() noexcept {
    return CurrentState() == State::DomReady;
}

bool HasFailed() noexcept {
    return CurrentState() == State::Failed;
}

bool SettingsOpen() noexcept {
    return g_settings_open.load(std::memory_order_acquire);
}

bool WantsInput() noexcept {
    return IsDomReady() && (SettingsOpen() || g_controls_enabled.load(std::memory_order_acquire) ||
                            g_dialog_active.load(std::memory_order_acquire));
}

const char* StateName() noexcept {
    switch (CurrentState()) {
    case State::Stopped: return "stopped";
    case State::Starting: return "starting";
    case State::DomReady: return "dom-ready";
    case State::Failed: return "failed";
    case State::Stopping: return "stopping";
    }
    return "unknown";
}

HCURSOR CurrentCursor() noexcept {
    HCURSOR cursor = g_cursor.load(std::memory_order_acquire);
    return cursor != nullptr ? cursor : LoadCursorW(nullptr, IDC_ARROW);
}

}  // namespace hq::overlay::webview
