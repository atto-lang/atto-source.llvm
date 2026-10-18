#include "oreslang/runtime/NativeActorCodeCheckpointCoordinator.hpp"
#include <openssl/evp.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace oreslang::runtime;
using Coordinator = NativeActorCodeCheckpointCoordinator;
using Wire = Coordinator::Wire;

static void require(bool truth, const char* message) {
    if (!truth) throw std::runtime_error(message);
}
template<class F> static bool fails(F&& f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}
static Wire::Hash sha(const std::uint8_t* data, std::size_t length) {
    Wire::Hash bytes{};
    unsigned int count = 0;
    if (EVP_Digest(data, length, bytes.data(), &count, EVP_sha256(), nullptr) != 1
        || count != bytes.size()) throw std::runtime_error("test SHA-256 failed");
    return bytes;
}
static Wire::ActorId actor_id() {
    return Wire::ActorId{0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0x4a, 0xbb,
                         0x8c, 0xdd, 1, 2, 3, 4, 5, 6};
}
static Coordinator::Shape shape() {
    return {Coordinator::Gate::Kind::trusted_private,
            "message-v1", "actor-state-v1", "shared-cell-v1", "caps-v1"};
}
static Wire::Header manifest() {
    return {actor_id(), 9, Wire::Digest{}, Wire::Digest{},
            Wire::Digest{}, Wire::Digest{}};
}
static Coordinator::Image version(std::uint8_t digest,
                                   Coordinator::Behavior fn) {
    auto header = manifest();
    header.code_digest.fill(digest);
    header.state_schema_digest.fill(1);
    header.message_abi_digest.fill(2);
    header.capability_digest.fill(3);
    return {fn, header};
}
static std::int64_t v1(std::int64_t n, std::int64_t delta) {
    return n + delta;
}
static std::int64_t v2(std::int64_t n, std::int64_t delta) {
    return n + (delta < 0 ? -delta : delta);
}
static std::int64_t identity(std::int64_t n) { return n; }
static std::int64_t invalid_restore(std::int64_t n) { return n + 1; }
static std::int64_t throws_restore(std::int64_t) {
    throw std::runtime_error("injected restore failure");
}
static void transaction_and_rollback() {
    const ActorHeapIdentity id{81, 9, 1};
    std::atomic<int> old{0}, rejected{0}, accepted{0};
    Coordinator actor(id, actor_id(), shape(), version(0xaa, v1), 1,
                      &sha, [&] { ++old; });
    actor.deliver(41);
    require(actor.value() == 42, "initial compiled v1 failed");
    auto initial = actor.manifest();
    auto frame = actor.snapshot_frame();
    const auto verified = Wire::decode(frame, initial, &sha);
    require(verified.state_bytes.size() == 8, "checkpoint bytes missing");
    require(fails([&] {
        (void)Wire::decode(frame, version(0xbb, v2).manifest, &sha);
    }), "foreign image checkpoint accepted");
    require(fails([&] {
        auto invalid = version(0xbb, v2);
        invalid.manifest.capability_digest[0] = 4;
        (void)actor.try_upgrade(1, invalid, identity, [&] { ++rejected; });
    }), "capability escalation accepted");
    require(fails([&] {
        (void)actor.try_upgrade(1, version(0xbb, v2), invalid_restore,
                                [&] { ++rejected; });
    }), "same-schema restore changed state");
    require(fails([&] {
        (void)actor.try_upgrade(1, version(0xbb, v2), throws_restore,
                                [&] { ++rejected; });
    }), "restore exception did not fail rollback");
    require(rejected == 3 && actor.current().revision == 1
            && actor.value() == 42 && old == 0,
            "rollback changed active code, state, or reclamation");
    require(actor.try_upgrade(1, version(0xbb, v2), identity,
                              [&] { ++accepted; }),
            "compatible replacement was refused");
    require(actor.current().revision == 2 && old == 1,
            "old image failed to retire after safe upgrade");
    actor.deliver(-3);
    require(actor.value() == 45, "compiled v2 behavior not selected");
    require(fails([&] {
        (void)actor.try_upgrade(1, version(0xcc, v1), identity,
                                [&] { ++rejected; });
    }), "stale code revision accepted");
    actor.close();
    require(accepted == 1 && rejected == 4,
            "candidate ownership reclaimed wrong number of times");
}
static std::atomic<bool> entered{false}, released{false};
static std::int64_t blocking_v1(std::int64_t n, std::int64_t delta) {
    entered.store(true, std::memory_order_release);
    while (!released.load(std::memory_order_acquire))
        std::this_thread::yield();
    return n + delta;
}
static void inflight_turn_cannot_be_replaced() {
    entered.store(false);
    released.store(false);
    const ActorHeapIdentity id{82, 9, 1};
    std::atomic<int> old{0}, dropped{0}, next{0};
    Coordinator actor(id, actor_id(), shape(), version(0xaa, blocking_v1),
                      1, &sha, [&] { ++old; });
    std::exception_ptr thread_error;
    std::thread turn([&] {
        try { actor.deliver(41); }
        catch (...) { thread_error = std::current_exception(); }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    try {
        require(entered.load(), "blocked turn did not enter compiled callback");
        require(!actor.try_upgrade(1, version(0xbb, v2), identity,
                                   [&] { ++dropped; }),
                "upgrade published over executing callback");
        require(actor.current().revision == 1 && dropped == 1 && old == 0,
                "busy rollback lost old code or candidate cleanup");
    } catch (...) {
        released.store(true, std::memory_order_release);
        turn.join();
        throw;
    }
    released.store(true, std::memory_order_release);
    turn.join();
    if (thread_error) std::rethrow_exception(thread_error);
    require(actor.value() == 42, "old turn failed to finish exactly once");
    require(actor.try_upgrade(1, version(0xbb, v2), identity,
                              [&] { ++next; }),
            "quiesced follow-up upgrade failed");
    actor.deliver(-5);
    require(actor.value() == 47, "new v2 behavior did not persist owned state");
    actor.close();
    require(old == 1 && next == 1, "code image cleanup lost ownership");
}
static void reclamation_closure_is_terminal() {
    const ActorHeapIdentity id{83, 9, 1};
    Coordinator* owner = nullptr;
    std::atomic<int> first{0}, second{0};
    Coordinator actor(id, actor_id(), shape(), version(0xaa, v1),
                      12, &sha, [&] {
                          ++first;
                          if (owner) owner->close();
                      });
    owner = &actor;
    require(actor.try_upgrade(1, version(0xbb, v2), identity,
                              [&] { ++second; }),
            "callback-triggered close blocked the publisher");
    require(fails([&] { actor.deliver(1); }),
            "terminal close reopened DATA admission");
    require(first == 1 && second == 1, "reentrant close lost cleanup callbacks");
}
int main() {
    transaction_and_rollback();
    inflight_turn_cannot_be_replaced();
    reclamation_closure_is_terminal();
}
