#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

// version.dll proxy layer.
//
// The overlay ships as a drop-in `version.dll` placed next to the game
// executable so the OS/Wine loader picks it up via DLL search-order
// hijacking (the same mechanism BepInEx's winhttp.dll uses). To stay a
// transparent drop-in, every public export of the real system
// `version.dll` (17 functions on Windows 10/11) is forwarded to the
// genuine implementation loaded from `%SystemRoot%\System32\version.dll`.
// On Wine/Proton, where the builtin VERSION.dll exports may be stubs, the
// proxy intentionally reports "no version information" instead of invoking
// an unimplemented export and terminating the process.
//
// The system module is resolved lazily and on demand: `Initialize` only
// opens a handle, and each forwarder resolves its target address the
// first time it is called (and caches it). This keeps DllMain cheap and
// never touches the real version.dll work while the loader lock is held.
namespace hq::version_proxy {

// Load the genuine system `version.dll`. Safe to call from DllMain
// (LoadLibraryExW with LOAD_LIBRARY_SEARCH_SYSTEM32 does not take the
// loader lock for an already-mapped known DLL). No-op if already loaded.
void Initialize() noexcept;

// Release the reference to the system module. Safe to call from DllMain
// DLL_PROCESS_DETACH. Forwarder address caches are left dangling; the
// process is tearing down so no further calls are expected.
void Shutdown() noexcept;

}  // namespace hq::version_proxy
