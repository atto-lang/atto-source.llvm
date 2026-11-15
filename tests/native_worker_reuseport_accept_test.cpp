// Host-only proof that four independent nonblocking listeners can accept on
// one TCP port without a central process accepting/transferring client FDs.
// This does NOT prove compiled Oreslang actor access, scheduling, or ownership.
#include "oreslang/runtime/NonblockingTcpLinux.hpp"
#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
#include <utility>

using oreslang::runtime::NonblockingTcpListener;
using oreslang::runtime::OwnedSocketFd;

static void check(bool condition) {
    if (!condition) throw std::runtime_error("worker SO_REUSEPORT accept proof failed");
}

static OwnedSocketFd connect_loopback(std::uint16_t port) {
    auto socket = OwnedSocketFd::create_tcp();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    const int result = ::connect(socket.get(),
                                 reinterpret_cast<sockaddr*>(&address),
                                 sizeof(address));
    check(result == 0 || (result < 0 && errno == EINPROGRESS));
    return socket;
}

int main() {
    constexpr std::size_t workers = 4;
    constexpr std::size_t client_count = 24;
    std::vector<NonblockingTcpListener> listeners;
    listeners.reserve(workers);
    listeners.push_back(NonblockingTcpListener::loopback_reuseport(0, 128));
    const std::uint16_t port = listeners.front().local_port();
    check(port != 0);
    for (std::size_t i = 1; i < workers; ++i) {
        listeners.push_back(NonblockingTcpListener::loopback_reuseport(port, 128));
        check(listeners.back().fd() != listeners.front().fd());
        check(listeners.back().local_port() == port);
        auto empty = listeners.back().try_accept();
        check(empty.would_block && !empty.client.valid());
    }

    // A listener not configured for SO_REUSEPORT must not silently join this
    // kernel-controlled group: all participants must opt in before bind().
    bool refused_mixed_group = false;
    try {
        auto invalid = NonblockingTcpListener::loopback(port, 16);
        (void)invalid;
    } catch (const std::system_error& error) {
        refused_mixed_group = error.code().value() == EADDRINUSE;
    }
    check(refused_mixed_group);

    std::vector<OwnedSocketFd> clients;
    clients.reserve(client_count);
    for (std::size_t i = 0; i < client_count; ++i) {
        clients.push_back(connect_loopback(port));
    }

    // Each worker has its own host thread and independently calls accept4()
    // on its own FD. No supervisor receives or forwards accepted FDs.
    std::array<std::vector<OwnedSocketFd>, workers> accepted_by_worker;
    std::atomic<std::size_t> total_accepted{0};
    std::atomic<bool> worker_failed{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::vector<std::thread> worker_threads;
    worker_threads.reserve(workers);
    for (std::size_t index = 0; index < workers; ++index) {
        worker_threads.emplace_back([&, index] {
            while (total_accepted.load(std::memory_order_acquire) < client_count
                    && !worker_failed.load(std::memory_order_relaxed)
                    && std::chrono::steady_clock::now() < deadline) {
                auto attempt = listeners[index].try_accept();
                if (attempt.error_number != 0) {
                    worker_failed.store(true, std::memory_order_release);
                    return;
                }
                if (attempt.would_block) {
                    if (attempt.client.valid()) worker_failed.store(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (!attempt.client.valid()) {
                    worker_failed.store(true, std::memory_order_release);
                    return;
                }
                accepted_by_worker[index].push_back(std::move(attempt.client));
                total_accepted.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }
    for (auto& worker : worker_threads) worker.join();
    check(!worker_failed.load(std::memory_order_acquire));
    check(total_accepted.load(std::memory_order_acquire) == client_count);
    std::set<int> accepted_fds;
    for (const auto& worker : accepted_by_worker) {
        for (const auto& fd : worker) {
            check(fd.valid());
            check(accepted_fds.insert(fd.get()).second);
        }
    }
    check(accepted_fds.size() == client_count);

    // Exactly one worker accepts each connection; the shared port is not
    // a duplicated per-worker stream of the same inbound connections.
    for (auto& worker : listeners) {
        auto none = worker.try_accept();
        check(none.would_block && !none.client.valid() && none.error_number == 0);
    }
    return 0;
}
