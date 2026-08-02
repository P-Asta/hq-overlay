#include "config.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
}

void Write(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      (L"hq_overlay_config_tests_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);

    {
        const auto loaded = hq::config::LoadOverlayConfig(root);
        Expect(loaded.value.overlay_virtual_key == VK_INSERT, "missing config uses Insert");
        Expect(loaded.value.overlay_modifiers == 0, "missing config uses no modifiers");
        Expect(!loaded.value.crosshair.enabled, "crosshair defaults off");
        Expect(!loaded.value.timer.enabled, "timer defaults off");
    }

    Write(root / L"general.json", R"({"enabled":true,"overlay_key":"PageDown"})");
    Write(root / L"crosshair.json",
          R"({"enabled":false,"style":"square","color":"#ff0000","size":12,"thickness":2,"gap":3,"opacity":0.5})");
    Write(root / L"modules" / L"crosshair.json",
          R"({"enabled":true,"style":"dot","color":"#00ff80","size":999,"thickness":0,"gap":99,"opacity":4,"toggleKey":"F8"})");
    Write(root / L"modules" / L"game_timer.json", R"({"enabled":true})");
    Write(root / L"widgets.json",
          R"({"crosshair":{"x":41.5,"y":59.25},"game_timer":{"x":-2,"y":120}})");

    {
        const auto loaded = hq::config::LoadOverlayConfig(root);
        Expect(loaded.value.overlay_virtual_key == VK_NEXT, "PageDown maps to VK_NEXT");
        Expect(loaded.value.overlay_modifiers == 0, "PageDown has no modifiers");
        Expect(loaded.value.crosshair.enabled, "module crosshair wins over legacy top-level file");
        Expect(loaded.value.crosshair.style == hq::config::CrosshairStyle::Dot, "dot style parsed");
        Expect(loaded.value.crosshair.color_hex == "#00ff80", "hex color parsed");
        Expect(loaded.value.crosshair.size == 96.0, "size clamped");
        Expect(loaded.value.crosshair.thickness == 1.0, "thickness clamped");
        Expect(loaded.value.crosshair.gap == 32.0, "gap clamped");
        Expect(loaded.value.crosshair.opacity == 1.0, "opacity clamped");
        Expect(loaded.value.crosshair.toggle_virtual_key == VK_F8, "F8 parsed");
        Expect(loaded.value.crosshair.position.x_percent == 41.5, "crosshair x read");
        Expect(loaded.value.crosshair.position.y_percent == 59.25, "crosshair y read");
        Expect(loaded.value.timer.enabled, "timer enabled parsed");
        Expect(loaded.value.timer.position.x_percent == 0.0, "timer x clamped");
        Expect(loaded.value.timer.position.y_percent == 100.0, "timer y clamped");
        Expect(loaded.value.crosshair_source == root / L"modules" / L"crosshair.json",
               "source path reports module config");
    }

    Write(root / L"general.json", R"({"enabled":true,"overlay_key":"Ctrl+Shift+K"})");
    {
        const auto loaded = hq::config::LoadOverlayConfig(root);
        Expect(loaded.value.overlay_virtual_key == 'K', "Ctrl+Shift+K maps its base key");
        Expect(loaded.value.overlay_modifiers ==
                   (hq::config::kHotkeyModifierControl | hq::config::kHotkeyModifierShift),
               "Ctrl+Shift+K preserves exact modifiers");
        Expect(hq::config::ParseVirtualKey("Ctrl+K") == 0,
               "legacy single-key parser does not silently drop modifiers");
    }

    Write(root / L"general.json", R"({"enabled":true,"overlay_key":"Ctrl+K+P"})");
    {
        const auto loaded = hq::config::LoadOverlayConfig(root);
        Expect(loaded.value.overlay_virtual_key == VK_INSERT,
               "multiple base keys fall back to Insert");
        Expect(loaded.value.overlay_modifiers == 0,
               "invalid chord fallback clears modifiers");
    }

    Write(root / L"modules" / L"crosshair.json", "{ definitely not json }");
    {
        const auto loaded = hq::config::LoadOverlayConfig(root);
        Expect(!loaded.value.crosshair.enabled, "malformed module falls back to legacy crosshair");
        Expect(loaded.value.crosshair.style == hq::config::CrosshairStyle::Square,
               "fallback legacy style parsed");
        Expect(!loaded.warnings.empty(), "malformed JSON reports warning");
    }

    std::filesystem::remove_all(root, cleanup_error);
    if (g_failures == 0) {
        std::cout << "All config parser tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " config parser test(s) failed.\n";
    return 1;
}
