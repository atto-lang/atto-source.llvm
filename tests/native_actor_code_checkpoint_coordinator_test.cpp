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
static void initial_image_admission_fail_closed() {
    const ActorHeapIdentity id{84, 9, 1};
    std::atomic<int> cleanup{0};
    require(fails([&] {
        Coordinator actor(id, actor_id(), shape(), version(0xaa, nullptr),
                          0, &sha, [&] { ++cleanup; });
    }), "null initial code was accepted");
    require(cleanup == 1, "null initial image was not reclaimed exactly once");
    require(fails([&] {
        auto invalid = version(0xaa, v1);
        invalid.manifest.actor_id[0] ^= 0x01;
        Coordinator actor(id, actor_id(), shape(), invalid,
                          0, &sha, [&] { ++cleanup; });
    }), "foreign actor checkpoint metadata was accepted");
    require(cleanup == 2, "foreign image leaked initial cleanup");
    require(fails([&] {
        auto invalid = version(0xaa, v1);
        invalid.manifest.incarnation = 0;
        Coordinator actor(id, actor_id(), shape(), invalid,
                          0, &sha, [&] { ++cleanup; });
    }), "zero checkpoint incarnation was accepted");
    require(cleanup == 3, "invalid initial checkpoint leaked cleanup");
    require(fails([&] {
        Coordinator actor(id, actor_id(), shape(), version(0xaa, v1),
                          0, nullptr, [&] { ++cleanup; });
    }), "null digest provider was accepted");
    require(cleanup == 4, "null hash provider leaked loaded image");
}

static Coordinator* restoring_actor = nullptr;
static std::atomic<int> denied_during_restore{0};
static std::int64_t restoration_probe(std::int64_t value) {
    if (restoring_actor) {
        try {
            restoring_actor->deliver(7);
            throw std::runtime_error("checkpoint restore admitted a DATA turn");
        } catch (const std::logic_error&) {
            ++denied_during_restore;
        }
    }
    return value;
}
static void restore_callback_does_not_admit_data() {
    const ActorHeapIdentity id{85, 9, 1};
    std::atomic<int> old{0}, next{0};
    Coordinator actor(id, actor_id(), shape(), version(0xaa, v1),
                      33, &sha, [&] { ++old; });
    restoring_actor = &actor;
    denied_during_restore.store(0);
    require(actor.try_upgrade(1, version(0xbb, v2), restoration_probe,
                              [&] { ++next; }),
            "compatible upgrade rejected after callback admission denial");
    restoring_actor = nullptr;
    require(denied_during_restore == 1 && actor.value() == 33
            && actor.current().revision == 2,
            "checkpoint restore admitted concurrent DATA or changed actor state");
    actor.close();
    require(old == 1 && next == 1, "restore probe leaked a code generation");
}
static void suspended_continuation_pins_old_generation() {
    const ActorHeapIdentity id{86, 9, 1};
    std::atomic<int> old_cleanup{0}, denied_cleanup{0}, new_cleanup{0};
    Coordinator actor(id, actor_id(), shape(), version(0xaa, v1),
                      17, &sha, [&] { ++old_cleanup; });
    auto continuation = actor.suspend_code();
    require(continuation.descriptor().revision == 1
            && continuation.manifest().code_digest[0] == 0xaa,
            "continuation captured the wrong code generation");
    require(!actor.try_upgrade(1, version(0xbb, v2), identity,
                               [&] { ++denied_cleanup; }),
            "upgrade ignored a suspended frame's live code lease");
    require(denied_cleanup == 1 && old_cleanup == 0,
            "failed upgrade reclaimed pinned code");

    std::atomic<int64_t> result{0};
    std::thread worker([lease = std::move(continuation), &result]() mutable {
        result.store(lease.invoke_pinned(40, -5), std::memory_order_release);
        require(lease.descriptor().revision == 1, "carrier transfer changed revision");
        lease.release();
    });
    worker.join();
    require(result.load(std::memory_order_acquire) == 35,
            "transferred continuation executed wrong native generation");
    require(actor.try_upgrade(1, version(0xbb, v2), identity,
                              [&] { ++new_cleanup; }),
            "last suspended lease failed to unlock code publication");
    require(old_cleanup == 1 && actor.current().revision == 2,
            "last-lease reclamation or promotion failed");

    // A pinned continuation may outlive the terminal coordinator wrapper,
    // but must never access its actor-owned state after owner teardown.
    auto final_code = actor.suspend_code();
    actor.close();
    require(new_cleanup == 0 && final_code.invoke_pinned(40, -5) == 45,
            "terminal actor close unloaded live old native code");
    final_code.release();
    require(new_cleanup == 1, "last lease did not reclaim terminal image");
}
static void concurrent_checkpoint_snapshot_and_upgrade() {
    for (int round = 0; round < 128; ++round) {
        const ActorHeapIdentity id{87, static_cast<std::uint64_t>(round + 1), 1};
        std::atomic<int> prior_cleanup{0}, next_cleanup{0};
        Coordinator actor(id, actor_id(), shape(), version(0xaa, v1),
                          11, &sha, [&] { ++prior_cleanup; });
        std::atomic<bool> go{false}, finished{false};
        std::atomic<int> snapshots{0};
        std::exception_ptr thread_error;
        std::thread reader([&] {
            try {
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                while (!finished.load(std::memory_order_acquire)) {
                    try {
                        const auto frame = actor.snapshot_frame();
                        const auto code = frame[36];
                        const auto expected = version(code, v1).manifest;
                        const auto state = Wire::decode(frame, expected, &sha);
                        require(state.state_bytes.size() == 8, "snapshot lost scalar state");
                        require(code == 0xaa || code == 0xbb,
                                "snapshot contained an uncommitted code image");
                        ++snapshots;
                    } catch (const std::logic_error&) {
                        // The gate blocks new snapshot/pins during CONTROL freeze.
                    }
                }
            } catch (...) { thread_error = std::current_exception(); }
        });
        go.store(true, std::memory_order_release);
        bool updated = false;
        for (int attempt = 0; attempt < 500 && !updated; ++attempt) {
            try {
                updated = actor.try_upgrade(1, version(0xbb, v2), identity,
                                            [&] { ++next_cleanup; });
            } catch (const std::logic_error&) {
                // A committed promotion races current read, so treat a
                // distinct revision as success, not an unbounded retry.
                updated = actor.current().revision == 2;
            }
            if (!updated) std::this_thread::yield();
        }
        finished.store(true, std::memory_order_release);
        reader.join();
        if (thread_error) std::rethrow_exception(thread_error);
        require(updated && actor.current().revision == 2,
                "concurrent snapshot prevented publication");
        actor.close();
        require(prior_cleanup == 1 && next_cleanup >= 1,
                "concurrent checkpoint leaked native image cleanup");
    }
}
int main() {
    suspended_continuation_pins_old_generation();
    concurrent_checkpoint_snapshot_and_upgrade();
    initial_image_admission_fail_closed();
    restore_callback_does_not_admit_data();
    transaction_and_rollback();
    inflight_turn_cannot_be_replaced();
    reclamation_closure_is_terminal();
}
