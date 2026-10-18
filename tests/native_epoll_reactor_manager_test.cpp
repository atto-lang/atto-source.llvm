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
#include <sys/eventfd.h>
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
        // A duplicated nonblocking pipe-read FD must not bypass the manager's
        // ownership fence by registering on another epoll reactor.
        NativeEpollReactorManager manager(2,
            NativeEpollReactorManager::Profile::preallocated_rt, 2);
        int pipefd[2]{-1, -1};
        check(::pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
        const int alias = ::dup(pipefd[0]);
        check(alias >= 0 && alias != pipefd[0]);
        auto original = manager.try_watch(pipefd[0], EPOLLIN);
        check(original.has_value());
        check(!manager.try_watch(alias, EPOLLIN).has_value());
        // The pipe's separate write end shares inode but not access mode:
        // a producer must retain an independent EPOLLOUT registration.
        auto writer = manager.try_watch(pipefd[1], EPOLLOUT);
        check(writer.has_value());
        check(::write(pipefd[1], "x", 1) == 1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool saw_original = false;
        while (!saw_original && std::chrono::steady_clock::now() < deadline) {
            auto result = manager.try_pop();
            if (!result) { std::this_thread::yield(); continue; }
            if (result->reactor == original->reactor
                && result->event.ticket.value == original->ticket.value)
                saw_original = true;
        }
        check(saw_original);
        auto replacement = manager.try_watch(alias, EPOLLIN);
        check(replacement.has_value());
        check(!manager.try_watch(pipefd[0], EPOLLIN).has_value());
        manager.stop();
        ::close(alias);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
    }
    {
        // Two socket aliases are one readiness owner until terminal pop.
        NativeEpollReactorManager manager(2,
            NativeEpollReactorManager::Profile::preallocated_rt, 2);
        int sockets[2]{-1, -1};
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                           0, sockets) == 0);
        const int alias = ::dup(sockets[0]);
        check(alias >= 0);
        auto first = manager.try_watch(sockets[0], EPOLLIN);
        check(first.has_value());
        check(!manager.try_watch(alias, EPOLLIN).has_value());
        check(::write(sockets[1], "a", 1) == 1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool seen = false;
        while (!seen && std::chrono::steady_clock::now() < deadline) {
            auto result = manager.try_pop();
            if (!result) { std::this_thread::yield(); continue; }
            seen = result->event.ticket.value == first->ticket.value
                   && result->reactor == first->reactor;
        }
        check(seen);
        check(manager.try_watch(alias, EPOLLIN).has_value());
        manager.stop();
        ::close(alias);
        ::close(sockets[0]);
        ::close(sockets[1]);
    }
    {
        // Independent anon-inode eventfd objects may share st_dev/st_ino.
        // Anonymous descriptors require authenticated host-issued identities.
        NativeEpollReactorManager manager(2,
            NativeEpollReactorManager::Profile::preallocated_rt, 2);
        const int raw = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        check(raw >= 0);
        check(!manager.try_watch(raw, EPOLLIN).has_value());
        ::close(raw); // unmanaged anonymous descriptor denied
        auto one = manager.open_owned_eventfd();
        auto two = manager.open_owned_eventfd();
        check(one.has_value() && two.has_value());
        auto alias = manager.duplicate_owned_eventfd(*one);
        check(alias.has_value());
        auto a = manager.try_watch(*one, EPOLLIN);
        auto b = manager.try_watch(*two, EPOLLIN);
        check(a.has_value() && b.has_value());
        // Preallocated RT must automatically close the syscall-bearing FD
        // creation phase when the first accepted operation is attempted.
        check(!manager.open_owned_eventfd().has_value());
        check(!manager.duplicate_owned_eventfd(*one).has_value());
        check(manager.seal_owned_bootstrap()); // idempotent host operation
        check(!manager.try_watch(*alias, EPOLLIN).has_value());
        check(!manager.close_owned_eventfd(*one));
        check(manager.signal_owned_eventfd(*one));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool consumed = false;
        while (!consumed && std::chrono::steady_clock::now() < deadline) {
            auto completed = manager.try_pop();
            if (completed && completed->reactor == a->reactor &&
                completed->event.ticket.value == a->ticket.value)
                consumed = true;
            else std::this_thread::yield();
        }
        check(consumed);
        check(manager.drain_owned_eventfd(*alias));
        check(!manager.drain_owned_eventfd(*one)); // counter already empty
        auto alias_watch = manager.try_watch(*alias, EPOLLIN);
        check(alias_watch.has_value());
        check(!manager.try_watch(*one, EPOLLIN).has_value());
        check(manager.signal_owned_eventfd(*alias));
        bool alias_completed = false;
        const auto alias_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::seconds(3);
        while (!alias_completed && std::chrono::steady_clock::now() < alias_deadline) {
            auto completion = manager.try_pop();
            if (completion && completion->reactor == alias_watch->reactor &&
                completion->event.ticket.value == alias_watch->ticket.value)
                alias_completed = true;
            else std::this_thread::yield();
        }
        check(alias_completed);
        check(manager.close_owned_eventfd(*one));
        // RT bootstrap has already been sealed. Reuse is tested separately
        // in an unsealed throughput manager; creating here must fail closed.
        check(!manager.open_owned_eventfd().has_value());
        check(!manager.signal_owned_eventfd(*one));
        check(!manager.try_watch(*one, EPOLLIN).has_value());
        check(!manager.close_owned_eventfd(*one));
        manager.stop();
        while (manager.try_pop().has_value()) { }
        check(manager.close_owned_eventfd(*alias));
        check(!manager.close_owned_eventfd(*alias));
        check(manager.close_owned_eventfd(*two));
        check(!manager.open_owned_eventfd().has_value()); // retired manager
    }
    {
        // Failed validation cannot seal the RT mint phase before a ticket
        // exists. Actual successful registration must seal it precisely once.
        NativeEpollReactorManager manager(1,
            NativeEpollReactorManager::Profile::preallocated_rt, 1);
        auto first = manager.open_owned_eventfd();
        check(first.has_value());
        check(!manager.try_watch(*first, 0).has_value());
        auto second = manager.open_owned_eventfd();
        check(second.has_value());
        check(!manager.try_watch(*first, 0xffffffffu).has_value());
        auto watch = manager.try_watch(*first, EPOLLIN);
        check(watch.has_value());
        check(!manager.open_owned_eventfd().has_value());
        manager.stop();
        auto terminal = manager.try_pop();
        check(terminal.has_value() &&
              terminal->event.ticket.value == watch->ticket.value &&
              terminal->event.outcome == NativeEpollReactor::Outcome::stopped);
        check(manager.close_owned_eventfd(*first));
        check(manager.close_owned_eventfd(*second));
    }
    {
        // Reuse a retired slot while bootstrap is still open, so stale keys
        // cannot authenticate against its replacement, even after FD-number
        // and registry-slot reuse. RT bootstrap MUST NOT allow this after seal.
        NativeEpollReactorManager manager(1);
        auto first = manager.open_owned_eventfd();
        check(first.has_value());
        check(manager.close_owned_eventfd(*first));
        auto replacement = manager.open_owned_eventfd();
        check(replacement.has_value());
        check(!manager.signal_owned_eventfd(*first));
        check(!manager.drain_owned_eventfd(*first));
        check(!manager.try_watch(*first, EPOLLIN).has_value());
        check(!manager.close_owned_eventfd(*first));
        check(manager.signal_owned_eventfd(*replacement));
        auto watch = manager.try_watch(*replacement, EPOLLIN);
        check(watch.has_value());
        check(!manager.close_owned_eventfd(*replacement));
        manager.stop();
        auto terminal = manager.try_pop();
        check(terminal.has_value() &&
              terminal->event.ticket.value == watch->ticket.value);
        check(manager.close_owned_eventfd(*replacement));
    }
    {
        // A copied capability must not authenticate against another manager,
        // even if both registries use the same slot and local generation.
        NativeEpollReactorManager source(1);
        NativeEpollReactorManager other(1);
        auto token = source.open_owned_eventfd();
        auto foreign = other.open_owned_eventfd();
        check(token.has_value() && foreign.has_value());
        check(!other.try_watch(*token, EPOLLIN).has_value());
        check(!other.signal_owned_eventfd(*token));
        check(!other.close_owned_eventfd(*token));
        check(source.close_owned_eventfd(*token));
        check(other.close_owned_eventfd(*foreign));
        check(!source.try_watch(*token, EPOLLIN).has_value());
    }
    {
        // Explicit host bootstrap seal is enforceable in throughput mode too:
        // no later kernel descriptors, but pre-minted capabilities still work.
        NativeEpollReactorManager manager(1);
        auto key = manager.open_owned_eventfd();
        check(key.has_value());
        check(manager.started_threads() == 0);
        check(manager.seal_owned_bootstrap());
        check(manager.seal_owned_bootstrap());
        check(!manager.open_owned_eventfd().has_value());
        check(!manager.duplicate_owned_eventfd(*key).has_value());
        check(manager.started_threads() == 0);
        auto watch = manager.try_watch(*key, EPOLLIN);
        check(watch.has_value());
        check(manager.close_owned_eventfd(*key) == false);
        manager.stop();
        auto terminal = manager.try_pop();
        check(terminal.has_value() &&
              terminal->event.ticket.value == watch->ticket.value &&
              terminal->event.outcome == NativeEpollReactor::Outcome::stopped);
        check(manager.close_owned_eventfd(*key));
    }
    {
        // Cold throughput admission must fail before spawning any pthread.
        NativeEpollReactorManager manager(1);
        const int raw = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        check(raw >= 0);
        check(!manager.try_watch(raw, EPOLLIN).has_value());
        check(manager.started_threads() == 0);
        ::close(raw);
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
    bool bad = false;
    try { NativeEpollReactorManager manager(5); }
    catch (const std::invalid_argument&) { bad = true; }
    check(bad);
    return 0;
}
