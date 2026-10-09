#include "oreslang/runtime/HungryCarrier.hpp"
#include "oreslang/runtime/FixedArena.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using namespace oreslang::runtime;

static void check(bool value, const char* failure) {
    if (!value) throw std::runtime_error(failure);
}
template<class Pred>
static bool until(Pred pred, std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}
struct State {
    std::atomic<std::uint64_t> first_thread{0};
    std::atomic<int> initial{0}, resumed{0}, completed{0}, violations{0};
    std::atomic<bool> io_pending{false};
    bool await_io = false;
    std::uint32_t delay_ms = 0;
};
static ActorStep handle(void* raw, ActorMessage msg, bool resumed, std::int64_t result) noexcept {
    auto& st = *static_cast<State*>(raw);
    const auto tag = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::uint64_t expected = 0;
    (void)st.first_thread.compare_exchange_strong(expected, tag);
    if (st.first_thread.load() != tag) st.violations.fetch_add(1);
    if (!resumed) {
        if (st.io_pending.load(std::memory_order_acquire)) st.violations.fetch_add(1);
        st.initial.fetch_add(1);
        if (st.await_io) {
            st.io_pending.store(true, std::memory_order_release);
            return {StepKind::await_io, msg.input, st.delay_ms};
        }
    } else {
        if (!st.io_pending.exchange(false)) st.violations.fetch_add(1);
        if (result != msg.input * 2) st.violations.fetch_add(1);
        st.resumed.fetch_add(1);
    }
    st.completed.fetch_add(1);
    return {StepKind::done, 0, 0};
}
static std::int64_t io_double(void*, std::int64_t value) noexcept { return value * 2; }
static void test_150ms_await_serial_and_same_carrier() {
    State st; st.await_io = true; st.delay_ms = 150;
    HungryCarrier actor(&st, handle, nullptr, io_double, {});
    actor.start();
    check(actor.try_send({1, 21}), "first message should be accepted");
    check(until([&] { return st.initial.load() == 1; }, 1s),
          "initial handler not executed");
    // Queue another user message while the first turn is waiting for IO.
    check(actor.try_send({2, 22}), "second message should queue");
    std::this_thread::sleep_for(40ms);
    check(st.initial.load() == 1, "user handler reentered during pending IO");
    check(until([&] { return st.completed.load() == 2; }, 2s),
          "two messages did not finish");
    const auto stats = actor.stats();
    actor.request_stop();
    actor.join();
    check(st.resumed.load() == 2, "await continuation did not resume once each");
    check(st.violations.load() == 0, "single-carrier/serial or result violation");
    check(stats.io_completions == 2 && stats.parked > 0, "IO completion or parking absent");
    check(st.first_thread.load() == stats.actor_thread_tag, "wrong carrier thread tag");
    std::cout << "PASS 150ms async/serial dedicated carrier\n";
}
static void test_full_mailbox_does_not_block_stop() {
    State st; st.await_io = true; st.delay_ms = 300;
    HungryCarrier actor(&st, handle, nullptr, io_double, {});
    actor.start();
    check(actor.try_send({1, 4}), "failed initial IO send");
    check(until([&]{return st.initial.load() == 1;}, 1s), "IO did not start");
    int full = 0;
    for (int i = 0; i < 400; ++i)
        if (!actor.try_send({static_cast<std::uint64_t>(i + 2), i})) ++full;
    check(full > 0, "bounded queue did not reject excess work");
    const auto begin = std::chrono::steady_clock::now();
    actor.request_stop(); // reserved control independent of saturated queue
    actor.join();
    check(std::chrono::steady_clock::now() - begin < 250ms,
          "cancellable IO wait did not wake promptly");
    const auto stats = actor.stats();
    check(stats.canceled_inflight >= 1 && stats.protocol_failures == 0,
          "active operation not canceled cleanly");
    check(st.resumed.load() == 0, "canceled operation resumed user handler");
    std::cout << "PASS bounded mailbox + out-of-band stop/cancel\n";
}
static void test_mpsc_producers_serial_actor() {
    State st;
    HungryCarrier actor(&st, handle, nullptr, io_double, {});
    actor.start();
    constexpr int n_producers = 4, per_producer = 800;
    std::vector<std::thread> writers;
    std::atomic<int> rejections{0};
    for (int i = 0; i < n_producers; ++i) writers.emplace_back([&, i] {
        for (int j = 0; j < per_producer; ++j) {
            ActorMessage m{static_cast<std::uint64_t>(i * per_producer + j + 1), j};
            while (!actor.try_send(m)) {
                rejections.fetch_add(1);
                std::this_thread::yield();
            }
        }
    });
    for (auto& w : writers) w.join();
    check(until([&] {return st.completed.load() == n_producers * per_producer;}, 3s),
          "MPSC actor lost user messages");
    const auto stats = actor.stats();
    actor.request_stop();
    actor.join();
    check(stats.accepted == n_producers * per_producer &&
          stats.completed == n_producers * per_producer, "MPSC count mismatch");
    check(st.violations.load() == 0 && stats.protocol_failures == 0,
          "parallel producers caused serial-handler violation");
    std::cout << "PASS four producers, 3200 messages, rejections=" << rejections.load() << "\n";
}
static void test_cpu_affinity_fails_closed() {
    State st;
    HungryCarrier::Options options{};
    options.pin_to_logical_cpu = 1000000;
    HungryCarrier actor(&st, handle, nullptr, io_double, options);
    bool rejected = false;
    try {actor.start();} catch (const std::runtime_error&) {rejected = true;}
    check(rejected, "invalid CPU affinity must reject actor startup");
    actor.request_stop();
    actor.join();
    check(!actor.try_send({1, 1}), "failed admission must not accept messages");
}
static void test_fixed_arena_bounds_and_alignment() {
    FixedArena<128> arena;
    auto* number = arena.emplace<std::uint64_t>(42);
    check(number && *number == 42, "arena POD construction failed");
    check(reinterpret_cast<std::uintptr_t>(number) % alignof(std::uint64_t) == 0,
          "actor arena misaligned storage");
    check(arena.allocate(200, 8) == nullptr, "arena exceeded fixed capacity");
    check(arena.allocate(1, 3) == nullptr, "arena accepted invalid alignment");
    check(arena.used() == sizeof(std::uint64_t),
          "failed allocations changed arena accounting");
}
struct SocketState {int socket = -1;};
static std::int64_t recv_broker(void* context, std::int64_t) noexcept {
    auto& state = *static_cast<SocketState*>(context);
    pollfd descriptor{state.socket, POLLIN, 0};
    if (::poll(&descriptor, 1, 500) <= 0) return -1;
    std::int64_t value = 0;
    return ::recv(state.socket, &value, sizeof(value), MSG_WAITALL)
               == static_cast<ssize_t>(sizeof(value)) ? value : -2;
}
static void test_actual_socket_io_off_carrier() {
    int fds[2]{-1, -1};
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair failed");
    State st; st.await_io = true;
    SocketState broker{fds[0]};
    HungryCarrier actor(&st, handle, &broker, recv_broker, {});
    actor.start();
    check(actor.try_send({1, 42}), "socket IO enqueue failed");
    check(until([&]{return st.initial.load() == 1;}, 1s),
          "socket request handler did not start");
    std::thread remote([fd=fds[1]] {
        std::this_thread::sleep_for(150ms);
        const std::int64_t result = 84;
        (void)::send(fd, &result, sizeof(result), MSG_NOSIGNAL);
    });
    check(until([&]{return st.completed.load() == 1;}, 2s),
          "socket callback did not return into actor continuation");
    const auto s = actor.stats();
    actor.request_stop();
    actor.join();
    remote.join();
    ::close(fds[0]); ::close(fds[1]);
    check(st.resumed.load() == 1 && st.violations.load() == 0,
          "I/O worker failed to deliver single safe continuation");
    check(s.parked > 0 && s.io_completions == 1,
          "actor carrier did not park during real socket I/O");
    std::cout << "PASS bounded socket broker and parked carrier\n";
}

static void test_strict_realtime_profiles_fail_closed() {
    State st;
    for (int policy = 0; policy < 3; ++policy) {
        HungryCarrier::Options options{};
        options.require_exclusive_core = (policy == 0);
        options.require_strict_no_gc = (policy == 1);
        options.require_hard_realtime = (policy == 2);
        bool rejected = false;
        try {
            HungryCarrier actor(&st, handle, nullptr, io_double, options);
        } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "unimplemented strict profile silently admitted");
    }
}
static void test_stop_completion_races() {
    for (int i = 0; i < 60; ++i) {
        State st;
        st.await_io = true;
        st.delay_ms = static_cast<std::uint32_t>(i % 4);
        HungryCarrier actor(&st, handle, nullptr, io_double, {});
        actor.start();
        check(actor.try_send({1, 9}), "race test message failed");
        check(until([&] {return st.initial.load() == 1;}, 1s),
              "race test handler not invoked");
        if ((i & 3) == 1) std::this_thread::yield();
        else if ((i & 3) == 2) std::this_thread::sleep_for(1ms);
        actor.request_stop();
        actor.join();
        const auto stats = actor.stats();
        check(st.resumed.load() <= 1 && st.completed.load() <= 1,
              "completion/cancellation resumed a handler twice");
        check(st.violations.load() == 0 && stats.protocol_failures == 0,
              "completion/cancellation terminal race corrupted actor state");
        check(actor.stopped(), "stopped flag not published");
    }
    std::cout << "PASS 60 cancellation/completion interleavings\n";
}

int main() {
    try {
        test_fixed_arena_bounds_and_alignment();
        test_150ms_await_serial_and_same_carrier();
        test_actual_socket_io_off_carrier();
        test_full_mailbox_does_not_block_stop();
        for (int i = 0; i < 4; ++i) test_mpsc_producers_serial_actor();
        test_cpu_affinity_fails_closed();
        test_strict_realtime_profiles_fail_closed();
        test_stop_completion_races();
        std::cout << "PASS hungry carrier end-to-end tests\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL hungry carrier: " << error.what() << "\n";
        return EXIT_FAILURE;
    }
}
