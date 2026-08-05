#include "overlay_runtime.hpp"

#include "config.hpp"
#include "logging.hpp"
#include "webview_overlay.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <MinHook.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace hq::overlay {
namespace {

using Microsoft::WRL::ComPtr;
using PresentFunction = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFunction = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

constexpr std::chrono::milliseconds kConfigPollInterval{1000};
constexpr wchar_t kDummyClassName[] = L"HQOverlay.D3D11.HookProbe";

std::atomic_bool g_process_detaching{false};
std::atomic_bool g_soft_disabled{false};
std::atomic_bool g_ready{false};
std::atomic_bool g_imgui_input_ready{false};
std::atomic_bool g_panel_open{false};
std::atomic_int g_cursor_visibility_adjustments{0};
std::atomic_bool g_crosshair_toggle_requested{false};
std::atomic_bool g_config_reload_requested{false};
std::atomic_bool g_window_invalidated{false};
std::atomic_bool g_exclusive_fullscreen_converted{false};
std::atomic_uint32_t g_overlay_hotkey{VK_INSERT};
std::atomic<std::uint8_t> g_overlay_hotkey_modifiers{0};
std::atomic_uint32_t g_crosshair_hotkey{0};
std::atomic_uint32_t g_selected_resize_count{0};
std::atomic<HWND> g_hooked_window{nullptr};
std::atomic<LONG_PTR> g_original_wndproc{0};
std::atomic<HMODULE> g_overlay_module{nullptr};

PresentFunction g_original_present = nullptr;
ResizeBuffersFunction g_original_resize_buffers = nullptr;
HANDLE g_ready_event = nullptr;
HANDLE g_shutdown_event = nullptr;
HANDLE g_disable_event = nullptr;
std::mutex g_renderer_mutex;
std::mutex g_prepared_config_mutex;
std::mutex g_wndproc_chain_mutex;
std::mutex g_ready_transition_mutex;
std::unordered_map<HWND, WNDPROC> g_wndproc_forwarders;

struct ConfigStamp {
    std::filesystem::file_time_type general{};
    std::filesystem::file_time_type crosshair{};
    std::filesystem::file_time_type timer{};
    std::filesystem::file_time_type widgets{};

    friend bool operator==(const ConfigStamp&, const ConfigStamp&) = default;
};

struct RendererState {
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11RenderTargetView> render_target;
    ComPtr<ID3D11Texture2D> webview_capture_texture;
    ComPtr<ID3D11ShaderResourceView> webview_capture_view;
    std::uint64_t webview_capture_generation = 0;
    HWND window = nullptr;
    WNDPROC original_wndproc = nullptr;
    bool imgui_context_created = false;
    bool win32_backend_initialized = false;
    bool dx11_backend_initialized = false;
    bool config_loaded = false;
    bool runtime_crosshair_enabled = false;
    bool last_config_crosshair_enabled = false;
    bool runtime_timer_enabled = false;
    bool last_config_timer_enabled = false;
    std::uint64_t config_generation = 0;
    config::LoadResult config;
    ImFont* ui_font = nullptr;
    ImFont* label_font = nullptr;
    ImFont* value_font = nullptr;
};

RendererState g_renderer;
config::LoadResult g_prepared_config;
ConfigStamp g_prepared_config_stamp{};
std::uint64_t g_prepared_config_generation = 0;
bool g_prepared_config_loaded = false;

class ScopedOutputMergerState final {
public:
    explicit ScopedOutputMergerState(ID3D11DeviceContext* context) : context_(context) {
        if (context_ == nullptr) return;
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> raw_targets{};
        ID3D11DepthStencilView* raw_depth_stencil = nullptr;
        context_->OMGetRenderTargets(static_cast<UINT>(raw_targets.size()), raw_targets.data(), &raw_depth_stencil);
        for (std::size_t index = 0; index < raw_targets.size(); ++index) {
            render_targets_[index].Attach(raw_targets[index]);
            if (raw_targets[index] != nullptr) render_target_count_ = static_cast<UINT>(index + 1);
        }
        depth_stencil_.Attach(raw_depth_stencil);
    }

    ~ScopedOutputMergerState() {
        if (context_ == nullptr) return;
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> raw_targets{};
        for (std::size_t index = 0; index < raw_targets.size(); ++index) {
            raw_targets[index] = render_targets_[index].Get();
        }
        context_->OMSetRenderTargets(
            render_target_count_, render_target_count_ == 0 ? nullptr : raw_targets.data(), depth_stencil_.Get());
    }

    ScopedOutputMergerState(const ScopedOutputMergerState&) = delete;
    ScopedOutputMergerState& operator=(const ScopedOutputMergerState&) = delete;

private:
    ID3D11DeviceContext* context_ = nullptr;
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> render_targets_{};
    ComPtr<ID3D11DepthStencilView> depth_stencil_;
    UINT render_target_count_ = 0;
};

class SelectedResizeGate final {
public:
    SelectedResizeGate() = default;

    void Activate() {
        if (active_) return;
        g_selected_resize_count.fetch_add(1, std::memory_order_acq_rel);
        active_ = true;
    }

    ~SelectedResizeGate() {
        if (active_) g_selected_resize_count.fetch_sub(1, std::memory_order_acq_rel);
    }

    SelectedResizeGate(const SelectedResizeGate&) = delete;
    SelectedResizeGate& operator=(const SelectedResizeGate&) = delete;

private:
    bool active_ = false;
};

[[nodiscard]] std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
}

[[nodiscard]] std::wstring EventName(const wchar_t* kind) {
    std::wostringstream name;
    name << L"Local\\HQOverlay" << kind << L'_' << GetCurrentProcessId();
    return name.str();
}

[[nodiscard]] bool EventSignaled(HANDLE event_handle) {
    return event_handle != nullptr && WaitForSingleObject(event_handle, 0) == WAIT_OBJECT_0;
}

[[nodiscard]] bool ExternalDisableSignaled() {
    return EventSignaled(g_shutdown_event) || EventSignaled(g_disable_event);
}

void InitializeEvents() {
    const std::wstring ready_name = EventName(L"Ready");
    const std::wstring shutdown_name = EventName(L"Shutdown");
    const std::wstring disable_name = EventName(L"Disable");
    // CreateEvent also opens a launcher-created event with the same name. All
    // three events are manual-reset by contract; the initial state is ignored
    // when the named object already exists.
    g_ready_event = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    g_shutdown_event = CreateEventW(nullptr, TRUE, FALSE, shutdown_name.c_str());
    g_disable_event = CreateEventW(nullptr, TRUE, FALSE, disable_name.c_str());
    logging::Write(logging::Level::Info, "Handshake events: " + Narrow(ready_name) + ", " +
                                              Narrow(shutdown_name) + ", " + Narrow(disable_name));
}

[[nodiscard]] std::filesystem::file_time_type FileTimestamp(const std::filesystem::path& path) {
    std::error_code error;
    const auto value = std::filesystem::last_write_time(path, error);
    return error ? std::filesystem::file_time_type{} : value;
}

[[nodiscard]] ConfigStamp ReadConfigStamp(const std::filesystem::path& root) {
    return ConfigStamp{
        FileTimestamp(root / L"general.json"),
        FileTimestamp(root / L"modules" / L"crosshair.json"),
        FileTimestamp(root / L"modules" / L"game_timer.json"),
        FileTimestamp(root / L"widgets.json"),
    };
}

void PollConfigurationOnWorker(bool force) {
    const std::filesystem::path root = config::DefaultOverlayConfigRoot();
    const ConfigStamp stamp = ReadConfigStamp(root);
    {
        std::scoped_lock lock(g_prepared_config_mutex);
        if (!force && g_prepared_config_loaded && stamp == g_prepared_config_stamp) return;
    }

    auto loaded = config::LoadOverlayConfig(root);
    std::ostringstream message;
    message << "Config loaded from " << root.string() << "; crosshair="
            << (loaded.value.crosshair.enabled ? "on" : "off") << " style="
            << config::CrosshairStyleName(loaded.value.crosshair.style) << "; timer="
            << (loaded.value.timer.enabled ? "on" : "off") << "; OBS game capture="
            << (loaded.value.obs_capture_armed ? "on" : "off") << "; warnings="
            << loaded.warnings.size();
    logging::Write(logging::Level::Info, message.str());
    for (const auto& warning : loaded.warnings) {
        logging::Write(logging::Level::Warning, warning);
    }
    const std::uint32_t overlay_hotkey = loaded.value.overlay_virtual_key;
    const std::uint8_t overlay_hotkey_modifiers = loaded.value.overlay_modifiers;
    const std::uint32_t crosshair_hotkey = loaded.value.crosshair.toggle_virtual_key;
    const bool backbuffer_capture_enabled = loaded.value.obs_capture_armed;
    {
        std::scoped_lock lock(g_prepared_config_mutex);
        g_prepared_config = std::move(loaded);
        g_prepared_config_stamp = stamp;
        g_prepared_config_loaded = true;
        ++g_prepared_config_generation;
    }
    // The HTML host and the game WndProc do not depend on Present to consume
    // these values. Publish them as soon as the worker observes a config
    // change so an open WebView uses the new hotkey immediately.
    g_overlay_hotkey.store(overlay_hotkey, std::memory_order_release);
    g_overlay_hotkey_modifiers.store(overlay_hotkey_modifiers, std::memory_order_release);
    webview::SetSettingsHotkey(overlay_hotkey, overlay_hotkey_modifiers);
    webview::SetBackBufferCaptureEnabled(backbuffer_capture_enabled);
    g_crosshair_hotkey.store(crosshair_hotkey, std::memory_order_release);
}

// Polls the launcher's LCStatsTracker relay file alongside the config poll. The
// launcher writes <config-root>/lcstats.json whenever it receives the day's
// stats over SSE — a dependable source the C# SSE server's lossy request-per-
// packet delivery cannot guarantee for the overlay. We only re-load when the
// file mtime changes, and only mark it consumed after a successful read so a
// partial write during the launcher's std::fs::write is retried next tick.
void PollLcStatsOnWorker() {
    static std::filesystem::file_time_type last_consumed{};
    const auto path = config::DefaultOverlayConfigRoot() / L"lcstats.json";
    const auto mtime = FileTimestamp(path);
    if (mtime == last_consumed) return;

    std::ifstream file(path, std::ios::binary);
    if (!file) return;  // missing or unreadable; try again next tick
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEFU &&
        static_cast<unsigned char>(content[1]) == 0xBBU &&
        static_cast<unsigned char>(content[2]) == 0xBFU) {
        content.erase(0, 3);
    }
    while (!content.empty() &&
           (content.back() == '\n' || content.back() == '\r' || content.back() == ' ' ||
            content.back() == '\t')) {
        content.pop_back();
    }
    constexpr std::size_t kMaximumLcStatsFileBytes = 8U * 1024U * 1024U;
    if (content.empty() || content.size() > kMaximumLcStatsFileBytes) return;
    webview::EnqueueLcStatsFilePayload(std::move(content));
    last_consumed = mtime;
}

void ApplyPreparedConfiguration() {
    config::LoadResult loaded;
    std::uint64_t generation = 0;
    {
        std::scoped_lock lock(g_prepared_config_mutex);
        if (!g_prepared_config_loaded || g_renderer.config_generation == g_prepared_config_generation) return;
        loaded = g_prepared_config;
        generation = g_prepared_config_generation;
    }
    if (!g_renderer.config_loaded || loaded.value.crosshair.enabled != g_renderer.last_config_crosshair_enabled) {
        g_renderer.runtime_crosshair_enabled = loaded.value.crosshair.enabled;
    }
    if (!g_renderer.config_loaded || loaded.value.timer.enabled != g_renderer.last_config_timer_enabled) {
        g_renderer.runtime_timer_enabled = loaded.value.timer.enabled;
    }
    g_renderer.last_config_crosshair_enabled = loaded.value.crosshair.enabled;
    g_renderer.last_config_timer_enabled = loaded.value.timer.enabled;
    g_renderer.config = std::move(loaded);
    g_renderer.config_generation = generation;
    g_renderer.config_loaded = true;
}

void PromoteWebViewReady() {
    // Fast path: this runs on every Present, and almost every frame the overlay
    // is already ready. Skip the mutex in the steady state to avoid taking a
    // kernel lock at frame rate. We only need the lock for the readiness
    // transition itself and for reflecting panel-open state.
    if (g_ready.load(std::memory_order_acquire)) return;
    std::scoped_lock transition_lock(g_ready_transition_mutex);
    if (g_soft_disabled.load(std::memory_order_acquire) ||
        g_process_detaching.load(std::memory_order_acquire) ||
        g_window_invalidated.load(std::memory_order_acquire) ||
        webview::CurrentState() != webview::State::DomReady) {
        return;
    }
    g_panel_open.store(webview::SettingsOpen(), std::memory_order_release);
    if (!g_ready.exchange(true, std::memory_order_acq_rel)) {
        if (g_ready_event != nullptr) SetEvent(g_ready_event);
        logging::Write(logging::Level::Info, "Embedded HTML DOM ready; native ready event signaled");
    }
}

void MarkWebViewNotReady() {
    std::scoped_lock transition_lock(g_ready_transition_mutex);
    g_ready.store(false, std::memory_order_release);
    if (g_ready_event != nullptr) ResetEvent(g_ready_event);
}

[[nodiscard]] ImU32 HexColor(const config::CrosshairConfig& crosshair, float alpha_multiplier = 1.0F) {
    unsigned red = 255;
    unsigned green = 255;
    unsigned blue = 255;
    if (crosshair.color_hex.size() == 7) {
        const auto hex_digit = [](char ch) -> unsigned {
            if (ch >= '0' && ch <= '9') return static_cast<unsigned>(ch - '0');
            if (ch >= 'a' && ch <= 'f') return 10U + static_cast<unsigned>(ch - 'a');
            return 10U + static_cast<unsigned>(ch - 'A');
        };
        const char* text = crosshair.color_hex.c_str() + 1;
        red = hex_digit(text[0]) * 16U + hex_digit(text[1]);
        green = hex_digit(text[2]) * 16U + hex_digit(text[3]);
        blue = hex_digit(text[4]) * 16U + hex_digit(text[5]);
    }
    const auto alpha = static_cast<unsigned>(std::clamp(crosshair.opacity * alpha_multiplier, 0.0, 1.0) * 255.0);
    return IM_COL32(red, green, blue, alpha);
}

void DrawCrosshair(const config::CrosshairConfig& crosshair) {
    if (!g_renderer.runtime_crosshair_enabled) return;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 center{
        io.DisplaySize.x * static_cast<float>(crosshair.position.x_percent / 100.0),
        io.DisplaySize.y * static_cast<float>(crosshair.position.y_percent / 100.0),
    };
    const float size = static_cast<float>(crosshair.size);
    const float thickness = static_cast<float>(crosshair.thickness);
    const float gap = static_cast<float>(crosshair.gap);
    const ImU32 color = HexColor(crosshair);
    ImDrawList* draw = ImGui::GetForegroundDrawList();

    const auto line = [&](ImVec2 from, ImVec2 to) {
        draw->AddLine(from, to, color, thickness);
    };
    const float half = size * 0.5F;
    const float half_gap = gap * 0.5F;
    switch (crosshair.style) {
    case config::CrosshairStyle::Dot:
        draw->AddCircleFilled(center, thickness * 0.5F, color, 20);
        break;
    case config::CrosshairStyle::Circle:
        draw->AddCircle(center, std::max(1.0F, half - thickness * 0.5F), color, 64, thickness);
        break;
    case config::CrosshairStyle::Square: {
        const ImVec2 top_left{center.x - half, center.y - half};
        const ImVec2 bottom_right{center.x + half, center.y + half};
        draw->AddRect(top_left, bottom_right, color, 0.0F, 0, thickness);
        break;
    }
    case config::CrosshairStyle::X: {
        const float diagonal_half = std::max(1.0F, (size - gap) * 0.35355339F);
        line({center.x - diagonal_half, center.y - diagonal_half},
             {center.x + diagonal_half, center.y + diagonal_half});
        line({center.x - diagonal_half, center.y + diagonal_half},
             {center.x + diagonal_half, center.y - diagonal_half});
        break;
    }
    case config::CrosshairStyle::Plus:
        line({center.x - half, center.y}, {center.x - half_gap, center.y});
        line({center.x + half_gap, center.y}, {center.x + half, center.y});
        line({center.x, center.y - half}, {center.x, center.y - half_gap});
        line({center.x, center.y + half_gap}, {center.x, center.y + half});
        break;
    }
}

[[nodiscard]] std::string RealTimeText() {
    std::time_t raw_time = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &raw_time);
    int hour = local.tm_hour % 12;
    if (hour == 0) hour = 12;
    std::ostringstream text;
    text << std::setfill('0') << std::setw(2) << hour << ':' << std::setw(2) << local.tm_min << ' '
         << (local.tm_hour >= 12 ? "PM" : "AM");
    return text.str();
}

void DrawTimer(const config::TimerConfig& timer) {
    if (!g_renderer.runtime_timer_enabled) return;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 origin{
        io.DisplaySize.x * static_cast<float>(timer.position.x_percent / 100.0),
        io.DisplaySize.y * static_cast<float>(timer.position.y_percent / 100.0),
    };
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImFont* label_font = g_renderer.label_font != nullptr ? g_renderer.label_font : ImGui::GetFont();
    ImFont* value_font = g_renderer.value_font != nullptr ? g_renderer.value_font : ImGui::GetFont();
    const ImVec2 label_position{origin.x + 20.0F, origin.y + 20.0F};
    const ImVec2 value_position{origin.x + 20.0F, origin.y + 33.0F};
    draw->AddText(label_font, 10.0F, label_position, IM_COL32(255, 255, 255, 133), timer.label.c_str());
    const std::string value = RealTimeText();
    draw->AddText(value_font, 24.0F, value_position, IM_COL32_WHITE, value.c_str());
}

void ApplyImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0F;
    style.ChildRounding = 4.0F;
    style.FrameRounding = 4.0F;
    style.PopupRounding = 4.0F;
    style.WindowBorderSize = 1.0F;
    style.FrameBorderSize = 1.0F;
    style.WindowPadding = {16.0F, 14.0F};
    style.ItemSpacing = {9.0F, 8.0F};
    style.Colors[ImGuiCol_Text] = {1.0F, 1.0F, 1.0F, 1.0F};
    style.Colors[ImGuiCol_TextDisabled] = {1.0F, 1.0F, 1.0F, 0.45F};
    style.Colors[ImGuiCol_WindowBg] = {18.0F / 255.0F, 19.0F / 255.0F, 24.0F / 255.0F, 0.98F};
    style.Colors[ImGuiCol_Border] = {1.0F, 1.0F, 1.0F, 0.15F};
    style.Colors[ImGuiCol_FrameBg] = {1.0F, 1.0F, 1.0F, 0.08F};
    style.Colors[ImGuiCol_FrameBgHovered] = {1.0F, 1.0F, 1.0F, 0.13F};
    style.Colors[ImGuiCol_FrameBgActive] = {1.0F, 1.0F, 1.0F, 0.17F};
    style.Colors[ImGuiCol_Button] = {1.0F, 1.0F, 1.0F, 0.08F};
    style.Colors[ImGuiCol_ButtonHovered] = {1.0F, 1.0F, 1.0F, 0.15F};
    style.Colors[ImGuiCol_ButtonActive] = {1.0F, 1.0F, 1.0F, 0.20F};
    style.Colors[ImGuiCol_CheckMark] = {1.0F, 0.83F, 0.23F, 1.0F};
    style.Colors[ImGuiCol_Header] = {1.0F, 1.0F, 1.0F, 0.08F};
    style.Colors[ImGuiCol_HeaderHovered] = {1.0F, 1.0F, 1.0F, 0.14F};
    style.Colors[ImGuiCol_HeaderActive] = {1.0F, 1.0F, 1.0F, 0.18F};
}

void DrawSettingsPanel() {
    bool open = g_panel_open.load(std::memory_order_acquire);
    if (!open) return;
    ImGui::SetNextWindowSize({520.0F, 355.0F}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({32.0F, 32.0F}, ImGuiCond_FirstUseEver);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("HQ Overlay / Native", &open, flags)) {
        ImGui::TextUnformatted("D3D11 injection renderer");
        ImGui::SameLine();
        ImGui::TextDisabled("v0.1.0");
        ImGui::Separator();
        ImGui::Text("Status: %s", g_ready.load(std::memory_order_acquire) ? "Ready" : "Initializing");
        ImGui::Text("Window: 0x%p", static_cast<void*>(g_renderer.window));
        ImGui::Text("Settings hotkey: %s", g_renderer.config.value.overlay_key.c_str());
        ImGui::TextWrapped("Config: %s", g_renderer.config.value.root.string().c_str());
        ImGui::Spacing();
        ImGui::Checkbox("Crosshair (this session)", &g_renderer.runtime_crosshair_enabled);
        ImGui::SameLine();
        ImGui::TextDisabled("%s / %.0f px", g_renderer.config.value.crosshair.style_name.c_str(),
                            g_renderer.config.value.crosshair.size);
        ImGui::Checkbox("Real Time (this session)", &g_renderer.runtime_timer_enabled);
        ImGui::TextDisabled("External JSON changes are reloaded automatically.");
        if (!g_renderer.config.warnings.empty()) {
            ImGui::TextColored({1.0F, 0.75F, 0.35F, 1.0F}, "%zu config warning(s); see log.",
                               g_renderer.config.warnings.size());
        }
        ImGui::Spacing();
        if (ImGui::Button("Reload configuration")) {
            g_config_reload_requested.store(true, std::memory_order_release);
        }
        ImGui::SameLine();
        if (ImGui::Button("Soft-disable native overlay")) {
            RequestSoftDisable("settings panel request");
        }
        ImGui::Separator();
        ImGui::TextDisabled("Soft-disable leaves the DLL loaded and lets the launcher use legacy mode safely.");
    }
    ImGui::End();
    g_panel_open.store(open, std::memory_order_release);
}

LRESULT CALLBACK HookWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

[[nodiscard]] WNDPROC WindowProcedureForwarder(HWND window) {
    // Fast path: the common case is a single hooked game window. Its HWND and
    // original WndProc are published atomically, so we can resolve the forwarder
    // without touching g_wndproc_chain_mutex. That lock was acquired on every
    // window message (WM_MOUSEMOVE etc. fire hundreds of times per second) and
    // contended with the render thread; skipping it removes per-message kernel
    // transitions from the hot path.
    if (g_hooked_window.load(std::memory_order_acquire) == window) {
        const auto original = g_original_wndproc.load(std::memory_order_acquire);
        if (original != 0) return reinterpret_cast<WNDPROC>(original);
    }
    // Fallback for the auxiliary map (multi-window / reinstall edge cases).
    std::scoped_lock lock(g_wndproc_chain_mutex);
    const auto entry = g_wndproc_forwarders.find(window);
    if (entry != g_wndproc_forwarders.end()) return entry->second;
    return nullptr;
}

void ForgetWindowProcedureForwarder(HWND window, WNDPROC expected) {
    bool removed = false;
    {
        std::scoped_lock lock(g_wndproc_chain_mutex);
        const auto entry = g_wndproc_forwarders.find(window);
        if (entry != g_wndproc_forwarders.end() && (expected == nullptr || entry->second == expected)) {
            g_wndproc_forwarders.erase(entry);
            removed = true;
        }
    }
    if (!removed) return;
    HWND hooked = window;
    if (g_hooked_window.compare_exchange_strong(
            hooked, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
        g_original_wndproc.store(0, std::memory_order_release);
    }
}

[[nodiscard]] bool InstallWindowProcedure(HWND window, WNDPROC& original) {
    {
        std::scoped_lock lock(g_wndproc_chain_mutex);
        const auto retained = g_wndproc_forwarders.find(window);
        if (retained != g_wndproc_forwarders.end()) {
            original = retained->second;
            g_hooked_window.store(window, std::memory_order_release);
            g_original_wndproc.store(reinterpret_cast<LONG_PTR>(original), std::memory_order_release);
            logging::Write(logging::Level::Info,
                           "Reusing retained WndProc forwarding entry without creating a chain cycle");
            return true;
        }
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR observed = GetWindowLongPtrW(window, GWLP_WNDPROC);
    if (observed == 0 && GetLastError() != ERROR_SUCCESS) {
        logging::Write(logging::Level::Error, "GetWindowLongPtrW failed before WndProc installation");
        return false;
    }
    if (observed == reinterpret_cast<LONG_PTR>(&HookWindowProcedure)) {
        logging::Write(logging::Level::Error,
                       "WndProc is already HQ Overlay but its forwarding entry is unavailable; refusing recursion");
        return false;
    }

    original = reinterpret_cast<WNDPROC>(observed);
    {
        std::scoped_lock lock(g_wndproc_chain_mutex);
        g_wndproc_forwarders.insert_or_assign(window, original);
    }
    // Publish the forwarding target before installing the hook. If a message
    // arrives immediately after SetWindowLongPtrW, HookWindowProcedure can
    // already forward it safely.
    g_hooked_window.store(window, std::memory_order_release);
    g_original_wndproc.store(observed, std::memory_order_release);

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR displaced = SetWindowLongPtrW(
        window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookWindowProcedure));
    if (displaced == 0 && GetLastError() != ERROR_SUCCESS) {
        ForgetWindowProcedureForwarder(window, original);
        logging::Write(logging::Level::Error, "SetWindowLongPtrW failed; renderer will not claim readiness");
        return false;
    }
    if (displaced != observed) {
        // Another hook won the small interval between observation and install.
        // Chain to the actual procedure we displaced.
        original = reinterpret_cast<WNDPROC>(displaced);
        {
            std::scoped_lock lock(g_wndproc_chain_mutex);
            g_wndproc_forwarders.insert_or_assign(window, original);
        }
        g_original_wndproc.store(displaced, std::memory_order_release);
        logging::Write(logging::Level::Warning,
                       "WndProc head changed during installation; forwarding to the procedure actually displaced");
    }
    return true;
}

LRESULT CALLBACK HookWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    WNDPROC original = WindowProcedureForwarder(window);
    if (original == &HookWindowProcedure) original = nullptr;
    const UINT restore_focus_message = webview::RestoreGameFocusMessage();
    if (restore_focus_message != 0 && message == restore_focus_message) {
        int cursor_adjustments = g_cursor_visibility_adjustments.exchange(0, std::memory_order_acq_rel);
        while (cursor_adjustments-- > 0) (void)ShowCursor(FALSE);
        if (GetFocus() != window) (void)SetFocus(window);
        return 0;
    }
    const bool key_down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool mouse_message = (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_MOUSELEAVE;
    const bool key_repeat = (static_cast<std::uintptr_t>(lparam) & (1ULL << 30U)) != 0;

    const auto current_modifier_mask = [] {
        std::uint8_t modifiers = 0;
        const auto down = [](int virtual_key) { return (GetKeyState(virtual_key) & 0x8000) != 0; };
        if (down(VK_CONTROL)) modifiers |= config::kHotkeyModifierControl;
        if (down(VK_SHIFT)) modifiers |= config::kHotkeyModifierShift;
        if (down(VK_MENU)) modifiers |= config::kHotkeyModifierAlt;
        if (down(VK_LWIN) || down(VK_RWIN)) modifiers |= config::kHotkeyModifierMeta;
        return modifiers;
    };

    if (webview::IsDomReady()) {
        g_panel_open.store(webview::SettingsOpen(), std::memory_order_release);
    }
    if (key_down && !key_repeat && !g_soft_disabled.load(std::memory_order_acquire)) {
        const auto virtual_key = static_cast<std::uint32_t>(wparam);
        if (virtual_key == g_overlay_hotkey.load(std::memory_order_acquire) &&
            current_modifier_mask() == g_overlay_hotkey_modifiers.load(std::memory_order_acquire)) {
            const bool current = webview::IsDomReady()
                                     ? webview::SettingsOpen()
                                     : g_panel_open.load(std::memory_order_acquire);
            const bool next = !current;
            g_panel_open.store(next, std::memory_order_release);
            if (next) {
                // Release mouse confinement for the composition-hosted settings
                // UI, but keep the game's keyboard focus and key state intact.
                // Synthetic WM_KILLFOCUS/WM_SETFOCUS messages make Unity clear
                // held keys, which feels like intermittent keyboard drop-outs.
                if (GetCapture() == window) ReleaseCapture();
                (void)ClipCursor(nullptr);
                int cursor_adjustments = 0;
                while (cursor_adjustments < 32) {
                    ++cursor_adjustments;
                    if (ShowCursor(TRUE) >= 0) break;
                }
                g_cursor_visibility_adjustments.store(cursor_adjustments, std::memory_order_release);
                (void)SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            }
            webview::SetSettingsOpen(next);
        }
        const std::uint32_t crosshair_key = g_crosshair_hotkey.load(std::memory_order_acquire);
        if (crosshair_key != 0 && virtual_key == crosshair_key) {
            g_crosshair_toggle_requested.store(true, std::memory_order_release);
        }
    }

    if (!g_soft_disabled.load(std::memory_order_acquire)) {
        (void)webview::ForwardShortcutMessage(message, wparam, lparam);
        if (message == WM_SIZE || message == WM_WINDOWPOSCHANGED || message == WM_DPICHANGED ||
            message == WM_DISPLAYCHANGE) {
            webview::UpdateBounds();
        }
    }

    if (webview::IsDomReady() && webview::WantsInput()) {
        if (message == WM_INPUT) {
            // The settings UI captures the mouse, but keyboard packets must
            // continue down the game's WndProc chain. Unity reads keyboard
            // state from WM_INPUT independently of the legacy WM_KEY messages.
            // Dropping these packets clears or interrupts held movement keys.
            RAWINPUTHEADER header{};
            UINT header_size = sizeof(header);
            const UINT read = GetRawInputData(
                reinterpret_cast<HRAWINPUT>(lparam),
                RID_HEADER,
                &header,
                &header_size,
                sizeof(RAWINPUTHEADER));
            const bool keyboard_input = read == sizeof(header) && header.dwType == RIM_TYPEKEYBOARD;
            if (!keyboard_input) {
                // Foreground raw input that is not forwarded must still be
                // cleaned up through DefWindowProc as required by Win32.
                if (GET_RAWINPUT_CODE_WPARAM(wparam) == RIM_INPUT) {
                    (void)DefWindowProcW(window, message, wparam, lparam);
                }
                return 0;
            }
        }
        if (message == WM_SETCURSOR) {
            HCURSOR cursor = webview::CurrentCursor();
            if (cursor == nullptr) cursor = LoadCursorW(nullptr, IDC_ARROW);
            SetCursor(cursor);
            return TRUE;
        }
        if (mouse_message) {
            if (message == WM_MOUSEMOVE) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
                (void)TrackMouseEvent(&tracking);
            }
            if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ||
                message == WM_XBUTTONDOWN) {
                SetCapture(window);
            } else if (message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP ||
                       message == WM_XBUTTONUP) {
                if ((wparam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) == 0 &&
                    GetCapture() == window) {
                    ReleaseCapture();
                }
            }
            if (webview::ForwardMouseMessage(window, message, wparam, lparam)) return 0;
        }
        // Keyboard input is observational: WebView2 may process it while the
        // settings panel is open, but the game must receive the same message.
    }
    // WndProc and Present may run on different game threads. A non-blocking
    // lock prevents concurrent ImGui access without risking a synchronous
    // window-message deadlock; the original chain still receives the message.
    std::unique_lock renderer_lock(g_renderer_mutex, std::try_to_lock);
    if (renderer_lock.owns_lock() && g_imgui_input_ready.load(std::memory_order_acquire) &&
        ImGui::GetCurrentContext() != nullptr) {
        const LRESULT handled = ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        if (g_panel_open.load(std::memory_order_acquire)) {
            const ImGuiIO& io = ImGui::GetIO();
            if (mouse_message && (handled != 0 || io.WantCaptureMouse)) {
                return handled != 0 ? handled : 1;
            }
        }
    }
    if (renderer_lock.owns_lock()) renderer_lock.unlock();
    const LRESULT result = original != nullptr ? CallWindowProcW(original, window, message, wparam, lparam)
                                               : DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_NCDESTROY) {
        if (g_hooked_window.load(std::memory_order_acquire) == window) {
            // The next real Present tears down the stale swapchain/window
            // state after any in-flight ResizeBuffers call has completed.
            std::scoped_lock transition_lock(g_ready_transition_mutex);
            g_window_invalidated.store(true, std::memory_order_release);
            g_ready.store(false, std::memory_order_release);
            if (g_ready_event != nullptr) ResetEvent(g_ready_event);
            webview::RequestStop();
        }
        ForgetWindowProcedureForwarder(window, original);
    }
    return result;
}

[[nodiscard]] bool CreateRenderTarget() {
    if (g_renderer.render_target || !g_renderer.swap_chain || !g_renderer.device) return true;
    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT result = g_renderer.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(result)) return false;
    result = g_renderer.device->CreateRenderTargetView(back_buffer.Get(), nullptr, &g_renderer.render_target);
    return SUCCEEDED(result);
}

[[nodiscard]] bool InitializeBackBufferRenderer() {
    if (!CreateRenderTarget()) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    g_renderer.imgui_context_created = true;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    if (!ImGui_ImplDX11_Init(g_renderer.device.Get(), g_renderer.context.Get())) return false;
    g_renderer.dx11_backend_initialized = true;
    return true;
}

void UpdateWebViewCaptureTexture() {
    webview::CapturedFrame frame;
    if (!webview::CopyLatestCapturedFrame(g_renderer.webview_capture_generation, frame)) return;
    g_renderer.webview_capture_generation = frame.generation;
    g_renderer.webview_capture_view.Reset();
    g_renderer.webview_capture_texture.Reset();
    if (frame.width == 0 || frame.height == 0 || frame.rgba.empty()) return;

    D3D11_TEXTURE2D_DESC description{};
    description.Width = frame.width;
    description.Height = frame.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = frame.rgba.data();
    initial.SysMemPitch = frame.row_pitch;
    HRESULT result = g_renderer.device->CreateTexture2D(
        &description, &initial, &g_renderer.webview_capture_texture);
    if (FAILED(result)) {
        logging::Write(logging::Level::Warning, "Could not create WebView capture texture");
        return;
    }
    result = g_renderer.device->CreateShaderResourceView(
        g_renderer.webview_capture_texture.Get(), nullptr, &g_renderer.webview_capture_view);
    if (FAILED(result)) {
        g_renderer.webview_capture_texture.Reset();
        logging::Write(logging::Level::Warning, "Could not create WebView capture shader view");
    }
}

void RenderWebViewCaptureFrame() {
    if (!webview::BackBufferCaptureEnabled()) return;
    if (!g_renderer.imgui_context_created || !g_renderer.dx11_backend_initialized ||
        !g_renderer.render_target || !g_renderer.context) {
        return;
    }
    UpdateWebViewCaptureTexture();
    if (!g_renderer.webview_capture_view) return;

    RECT client{};
    if (!GetClientRect(g_renderer.window, &client) ||
        client.right <= client.left || client.bottom <= client.top) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(
        static_cast<float>(client.right - client.left),
        static_cast<float>(client.bottom - client.top));
    io.DeltaTime = 1.0F / 60.0F;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    const ImTextureID texture_id = static_cast<ImTextureID>(
        reinterpret_cast<std::uintptr_t>(g_renderer.webview_capture_view.Get()));
    ImGui::GetBackgroundDrawList()->AddImage(
        ImTextureRef(texture_id),
        ImVec2(0.0F, 0.0F),
        io.DisplaySize,
        ImVec2(0.0F, 0.0F),
        ImVec2(1.0F, 1.0F),
        IM_COL32_WHITE);
    ImGui::Render();
    ScopedOutputMergerState previous_output_merger(g_renderer.context.Get());
    ID3D11RenderTargetView* target = g_renderer.render_target.Get();
    g_renderer.context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void RestoreWindowProcedure() {
    g_imgui_input_ready.store(false, std::memory_order_release);
    if (g_renderer.window == nullptr || g_renderer.original_wndproc == nullptr) return;
    const HWND window = g_renderer.window;
    const WNDPROC original = g_renderer.original_wndproc;
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR current = GetWindowLongPtrW(window, GWLP_WNDPROC);
    if (current == 0 && GetLastError() != ERROR_SUCCESS) {
        logging::Write(logging::Level::Warning,
                       "WndProc head could not be inspected; retaining resident forwarding entry");
        return;
    }
    if (current == reinterpret_cast<LONG_PTR>(&HookWindowProcedure)) {
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR displaced = SetWindowLongPtrW(
            window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        const DWORD restore_error = GetLastError();
        if (displaced == reinterpret_cast<LONG_PTR>(&HookWindowProcedure)) {
            ForgetWindowProcedureForwarder(window, original);
            return;
        }
        if (displaced == 0) {
            SetLastError(ERROR_SUCCESS);
            const LONG_PTR after_restore = GetWindowLongPtrW(window, GWLP_WNDPROC);
            if (after_restore == reinterpret_cast<LONG_PTR>(original)) {
                ForgetWindowProcedureForwarder(window, original);
                return;
            }
            logging::Write(
                logging::Level::Warning,
                restore_error == ERROR_SUCCESS
                    ? "WndProc restore returned an ambiguous result; forwarding retained"
                    : "WndProc restore failed; retaining resident forwarding entry");
            return;
        }

        {
            // A newer hook raced the head check. Put it back immediately and
            // keep HQ's resident forwarding entry because that hook may still
            // call us as its downstream procedure.
            SetLastError(ERROR_SUCCESS);
            const LONG_PTR repair = SetWindowLongPtrW(window, GWLP_WNDPROC, displaced);
            if (repair == 0 && GetLastError() != ERROR_SUCCESS) {
                logging::Write(logging::Level::Error,
                               "WndProc restore raced a newer hook and the newer head could not be repaired");
            } else {
                logging::Write(logging::Level::Warning,
                               "WndProc restore raced a newer hook; newer head restored and forwarding retained");
            }
        }
    } else {
        logging::Write(logging::Level::Warning,
                       "WndProc has a newer head; preserving its chain and retaining HQ forwarding");
    }
}

void ShutdownRenderer() {
    webview::RequestStop();
    if (!g_renderer.swap_chain && !g_renderer.imgui_context_created) return;
    RestoreWindowProcedure();
    if (g_renderer.dx11_backend_initialized) {
        ImGui_ImplDX11_Shutdown();
        g_renderer.dx11_backend_initialized = false;
    }
    if (g_renderer.win32_backend_initialized) {
        ImGui_ImplWin32_Shutdown();
        g_renderer.win32_backend_initialized = false;
    }
    if (g_renderer.imgui_context_created) {
        ImGui::DestroyContext();
        g_renderer.imgui_context_created = false;
    }
    g_renderer.webview_capture_view.Reset();
    g_renderer.webview_capture_texture.Reset();
    g_renderer.webview_capture_generation = 0;
    g_renderer.render_target.Reset();
    g_renderer.context.Reset();
    g_renderer.device.Reset();
    g_renderer.swap_chain.Reset();
    g_renderer.ui_font = nullptr;
    g_renderer.label_font = nullptr;
    g_renderer.value_font = nullptr;
    g_renderer.window = nullptr;
    g_renderer.original_wndproc = nullptr;
    logging::Write(logging::Level::Info, "Renderer released on the render thread");
}

[[nodiscard]] bool IsCandidateSwapChain(IDXGISwapChain* swap_chain, DXGI_SWAP_CHAIN_DESC& description) {
    if (swap_chain == nullptr || FAILED(swap_chain->GetDesc(&description)) || description.OutputWindow == nullptr) {
        return false;
    }
    DWORD owner_process = 0;
    GetWindowThreadProcessId(description.OutputWindow, &owner_process);
    if (owner_process != GetCurrentProcessId()) return false;
    if (!IsWindow(description.OutputWindow) || !IsWindowVisible(description.OutputWindow)) return false;
    if (GetAncestor(description.OutputWindow, GA_ROOT) != description.OutputWindow) return false;
    RECT client{};
    if (!GetClientRect(description.OutputWindow, &client) || client.right - client.left < 320 ||
        client.bottom - client.top < 200) {
        return false;
    }
    ComPtr<ID3D11Device> device;
    return SUCCEEDED(swap_chain->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr;
}

// A WebView2 composition controller is presented through DirectComposition.
// DXGI exclusive fullscreen bypasses the desktop compositor, so its visual can
// never appear above the game's back buffer. Preserve the fullscreen look by
// moving the swap chain back to windowed mode and sizing its HWND to the output.
// This also covers games that enter exclusive mode after the renderer started.
void NormalizeExclusiveFullscreen(IDXGISwapChain* swap_chain) {
    if (swap_chain == nullptr) return;

    BOOL exclusive = FALSE;
    ComPtr<IDXGIOutput> output;
    const HRESULT state_result = swap_chain->GetFullscreenState(&exclusive, &output);
    if (FAILED(state_result) || exclusive == FALSE) return;

    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swap_chain->GetDesc(&description)) || description.OutputWindow == nullptr) return;

    RECT target{};
    bool have_target = false;
    if (output) {
        DXGI_OUTPUT_DESC output_description{};
        if (SUCCEEDED(output->GetDesc(&output_description))) {
            target = output_description.DesktopCoordinates;
            have_target = true;
        }
    }
    if (!have_target) {
        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        const HMONITOR monitor = MonitorFromWindow(description.OutputWindow, MONITOR_DEFAULTTONEAREST);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info)) {
            target = monitor_info.rcMonitor;
            have_target = true;
        }
    }

    const HRESULT windowed_result = swap_chain->SetFullscreenState(FALSE, nullptr);
    if (FAILED(windowed_result)) {
        if (!g_exclusive_fullscreen_converted.exchange(true, std::memory_order_acq_rel)) {
            logging::Write(
                logging::Level::Warning,
                "DXGI exclusive fullscreen could not be converted; HTML overlay composition is unavailable");
        }
        return;
    }

    const HWND window = description.OutputWindow;
    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP | WS_VISIBLE;
    (void)SetWindowLongPtrW(window, GWL_STYLE, style);

    LONG_PTR extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    extended_style &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
    (void)SetWindowLongPtrW(window, GWL_EXSTYLE, extended_style);
    if (have_target) {
        (void)SetWindowPos(
            window,
            HWND_TOP,
            target.left,
            target.top,
            target.right - target.left,
            target.bottom - target.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    webview::UpdateBounds();

    if (!g_exclusive_fullscreen_converted.exchange(true, std::memory_order_acq_rel)) {
        logging::Write(
            logging::Level::Info,
            "Converted DXGI exclusive fullscreen to borderless fullscreen for HTML overlay composition");
    }
}

[[nodiscard]] bool InitializeRenderer(IDXGISwapChain* swap_chain) {
    if (g_soft_disabled.load(std::memory_order_acquire) || ExternalDisableSignaled()) return false;

    DXGI_SWAP_CHAIN_DESC description{};
    if (!IsCandidateSwapChain(swap_chain, description)) return false;
    ComPtr<ID3D11Device> device;
    if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&device)))) return false;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) return false;

    g_renderer.swap_chain = swap_chain;
    g_renderer.device = std::move(device);
    g_renderer.context = std::move(context);
    g_renderer.window = description.OutputWindow;

    WNDPROC original_wndproc = nullptr;
    if (!InstallWindowProcedure(g_renderer.window, original_wndproc)) {
        ShutdownRenderer();
        return false;
    }
    g_renderer.original_wndproc = original_wndproc;
    g_imgui_input_ready.store(false, std::memory_order_release);
    ApplyPreparedConfiguration();

    if (!InitializeBackBufferRenderer()) {
        logging::Write(logging::Level::Error, "D3D11 back-buffer overlay renderer could not be initialized");
        ShutdownRenderer();
        return false;
    }

    if (g_soft_disabled.load(std::memory_order_acquire) || ExternalDisableSignaled()) {
        RequestSoftDisable("disable event observed during renderer initialization");
        ShutdownRenderer();
        return false;
    }

    std::ostringstream message;
    RECT rect{};
    GetClientRect(g_renderer.window, &rect);
    message << "Selected D3D11 swapchain hwnd=0x" << std::hex
            << reinterpret_cast<std::uintptr_t>(g_renderer.window) << std::dec << " client="
            << (rect.right - rect.left) << 'x' << (rect.bottom - rect.top);
    logging::Write(logging::Level::Info, message.str());
    MarkWebViewNotReady();
    if (!webview::Start(g_overlay_module.load(std::memory_order_acquire), g_renderer.window)) {
        logging::Write(logging::Level::Error, "WebView2 STA host could not be started");
        ShutdownRenderer();
        return false;
    }
    webview::UpdateBounds();
    webview::SetSettingsOpen(g_panel_open.load(std::memory_order_acquire));
    logging::Write(logging::Level::Info,
                   "Waiting for embedded HTML frontend.ready before signaling native readiness");
    return true;
}

void RenderFrame() {
    ApplyPreparedConfiguration();
    const auto& loaded = g_renderer.config.value;
    if (g_crosshair_toggle_requested.exchange(false, std::memory_order_acq_rel)) {
        g_renderer.runtime_crosshair_enabled = !g_renderer.runtime_crosshair_enabled;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (loaded.enabled) {
        DrawCrosshair(loaded.crosshair);
        DrawTimer(loaded.timer);
    }
    DrawSettingsPanel();
    ImGui::Render();
    ScopedOutputMergerState previous_output_merger(g_renderer.context.Get());
    ID3D11RenderTargetView* target = g_renderer.render_target.Get();
    g_renderer.context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

HRESULT WINAPI HookPresent(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    thread_local bool reentrant = false;
    if (reentrant || g_original_present == nullptr) {
        return g_original_present != nullptr ? g_original_present(swap_chain, sync_interval, flags) : E_FAIL;
    }
    reentrant = true;
    try {
        if (ExternalDisableSignaled()) RequestSoftDisable("named disable/shutdown event");
        if ((flags & DXGI_PRESENT_TEST) == 0) NormalizeExclusiveFullscreen(swap_chain);

        {
            std::scoped_lock lock(g_renderer_mutex);
            if (g_window_invalidated.load(std::memory_order_acquire) &&
                g_selected_resize_count.load(std::memory_order_acquire) == 0) {
                g_window_invalidated.store(false, std::memory_order_release);
                MarkWebViewNotReady();
                ShutdownRenderer();
            }
            if (g_soft_disabled.load(std::memory_order_acquire)) {
                // ResizeBuffers calls the original implementation without our
                // renderer mutex held. Defer ImGui/context/WndProc teardown
                // until its selected-swapchain gate reaches zero.
                if (g_selected_resize_count.load(std::memory_order_acquire) == 0) {
                    ShutdownRenderer();
                }
            } else if ((flags & DXGI_PRESENT_TEST) == 0) {
                // RequestStop is asynchronous. Wait until the old STA host has
                // fully stopped before selecting and starting a replacement.
                if (!g_renderer.swap_chain && webview::CurrentState() != webview::State::Stopping) {
                    const bool initialized = InitializeRenderer(swap_chain);
                    if (!initialized && webview::HasFailed()) {
                        RequestSoftDisable("WebView2 initialization or browser process failure");
                    }
                }
                if (g_renderer.swap_chain.Get() == swap_chain &&
                    g_selected_resize_count.load(std::memory_order_acquire) == 0) {
                    PromoteWebViewReady();
                    RenderWebViewCaptureFrame();
                    if (webview::HasFailed()) {
                        RequestSoftDisable("WebView2 initialization or browser process failure");
                    }
                }
            }
        }
    } catch (...) {
        RequestSoftDisable("unhandled C++ exception in Present hook");
    }

    const HRESULT result = g_original_present(swap_chain, sync_interval, flags);
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        std::scoped_lock lock(g_renderer_mutex);
        if (g_renderer.swap_chain.Get() == swap_chain) {
            MarkWebViewNotReady();
            ShutdownRenderer();
            logging::Write(logging::Level::Warning, "D3D11 device lost; waiting for a replacement swapchain");
        }
    }
    reentrant = false;
    return result;
}

HRESULT WINAPI HookResizeBuffers(
    IDXGISwapChain* swap_chain,
    UINT buffer_count,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    UINT swap_chain_flags) {
    bool selected = false;
    SelectedResizeGate resize_gate;
    {
        std::scoped_lock lock(g_renderer_mutex);
        selected = g_renderer.swap_chain.Get() == swap_chain;
        if (selected) {
            // Activate while holding the same mutex used by Present. Once the
            // mutex is released, Present observes the gate before mutating the
            // selected renderer state, while the original resize remains free
            // to call without our renderer mutex held.
            resize_gate.Activate();
        }
    }
    const HRESULT result = g_original_resize_buffers(
        swap_chain, buffer_count, width, height, format, swap_chain_flags);
    if (selected && FAILED(result)) {
        logging::Write(logging::Level::Warning, "ResizeBuffers failed; WebView bounds update deferred");
    } else if (selected) {
        webview::UpdateBounds();
    }
    return result;
}

LRESULT CALLBACK DummyWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

[[nodiscard]] bool CreateHookProbe(void** present_address, void** resize_address) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = DummyWindowProcedure;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kDummyClassName;
    const ATOM class_atom = RegisterClassExW(&window_class);
    if (class_atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logging::Write(logging::Level::Error, "Cannot register the D3D11 hook-probe window class");
        return false;
    }
    HWND window = CreateWindowExW(0, kDummyClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 320, 200,
                                  nullptr, nullptr, window_class.hInstance, nullptr);
    if (window == nullptr) {
        logging::Write(logging::Level::Error, "Cannot create the D3D11 hook-probe window");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level{};
    constexpr D3D_FEATURE_LEVEL requested_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested_levels,
        static_cast<UINT>(std::size(requested_levels)), D3D11_SDK_VERSION, &description,
        &swap_chain, &device, &feature_level, &context);
    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, requested_levels,
            static_cast<UINT>(std::size(requested_levels)), D3D11_SDK_VERSION, &description,
            &swap_chain, &device, &feature_level, &context);
    }
    if (SUCCEEDED(result) && swap_chain) {
        void** virtual_table = *reinterpret_cast<void***>(swap_chain.Get());
        *present_address = virtual_table[8];
        *resize_address = virtual_table[13];
    }
    swap_chain.Reset();
    context.Reset();
    device.Reset();
    DestroyWindow(window);
    if (class_atom != 0) UnregisterClassW(kDummyClassName, window_class.hInstance);
    return SUCCEEDED(result) && *present_address != nullptr && *resize_address != nullptr;
}

[[nodiscard]] bool InstallHooks() {
    void* present_address = nullptr;
    void* resize_address = nullptr;
    if (!CreateHookProbe(&present_address, &resize_address)) {
        logging::Write(logging::Level::Error, "Could not resolve DXGI Present/ResizeBuffers addresses");
        return false;
    }
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        logging::Write(logging::Level::Error, std::string("MH_Initialize failed: ") + MH_StatusToString(status));
        return false;
    }
    status = MH_CreateHook(present_address, &HookPresent, reinterpret_cast<void**>(&g_original_present));
    if (status != MH_OK) {
        logging::Write(logging::Level::Error, std::string("Present hook creation failed: ") + MH_StatusToString(status));
        return false;
    }
    status = MH_CreateHook(resize_address, &HookResizeBuffers,
                           reinterpret_cast<void**>(&g_original_resize_buffers));
    if (status != MH_OK) {
        MH_RemoveHook(present_address);
        logging::Write(logging::Level::Error,
                       std::string("ResizeBuffers hook creation failed: ") + MH_StatusToString(status));
        return false;
    }
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        MH_RemoveHook(resize_address);
        MH_RemoveHook(present_address);
        logging::Write(logging::Level::Error, std::string("MH_EnableHook failed: ") + MH_StatusToString(status));
        return false;
    }
    logging::Write(logging::Level::Info, "DXGI Present and ResizeBuffers hooks installed");
    return true;
}

}  // namespace

DWORD WINAPI BootstrapThread(void* module_parameter) {
    g_overlay_module.store(static_cast<HMODULE>(module_parameter), std::memory_order_release);
    logging::Initialize();
    logging::Write(logging::Level::Info, "hq_overlay.dll bootstrap started outside loader lock");
    InitializeEvents();
    if (ExternalDisableSignaled()) {
        RequestSoftDisable("disable/shutdown event was already signaled");
        return 0;
    }
    PollConfigurationOnWorker(true);
    if (!InstallHooks()) return 1;
    HANDLE events[] = {g_shutdown_event, g_disable_event};
    if (events[0] != nullptr && events[1] != nullptr) {
        for (;;) {
            const DWORD result = WaitForMultipleObjects(2, events, FALSE, static_cast<DWORD>(kConfigPollInterval.count()));
            if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + 2) {
                RequestSoftDisable(result == WAIT_OBJECT_0 ? "shutdown event" : "disable event");
                break;
            }
            if (result != WAIT_TIMEOUT) break;
            const bool force = g_config_reload_requested.exchange(false, std::memory_order_acq_rel);
            PollConfigurationOnWorker(force);
            PollLcStatsOnWorker();
            PromoteWebViewReady();
        }
    }
    return 0;
}

void NotifyProcessDetach() noexcept {
    g_process_detaching.store(true, std::memory_order_release);
    g_soft_disabled.store(true, std::memory_order_release);
    g_imgui_input_ready.store(false, std::memory_order_release);
    webview::NotifyProcessDetach();
}

bool RequestSoftDisable(const char* reason) noexcept {
    const bool was_disabled = g_soft_disabled.exchange(true, std::memory_order_acq_rel);
    g_panel_open.store(false, std::memory_order_release);
    webview::SetSettingsOpen(false);
    webview::RequestStop();
    if (!was_disabled && !g_process_detaching.load(std::memory_order_acquire)) {
        try {
            logging::Write(logging::Level::Info,
                           std::string("Native overlay soft-disabled: ") + (reason != nullptr ? reason : "unspecified"));
        } catch (...) {
        }
    }
    return !was_disabled;
}

bool IsReady() noexcept {
    return g_ready.load(std::memory_order_acquire) && !g_soft_disabled.load(std::memory_order_acquire);
}

}  // namespace hq::overlay
