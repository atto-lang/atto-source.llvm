#include "oreslang/runtime/NativeEpollReactorManager.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
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
        // Actor-owned readiness holds one physical resource loan from epoll
        // registration through terminal completion handoff. STOP may prevent
        // new watches, but it cannot pretend an accepted watch is already DONE.
        NativeActorGroup group(880001, 1);
        auto actor = group.adopt(880001, 1, 1, [] {});
        check(group.mark_ready(actor));
        NativeEpollReactorManager manager(1);
        int pipefd[2]{-1, -1};
        check(::pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);

        auto owned = manager.try_watch_actor(actor, pipefd[0], EPOLLIN);
        check(owned.has_value());
        check(actor.outstanding_resource_loans() == 1);
        group.stop_workers();

        int second[2]{-1, -1};
        check(::pipe2(second, O_NONBLOCK | O_CLOEXEC) == 0);
        check(!manager.try_watch_actor(actor, second[0], EPOLLIN).has_value());
        check(actor.outstanding_resource_loans() == 1);
        // A bare reactor token is never authority to cancel an actor watch.
        check(!manager.try_cancel(owned->watch));
        check(manager.try_cancel_actor(*owned));

        std::optional<NativeEpollReactorManager::Completion> completion;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(3);
        while (!completion && std::chrono::steady_clock::now() < deadline) {
            completion = manager.try_pop();
            if (!completion) std::this_thread::yield();
        }
        check(completion.has_value());
        check(completion->actor.has_value());
        check(completion->actor->same_incarnation(actor));
        check(completion->event.outcome == NativeEpollReactor::Outcome::cancelled);
        // Popping the terminal record is not yet scheduler handoff ACK.
        check(actor.outstanding_resource_loans() == 1);
        check(!group.finalized(actor));
        check(manager.ack_actor_completion(*completion));
        check(!manager.ack_actor_completion(*completion));
        check(actor.outstanding_resource_loans() == 0);
        check(group.finalized(actor));
        actor.get_done().get();

        ::close(pipefd[0]); ::close(pipefd[1]);
        ::close(second[0]); ::close(second[1]);
    }
    {
        // Readiness itself is terminal for the one-shot kernel registration,
        // but actor DONE still waits until trusted scheduler code acknowledges
        // the reactor-to-continuation ownership transfer.
        NativeActorGroup group(880002, 1);
        auto actor = group.adopt(880002, 7, 1, [] {});
        check(group.mark_ready(actor));
        NativeEpollReactorManager manager(1);
        int pipefd[2]{-1, -1};
        check(::pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
        auto owned = manager.try_watch_actor(actor, pipefd[0], EPOLLIN);
        check(owned.has_value());
        check(::write(pipefd[1], "x", 1) == 1);

        std::optional<NativeEpollReactorManager::Completion> completion;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(3);
        while (!completion && std::chrono::steady_clock::now() < deadline) {
            completion = manager.try_pop();
            if (!completion) std::this_thread::yield();
        }
        check(completion.has_value());
        check(completion->event.outcome == NativeEpollReactor::Outcome::ready);
        check(completion->actor && completion->actor->same_incarnation(actor));
        group.stop_workers();
        check(!group.finalized(actor));
        check(manager.ack_actor_completion(*completion));
        check(group.finalized(actor));
        actor.get_done().get();
        ::close(pipefd[0]); ::close(pipefd[1]);
    }
    {
        // Reusing numeric runtime/id/generation cannot let an old ActorWatch
        // cancel or acknowledge a new incarnation's kernel registration.
        NativeEpollReactorManager manager(1);
        NativeActorGroup old_group(880003, 1);
        auto old_actor = old_group.adopt(880003, 9, 1, [] {});
        check(old_group.mark_ready(old_actor));
        int pipefd[2]{-1, -1};
        check(::pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
        auto old_watch = manager.try_watch_actor(old_actor, pipefd[0], EPOLLIN);
        check(old_watch.has_value());
        old_group.stop_workers();
        check(manager.try_cancel_actor(*old_watch));
        std::optional<NativeEpollReactorManager::Completion> old_completion;
        for (int retry = 0; retry < 3000 && !old_completion; ++retry) {
            old_completion = manager.try_pop();
            if (!old_completion) std::this_thread::yield();
        }
        check(old_completion && manager.ack_actor_completion(*old_completion));
        check(old_group.finalized(old_actor));
        old_actor.get_done().get();

        NativeActorGroup new_group(880003, 1);
        auto new_actor = new_group.adopt(880003, 9, 1, [] {});
        check(new_group.mark_ready(new_actor));
        check(old_actor == new_actor);
        check(!old_actor.same_incarnation(new_actor));
        auto new_watch = manager.try_watch_actor(new_actor, pipefd[0], EPOLLIN);
        check(new_watch.has_value());
        check(!manager.try_cancel_actor(*old_watch));
        check(new_actor.outstanding_resource_loans() == 1);
        check(manager.try_cancel_actor(*new_watch));
        std::optional<NativeEpollReactorManager::Completion> new_completion;
        for (int retry = 0; retry < 3000 && !new_completion; ++retry) {
            new_completion = manager.try_pop();
            if (!new_completion) std::this_thread::yield();
        }
        check(new_completion && new_completion->actor
              && new_completion->actor->same_incarnation(new_actor));
        check(manager.ack_actor_completion(*new_completion));
        new_group.stop_workers();
        check(new_group.finalized(new_actor));
        new_actor.get_done().get();
        ::close(pipefd[0]); ::close(pipefd[1]);
    }
    {
        // Manager destruction is a terminal abandonment path: reactor threads
        // are joined first, then any unconsumed actor watch loans are released
        // exactly once because no completion can be delivered afterward.
        NativeActorGroup group(880004, 1);
        auto actor = group.adopt(880004, 11, 1, [] {});
        check(group.mark_ready(actor));
        int pipefd[2]{-1, -1};
        check(::pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
        {
            NativeEpollReactorManager manager(1);
            auto watch = manager.try_watch_actor(actor, pipefd[0], EPOLLIN);
            check(watch.has_value());
            check(actor.outstanding_resource_loans() == 1);
        }
        check(actor.outstanding_resource_loans() == 0);
        group.stop_workers();
        check(group.finalized(actor));
        actor.get_done().get();
        ::close(pipefd[0]); ::close(pipefd[1]);
    }
    {
        // STOP and actor-owned watch admission have one atomic winner at the
        // actor resource-loan seal. A pre-STOP admission may finish and must
        // drain; a post-STOP admission never reaches epoll.
        std::uint32_t watch_wins = 0;
        std::uint32_t stop_wins = 0;
        for (std::uint32_t round = 0; round < 1000; ++round) {
            const std::uint64_t runtime = 881000 + round;
            NativeActorGroup group(runtime, 1);
            auto actor = group.adopt(runtime, 1, 1, [] {});
            check(group.mark_ready(actor));
            NativeEpollReactorManager manager(1);
            int pipefd[2]{-1, -1};
            check(::pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
            std::atomic<int> ready{0};
            std::optional<NativeEpollReactorManager::ActorWatch> watch;
            std::thread watcher([&] {
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (ready.load(std::memory_order_acquire) != 2)
                    std::this_thread::yield();
                watch = manager.try_watch_actor(actor, pipefd[0], EPOLLIN);
            });
            std::thread stopper([&] {
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (ready.load(std::memory_order_acquire) != 2)
                    std::this_thread::yield();
                group.stop_workers();
            });
            watcher.join();
            stopper.join();

            if (watch) {
                ++watch_wins;
                check(actor.outstanding_resource_loans() == 1);
                check(manager.try_cancel_actor(*watch));
                std::optional<NativeEpollReactorManager::Completion> completion;
                for (int retry = 0; retry < 3000 && !completion; ++retry) {
                    completion = manager.try_pop();
                    if (!completion) std::this_thread::yield();
                }
                check(completion && completion->actor
                      && completion->actor->same_incarnation(actor));
                check(manager.ack_actor_completion(*completion));
            } else {
                ++stop_wins;
                check(actor.outstanding_resource_loans() == 0);
            }
            check(actor.outstanding_resource_loans() == 0);
            check(group.finalized(actor));
            actor.get_done().get();
            ::close(pipefd[0]); ::close(pipefd[1]);
        }
        check(watch_wins + stop_wins == 1000);
    }
    {
        // Delivered actor completions deliberately retain their manager claim
        // until scheduler handoff ACK. Exhaust that bounded claim table while
        // reactor slots are being reused, and prove the next admission fails
        // before epoll registration/ownership transfer. ACKing one claim makes
        // exactly one slot available again.
        constexpr std::size_t claims =
            NativeEpollReactorManager::maximum_reactors
            * NativeEpollReactor::capacity;
        NativeActorGroup group(880005, 1);
        auto actor = group.adopt(880005, 12, 1, [] {});
        check(group.mark_ready(actor));
        NativeEpollReactorManager manager(1);
        std::array<int, claims + 1> fds{};
        std::array<std::optional<NativeEpollReactorManager::Completion>, claims> held{};
        fds.fill(-1);

        const std::uint64_t one = 1;
        for (std::size_t i = 0; i < fds.size(); ++i) {
            fds[i] = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            check(fds[i] >= 0);
        }
        for (std::size_t i = 0; i < claims; ++i) {
            std::optional<NativeEpollReactorManager::ActorWatch> watch;
            for (int retry = 0; retry < 3000 && !watch; ++retry) {
                watch = manager.try_watch_actor(actor, fds[i], EPOLLIN);
                if (!watch) std::this_thread::yield();
            }
            check(watch.has_value());
            check(::write(fds[i], &one, sizeof(one)) == (ssize_t)sizeof(one));
            for (int retry = 0; retry < 3000 && !held[i]; ++retry) {
                held[i] = manager.try_pop();
                if (!held[i]) std::this_thread::yield();
            }
            check(held[i] && held[i]->actor
                  && held[i]->actor->same_incarnation(actor));
        }
        check(actor.outstanding_resource_loans() == claims);

        // No free manager claim: admission must be refused and its speculative
        // actor retain refunded, leaving the reactor completion queue clean.
        check(!manager.try_watch_actor(actor, fds[claims], EPOLLIN).has_value());
        check(actor.outstanding_resource_loans() == claims);
        check(!manager.try_pop().has_value());

        check(manager.ack_actor_completion(*held[0]));
        check(actor.outstanding_resource_loans() == claims - 1);
        auto recycled = manager.try_watch_actor(actor, fds[claims], EPOLLIN);
        check(recycled.has_value());
        check(actor.outstanding_resource_loans() == claims);
        check(manager.try_cancel_actor(*recycled));
        std::optional<NativeEpollReactorManager::Completion> recycled_done;
        for (int retry = 0; retry < 3000 && !recycled_done; ++retry) {
            recycled_done = manager.try_pop();
            if (!recycled_done) std::this_thread::yield();
        }
        check(recycled_done && recycled_done->actor
              && recycled_done->actor->same_incarnation(actor));
        check(manager.ack_actor_completion(*recycled_done));

        for (std::size_t i = 1; i < claims; ++i)
            check(manager.ack_actor_completion(*held[i]));
        check(actor.outstanding_resource_loans() == 0);
        group.stop_workers();
        check(group.finalized(actor));
        actor.get_done().get();
        for (int fd : fds) ::close(fd);
    }
    bool bad = false;
    try { NativeEpollReactorManager manager(5); }
    catch (const std::invalid_argument&) { bad = true; }
    check(bad);
    return 0;
}
