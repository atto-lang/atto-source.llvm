// Real two-actor native host execution. Each HungryCarrier owns its OS
// thread and handles one admitted message. This probes cross-actor overlap,
// NOT simultaneous receivers within one actor and NOT compiled .atto guests.
// Event ordering is captured online in a fixed-size, allocation-free array,
// then emitted after join; send_accept is observed after try_send returns.
#include "oreslang/runtime/HungryCarrier.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace oreslang::runtime;
using namespace std::chrono_literals;

enum class EventKind {
    spawn, accept, start, complete, close, retire,
};
struct TraceEvent {
    EventKind kind;
    unsigned slot;
};
struct Shared {
    std::array<TraceEvent, 12> history{};
    std::atomic<unsigned> next_seq{0};
    std::atomic<unsigned> entered{0};
    std::atomic<unsigned> finished{0};
    std::array<std::atomic<bool>, 2> admitted{};
    std::atomic<bool> concurrent{false};
    std::atomic<bool> malformed{false};

    void record(EventKind kind, unsigned slot) noexcept {
        const unsigned index = next_seq.fetch_add(1, std::memory_order_acq_rel);
        if (index >= history.size()) {
            malformed.store(true, std::memory_order_release);
            return;
        }
        history[index] = TraceEvent{kind, slot};
    }
};

struct Worker {
    Shared* shared;
    unsigned slot;
};

static ActorStep handle(void* ptr, ActorMessage msg, bool resumed,
                        std::int64_t) noexcept {
    auto& worker = *static_cast<Worker*>(ptr);
    auto& shared = *worker.shared;
    if (resumed || msg.id != worker.slot + 1 ||
        msg.input != static_cast<std::int64_t>(msg.id * 3)) {
        shared.malformed.store(true, std::memory_order_release);
        return {StepKind::done, 0, 0};
    }

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!shared.admitted[worker.slot].load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (!shared.admitted[worker.slot].load(std::memory_order_acquire)) {
        shared.malformed.store(true, std::memory_order_release);
        return {StepKind::done, 0, 0};
    }

    shared.record(EventKind::start, worker.slot);
    const unsigned now = shared.entered.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (now == 2)
        shared.concurrent.store(true, std::memory_order_release);
    // Neither callback may finish until BOTH independent native carrier
    // threads have entered. A single-threaded scheduler cannot fake the pass.
    while (shared.entered.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (shared.entered.load(std::memory_order_acquire) != 2) {
        shared.malformed.store(true, std::memory_order_release);
    }
    shared.record(EventKind::complete, worker.slot);
    shared.finished.fetch_add(1, std::memory_order_acq_rel);
    return {StepKind::done, 0, 0};
}

static std::int64_t broker(void*, std::int64_t input,
                           const std::atomic<bool>&) noexcept {
    return input;
}
static void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
static const char* actor(unsigned slot) {
    return slot == 0 ? "alpha" : "beta";
}
static const char* message(unsigned slot) {
    return slot == 0 ? "msg_alpha" : "msg_beta";
}
static const char* receiver(unsigned slot) {
    return slot == 0 ? "receiver_alpha" : "receiver_beta";
}

static void execute(bool emit_trace) {
    Shared shared;
    Worker a{&shared, 0}, b{&shared, 1};
    HungryCarrier first(&a, handle, nullptr, broker, {});
    HungryCarrier second(&b, handle, nullptr, broker, {});
    auto pa = first.producer_handle();
    auto pb = second.producer_handle();
    shared.record(EventKind::spawn, 0);
    shared.record(EventKind::spawn, 1);
    first.start();
    second.start();

    require(pa.try_send({1, 3}), "alpha mailbox refused admission");
    shared.record(EventKind::accept, 0);
    shared.admitted[0].store(true, std::memory_order_release);
    require(pb.try_send({2, 6}), "beta mailbox refused admission");
    shared.record(EventKind::accept, 1);
    shared.admitted[1].store(true, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (shared.finished.load(std::memory_order_acquire) < 2 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);

    // Join only after BOTH callbacks have exited, avoiding cancellation of
    // the second carrier by the first carrier's logical shutdown.
    require(shared.finished.load(std::memory_order_acquire) == 2,
            "native actors did not complete both receiver turns");
    first.join();
    second.join();
    for (unsigned slot = 0; slot < 2; ++slot) {
        shared.record(EventKind::close, slot);
        shared.record(EventKind::retire, slot);
    }
    require(shared.concurrent.load(std::memory_order_acquire),
            "no real overlapping carrier execution observed");
    require(!shared.malformed.load(std::memory_order_acquire),
            "invalid native actor execution");
    require(first.stats().accepted == 1 && first.stats().completed == 1 &&
            second.stats().accepted == 1 && second.stats().completed == 1,
            "actor mailbox accounting mismatch");
    require(shared.next_seq.load(std::memory_order_acquire) == 12,
            "missing or excessive native trace events");
    require(!pa.try_send({11, 33}) && !pb.try_send({12, 36}),
            "terminated carrier admitted a message");

    if (!emit_trace) {
        std::cout << "PASS real two-carrier host overlap with complete admission accounting\n";
        return;
    }
    std::cout << "{\"schema\":\"atto.actor-trace.v2\",\"complete\":true,"
                 "\"producer\":{\"backend\":\"llvm\",\"subject\":\"runtime_host\","
                 "\"emission\":\"reconstructed_after_quiescence\"},\"events\":[";
    for (unsigned i = 0; i < shared.history.size(); ++i) {
        const auto& e = shared.history[i];
        const char* name = actor(e.slot);
        if (i) std::cout << ",";
        std::cout << "{\"seq\":" << i << ",\"actor_id\":\"" << name
                  << "\",\"generation\":1,";
        switch (e.kind) {
            case EventKind::spawn:
                std::cout << "\"kind\":\"actor_spawn\",\"serial\":true";
                break;
            case EventKind::accept:
                std::cout << "\"kind\":\"send_accept\",\"message_id\":\""
                          << message(e.slot) << "\"";
                break;
            case EventKind::start:
                std::cout << "\"kind\":\"receive_start\",\"message_id\":\""
                          << message(e.slot) << "\",\"receiver_id\":\""
                          << receiver(e.slot) << "\"";
                break;
            case EventKind::complete:
                std::cout << "\"kind\":\"receive_complete\",\"message_id\":\""
                          << message(e.slot) << "\",\"receiver_id\":\""
                          << receiver(e.slot) << "\"";
                break;
            case EventKind::close:
                std::cout << "\"kind\":\"actor_close\"";
                break;
            case EventKind::retire:
                std::cout << "\"kind\":\"actor_retire\"";
                break;
        }
        std::cout << "}";
    }
    std::cout << "]}\n";
}

int main(int argc, char** argv) {
    try {
        if (argc > 2 ||
            (argc == 2 && std::string(argv[1]) != "--trace-v2"))
            throw std::invalid_argument("usage: atto-native-cross-actor-test [--trace-v2]");
        execute(argc == 2);
    } catch (const std::exception& e) {
        std::cerr << "FAIL real native cross-actor overlap: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
