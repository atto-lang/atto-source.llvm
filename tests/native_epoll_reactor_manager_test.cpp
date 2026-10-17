#include "oreslang/runtime/NativeEpollReactorManager.hpp"
#include <array>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
using namespace oreslang::runtime;
static void check(bool good) { if (!good) throw std::runtime_error("reactor manager test failed"); }
int main() {
    {
        NativeEpollReactorManager manager(4);
        check(manager.started_threads() == 1);
        std::array<std::array<int, 2>, 5> fds{};
        for (auto& p : fds) p = {-1, -1};
        std::array<NativeEpollReactorManager::Watch, 5> tokens{};
        for (std::size_t i = 0; i < 5; ++i) {
            check(::pipe2(fds[i].data(), O_NONBLOCK | O_CLOEXEC) == 0);
            auto watch = manager.try_watch(fds[i][0], EPOLLIN);
            check(watch.has_value());
            tokens[i] = *watch;
            check(tokens[i].reactor == 0);
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
    bool bad = false;
    try { NativeEpollReactorManager manager(5); }
    catch (const std::invalid_argument&) { bad = true; }
    check(bad);
    return 0;
}
