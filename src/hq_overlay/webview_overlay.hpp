#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <string>

namespace hq::overlay::webview {

enum class State : unsigned char {
    Stopped,
    Starting,
    DomReady,
    Failed,
    Stopping,
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

// Hands a LCStatsTracker raw stats JSON (read from the launcher's relay file)
// to the STA thread through the same queue used by the SSE client. Safe to call
// from the config poller thread; the payload is validated + de-duplicated on
// the STA thread by HandleLcStats.
void EnqueueLcStatsFilePayload(std::string raw_json) noexcept;

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

}  // namespace hq::overlay::webview
