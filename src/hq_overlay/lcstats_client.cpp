#include "lcstats_client.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

namespace hq::lcstats {
namespace {

using namespace std::chrono_literals;

constexpr wchar_t kServerName[] = L"localhost";
constexpr wchar_t kRequestPath[] = L"/";
constexpr INTERNET_PORT kServerPort = 2145;
constexpr std::chrono::milliseconds kInitialReconnectDelay = 250ms;
constexpr std::chrono::milliseconds kMaximumReconnectDelay = 5s;
constexpr DWORD kResolveTimeoutMs = 1'000;
constexpr DWORD kConnectTimeoutMs = 1'500;
constexpr DWORD kSendTimeoutMs = 1'500;
constexpr DWORD kReceiveTimeoutMs = 1'000;
constexpr std::size_t kMaximumLineBytes = 1024 * 1024;
constexpr std::size_t kMaximumEventBytes = 8 * 1024 * 1024;
constexpr DWORD kMaximumReadBytes = 64 * 1024;

// GetModuleHandleExW treats this pointer as an address when FROM_ADDRESS is
// supplied; a data object avoids converting a function pointer to an object
// pointer under /permissive-.
const unsigned char kModuleAnchor = 0;

class InternetHandle final {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) noexcept : value_(value) {}
    ~InternetHandle() {
        Reset();
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    InternetHandle(InternetHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HINTERNET Get() const noexcept {
        return value_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

    void Reset(HINTERNET replacement = nullptr) noexcept {
        if (value_ != nullptr) {
            WinHttpCloseHandle(value_);
        }
        value_ = replacement;
    }

private:
    HINTERNET value_ = nullptr;
};

[[nodiscard]] std::int64_t UnixTimeMilliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] bool IsAsciiWhitespaceOnly(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    });
}

class EventStreamDecoder final {
public:
    template <typename OnPayload>
    [[nodiscard]] bool Feed(std::string_view bytes, OnPayload&& on_payload) {
        for (const char character : bytes) {
            if (character == '\n') {
                if (!ConsumeLine(std::forward<OnPayload>(on_payload))) return false;
                line_.clear();
                continue;
            }
            if (line_.size() >= kMaximumLineBytes) return false;
            line_.push_back(character);
        }
        return true;
    }

private:
    template <typename OnPayload>
    [[nodiscard]] bool ConsumeLine(OnPayload&& on_payload) {
        if (!line_.empty() && line_.back() == '\r') line_.pop_back();
        if (first_line_) {
            first_line_ = false;
            constexpr std::string_view utf8_bom{"\xEF\xBB\xBF", 3};
            if (line_.starts_with(utf8_bom)) line_.erase(0, utf8_bom.size());
        }

        if (line_.empty()) {
            if (saw_data_) {
                if (!data_.empty() && data_.back() == '\n') data_.pop_back();
                if (!data_.empty() && !IsAsciiWhitespaceOnly(data_)) {
                    on_payload(std::move(data_));
                }
            }
            data_.clear();
            saw_data_ = false;
            return true;
        }

        std::string_view line(line_);
        std::string_view value;
        if (line == "data") {
            value = {};
        } else if (line.starts_with("data:")) {
            value = line.substr(5);
            if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        } else {
            return true;
        }

        if (value.size() > kMaximumEventBytes || data_.size() > kMaximumEventBytes - value.size()) {
            return false;
        }
        data_.append(value);
        if (data_.size() == kMaximumEventBytes) return false;
        data_.push_back('\n');
        saw_data_ = true;
        return true;
    }

    bool first_line_ = true;
    bool saw_data_ = false;
    std::string line_;
    std::string data_;
};

struct ClientState final {
    explicit ClientState(Client::UpdateCallback receiver) : callback(std::move(receiver)) {
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stopped_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~ClientState() {
        if (stop_event != nullptr) CloseHandle(stop_event);
        if (stopped_event != nullptr) CloseHandle(stopped_event);
    }

    ClientState(const ClientState&) = delete;
    ClientState& operator=(const ClientState&) = delete;

    [[nodiscard]] bool IsValid() const noexcept {
        return stop_event != nullptr && stopped_event != nullptr;
    }

    [[nodiscard]] bool StopRequested() const noexcept {
        return stop_requested.load(std::memory_order_acquire);
    }

    void RequestStop() noexcept {
        stop_requested.store(true, std::memory_order_release);
        if (stop_event != nullptr) SetEvent(stop_event);
    }

    [[nodiscard]] bool WaitForStop(std::chrono::milliseconds timeout) const noexcept {
        if (StopRequested()) return true;
        if (stop_event == nullptr) return true;
        const auto nonnegative = std::max(timeout, std::chrono::milliseconds::zero());
        const auto bounded = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(nonnegative.count()),
            static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max() - 1));
        return WaitForSingleObject(stop_event, static_cast<DWORD>(bounded)) == WAIT_OBJECT_0;
    }

    void Deliver(std::string raw_json) noexcept {
        if (StopRequested()) return;
        try {
            Client::UpdateCallback receiver;
            {
                std::scoped_lock lock(callback_mutex);
                receiver = callback;
            }
            if (!receiver || StopRequested()) return;
            receiver(Update{std::move(raw_json), UnixTimeMilliseconds()});
        } catch (...) {
            // A consumer exception must not kill the reconnect worker or cross
            // the Win32 thread-entry boundary.
        }
    }

    void MarkStopped() noexcept {
        {
            std::scoped_lock lock(callback_mutex);
            callback = {};
        }
        running.store(false, std::memory_order_release);
        if (stopped_event != nullptr) SetEvent(stopped_event);
    }

    HANDLE stop_event = nullptr;
    HANDLE stopped_event = nullptr;
    std::atomic_bool stop_requested{false};
    std::atomic_bool running{true};
    std::mutex callback_mutex;
    Client::UpdateCallback callback;
};

[[nodiscard]] bool ReceiveOneConnection(const std::shared_ptr<ClientState>& state) {
    InternetHandle session(WinHttpOpen(L"HQOverlay-LCStats/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session || state->StopRequested()) return false;

    if (!WinHttpSetTimeouts(session.Get(), kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
                            kReceiveTimeoutMs)) {
        return false;
    }

    InternetHandle connection(WinHttpConnect(session.Get(), kServerName, kServerPort, 0));
    if (!connection || state->StopRequested()) return false;

    InternetHandle request(WinHttpOpenRequest(connection.Get(), L"GET", kRequestPath, nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_REFRESH));
    if (!request || state->StopRequested()) return false;

    constexpr wchar_t headers[] = L"Accept: text/event-stream\r\nCache-Control: no-cache\r\n";
    if (!WinHttpSendRequest(request.Get(), headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) ||
        state->StopRequested()) {
        return false;
    }
    if (!WinHttpReceiveResponse(request.Get(), nullptr) || state->StopRequested()) return false;

    DWORD status_code = 0;
    DWORD status_code_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_code_size,
                             WINHTTP_NO_HEADER_INDEX) ||
        status_code != HTTP_STATUS_OK) {
        return false;
    }

    EventStreamDecoder decoder;
    bool delivered_payload = false;
    while (!state->StopRequested()) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.Get(), &available)) {
            const DWORD error = GetLastError();
            if (error == ERROR_WINHTTP_TIMEOUT) continue;
            break;
        }
        if (available == 0) break;

        const DWORD requested = std::min(available, kMaximumReadBytes);
        std::string bytes(requested, '\0');
        DWORD received = 0;
        if (!WinHttpReadData(request.Get(), bytes.data(), requested, &received)) {
            const DWORD error = GetLastError();
            if (error == ERROR_WINHTTP_TIMEOUT) continue;
            break;
        }
        if (received == 0) break;
        bytes.resize(received);
        if (!decoder.Feed(bytes, [&](std::string payload) {
                delivered_payload = true;
                state->Deliver(std::move(payload));
            })) {
            break;
        }
    }
    return delivered_payload;
}

void RunClient(const std::shared_ptr<ClientState>& state) noexcept {
    auto reconnect_delay = kInitialReconnectDelay;
    while (!state->StopRequested()) {
        bool received_payload = false;
        try {
            received_payload = ReceiveOneConnection(state);
        } catch (...) {
            received_payload = false;
        }
        if (state->StopRequested()) break;

        if (received_payload) {
            reconnect_delay = kInitialReconnectDelay;
        } else {
            reconnect_delay = std::min(reconnect_delay * 2, kMaximumReconnectDelay);
        }
        if (state->WaitForStop(reconnect_delay)) break;
    }
}

struct ThreadStart final {
    std::shared_ptr<ClientState> state;
    HMODULE pinned_module = nullptr;
};

DWORD WINAPI ClientThreadEntry(void* parameter) noexcept {
    auto start = std::unique_ptr<ThreadStart>(static_cast<ThreadStart*>(parameter));
    auto state = std::move(start->state);
    const HMODULE pinned_module = start->pinned_module;
    start.reset();

    RunClient(state);
    state->MarkStopped();
    state.reset();

    // The worker pins the containing DLL before CreateThread. Combining the
    // final reference release and thread exit prevents execution from
    // returning into code that another thread just unloaded.
    FreeLibraryAndExitThread(pinned_module, 0);
    return 0;
}

}  // namespace

struct Client::Impl final {
    ~Impl() {
        std::scoped_lock lock(mutex);
        if (thread != nullptr) CloseHandle(thread);
    }

    mutable std::mutex mutex;
    std::shared_ptr<ClientState> state;
    HANDLE thread = nullptr;
};

Client::Client() : impl_(std::make_unique<Impl>()) {}

Client::~Client() {
    Stop();
}

bool Client::Start(UpdateCallback callback) noexcept {
    if (!callback || !impl_) return false;
    try {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->state && impl_->state->running.load(std::memory_order_acquire)) return false;

        if (impl_->thread != nullptr) {
            CloseHandle(impl_->thread);
            impl_->thread = nullptr;
        }
        impl_->state.reset();

        auto state = std::make_shared<ClientState>(std::move(callback));
        if (!state->IsValid()) return false;

        // Allocate everything that can throw before incrementing the module's
        // reference count, otherwise an allocation failure would leak the pin.
        auto start = std::make_unique<ThreadStart>();
        start->state = state;

        HMODULE pinned_module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(&kModuleAnchor), &pinned_module)) {
            return false;
        }

        start->pinned_module = pinned_module;
        HANDLE thread = CreateThread(nullptr, 0, ClientThreadEntry, start.get(), 0, nullptr);
        if (thread == nullptr) {
            FreeLibrary(pinned_module);
            return false;
        }
        start.release();
        impl_->state = std::move(state);
        impl_->thread = thread;
        return true;
    } catch (...) {
        return false;
    }
}

void Client::Stop() noexcept {
    if (!impl_) return;
    std::shared_ptr<ClientState> state;
    {
        std::scoped_lock lock(impl_->mutex);
        state = impl_->state;
    }
    if (state) state->RequestStop();
}

bool Client::WaitUntilStopped(std::chrono::milliseconds timeout) noexcept {
    if (!impl_) return true;
    std::shared_ptr<ClientState> state;
    {
        std::scoped_lock lock(impl_->mutex);
        state = impl_->state;
    }
    if (!state) return true;
    if (state->stopped_event == nullptr) return false;

    const auto nonnegative = std::max(timeout, std::chrono::milliseconds::zero());
    const auto bounded = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(nonnegative.count()),
        static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max() - 1));
    return WaitForSingleObject(state->stopped_event, static_cast<DWORD>(bounded)) == WAIT_OBJECT_0;
}

bool Client::IsRunning() const noexcept {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    return impl_->state && impl_->state->running.load(std::memory_order_acquire);
}

void Client::AbandonForProcessTermination() noexcept {
    if (!impl_) return;
    std::shared_ptr<ClientState> state;
    {
        std::scoped_lock lock(impl_->mutex);
        state = std::move(impl_->state);
        if (impl_->thread != nullptr) {
            CloseHandle(impl_->thread);
            impl_->thread = nullptr;
        }
    }
    if (state) state->RequestStop();
}

}  // namespace hq::lcstats
