#include "oreslang/runtime/NativeEpollReactorManager.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <cerrno>
using namespace oreslang::runtime;
static void check(bool good) { if (!good) throw std::runtime_error("reactor manager test failed"); }
int main() {
    {
        NativeEpollReactorManager manager(4);
        check(manager.started_threads() == 0);
        check(!manager.try_watch(-1, EPOLLIN).has_value());
        check(manager.started_threads() == 0); // rejected admission stays cold
        std::array<std::array<int, 2>, 5> fds{};
        for (auto& p : fds) p = {-1, -1};
        std::array<NativeEpollReactorManager::Watch, 5> tokens{};
        for (std::size_t i = 0; i < 5; ++i) {
            check(::pipe2(fds[i].data(), O_NONBLOCK | O_CLOEXEC) == 0);
            auto watch = manager.try_watch(fds[i][0], EPOLLIN);
            check(watch.has_value());
            tokens[i] = *watch;
            check(tokens[i].reactor == 0);
            if (i == 0) check(manager.started_threads() == 1);
        }
        check(manager.started_threads() == 1); // five pending I/O != five threads
        for (auto& p : fds) check(::write(p[1], "x", 1) == 1);
        // A host poller is allowed to acknowledge eventfd before consuming
        // the completion records. They must remain signalled until drained:
        // try_pop is nonparking and can temporarily fail due to lock contention.
        struct pollfd early{manager.completion_fd(0), POLLIN, 0};
        check(early.fd >= 0);
        check(::poll(&early, 1, 3000) == 1);
        manager.drain_completion_signal(0);
        early.revents = 0;
        check(::poll(&early, 1, 0) == 1);
        std::array<bool, 5> seen{};
        std::size_t done = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (done < 5 && std::chrono::steady_clock::now() < deadline) {
            auto event = manager.try_pop();
            if (!event) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            for (std::size_t i = 0; i < 5; ++i) {
                if (event->reactor == tokens[i].reactor
                    && event->event.ticket.value == tokens[i].ticket.value) {
                    check(!seen[i]);
                    seen[i] = true;
                    ++done;
                }
            }
        }
        check(done == 5);
        // Completion eventfd is a notification, not the queue itself. An
        // undrained eventfd would remain readable and spin an outer poll loop.
        struct pollfd notification{manager.completion_fd(0), POLLIN, 0};
        check(notification.fd >= 0);
        check(::poll(&notification, 1, 0) == 1);
        manager.drain_completion_signal(0);
        notification.revents = 0;
        check(::poll(&notification, 1, 0) == 0);
        manager.stop();
        check(!manager.try_watch(fds[0][0], EPOLLIN).has_value());
        for (auto& p : fds) { ::close(p[0]); ::close(p[1]); }
    }
    {
        // A blocking FD must fail admission BEFORE starting a lazy reactor.
        // Otherwise the worker could park on a syscall unexpectedly and
        // violate the nonparking actor transport contract.
        NativeEpollReactorManager manager(2);
        int endpoints[2]{-1, -1};
        check(::pipe2(endpoints, O_CLOEXEC) == 0);
        check(!manager.try_watch(endpoints[0], EPOLLIN).has_value());
        check(manager.started_threads() == 0);
        const int flags = ::fcntl(endpoints[0], F_GETFL, 0);
        check(flags >= 0);
        check(::fcntl(endpoints[0], F_SETFL, flags | O_NONBLOCK) == 0);
        auto admitted = manager.try_watch(endpoints[0], EPOLLIN);
        check(admitted.has_value());
        check(manager.started_threads() == 1);
        check(!manager.try_watch(endpoints[0], EPOLLIN).has_value());
        manager.stop();
        ::close(endpoints[0]);
        ::close(endpoints[1]);
    }
    {
        NativeEpollReactorManager manager(3,
            NativeEpollReactorManager::Profile::preallocated_rt, 3);
        check(manager.started_threads() == 3);
        check(manager.limit() == 3);
        check(manager.profile() == NativeEpollReactorManager::Profile::preallocated_rt);
        // Production RT mode must never provision additional worker threads
        // on the operation path; all three are already prestarted.
        manager.stop();
    }
    {
        // A Linux FD can be installed in multiple *different* epoll instances.
        // Only the manager-level claim table stops two reactors from racing
        // to consume one socket/pipe's data. A stopped/cancelled watch keeps
        // ownership until its terminal completion is consumed.
        NativeEpollReactorManager manager(2,
            NativeEpollReactorManager::Profile::preallocated_rt, 2);
        int endpoints[2]{-1, -1};
        check(::pipe2(endpoints, O_NONBLOCK | O_CLOEXEC) == 0);
        auto original = manager.try_watch(endpoints[0], EPOLLIN);
        check(original.has_value());
        check(!manager.try_watch(endpoints[0], EPOLLIN).has_value());
        check(::write(endpoints[1], "x", 1) == 1);
        std::optional<NativeEpollReactorManager::Completion> delivered;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!delivered && std::chrono::steady_clock::now() < deadline) {
            delivered = manager.try_pop();
            if (!delivered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(delivered.has_value());
        check(delivered->reactor == original->reactor);
        check(delivered->event.ticket.value == original->ticket.value);
        // Ownership releases only after pop; a new watch gets a fresh ticket.
        std::optional<NativeEpollReactorManager::Watch> next;
        for (int attempt = 0; attempt < 50 && !next; ++attempt) {
            next = manager.try_watch(endpoints[0], EPOLLIN);
            if (!next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(next.has_value());
        check(!manager.try_watch(endpoints[0], EPOLLIN).has_value());
        manager.stop();
        ::close(endpoints[0]);
        ::close(endpoints[1]);
    }
    {
        NativeEpollReactorManager manager(1);
        int endpoints[2]{-1, -1};
        check(::pipe2(endpoints, O_NONBLOCK | O_CLOEXEC) == 0);
        auto watch = manager.try_watch(endpoints[0], EPOLLIN);
        check(watch.has_value());
        manager.stop();
        // Retirement must win every later cancellation. Stopped terminal
        // records are still consumable by trusted host cleanup code.
        check(!manager.try_cancel(*watch));
        auto stopped = manager.try_pop();
        check(stopped.has_value());
        check(stopped->event.ticket.value == watch->ticket.value);
        check(stopped->event.outcome == NativeEpollReactor::Outcome::stopped);
        check(!manager.try_cancel(*watch));
        check(!manager.try_watch(endpoints[0], EPOLLIN).has_value());
        ::close(endpoints[0]);
        ::close(endpoints[1]);
    }
    {
        // Cancellation and retirement may race on different host threads.
        // Exactly one terminal record must win; a failed try-lock is not
        // evidence of successful cancellation.
        for (int round = 0; round < 48; ++round) {
            NativeEpollReactorManager manager(1);
            int pipefd[2]{-1, -1};
            check(::pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
            auto watch = manager.try_watch(pipefd[0], EPOLLIN);
            check(watch.has_value());
            std::atomic<bool> cancelled{false};
            std::thread racing([&] {
                cancelled.store(manager.try_cancel(*watch), std::memory_order_release);
            });
            manager.stop();
            racing.join();
            auto terminal = manager.try_pop();
            check(terminal.has_value());
            check(terminal->event.ticket.value == watch->ticket.value);
            check(terminal->event.outcome ==
                (cancelled.load(std::memory_order_acquire)
                    ? NativeEpollReactor::Outcome::cancelled
                    : NativeEpollReactor::Outcome::stopped));
            check(!manager.try_pop().has_value());
            check(!manager.try_cancel(*watch));
            ::close(pipefd[0]);
            ::close(pipefd[1]);
        }
    }
    {
        // Capacity is a hard bound, not a suggestion to grow threads for idle
        // socket watches. Releasing a terminal completion must restore one
        // exactly-once FD claim and permit safe admission again.
        NativeEpollReactorManager manager(1);
        constexpr std::size_t count = NativeEpollReactor::capacity;
        std::array<std::array<int, 2>, count + 1> fds{};
        for (auto& p : fds) p = {-1, -1};
        for (auto& p : fds)
            check(::pipe2(p.data(), O_NONBLOCK | O_CLOEXEC) == 0);
        std::array<NativeEpollReactorManager::Watch, count> tickets{};
        for (std::size_t i = 0; i < count; ++i) {
            std::optional<NativeEpollReactorManager::Watch> watch;
            // try_watch is intentionally nonparking, so lock contention can
            // reject an individual test-host attempt; a test driver may retry.
            for (int retry = 0; retry < 2000 && !watch; ++retry) {
                watch = manager.try_watch(fds[i][0], EPOLLIN);
                if (!watch) std::this_thread::yield();
            }
            check(watch.has_value());
            tickets[i] = *watch;
        }
        check(manager.started_threads() == 1);
        check(!manager.try_watch(fds[count][0], EPOLLIN).has_value());
        bool cancelled = false;
        for (int retry = 0; retry < 2000 && !cancelled; ++retry) {
            cancelled = manager.try_cancel(tickets[0]);
            if (!cancelled) std::this_thread::yield();
        }
        check(cancelled);
        std::optional<NativeEpollReactorManager::Completion> terminal;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(3);
        while (!terminal && std::chrono::steady_clock::now() < deadline) {
            terminal = manager.try_pop();
            if (!terminal) std::this_thread::yield();
        }
        check(terminal.has_value());
        check(terminal->event.ticket.value == tickets[0].ticket.value);
        check(terminal->event.outcome == NativeEpollReactor::Outcome::cancelled);
        std::optional<NativeEpollReactorManager::Watch> recycled;
        for (int retry = 0; retry < 2000 && !recycled; ++retry) {
            recycled = manager.try_watch(fds[count][0], EPOLLIN);
            if (!recycled) std::this_thread::yield();
        }
        check(recycled.has_value());
        check(recycled->ticket.value != tickets[0].ticket.value);
        manager.stop();
        std::size_t stopped_records = 0;
        while (auto completion = manager.try_pop()) {
            check(completion->event.outcome == NativeEpollReactor::Outcome::stopped);
            ++stopped_records;
        }
        check(stopped_records == count);
        for (auto& p : fds) { ::close(p[0]); ::close(p[1]); }
    }

    {
        // dup() preserves the underlying open file description. Checking
        // only numeric FD equality allows two actors/reactors to consume
        // the same TCP bytes through distinct epoll registrations.
        for (int mode = 0; mode < 2; ++mode) {
            const bool socket_mode = mode == 1;
            NativeEpollReactorManager manager(2,
                NativeEpollReactorManager::Profile::preallocated_rt, 2);
            int endpoints[2]{-1, -1};
            if (socket_mode) {
                check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                    0, endpoints) == 0);
            } else {
                check(::pipe2(endpoints, O_NONBLOCK | O_CLOEXEC) == 0);
            }
            const int alias = ::dup(endpoints[0]);
            check(alias >= 0);
            auto original = manager.try_watch(endpoints[0], EPOLLIN);
            check(original.has_value());
            // Same socket/pipe inode, different FD value: must fail closed.
            check(!manager.try_watch(alias, EPOLLIN).has_value());
            check(!manager.try_watch(endpoints[0], EPOLLIN).has_value());
            // Opposite ends of a socketpair are different physical sockets.
            // Reject aliases without globally blocking unrelated descriptors.
            if (socket_mode) {
                auto other_end = manager.try_watch(endpoints[1], EPOLLIN);
                check(other_end.has_value());
                check(!manager.try_watch(endpoints[1], EPOLLIN).has_value());
            }
            bool cancelled = false;
            for (int attempt = 0; attempt < 2000 && !cancelled; ++attempt) {
                cancelled = manager.try_cancel(*original);
                if (!cancelled) std::this_thread::yield();
            }
            check(cancelled);
            std::optional<NativeEpollReactorManager::Completion> terminal;
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(3);
            while (!terminal && std::chrono::steady_clock::now() < deadline) {
                terminal = manager.try_pop();
                if (!terminal) std::this_thread::yield();
            }
            check(terminal.has_value());
            check(terminal->event.ticket.value == original->ticket.value);
            check(terminal->event.outcome == NativeEpollReactor::Outcome::cancelled);
            // The alias becomes admissible only AFTER terminal consumption.
            std::optional<NativeEpollReactorManager::Watch> next;
            for (int retry = 0; retry < 2000 && !next; ++retry) {
                next = manager.try_watch(alias, EPOLLIN);
                if (!next) std::this_thread::yield();
            }
            check(next.has_value());
            check(next->ticket.value != original->ticket.value
                || next->reactor != original->reactor);
            // Alias ownership also fences the original FD in reverse.
            check(!manager.try_watch(endpoints[0], EPOLLIN).has_value());
            manager.stop();
            ::close(alias);
            ::close(endpoints[0]);
            ::close(endpoints[1]);
        }
    }
    {
        // Stress duplicate references to ONE socket file description across
        // eight racing host producers. A separate socketpair endpoint remains
        // independently watchable. This exercises the manager's bounded
        // nonparking transport identity gate, not guest actor authority.
        NativeEpollReactorManager manager(2,
            NativeEpollReactorManager::Profile::preallocated_rt, 2);
        int endpoints[2]{-1, -1};
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0, endpoints) == 0);
        constexpr std::size_t aliases_count = 8;
        std::array<int, aliases_count> aliases{};
        for (auto& alias : aliases) {
            alias = ::dup(endpoints[0]);
            check(alias >= 0 && alias != endpoints[0]);
        }
        auto watch = manager.try_watch(endpoints[0], EPOLLIN);
        check(watch.has_value());
        auto separate = manager.try_watch(endpoints[1], EPOLLIN);
        check(separate.has_value());
        std::atomic<unsigned> unsafe_alias_claims{0};
        std::array<std::thread, aliases_count> threads;
        for (std::size_t i = 0; i < aliases_count; ++i) {
            threads[i] = std::thread([&, i] {
                for (unsigned attempt = 0; attempt < 128; ++attempt) {
                    if (manager.try_watch(aliases[i], attempt % 2
                            ? EPOLLOUT : EPOLLIN).has_value()) {
                        unsafe_alias_claims.fetch_add(1,
                            std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& thread : threads) thread.join();
        check(unsafe_alias_claims.load(std::memory_order_acquire) == 0);
        // Successful cancellation does not instantly release ownership:
        // the terminal completion MUST first be consumed.
        bool cancelled = false;
        for (int retry = 0;retry < 2000 && !cancelled;++retry) {
            cancelled = manager.try_cancel(*watch);
            if (!cancelled) std::this_thread::yield();
        }
        check(cancelled);
        for (int i = 0; i < 32; ++i) {
            check(!manager.try_watch(aliases[0], EPOLLIN).has_value());
        }
        bool consumed = false;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(3);
        while (!consumed && std::chrono::steady_clock::now() < deadline) {
            auto terminal = manager.try_pop();
            if (!terminal) {
                std::this_thread::yield();
                continue;
            }
            if (terminal->reactor == watch->reactor
                && terminal->event.ticket.value == watch->ticket.value) {
                check(terminal->event.outcome ==
                    NativeEpollReactor::Outcome::cancelled);
                consumed = true;
            } else {
                check(false); // separate opposite endpoint is still pending
            }
        }
        check(consumed);
        std::optional<NativeEpollReactorManager::Watch> reused;
        for (int retry = 0;retry < 2000 && !reused;++retry) {
            reused = manager.try_watch(aliases[0], EPOLLIN);
            if (!reused) std::this_thread::yield();
        }
        check(reused.has_value());
        check(!manager.try_watch(endpoints[0], EPOLLOUT).has_value());
        manager.stop();
        for (const int alias : aliases) ::close(alias);
        ::close(endpoints[0]);
        ::close(endpoints[1]);
    }
    {
        // Pipe read and write endpoints share the same inode, yet are
        // independent directions. A conservative inode-only alias guard
        // must not prevent full-duplex reactor event admission on pipes.
        NativeEpollReactorManager manager(1,
            NativeEpollReactorManager::Profile::preallocated_rt, 1);
        int endpoints[2]{-1, -1};
        check(::pipe2(endpoints, O_NONBLOCK | O_CLOEXEC) == 0);
        auto reader = manager.try_watch(endpoints[0], EPOLLIN);
        check(reader.has_value());
        auto writer = manager.try_watch(endpoints[1], EPOLLOUT);
        check(writer.has_value());
        const int reader_alias = ::dup(endpoints[0]);
        const int writer_alias = ::dup(endpoints[1]);
        check(reader_alias >= 0 && writer_alias >= 0);
        check(!manager.try_watch(reader_alias, EPOLLIN).has_value());
        check(!manager.try_watch(writer_alias, EPOLLOUT).has_value());
        manager.stop();
        ::close(reader_alias);
        ::close(writer_alias);
        ::close(endpoints[0]);
        ::close(endpoints[1]);
    }
    {
        // dup2 can overwrite a numeric FD while retaining the original
        // underlying socket. It must not bypass the ownership claim.
        NativeEpollReactorManager manager(1,
            NativeEpollReactorManager::Profile::preallocated_rt, 1);
        int endpoints[2]{-1, -1};
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0, endpoints) == 0);
        const int alias = ::dup(endpoints[0]);
        check(alias >= 0 && alias != endpoints[0]);
        check(::dup2(endpoints[0], alias) == alias);
        auto watch = manager.try_watch(endpoints[0], EPOLLIN);
        check(watch.has_value());
        check(!manager.try_watch(alias, EPOLLOUT).has_value());
        manager.stop();
        ::close(alias);
        ::close(endpoints[0]);
        ::close(endpoints[1]);
    }
    {
        // Native physical stream proof: a socketpair endpoint cannot be
        // watched twice for separate read/write ownership, even when the
        // second caller supplies a duplicated numeric FD. The opposite
        // endpoint is independent. Completion consumption releases the
        // exact original claim; no guest TCP pooling is implied.
        NativeEpollReactorManager manager(2,
            NativeEpollReactorManager::Profile::preallocated_rt, 2);
        int endpoints[2]{-1, -1};
        check(::socketpair(AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, endpoints) == 0);
        const int alias = ::dup(endpoints[0]);
        check(alias >= 0 && alias != endpoints[0]);
        const auto original = manager.try_watch(endpoints[0], EPOLLIN);
        check(original.has_value());
        check(!manager.try_watch(endpoints[0], EPOLLOUT).has_value());
        check(!manager.try_watch(alias, EPOLLIN).has_value());
        check(!manager.try_watch(alias, EPOLLOUT).has_value());
        const auto independent = manager.try_watch(endpoints[1], EPOLLIN);
        check(independent.has_value());
        check(::write(endpoints[1], "x", 1) == 1);
        bool consumed = false;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(3);
        while (!consumed && std::chrono::steady_clock::now() < deadline) {
            const auto event = manager.try_pop();
            if (!event) { std::this_thread::yield(); continue; }
            check(event->reactor == original->reactor);
            check(event->event.ticket.value == original->ticket.value);
            check(event->event.outcome ==
                NativeEpollReactor::Outcome::ready);
            consumed = true;
        }
        check(consumed);
        // Reuse is allowed only after that exact terminal event is popped.
        std::optional<NativeEpollReactorManager::Watch> replacement;
        for (int retry = 0; retry < 2000 && !replacement; ++retry) {
            replacement = manager.try_watch(alias, EPOLLIN);
            if (!replacement) std::this_thread::yield();
        }
        check(replacement.has_value());
        check(!manager.try_watch(endpoints[0], EPOLLIN).has_value());
        manager.stop();
        ::close(alias);
        ::close(endpoints[0]);
        ::close(endpoints[1]);
    }
    {
        // Real AF_INET loopback rather than socketpair: an epoll watch on
        // the connected TCP stream excludes dup() aliases until its terminal
        // record is consumed, while the accepted peer is independent.
        // This remains HOST reactor proof, NOT actor-owned pool integration.
        int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        check(listener >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        check(::bind(listener, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)) == 0);
        check(::listen(listener, 4) == 0);
        socklen_t address_length = sizeof(address);
        check(::getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                            &address_length) == 0);
        int client = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        check(client >= 0);
        int connect_result = ::connect(client,
            reinterpret_cast<sockaddr*>(&address), sizeof(address));
        check(connect_result == 0 || (connect_result == -1 && errno == EINPROGRESS));
        pollfd outgoing{client, POLLOUT, 0};
        check(::poll(&outgoing, 1, 3000) == 1);
        int connect_error = 0;
        socklen_t connect_length = sizeof(connect_error);
        check(::getsockopt(client, SOL_SOCKET, SO_ERROR,
            &connect_error, &connect_length) == 0 && connect_error == 0);
        pollfd listening{listener, POLLIN, 0};
        check(::poll(&listening, 1, 3000) == 1);
        int accepted = ::accept4(listener, nullptr, nullptr,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
        check(accepted >= 0);
        int alias = ::dup(client);
        check(alias >= 0 && alias != client);
        NativeEpollReactorManager manager(2,
            NativeEpollReactorManager::Profile::preallocated_rt, 2);
        auto original = manager.try_watch(client, EPOLLIN);
        check(original.has_value());
        auto peer_watch = manager.try_watch(accepted, EPOLLIN);
        check(peer_watch.has_value());
        check(!manager.try_watch(alias, EPOLLIN).has_value());
        const char response[2]{'x', 'y'};
        std::size_t sent = 0;
        const auto send_deadline = std::chrono::steady_clock::now()
                                 + std::chrono::seconds(3);
        while (sent < 2 && std::chrono::steady_clock::now() < send_deadline) {
            const auto n = ::write(accepted, response + sent, 2 - sent);
            if (n > 0) sent += static_cast<std::size_t>(n);
            else if (n < 0 && (errno == EINTR || errno == EAGAIN
                               || errno == EWOULDBLOCK))
                std::this_thread::yield();
            else break;
        }
        check(sent == 2);
        std::optional<NativeEpollReactorManager::Completion> completed;
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(3);
        while (!completed && std::chrono::steady_clock::now() < deadline) {
            completed = manager.try_pop();
            if (!completed) std::this_thread::yield();
        }
        check(completed.has_value());
        check(completed->reactor == original->reactor &&
              completed->event.ticket.value == original->ticket.value);
        // Only after the terminal event has been consumed can manager
        // bookkeeping release this claim. A FUTURE actor-owner ACK fence
        // must be layered on top; it is not implemented by this manager.
        char payload[2]{};
        std::size_t received = 0;
        const auto read_deadline = std::chrono::steady_clock::now()
                                 + std::chrono::seconds(3);
        // TCP has no message-sized reads: tolerate a partial/fragmented
        // transfer and never infer a two-byte frame from one syscall.
        while (received < 2 && std::chrono::steady_clock::now() < read_deadline) {
            const auto n = ::read(client, payload + received, 2 - received);
            if (n > 0) received += static_cast<std::size_t>(n);
            else if (n < 0 && (errno == EINTR || errno == EAGAIN
                               || errno == EWOULDBLOCK))
                std::this_thread::yield();
            else break;
        }
        check(received == 2 && payload[0] == 'x' && payload[1] == 'y');
        std::optional<NativeEpollReactorManager::Watch> reacquired;
        for (int retry = 0; retry < 2000 && !reacquired; ++retry) {
            reacquired = manager.try_watch(alias, EPOLLIN);
            if (!reacquired) std::this_thread::yield();
        }
        check(reacquired.has_value());
        check(!manager.try_watch(client, EPOLLIN).has_value());
        manager.stop();
        ::close(alias);
        ::close(accepted);
        ::close(client);
        ::close(listener);
    }
    bool bad = false;
    try { NativeEpollReactorManager manager(5); }
    catch (const std::invalid_argument&) { bad = true; }
    check(bad);
    return 0;
}
