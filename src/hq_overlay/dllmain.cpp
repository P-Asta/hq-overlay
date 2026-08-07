#include "overlay_runtime.hpp"
#include "version_proxy.hpp"

extern "C" __declspec(dllexport) const wchar_t* WINAPI HQOverlay_GetVersion() {
    return L"0.1.0-native";
}

extern "C" __declspec(dllexport) BOOL WINAPI HQOverlay_IsReady() {
    return hq::overlay::IsReady() ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI HQOverlay_RequestSoftDisable() {
    return hq::overlay::RequestSoftDisable("exported API request") ? TRUE : FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // Resolve the genuine system version.dll so the forwarded exports are
        // available before the host queries them. LoadLibraryExW with the
        // system32 search flag does not take the loader lock for a known DLL.
        hq::version_proxy::Initialize();
        // No graphics, COM, file I/O, waits, or hook installation occurs while the
        // loader lock is held. The worker cannot execute user work until DllMain
        // returns; its handle is deliberately closed immediately.
        HANDLE worker = CreateThread(nullptr, 0, hq::overlay::BootstrapThread, instance, 0, nullptr);
        if (worker != nullptr) CloseHandle(worker);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Process teardown owns all remaining address space. Never wait or call
        // MinHook/ImGui from DllMain.
        hq::overlay::NotifyProcessDetach();
        hq::version_proxy::Shutdown();
    }
    return TRUE;
}
