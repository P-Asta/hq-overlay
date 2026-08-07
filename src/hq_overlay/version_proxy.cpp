// Deliberately avoid pulling in <winver.h>: it declares the version.dll
// exports with __declspec(dllimport), which would clash with our own
// definitions below (C2375 redefinition / C2733 extern "C" overload).
// Windows.h alone gives us the handle/process primitives we need.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "version_proxy.hpp"
#include "logging.hpp"

#include <atomic>
#include <string>

namespace hq::version_proxy {

namespace {

// System version.dll handle. Resolved once in Initialize(); never unloaded
// until Shutdown(). Stored as a raw HMODULE because the forwarders run
// before any C++ static initialization guarantees and from arbitrary
// threads (the host calls version.dll exports on its own threads).
std::atomic<HMODULE> g_system_version{nullptr};

// Wine/Proton may load this proxy as VERSION.dll, but its builtin VERSION.dll
// exports are not consistently implemented across Proton versions. In that
// environment forwarding into the builtin module can terminate the process
// with an "unimplemented function" error. Returning the normal "no version
// information" result is safe for a transparent proxy and keeps the overlay
// alive.
std::atomic<bool> g_wine_environment{false};

bool IsWineEnvironment() noexcept {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return false;
    // wine_get_version is a Wine-only ntdll export. Resolve it dynamically so
    // the Windows build has no dependency on Wine headers or libraries.
    return GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

HMODULE ThisProxyModule() noexcept {
    HMODULE module = nullptr;
    // The address of a function in this translation unit identifies this DLL.
    // Do not increment the reference count; this is only an identity check.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisProxyModule), &module);
    return module;
}

// Resolve the genuine system version.dll. Uses an absolute System32 path so
// the lookup is pinned to %SystemRoot%\System32 regardless of the calling
// process's DLL search order (otherwise this proxy would resolve to itself).
HMODULE LoadSystemVersionDll() noexcept {
    wchar_t system_dir[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    wchar_t full_path[MAX_PATH] = {};
    const int written = wsprintfW(full_path, L"%s\\version.dll", system_dir);
    if (written <= 0 || written >= MAX_PATH) {
        return LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    HMODULE module = LoadLibraryExW(full_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    // A DLL override can cause Wine to return the already-loaded proxy even
    // for a system32 path. Never accept that handle as the forwarding target.
    if (module == ThisProxyModule()) {
        FreeLibrary(module);
        return nullptr;
    }
    return module;
}

HMODULE EnsureSystemVersionDll() noexcept {
    HMODULE module = g_system_version.load(std::memory_order_acquire);
    if (module != nullptr) return module;

    module = LoadSystemVersionDll();
    if (module == nullptr) return nullptr;

    HMODULE expected = nullptr;
    if (g_system_version.compare_exchange_strong(expected, module, std::memory_order_acq_rel)) {
        return module;
    }
    // Lost the race; prefer the published handle and drop ours.
    if (expected != module) {
        FreeLibrary(module);
    }
    return g_system_version.load(std::memory_order_acquire);
}

// Resolve a forwarder target by name from the genuine system version.dll.
// Returns nullptr if the system module/export could not be found.
void* ResolveExport(const char* name) noexcept {
    if (IsWineEnvironment()) {
        g_wine_environment.store(true, std::memory_order_release);
        return nullptr;
    }
    if (g_wine_environment.load(std::memory_order_acquire)) return nullptr;
    HMODULE module = EnsureSystemVersionDll();
    if (module == nullptr) {
        logging::Write(logging::Level::Warning,
                       std::string("version_proxy: system version.dll unavailable for ") + name);
        return nullptr;
    }
    void* address = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (address == nullptr) {
        logging::Write(logging::Level::Warning,
                       std::string("version_proxy: missing system export ") + name);
    }
    return address;
}

}  // namespace

void Initialize() noexcept {
    if (IsWineEnvironment()) {
        g_wine_environment.store(true, std::memory_order_release);
        return;
    }
    EnsureSystemVersionDll();
}

void Shutdown() noexcept {
    HMODULE module = g_system_version.exchange(nullptr, std::memory_order_acq_rel);
    if (module != nullptr) {
        FreeLibrary(module);
    }
}

}  // namespace hq::version_proxy

// --- Forwarded version.dll exports ------------------------------------------
//
// Each forwarder resolves the genuine system version.dll export lazily and
// tail-calls it. On resolution failure it returns a conservative zero/FALSE,
// matching the real version.dll behaviour for an unreachable file so the host
// sees "no version data" instead of a missing-export crash.
//
// The functions are defined with an internal `hq_proxy_*` name and exported
// under the canonical version.dll name via linker /EXPORT directives below.
// This sidesteps the C2375/C2733 collisions with the winver.h declarations,
// which mark the same symbols as __declspec(dllimport) and therefore cannot
// coexist with a same-named definition.

// Pointer-type aliases keep the forwarders faithful to the Win32 calling
// convention without spelling out WINAPI prototypes by hand.
using FARPROC_VOID = FARPROC;

template <typename Fn>
Fn ResolveCached(std::atomic<FARPROC_VOID>& cache, const char* name) noexcept {
    FARPROC cached = cache.load(std::memory_order_acquire);
    if (cached != nullptr) return reinterpret_cast<Fn>(cached);
    cached = reinterpret_cast<FARPROC_VOID>(hq::version_proxy::ResolveExport(name));
    cache.store(cached, std::memory_order_release);
    return reinterpret_cast<Fn>(cached);
}

// Function-pointer shapes matching each version.dll export.
using GetFileVersionInfoA_t = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID);
using GetFileVersionInfoW_t = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
using GetFileVersionInfoExA_t = BOOL(WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
using GetFileVersionInfoExW_t = BOOL(WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
using GetFileVersionInfoSizeA_t = DWORD(WINAPI*)(LPCSTR, LPDWORD);
using GetFileVersionInfoSizeW_t = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
using GetFileVersionInfoSizeExA_t = DWORD(WINAPI*)(DWORD, LPCSTR, LPDWORD);
using GetFileVersionInfoSizeExW_t = DWORD(WINAPI*)(DWORD, LPCWSTR, LPDWORD);
using GetFileVersionInfoByHandle_t = BOOL(WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
using VerQueryValueA_t = BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT);
using VerQueryValueW_t = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
using VerLanguageNameA_t = DWORD(WINAPI*)(DWORD, LPSTR, DWORD);
using VerLanguageNameW_t = DWORD(WINAPI*)(DWORD, LPWSTR, DWORD);
using VerFindFileA_t = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
using VerFindFileW_t = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
using VerInstallFileA_t = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
using VerInstallFileW_t = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);

std::atomic<FARPROC_VOID> g_GetFileVersionInfoA{nullptr};
std::atomic<FARPROC_VOID> g_GetFileVersionInfoW{nullptr};
std::atomic<FARPROC_VOID> g_GetFileVersionInfoExA{nullptr};
std::atomic<FARPROC_VOID> g_GetFileVersionInfoExW{nullptr};
std::atomic<FARPROC_VOID> g_GetFileVersionInfoSizeA{nullptr};
std::atomic<FARPROC_VOID> g_GetFileVersionInfoSizeW{nullptr};
std::atomic<FARPROC_VOID> g_GetFileVersionInfoSizeExA{nullptr};
std::atomic<FARPROC_VOID> g_GetFileVersionInfoSizeExW{nullptr};
std::atomic<FARPROC_VOID> g_GetFileVersionInfoByHandle{nullptr};
std::atomic<FARPROC_VOID> g_VerQueryValueA{nullptr};
std::atomic<FARPROC_VOID> g_VerQueryValueW{nullptr};
std::atomic<FARPROC_VOID> g_VerLanguageNameA{nullptr};
std::atomic<FARPROC_VOID> g_VerLanguageNameW{nullptr};
std::atomic<FARPROC_VOID> g_VerFindFileA{nullptr};
std::atomic<FARPROC_VOID> g_VerFindFileW{nullptr};
std::atomic<FARPROC_VOID> g_VerInstallFileA{nullptr};
std::atomic<FARPROC_VOID> g_VerInstallFileW{nullptr};

extern "C" BOOL WINAPI hq_proxy_GetFileVersionInfoA(LPCSTR a, DWORD b, DWORD c, LPVOID d) {
    auto fn = ResolveCached<GetFileVersionInfoA_t>(g_GetFileVersionInfoA, "GetFileVersionInfoA");
    return fn ? fn(a, b, c, d) : FALSE;
}
extern "C" BOOL WINAPI hq_proxy_GetFileVersionInfoW(LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
    auto fn = ResolveCached<GetFileVersionInfoW_t>(g_GetFileVersionInfoW, "GetFileVersionInfoW");
    return fn ? fn(a, b, c, d) : FALSE;
}
extern "C" BOOL WINAPI hq_proxy_GetFileVersionInfoExA(DWORD a, LPCSTR b, DWORD c, DWORD d, LPVOID e) {
    auto fn = ResolveCached<GetFileVersionInfoExA_t>(g_GetFileVersionInfoExA, "GetFileVersionInfoExA");
    return fn ? fn(a, b, c, d, e) : FALSE;
}
extern "C" BOOL WINAPI hq_proxy_GetFileVersionInfoExW(DWORD a, LPCWSTR b, DWORD c, DWORD d, LPVOID e) {
    auto fn = ResolveCached<GetFileVersionInfoExW_t>(g_GetFileVersionInfoExW, "GetFileVersionInfoExW");
    return fn ? fn(a, b, c, d, e) : FALSE;
}
extern "C" DWORD WINAPI hq_proxy_GetFileVersionInfoSizeA(LPCSTR a, LPDWORD b) {
    auto fn = ResolveCached<GetFileVersionInfoSizeA_t>(g_GetFileVersionInfoSizeA, "GetFileVersionInfoSizeA");
    return fn ? fn(a, b) : 0;
}
extern "C" DWORD WINAPI hq_proxy_GetFileVersionInfoSizeW(LPCWSTR a, LPDWORD b) {
    auto fn = ResolveCached<GetFileVersionInfoSizeW_t>(g_GetFileVersionInfoSizeW, "GetFileVersionInfoSizeW");
    return fn ? fn(a, b) : 0;
}
extern "C" DWORD WINAPI hq_proxy_GetFileVersionInfoSizeExA(DWORD a, LPCSTR b, LPDWORD c) {
    auto fn = ResolveCached<GetFileVersionInfoSizeExA_t>(g_GetFileVersionInfoSizeExA, "GetFileVersionInfoSizeExA");
    return fn ? fn(a, b, c) : 0;
}
extern "C" DWORD WINAPI hq_proxy_GetFileVersionInfoSizeExW(DWORD a, LPCWSTR b, LPDWORD c) {
    auto fn = ResolveCached<GetFileVersionInfoSizeExW_t>(g_GetFileVersionInfoSizeExW, "GetFileVersionInfoSizeExW");
    return fn ? fn(a, b, c) : 0;
}
extern "C" BOOL WINAPI hq_proxy_GetFileVersionInfoByHandle(DWORD a, LPCSTR b, DWORD c, DWORD d, LPVOID e) {
    auto fn = ResolveCached<GetFileVersionInfoByHandle_t>(g_GetFileVersionInfoByHandle, "GetFileVersionInfoByHandle");
    return fn ? fn(a, b, c, d, e) : FALSE;
}
extern "C" BOOL WINAPI hq_proxy_VerQueryValueA(LPCVOID a, LPCSTR b, LPVOID* c, PUINT d) {
    auto fn = ResolveCached<VerQueryValueA_t>(g_VerQueryValueA, "VerQueryValueA");
    return fn ? fn(a, b, c, d) : FALSE;
}
extern "C" BOOL WINAPI hq_proxy_VerQueryValueW(LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d) {
    auto fn = ResolveCached<VerQueryValueW_t>(g_VerQueryValueW, "VerQueryValueW");
    return fn ? fn(a, b, c, d) : FALSE;
}
extern "C" DWORD WINAPI hq_proxy_VerLanguageNameA(DWORD a, LPSTR b, DWORD c) {
    auto fn = ResolveCached<VerLanguageNameA_t>(g_VerLanguageNameA, "VerLanguageNameA");
    return fn ? fn(a, b, c) : 0;
}
extern "C" DWORD WINAPI hq_proxy_VerLanguageNameW(DWORD a, LPWSTR b, DWORD c) {
    auto fn = ResolveCached<VerLanguageNameW_t>(g_VerLanguageNameW, "VerLanguageNameW");
    return fn ? fn(a, b, c) : 0;
}
extern "C" DWORD WINAPI hq_proxy_VerFindFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d,
                                              LPSTR e, PUINT f, LPSTR g, PUINT h) {
    auto fn = ResolveCached<VerFindFileA_t>(g_VerFindFileA, "VerFindFileA");
    return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}
extern "C" DWORD WINAPI hq_proxy_VerFindFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d,
                                              LPWSTR e, PUINT f, LPWSTR g, PUINT h) {
    auto fn = ResolveCached<VerFindFileW_t>(g_VerFindFileW, "VerFindFileW");
    return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}
extern "C" DWORD WINAPI hq_proxy_VerInstallFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d,
                                                 LPCSTR e, LPCSTR f, LPSTR g, PUINT h,
                                                 LPSTR i, PUINT j) {
    auto fn = ResolveCached<VerInstallFileA_t>(g_VerInstallFileA, "VerInstallFileA");
    return fn ? fn(a, b, c, d, e, f, g, h, i, j) : 0;
}
extern "C" DWORD WINAPI hq_proxy_VerInstallFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d,
                                                 LPCWSTR e, LPCWSTR f, LPWSTR g, PUINT h,
                                                 LPWSTR i, PUINT j) {
    auto fn = ResolveCached<VerInstallFileW_t>(g_VerInstallFileW, "VerInstallFileW");
    return fn ? fn(a, b, c, d, e, f, g, h, i, j) : 0;
}

// Export the forwarders under the canonical version.dll names. The linker
// /EXPORT directive maps the public name to our internal implementation,
// producing an export table identical to the genuine system DLL without ever
// declaring those names as source-level symbols (which winver.h already owns).
#pragma comment(linker, "/EXPORT:GetFileVersionInfoA=hq_proxy_GetFileVersionInfoA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoW=hq_proxy_GetFileVersionInfoW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExA=hq_proxy_GetFileVersionInfoExA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExW=hq_proxy_GetFileVersionInfoExW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeA=hq_proxy_GetFileVersionInfoSizeA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeW=hq_proxy_GetFileVersionInfoSizeW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExA=hq_proxy_GetFileVersionInfoSizeExA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExW=hq_proxy_GetFileVersionInfoSizeExW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoByHandle=hq_proxy_GetFileVersionInfoByHandle")
#pragma comment(linker, "/EXPORT:VerQueryValueA=hq_proxy_VerQueryValueA")
#pragma comment(linker, "/EXPORT:VerQueryValueW=hq_proxy_VerQueryValueW")
#pragma comment(linker, "/EXPORT:VerLanguageNameA=hq_proxy_VerLanguageNameA")
#pragma comment(linker, "/EXPORT:VerLanguageNameW=hq_proxy_VerLanguageNameW")
#pragma comment(linker, "/EXPORT:VerFindFileA=hq_proxy_VerFindFileA")
#pragma comment(linker, "/EXPORT:VerFindFileW=hq_proxy_VerFindFileW")
#pragma comment(linker, "/EXPORT:VerInstallFileA=hq_proxy_VerInstallFileA")
#pragma comment(linker, "/EXPORT:VerInstallFileW=hq_proxy_VerInstallFileW")
