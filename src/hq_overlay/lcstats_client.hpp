#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hq::lcstats {

struct Update {
    // The complete SSE data field, without the "data:" prefix. Multiple data
    // lines are joined with '\n' as required by the event-stream format.
    std::string raw_json;
    std::int64_t received_at_unix_ms = 0;
};

class Client final {
public:
    // Invoked on the WinHTTP worker thread. The receiver should copy/move the
    // update into its own queue (for example by PostMessage) and return quickly.
    using UpdateCallback = std::function<void(Update)>;

    Client();
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    // Starts the reconnecting http://localhost:2145/ event-stream worker and
    // returns without waiting for DNS, a connection, or response data.
    // Returns false for an empty callback, an already-running client, or a
    // local resource/thread creation failure.
    [[nodiscard]] bool Start(UpdateCallback callback) noexcept;

    // Requests cancellation and returns without joining the worker. The
    // worker observes the manual-reset event between WinHTTP calls and all
    // WinHTTP calls have short timeouts, so no caller is held by network I/O.
    void Stop() noexcept;

    // Optional orderly-shutdown and callback-quiescence barrier. A callback
    // already in flight may finish after Stop() returns, so keep its target
    // alive until this succeeds. Never call this from DllMain or while the
    // loader lock is held. Stop() should normally be called first.
    [[nodiscard]] bool WaitUntilStopped(std::chrono::milliseconds timeout) noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;

    // Process termination must not wait or call WinHTTP while the loader lock
    // is held. This only signals the worker and releases the owner's handles;
    // the OS owns the remaining address space. For a normal FreeLibrary path,
    // call Stop() and WaitUntilStopped() before releasing the DLL instead.
    void AbandonForProcessTermination() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hq::lcstats
