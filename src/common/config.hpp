#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hq::config {

inline constexpr std::uint8_t kHotkeyModifierControl = 1U << 0U;
inline constexpr std::uint8_t kHotkeyModifierShift = 1U << 1U;
inline constexpr std::uint8_t kHotkeyModifierAlt = 1U << 2U;
inline constexpr std::uint8_t kHotkeyModifierMeta = 1U << 3U;

struct HotkeyBinding {
    std::uint32_t virtual_key = 0;
    std::uint8_t modifiers = 0;
};

enum class CrosshairStyle {
    Plus,
    Dot,
    Circle,
    X,
    Square,
};

struct RgbaColor {
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    float a = 0.9F;
};

struct WidgetPosition {
    double x_percent = 0.0;
    double y_percent = 0.0;
};

struct CrosshairConfig {
    bool enabled = false;
    CrosshairStyle style = CrosshairStyle::Plus;
    std::string style_name = "plus";
    std::string color_hex = "#ffffff";
    double size = 24.0;
    double thickness = 2.0;
    double gap = 5.0;
    double opacity = 0.9;
    std::string toggle_key;
    std::uint32_t toggle_virtual_key = 0;
    WidgetPosition position{50.0, 50.0};
};

struct TimerConfig {
    bool enabled = false;
    bool show_real_time = true;
    std::string label = "REAL TIME";
    WidgetPosition position{4.0, 6.0};
};

struct OverlayConfig {
    bool enabled = true;
    bool obs_capture_armed = false;
    std::string overlay_key = "Insert";
    std::uint32_t overlay_virtual_key = 0x2D;
    std::uint8_t overlay_modifiers = 0;
    CrosshairConfig crosshair;
    TimerConfig timer;
    std::filesystem::path root;
    std::filesystem::path crosshair_source;
};

struct LoadResult {
    OverlayConfig value;
    std::vector<std::string> warnings;
};

[[nodiscard]] std::filesystem::path DefaultOverlayConfigRoot();
[[nodiscard]] LoadResult LoadOverlayConfig(const std::filesystem::path& root);
[[nodiscard]] std::uint32_t ParseVirtualKey(const std::string& name);
[[nodiscard]] HotkeyBinding ParseHotkey(const std::string& name);
[[nodiscard]] const char* CrosshairStyleName(CrosshairStyle style) noexcept;

}  // namespace hq::config
