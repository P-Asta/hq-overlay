#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

struct UniqueHandle {
    HANDLE value = nullptr;
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : value(handle) {}
    ~UniqueHandle() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value(other.value) { other.value = nullptr; }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    [[nodiscard]] explicit operator bool() const { return value != nullptr && value != INVALID_HANDLE_VALUE; }
};

[[nodiscard]] std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) { return std::towlower(ch); });
    return text;
}

[[nodiscard]] std::optional<DWORD> ParseProcessId(const std::wstring& text) {
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text.c_str(), &end, 10);
    if (errno == 0 && end == text.c_str() + text.size() && value > 0 && value <= MAXDWORD) {
        return static_cast<DWORD>(value);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<DWORD> FindProcess(const std::wstring& name) {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return std::nullopt;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const std::wstring wanted = Lower(name);
    for (BOOL more = Process32FirstW(snapshot.value, &entry); more; more = Process32NextW(snapshot.value, &entry)) {
        if (Lower(entry.szExeFile) == wanted) return entry.th32ProcessID;
    }
    return std::nullopt;
}

[[nodiscard]] std::wstring EventName(const wchar_t* kind, DWORD process_id) {
    return L"Local\\HQOverlay" + std::wstring(kind) + L"_" + std::to_wstring(process_id);
}

[[nodiscard]] bool IsX64Process(HANDLE process) {
    using IsWow64Process2Function = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto function = reinterpret_cast<IsWow64Process2Function>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (function != nullptr) {
        USHORT process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (!function(process, &process_machine, &native_machine)) return false;
        return process_machine == IMAGE_FILE_MACHINE_UNKNOWN && native_machine == IMAGE_FILE_MACHINE_AMD64;
    }
    BOOL wow64 = FALSE;
    return IsWow64Process(process, &wow64) != FALSE && wow64 == FALSE;
}

[[nodiscard]] void* RemoteLoadLibraryAddress(DWORD process_id) {
    const HMODULE local_kernel_module = GetModuleHandleW(L"kernel32.dll");
    if (local_kernel_module == nullptr) return nullptr;
    const auto local_kernel = reinterpret_cast<std::uintptr_t>(local_kernel_module);
    const auto local_loader = reinterpret_cast<std::uintptr_t>(GetProcAddress(local_kernel_module, "LoadLibraryW"));
    if (local_loader == 0) return nullptr;
    const std::uintptr_t offset = local_loader - local_kernel;

    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id));
    if (!snapshot) return nullptr;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32FirstW(snapshot.value, &entry); more; more = Module32NextW(snapshot.value, &entry)) {
        if (Lower(entry.szModule) == L"kernel32.dll") {
            return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(entry.modBaseAddr) + offset);
        }
    }
    return nullptr;
}

[[nodiscard]] DWORD ParseTimeout(int argc, wchar_t** argv) {
    for (int index = 2; index + 1 < argc; ++index) {
        if (std::wstring_view(argv[index]) != L"--timeout-ms") continue;
        errno = 0;
        wchar_t* end = nullptr;
        const unsigned long value = std::wcstoul(argv[index + 1], &end, 10);
        if (errno == 0 && end == argv[index + 1] + std::wcslen(argv[index + 1]) &&
            value >= 100 && value <= 120000) {
            return static_cast<DWORD>(value);
        }
    }
    return 15000;
}

void PrintLastError(const wchar_t* operation) {
    std::wcerr << operation << L" failed (Win32 " << GetLastError() << L")\n";
}

void PrintUsage() {
    std::wcout
        << L"HQ Overlay x64 injector\n\n"
        << L"Usage:\n"
        << L"  hq_injector.exe <pid|process.exe> [hq_overlay.dll] [--timeout-ms 15000]\n"
        << L"  hq_injector.exe --signal-disable <pid|process.exe>\n"
        << L"  hq_injector.exe --signal-shutdown <pid|process.exe>\n"
        << L"  hq_injector.exe --help\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    static_assert(sizeof(void*) == 8, "hq_injector must be built for x64");
    if (argc == 2 && (std::wstring_view(argv[1]) == L"--help" || std::wstring_view(argv[1]) == L"-h" ||
                      std::wstring_view(argv[1]) == L"/?")) {
        PrintUsage();
        return 0;
    }
    if (argc < 2) {
        PrintUsage();
        return 64;
    }

    const bool signal_disable = std::wstring_view(argv[1]) == L"--signal-disable";
    const bool signal_shutdown = std::wstring_view(argv[1]) == L"--signal-shutdown";
    if ((signal_disable || signal_shutdown) && argc != 3) {
        PrintUsage();
        return 64;
    }
    const wchar_t* target_argument = (signal_disable || signal_shutdown) ? argv[2] : argv[1];

    std::optional<DWORD> process_id = ParseProcessId(target_argument);
    if (!process_id.has_value()) process_id = FindProcess(target_argument);
    if (!process_id.has_value()) {
        std::wcerr << L"Target process was not found: " << target_argument << L'\n';
        return 2;
    }

    if (signal_disable || signal_shutdown) {
        const wchar_t* kind = signal_disable ? L"Disable" : L"Shutdown";
        const std::wstring event_name = EventName(kind, *process_id);
        UniqueHandle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str()));
        if (!event) {
            PrintLastError(L"OpenEvent");
            return 14;
        }
        if (!SetEvent(event.value)) {
            PrintLastError(L"SetEvent");
            return 15;
        }
        std::wcout << event_name << L" signaled.\n";
        return 0;
    }

    std::filesystem::path dll_path;
    if (argc >= 3 && std::wstring_view(argv[2]) != L"--timeout-ms") {
        dll_path = argv[2];
    } else {
        wchar_t executable_path[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable_path, static_cast<DWORD>(std::size(executable_path)));
        dll_path = std::filesystem::path(std::wstring_view(executable_path, length)).parent_path() / L"hq_overlay.dll";
    }
    std::error_code path_error;
    dll_path = std::filesystem::absolute(dll_path, path_error);
    if (path_error || !std::filesystem::is_regular_file(dll_path)) {
        std::wcerr << L"DLL does not exist: " << dll_path.wstring() << L'\n';
        return 3;
    }

    constexpr DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION |
                             PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    UniqueHandle process(OpenProcess(access, FALSE, *process_id));
    if (!process) {
        PrintLastError(L"OpenProcess");
        return 4;
    }
    if (!IsX64Process(process.value)) {
        std::wcerr << L"Target is not an x64 process; refusing cross-architecture injection.\n";
        return 5;
    }

    const std::wstring ready_name = EventName(L"Ready", *process_id);
    const std::wstring disable_name = EventName(L"Disable", *process_id);
    const std::wstring shutdown_name = EventName(L"Shutdown", *process_id);
    UniqueHandle ready(CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
    UniqueHandle disable(CreateEventW(nullptr, TRUE, FALSE, disable_name.c_str()));
    UniqueHandle shutdown(CreateEventW(nullptr, TRUE, FALSE, shutdown_name.c_str()));
    if (!ready || !disable || !shutdown) {
        PrintLastError(L"CreateEvent");
        return 6;
    }
    if (WaitForSingleObject(ready.value, 0) == WAIT_OBJECT_0 &&
        WaitForSingleObject(disable.value, 0) != WAIT_OBJECT_0) {
        std::wcout << L"Native overlay is already ready in PID " << *process_id << L".\n";
        return 0;
    }
    ResetEvent(ready.value);
    ResetEvent(disable.value);
    ResetEvent(shutdown.value);

    const std::wstring dll_text = dll_path.wstring();
    const SIZE_T byte_count = (dll_text.size() + 1) * sizeof(wchar_t);
    void* remote_buffer = VirtualAllocEx(process.value, nullptr, byte_count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_buffer == nullptr) {
        PrintLastError(L"VirtualAllocEx");
        return 7;
    }
    if (!WriteProcessMemory(process.value, remote_buffer, dll_text.c_str(), byte_count, nullptr)) {
        PrintLastError(L"WriteProcessMemory");
        VirtualFreeEx(process.value, remote_buffer, 0, MEM_RELEASE);
        return 8;
    }

    void* remote_loader = RemoteLoadLibraryAddress(*process_id);
    if (remote_loader == nullptr) {
        std::wcerr << L"Could not locate remote kernel32!LoadLibraryW.\n";
        VirtualFreeEx(process.value, remote_buffer, 0, MEM_RELEASE);
        return 9;
    }
    UniqueHandle thread(CreateRemoteThread(
        process.value, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_loader), remote_buffer, 0, nullptr));
    if (!thread) {
        PrintLastError(L"CreateRemoteThread");
        VirtualFreeEx(process.value, remote_buffer, 0, MEM_RELEASE);
        return 10;
    }

    const DWORD timeout = ParseTimeout(argc, argv);
    const DWORD load_wait = WaitForSingleObject(thread.value, timeout);
    if (load_wait != WAIT_OBJECT_0) {
        SetEvent(disable.value);
        std::wcerr << L"LoadLibraryW did not finish in time; Disable was signaled to prevent late rendering.\n";
        return 11;
    }
    DWORD remote_result = 0;
    GetExitCodeThread(thread.value, &remote_result);
    VirtualFreeEx(process.value, remote_buffer, 0, MEM_RELEASE);
    if (remote_result == 0) {
        SetEvent(disable.value);
        std::wcerr << L"Remote LoadLibraryW returned null.\n";
        return 12;
    }

    const DWORD ready_wait = WaitForSingleObject(ready.value, timeout);
    if (ready_wait != WAIT_OBJECT_0) {
        SetEvent(disable.value);
        std::wcerr << L"DLL loaded but no supported D3D11 swapchain became ready; Disable was signaled.\n";
        return 13;
    }
    std::wcout << L"hq_overlay.dll is ready in PID " << *process_id << L" (event " << ready_name << L").\n";
    return 0;
}
