#include "config.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>

namespace hq::config {
namespace {

struct JsonValue {
    using Object = std::map<std::string, JsonValue, std::less<>>;
    using Array = std::vector<JsonValue>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Object, Array>;

    Storage storage = nullptr;

    [[nodiscard]] const Object* AsObject() const { return std::get_if<Object>(&storage); }
    [[nodiscard]] const std::string* AsString() const { return std::get_if<std::string>(&storage); }
    [[nodiscard]] const bool* AsBool() const { return std::get_if<bool>(&storage); }
    [[nodiscard]] const double* AsNumber() const { return std::get_if<double>(&storage); }

    [[nodiscard]] const JsonValue* Find(std::string_view key) const {
        const auto* object = AsObject();
        if (object == nullptr) {
            return nullptr;
        }
        const auto it = object->find(key);
        return it == object->end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    [[nodiscard]] std::optional<JsonValue> Parse(std::string& error) {
        SkipWhitespace();
        auto value = ParseValue(error, 0);
        if (!value.has_value()) {
            return std::nullopt;
        }
        SkipWhitespace();
        if (cursor_ != input_.size()) {
            error = Error("unexpected trailing content");
            return std::nullopt;
        }
        return value;
    }

private:
    static constexpr std::size_t kMaxDepth = 64;

    [[nodiscard]] std::optional<JsonValue> ParseValue(std::string& error, std::size_t depth) {
        if (depth > kMaxDepth) {
            error = Error("maximum nesting depth exceeded");
            return std::nullopt;
        }
        SkipWhitespace();
        if (cursor_ >= input_.size()) {
            error = Error("unexpected end of input");
            return std::nullopt;
        }

        switch (input_[cursor_]) {
        case '{': return ParseObject(error, depth + 1);
        case '[': return ParseArray(error, depth + 1);
        case '"': {
            auto text = ParseString(error);
            if (!text.has_value()) return std::nullopt;
            return JsonValue{std::move(*text)};
        }
        case 't': return ParseLiteral("true", JsonValue{true}, error);
        case 'f': return ParseLiteral("false", JsonValue{false}, error);
        case 'n': return ParseLiteral("null", JsonValue{nullptr}, error);
        default: return ParseNumber(error);
        }
    }

    [[nodiscard]] std::optional<JsonValue> ParseObject(std::string& error, std::size_t depth) {
        ++cursor_;
        JsonValue::Object object;
        SkipWhitespace();
        if (Consume('}')) {
            return JsonValue{std::move(object)};
        }
        for (;;) {
            SkipWhitespace();
            auto key = ParseString(error);
            if (!key.has_value()) return std::nullopt;
            SkipWhitespace();
            if (!Consume(':')) {
                error = Error("expected ':'");
                return std::nullopt;
            }
            auto value = ParseValue(error, depth);
            if (!value.has_value()) return std::nullopt;
            object.insert_or_assign(std::move(*key), std::move(*value));
            SkipWhitespace();
            if (Consume('}')) break;
            if (!Consume(',')) {
                error = Error("expected ',' or '}'");
                return std::nullopt;
            }
        }
        return JsonValue{std::move(object)};
    }

    [[nodiscard]] std::optional<JsonValue> ParseArray(std::string& error, std::size_t depth) {
        ++cursor_;
        JsonValue::Array array;
        SkipWhitespace();
        if (Consume(']')) {
            return JsonValue{std::move(array)};
        }
        for (;;) {
            auto value = ParseValue(error, depth);
            if (!value.has_value()) return std::nullopt;
            array.emplace_back(std::move(*value));
            SkipWhitespace();
            if (Consume(']')) break;
            if (!Consume(',')) {
                error = Error("expected ',' or ']'");
                return std::nullopt;
            }
        }
        return JsonValue{std::move(array)};
    }

    [[nodiscard]] std::optional<std::string> ParseString(std::string& error) {
        if (!Consume('"')) {
            error = Error("expected string");
            return std::nullopt;
        }
        std::string result;
        while (cursor_ < input_.size()) {
            const char ch = input_[cursor_++];
            if (ch == '"') return result;
            if (static_cast<unsigned char>(ch) < 0x20U) {
                error = Error("control character in string");
                return std::nullopt;
            }
            if (ch != '\\') {
                result.push_back(ch);
                continue;
            }
            if (cursor_ >= input_.size()) {
                error = Error("unterminated escape");
                return std::nullopt;
            }
            const char escaped = input_[cursor_++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                if (cursor_ + 4 > input_.size()) {
                    error = Error("short unicode escape");
                    return std::nullopt;
                }
                unsigned codepoint = 0;
                for (int index = 0; index < 4; ++index) {
                    const char digit = input_[cursor_++];
                    codepoint <<= 4U;
                    if (digit >= '0' && digit <= '9') codepoint += static_cast<unsigned>(digit - '0');
                    else if (digit >= 'a' && digit <= 'f') codepoint += 10U + static_cast<unsigned>(digit - 'a');
                    else if (digit >= 'A' && digit <= 'F') codepoint += 10U + static_cast<unsigned>(digit - 'A');
                    else {
                        error = Error("invalid unicode escape");
                        return std::nullopt;
                    }
                }
                AppendUtf8(result, codepoint);
                break;
            }
            default:
                error = Error("invalid string escape");
                return std::nullopt;
            }
        }
        error = Error("unterminated string");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<JsonValue> ParseNumber(std::string& error) {
        const std::size_t start = cursor_;
        if (Peek('-')) ++cursor_;
        if (Peek('0')) {
            ++cursor_;
        } else {
            if (!HasDigit()) {
                error = Error("expected JSON value");
                return std::nullopt;
            }
            while (HasDigit()) ++cursor_;
        }
        if (Peek('.')) {
            ++cursor_;
            if (!HasDigit()) {
                error = Error("expected digit after decimal point");
                return std::nullopt;
            }
            while (HasDigit()) ++cursor_;
        }
        if (Peek('e') || Peek('E')) {
            ++cursor_;
            if (Peek('+') || Peek('-')) ++cursor_;
            if (!HasDigit()) {
                error = Error("expected exponent digits");
                return std::nullopt;
            }
            while (HasDigit()) ++cursor_;
        }
        const std::string token(input_.substr(start, cursor_ - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0' || !std::isfinite(value)) {
            error = Error("invalid number");
            return std::nullopt;
        }
        return JsonValue{value};
    }

    [[nodiscard]] std::optional<JsonValue> ParseLiteral(
        std::string_view literal,
        JsonValue value,
        std::string& error) {
        if (input_.substr(cursor_, literal.size()) != literal) {
            error = Error("invalid literal");
            return std::nullopt;
        }
        cursor_ += literal.size();
        return value;
    }

    static void AppendUtf8(std::string& output, unsigned codepoint) {
        if (codepoint <= 0x7FU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    void SkipWhitespace() {
        while (cursor_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[cursor_]))) {
            ++cursor_;
        }
    }

    [[nodiscard]] bool Consume(char expected) {
        if (!Peek(expected)) return false;
        ++cursor_;
        return true;
    }

    [[nodiscard]] bool Peek(char expected) const {
        return cursor_ < input_.size() && input_[cursor_] == expected;
    }

    [[nodiscard]] bool HasDigit() const {
        return cursor_ < input_.size() && input_[cursor_] >= '0' && input_[cursor_] <= '9';
    }

    [[nodiscard]] std::string Error(std::string_view message) const {
        std::ostringstream stream;
        stream << message << " at byte " << cursor_;
        return stream.str();
    }

    std::string_view input_;
    std::size_t cursor_ = 0;
};

[[nodiscard]] std::optional<JsonValue> ReadJson(
    const std::filesystem::path& path,
    std::vector<std::string>& warnings) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        warnings.emplace_back("Cannot stat " + path.string() + ": " + size_error.message());
        return std::nullopt;
    }
    if (size > 1024U * 1024U) {
        warnings.emplace_back("Refusing oversized config file " + path.string());
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        warnings.emplace_back("Cannot read " + path.string());
        return std::nullopt;
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEFU &&
        static_cast<unsigned char>(content[1]) == 0xBBU && static_cast<unsigned char>(content[2]) == 0xBFU) {
        content.erase(0, 3);
    }
    std::string parse_error;
    auto value = JsonParser(content).Parse(parse_error);
    if (!value.has_value() || value->AsObject() == nullptr) {
        warnings.emplace_back("Invalid JSON object in " + path.string() + ": " +
                              (parse_error.empty() ? "root is not an object" : parse_error));
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

[[nodiscard]] std::string TrimAscii(std::string_view value) {
    const auto whitespace = [](unsigned char character) { return std::isspace(character) != 0; };
    while (!value.empty() && whitespace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && whitespace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return std::string(value);
}

[[nodiscard]] std::uint32_t ParseBaseVirtualKey(std::string_view name) {
    const std::string key = LowerAscii(TrimAscii(name));
    if (key.empty()) return 0;
    if (key == "insert" || key == "ins") return VK_INSERT;
    if (key == "pagedown" || key == "page down" || key == "pgdn") return VK_NEXT;
    if (key == "pageup" || key == "page up" || key == "pgup") return VK_PRIOR;
    if (key == "home") return VK_HOME;
    if (key == "end") return VK_END;
    if (key == "delete" || key == "del") return VK_DELETE;
    if (key == "escape" || key == "esc") return VK_ESCAPE;
    if (key == "tab") return VK_TAB;
    if (key == "space") return VK_SPACE;
    if (key.size() == 1) {
        const unsigned char ch = static_cast<unsigned char>(key.front());
        if (std::isalnum(ch)) return static_cast<std::uint32_t>(std::toupper(ch));
    }
    if (key.size() >= 2 && key.front() == 'f') {
        int number = 0;
        const auto* begin = key.data() + 1;
        const auto* end = key.data() + key.size();
        const auto [ptr, ec] = std::from_chars(begin, end, number);
        if (ec == std::errc{} && ptr == end && number >= 1 && number <= 24) {
            return static_cast<std::uint32_t>(VK_F1 + number - 1);
        }
    }
    return 0;
}

[[nodiscard]] bool IsHexColor(const std::string& value) {
    return value.size() == 7 && value.front() == '#' &&
           std::all_of(value.begin() + 1, value.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

[[nodiscard]] int HexPair(std::string_view value) {
    auto digit = [](char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
        return 10 + ch - 'A';
    };
    return digit(value[0]) * 16 + digit(value[1]);
}

void ReadBool(const JsonValue& root, std::string_view key, bool& target) {
    if (const auto* value = root.Find(key); value != nullptr) {
        if (const auto* boolean = value->AsBool(); boolean != nullptr) target = *boolean;
    }
}

void ReadNumber(const JsonValue& root, std::string_view key, double& target) {
    if (const auto* value = root.Find(key); value != nullptr) {
        if (const auto* number = value->AsNumber(); number != nullptr && std::isfinite(*number)) target = *number;
    }
}

void ReadString(const JsonValue& root, std::string_view key, std::string& target) {
    if (const auto* value = root.Find(key); value != nullptr) {
        if (const auto* text = value->AsString(); text != nullptr) target = *text;
    }
}

void ApplyCrosshair(const JsonValue& root, CrosshairConfig& target, std::vector<std::string>& warnings) {
    ReadBool(root, "enabled", target.enabled);
    ReadString(root, "style", target.style_name);
    ReadString(root, "color", target.color_hex);
    ReadNumber(root, "size", target.size);
    ReadNumber(root, "thickness", target.thickness);
    ReadNumber(root, "gap", target.gap);
    ReadNumber(root, "opacity", target.opacity);
    ReadString(root, "toggleKey", target.toggle_key);

    target.style_name = LowerAscii(target.style_name);
    if (target.style_name == "dot") target.style = CrosshairStyle::Dot;
    else if (target.style_name == "circle") target.style = CrosshairStyle::Circle;
    else if (target.style_name == "x") target.style = CrosshairStyle::X;
    else if (target.style_name == "square") target.style = CrosshairStyle::Square;
    else {
        if (target.style_name != "plus") warnings.emplace_back("Unknown crosshair style; using plus");
        target.style_name = "plus";
        target.style = CrosshairStyle::Plus;
    }

    if (!IsHexColor(target.color_hex)) {
        warnings.emplace_back("Invalid crosshair color; using #ffffff");
        target.color_hex = "#ffffff";
    }
    target.size = std::clamp(target.size, 4.0, 96.0);
    target.thickness = std::clamp(target.thickness, 1.0, 12.0);
    target.gap = std::clamp(target.gap, 0.0, 32.0);
    target.opacity = std::clamp(target.opacity, 0.05, 1.0);
    target.toggle_virtual_key = ParseVirtualKey(target.toggle_key);
}

void ApplyPosition(const JsonValue& widgets, std::string_view id, WidgetPosition& target) {
    const auto* value = widgets.Find(id);
    if (value == nullptr || value->AsObject() == nullptr) return;
    ReadNumber(*value, "x", target.x_percent);
    ReadNumber(*value, "y", target.y_percent);
    target.x_percent = std::clamp(target.x_percent, 0.0, 100.0);
    target.y_percent = std::clamp(target.y_percent, 0.0, 100.0);
}

}  // namespace

std::filesystem::path DefaultOverlayConfigRoot() {
    wchar_t override_path[32768]{};
    const DWORD override_length = GetEnvironmentVariableW(
        L"HQ_OVERLAY_CONFIG_DIR", override_path, static_cast<DWORD>(std::size(override_path)));
    if (override_length > 0 && override_length < std::size(override_path)) {
        return std::filesystem::path(override_path);
    }
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length > 0 && length < std::size(buffer)) {
        return std::filesystem::path(buffer) / L"asta.hq-launcher" / L"config" / L"overlay";
    }
    return std::filesystem::temp_directory_path() / L"asta.hq-launcher" / L"config" / L"overlay";
}

LoadResult LoadOverlayConfig(const std::filesystem::path& root) {
    LoadResult result;
    result.value.root = root;

    const auto general_path = root / L"general.json";
    if (std::filesystem::is_regular_file(general_path)) {
        if (const auto general = ReadJson(general_path, result.warnings); general.has_value()) {
            ReadBool(*general, "enabled", result.value.enabled);
            ReadBool(*general, "obs_capture_armed", result.value.obs_capture_armed);
            ReadString(*general, "overlay_key", result.value.overlay_key);
        }
    }
    const HotkeyBinding overlay_hotkey = ParseHotkey(result.value.overlay_key);
    result.value.overlay_virtual_key = overlay_hotkey.virtual_key;
    result.value.overlay_modifiers = overlay_hotkey.modifiers;
    if (result.value.overlay_virtual_key == 0) {
        result.warnings.emplace_back("Unknown overlay key; using Insert");
        result.value.overlay_key = "Insert";
        result.value.overlay_virtual_key = VK_INSERT;
        result.value.overlay_modifiers = 0;
    }

    const auto module_crosshair = root / L"modules" / L"crosshair.json";
    const auto legacy_crosshair = root / L"crosshair.json";
    bool loaded_crosshair = false;
    if (std::filesystem::is_regular_file(module_crosshair)) {
        if (const auto value = ReadJson(module_crosshair, result.warnings); value.has_value()) {
            ApplyCrosshair(*value, result.value.crosshair, result.warnings);
            result.value.crosshair_source = module_crosshair;
            loaded_crosshair = true;
        }
    }
    if (!loaded_crosshair && std::filesystem::is_regular_file(legacy_crosshair)) {
        if (const auto value = ReadJson(legacy_crosshair, result.warnings); value.has_value()) {
            ApplyCrosshair(*value, result.value.crosshair, result.warnings);
            result.value.crosshair_source = legacy_crosshair;
        }
    }

    const auto timer_path = root / L"modules" / L"game_timer.json";
    if (std::filesystem::is_regular_file(timer_path)) {
        if (const auto timer = ReadJson(timer_path, result.warnings); timer.has_value()) {
            ReadBool(*timer, "enabled", result.value.timer.enabled);
        }
    }

    const auto widgets_path = root / L"widgets.json";
    if (std::filesystem::is_regular_file(widgets_path)) {
        if (const auto widgets = ReadJson(widgets_path, result.warnings); widgets.has_value()) {
            ApplyPosition(*widgets, "crosshair", result.value.crosshair.position);
            ApplyPosition(*widgets, "game_timer", result.value.timer.position);
        }
    }
    return result;
}

std::uint32_t ParseVirtualKey(const std::string& name) {
    if (name.find('+') != std::string::npos) return 0;
    return ParseBaseVirtualKey(name);
}

HotkeyBinding ParseHotkey(const std::string& name) {
    HotkeyBinding binding{};
    std::size_t offset = 0;
    for (;;) {
        const std::size_t separator = name.find('+', offset);
        const std::string token = LowerAscii(TrimAscii(std::string_view(name).substr(
            offset, separator == std::string::npos ? std::string::npos : separator - offset)));
        if (token.empty()) return {};
        if (token == "ctrl" || token == "control") {
            binding.modifiers |= kHotkeyModifierControl;
        } else if (token == "shift") {
            binding.modifiers |= kHotkeyModifierShift;
        } else if (token == "alt" || token == "option") {
            binding.modifiers |= kHotkeyModifierAlt;
        } else if (token == "meta" || token == "win" || token == "windows" || token == "cmd") {
            binding.modifiers |= kHotkeyModifierMeta;
        } else {
            if (binding.virtual_key != 0) return {};
            binding.virtual_key = ParseBaseVirtualKey(token);
            if (binding.virtual_key == 0) return {};
        }
        if (separator == std::string::npos) break;
        offset = separator + 1;
    }
    return binding.virtual_key == 0 ? HotkeyBinding{} : binding;
}

const char* CrosshairStyleName(CrosshairStyle style) noexcept {
    switch (style) {
    case CrosshairStyle::Plus: return "plus";
    case CrosshairStyle::Dot: return "dot";
    case CrosshairStyle::Circle: return "circle";
    case CrosshairStyle::X: return "x";
    case CrosshairStyle::Square: return "square";
    }
    return "plus";
}

}  // namespace hq::config
