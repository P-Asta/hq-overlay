# HQ Overlay Native (D3D11, x64)

`hq_overlay.dll` is a standalone C++20 overlay for authorized x64 Windows game processes. The launcher injects it with `LoadLibraryW`; it does not require BepInEx, Unity managed assemblies, or a separate companion overlay process.

The DLL hooks a D3D11 swapchain only to discover and track the game's window. It creates a transparent WebView2 composition controller on that HWND and renders the launcher's existing React `GameOverlay` inside the process. The complete production HTML/JavaScript/CSS bundle is embedded as RCDATA resource 101, and the WebView2 loader is statically linked, so `hq_overlay.dll` is the only native overlay file that must be deployed.

The launcher's existing external WebView overlay remains available as the **legacy renderer**. `auto` mode may fall back to it when injection, D3D11 discovery, WebView2 Runtime creation, or the HTML ready handshake fails. Native soft-disable never deletes or rewrites legacy files.

## Implemented surface

- D3D11 swapchain discovery and process-owned top-level target-window selection.
- `Present` and `ResizeBuffers` hooks used to initialize the native window integration and keep WebView bounds synchronized.
- DXGI exclusive fullscreen is normalized to same-monitor borderless fullscreen because DirectComposition HTML visuals cannot be displayed through the exclusive presentation path.
- DirectComposition + `ICoreWebView2CompositionController` rendering directly on the game HWND with a transparent background.
- The same React `GameOverlay` HUD and settings panel used by the legacy renderer; there is no second native-looking settings implementation.
- Runtime loading and evaluation of `%APPDATA%\asta.hq-launcher\overlayModule\*.js`, including nested module configuration reads/writes under `config\overlay`.
- Module shortcut, mouse, focus, controls-open, active-state, diagnostics, folder-open, and LCStats message bridges between the embedded page and the native host.
- Minimal loader-lock entry: `DllMain` only disables thread callbacks and starts a bootstrap worker. Hook setup, WebView2/COM creation, configuration and module I/O, and waits run outside `DllMain`.
- WndProc chaining with HWND-scoped resident forwarders. A forwarding entry is removed only after a verified head restore; if a newer hook is ahead, HQ remains a safe downstream link for the resident DLL lifetime.
- PID-scoped ready/disable/shutdown events and an exported soft-disable API.
- Standalone x64 LoadLibrary injector, D3D11 test host, bridge tests, and config parser tests.

## Runtime data and compatibility

The production page is embedded in the DLL. User data remains external so existing launcher configuration and custom modules continue to work:

```text
%APPDATA%\asta.hq-launcher\overlayModule\*.js
%APPDATA%\asta.hq-launcher\config\overlay\general.json
%APPDATA%\asta.hq-launcher\config\overlay\modules\*.json
%APPDATA%\asta.hq-launcher\config\overlay\widgets.json
```

The native host exposes a narrow string-RPC bridge to the embedded page. File access is confined to the two roots above; paths must be relative, stay inside their root after canonicalization, and use the expected `.js` or `.json` extension. Top-level navigation, popups, context menus, script dialogs, and untrusted WebMessages are blocked. Module files execute because that is the existing overlay module contract; install only modules you trust.

For development only, set `HQ_OVERLAY_HTML` to an absolute HTML file to replace embedded resource 101. `HQ_OVERLAY_CONFIG_DIR` can override the configuration root. Neither override is required in a release installation.

## WebView2 requirement

The x64 WebView2 loader is statically linked into `hq_overlay.dll`; do not ship `WebView2Loader.dll`. Microsoft Edge WebView2 **Evergreen Runtime** must still be installed on the machine. Windows 11 and most current Windows 10 systems already have it, and the launcher treats a missing or unusable Runtime as a native initialization failure so `auto` can choose legacy.

The WebView2 environment runs on its own STA thread with a per-user data folder under:

```text
%LOCALAPPDATA%\asta.hq-launcher\WebView2Native
```

## Launcher handshake contract

All named objects are manual-reset events in the target process's logon session. `<pid>` is the decimal target process ID.

| Event | Owner | Meaning |
|---|---|---|
| `Local\HQOverlayReady_<pid>` | Launcher should create before injection; DLL creates if absent | Set only after a process-owned D3D11 target/window, transparent WebView2 composition controller, embedded page, user modules/configuration, and `frontend.ready` handshake are complete. |
| `Local\HQOverlayDisable_<pid>` | Launcher creates before injection | Set when native readiness times out or legacy fallback wins. Once observed, the native renderer remains disabled for that injected instance. |
| `Local\HQOverlayShutdown_<pid>` | Launcher or operator | Requests the same permanent soft-disable during normal teardown. |

Recommended `auto` flow:

1. Create Ready and Disable as nonsignaled manual-reset events and keep their handles alive.
2. Inject `hq_overlay.dll`; resume the game if it was created suspended.
3. Wait a bounded interval for Ready.
4. If Ready wins, keep legacy rendering off for that PID.
5. On timeout or injection failure, set Disable before enabling legacy. This prevents late native initialization from double-rendering.
6. Set Shutdown during normal launcher/game teardown.

Soft-disable stops the WebView host, restores the WndProc when safe, and releases native resources. Hooks and the DLL remain resident; **do not call `FreeLibrary` on an injected instance**. Process exit performs final reclamation.

Diagnostic exports are `HQOverlay_GetVersion`, `HQOverlay_IsReady`, and `HQOverlay_RequestSoftDisable`.

## Building

Requirements:

- Windows 10/11 SDK
- Visual Studio 2022 C++ x64 tools (v143)
- MSBuild
- Microsoft Edge WebView2 Evergreen Runtime for execution/tests

The pinned WebView2 SDK headers/static loader, Dear ImGui compatibility sources, and MinHook sources are already vendored, so the build performs no package restore or network access. Projects use the static MSVC runtime (`/MT` Release, `/MTd` Debug).

```powershell
.\scripts\build.ps1
```

Or build directly:

```powershell
msbuild .\hq-overlay.sln /m /p:Configuration=Release /p:Platform=x64
msbuild .\hq-overlay.sln /m /p:Configuration=Debug /p:Platform=x64
```

Artifacts:

```text
bin\x64\Release\hq_overlay.dll
bin\x64\Release\hq_injector.exe
bin\x64\Release\d3d11_test_host.exe
bin\x64\Release\config_parser_tests.exe
bin\x64\Debug\...
```

The build script also refreshes the deployable directory. The injector defaults to its sibling DLL:

```text
dist\x64\Release\hq_overlay.dll
dist\x64\Release\hq_injector.exe
dist\x64\Release\THIRD_PARTY_NOTICES.md
```

The embedded page is built from the launcher repository with `yarn run build:embedded-overlay`; copy the generated `dist-embedded-overlay\embedded-overlay.html` to `assets\overlay.html` before rebuilding the native project whenever `GameOverlay` or its bridge changes.

## Local smoke test

1. Run `bin\x64\Release\d3d11_test_host.exe`.
2. Find its PID in Task Manager or PowerShell.
3. Inject and wait for the real HTML ready handshake:

```powershell
.\bin\x64\Release\hq_injector.exe <pid> .\bin\x64\Release\hq_overlay.dll
```

The current overlay hotkey from `general.json` (commonly `PageDown` or `Insert`) opens the same React settings panel as legacy. Escape closes the panel first; Escape in the host exits when the panel is closed.

For a running target:

```powershell
.\bin\x64\Release\hq_injector.exe --signal-disable <pid>
.\bin\x64\Release\hq_injector.exe --signal-shutdown <pid>
```

Logs default to `%LOCALAPPDATA%\asta.hq-launcher\logs\hq_overlay_<pid>.log`. Set `HQ_OVERLAY_LOG` to an absolute file for development.

## Operational boundaries

- Only inject into software you own or are authorized to instrument. Anti-cheat/protected processes may reject injection or treat it as tampering.
- The injector refuses non-x64 targets and uses the remote process's `kernel32!LoadLibraryW` address.
- The selected swapchain must expose `ID3D11Device`, own a visible top-level window in the target process, and have a client area of at least 320x200. D3D12/Vulkan/OpenGL-only programs fall back to legacy.
- The WebView2 Runtime spawns its normal browser/render helper processes even though the host UI is injected and no companion overlay application is used.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for pinned upstream versions and license locations.
