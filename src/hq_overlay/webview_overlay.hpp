#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <vector>

namespace hq::overlay::webview {

enum class State : unsigned char {
    Stopped,
    Starting,
    DomReady,
    Failed,
    Stopping,
};

struct CapturedFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_pitch = 0;
    std::uint64_t generation = 0;
    std::vector<std::uint8_t> rgba;
};

// Starts one STA WebView2/DirectComposition host for the selected game HWND.
// This function never performs COM work and is safe to call from Present.
[[nodiscard]] bool Start(HMODULE overlay_module, HWND game_window) noexcept;

// All teardown is queued to the STA thread. These functions never wait.
void RequestStop() noexcept;
void NotifyProcessDetach() noexcept;

// Thread-safe commands queued to the STA thread.
void UpdateBounds() noexcept;
void SetSettingsOpen(bool open) noexcept;
void SetSettingsHotkey(UINT virtual_key, std::uint8_t modifiers) noexcept;
void SetBackBufferCaptureEnabled(bool enabled) noexcept;
[[nodiscard]] bool BackBufferCaptureEnabled() noexcept;

// A registered window message posted back to the game HWND when WebView focus
// must be released. The game WndProc handles it on its owning thread.
[[nodiscard]] UINT RestoreGameFocusMessage() noexcept;

// Converts and queues Win32 mouse input for ICoreWebView2CompositionController.
// Returns true only when the message was recognized and successfully queued.
[[nodiscard]] bool ForwardMouseMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
// Emits registered module shortcut events even while the settings UI is closed.
[[nodiscard]] bool ForwardShortcutMessage(UINT message, WPARAM wparam, LPARAM lparam) noexcept;

[[nodiscard]] State CurrentState() noexcept;
[[nodiscard]] bool IsDomReady() noexcept;
[[nodiscard]] bool HasFailed() noexcept;
[[nodiscard]] bool SettingsOpen() noexcept;
[[nodiscard]] bool WantsInput() noexcept;
[[nodiscard]] const char* StateName() noexcept;
[[nodiscard]] HCURSOR CurrentCursor() noexcept;

// Copies a newly captured transparent WebView frame for D3D11 back-buffer
// composition. Returns false when the caller already has the latest frame.
[[nodiscard]] bool CopyLatestCapturedFrame(
    std::uint64_t known_generation,
    CapturedFrame& frame) noexcept;

}  // namespace hq::overlay::webview
