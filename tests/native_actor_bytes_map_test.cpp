#include "oreslang/runtime/NativeActorBytesMap.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::ActorHeapIdentity;
using oreslang::runtime::NativeActorBytesMap;

static void check(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

template<class F>
static auto nonparking_retry(F&& call) {
    for (unsigned iteration = 0; iteration < 1000000; ++iteration) {
        auto result = call();
        if (result != decltype(result)::busy) return result;
        std::this_thread::yield();
    }
    throw std::runtime_error("nonparking actor Bytes map admission starved");
}

int main() {
    const ActorHeapIdentity owner{71, 19, 4};
    const ActorHeapIdentity different_actor{71, 20, 4};
    const ActorHeapIdentity reincarnated{71, 19, 5};
    using Map = NativeActorBytesMap<4, 16, 32>;
    using Status = Map::Status;
    Map actor(owner);
    Map sibling(different_actor);
    Map recreated(owner);
    check(actor.domain() != recreated.domain() &&
          actor.domain() != sibling.domain(),
          "map domain identity reused across distinct actor heaps");

    const std::array<unsigned char, 6> binary_key{0, 0xff, 'a', 0, '\r', 0x80};
    const std::array<unsigned char, 8> binary_value{0xff, 0, 0, '\n', 0x81, 0, 0xf0, 1};
    check(actor.put(different_actor, binary_key.data(), binary_key.size(),
                    binary_value.data(), binary_value.size()) == Status::denied,
          "foreign actor modified private Map");
    check(actor.put(reincarnated, binary_key.data(), binary_key.size(),
                    binary_value.data(), binary_value.size()) == Status::denied,
          "reincarnated numeric actor ID reused old storage authority");
    check(actor.put(owner, binary_key.data(), binary_key.size(),
                    binary_value.data(), binary_value.size()) == Status::ok,
          "binary key/value insert failed");
    std::array<unsigned char, 32> bytes{};
    auto first = actor.get(owner, binary_key.data(), binary_key.size(),
                           bytes.data(), bytes.size());
    check(first.status == Status::ok && first.length == binary_value.size() &&
          std::memcmp(bytes.data(), binary_value.data(), binary_value.size()) == 0,
          "binary exact key/value round trip failed");

    std::array<unsigned char, 7> short_output{};
    short_output.fill(0xa5);
    auto too_small = actor.get(owner, binary_key.data(), binary_key.size(),
                               short_output.data(), short_output.size());
    check(too_small.status == Status::too_small &&
          too_small.length == binary_value.size() &&
          too_small.version == first.version,
          "bounded result must report required byte count");
    for (auto c : short_output)
        check(c == 0xa5, "short-output rejection partially changed caller memory");

    const unsigned char alternative_key[]{0, 'a'};
    check(actor.get(owner, alternative_key, sizeof alternative_key,
                    bytes.data(), bytes.size()).status == Status::absent,
          "Map collapsed two binary distinct keys");
    check(actor.get(reincarnated, binary_key.data(), binary_key.size(),
                    bytes.data(), bytes.size()).status == Status::denied,
          "old storage leaked to same numeric actor in new incarnation");
    check(actor.get(owner, binary_key.data(), binary_key.size(),
                    nullptr, 1).status == Status::invalid,
          "nonzero output capacity without buffer admitted");
    check(actor.put(owner, binary_key.data(), binary_key.size(), nullptr, 1)
            == Status::invalid,
          "null payload with positive byte length admitted");
    check(actor.put(owner, binary_key.data(), binary_key.size(), binary_value.data(), 33)
            == Status::invalid,
          "oversize value admitted");
    check(actor.put(owner, binary_key.data(), 17, binary_value.data(),
                    binary_value.size()) == Status::invalid,
          "oversize key admitted");
    auto unaffected = actor.get(owner, binary_key.data(), binary_key.size(),
                                bytes.data(), bytes.size());
    check(unaffected.status == Status::ok &&
          unaffected.version == first.version &&
          std::memcmp(bytes.data(), binary_value.data(), binary_value.size()) == 0,
          "failed update modified a previous complete value");

    // Empty key and empty value are both legal; distinguish Some(empty) from
    // missing without borrowing an internal allocation or returning a pointer.
    check(actor.put(owner, nullptr, 0, nullptr, 0) == Status::ok,
          "empty key/value insert failed");
    auto empty = actor.get(owner, nullptr, 0, nullptr, 0);
    check(empty.status == Status::ok && empty.length == 0,
          "empty value was mistaken for absent");
    const char k2[] = "b";
    const char k3[] = "c";
    const char k4[] = "d";
    const char k5[] = "e";
    check(actor.put(owner, k2, 1, k2, 1) == Status::ok, "third slot insert");
    check(actor.put(owner, k3, 1, k3, 1) == Status::ok, "fourth slot insert");
    check(actor.put(owner, k4, 1, k4, 1) == Status::full,
          "bounded slot quota was bypassed");
    std::size_t count = 0;
    check(actor.count(owner, &count) == Status::ok && count == 4,
          "bounded Map live count mismatch");
    check(actor.erase(owner, binary_key.data(), binary_key.size()) == Status::ok,
          "binary entry erasure failed");
    check(actor.erase(owner, binary_key.data(), binary_key.size()) == Status::absent,
          "double erase was not rejected");
    check(actor.get(owner, binary_key.data(), binary_key.size(),
                    bytes.data(), bytes.size()).status == Status::absent,
          "erased bytes remain readable");
    check(actor.put(owner, k5, 1, k5, 1) == Status::ok,
          "reclaimed slot not reused");
    check(actor.count(owner, &count) == Status::ok && count == 4,
          "recycled slot incorrectly double-counted");

    // Overwriting a shorter byte sequence must scrub the previous tail and
    // mint a new version, not expose stale bytes through another lookup.
    check(actor.put(owner, k5, 1, binary_value.data(), binary_value.size())
            == Status::ok, "binary replacement failed");
    auto before = actor.get(owner, k5, 1, bytes.data(), bytes.size());
    check(actor.put(owner, k5, 1, k5, 1) == Status::ok,
          "short replacement failed");
    bytes.fill(0xa5);
    auto after = actor.get(owner, k5, 1, bytes.data(), bytes.size());
    check(after.status == Status::ok && after.length == 1 &&
          after.version > before.version && bytes[0] == 'e' &&
          bytes[1] == 0xa5, "replacement leaked previous payload tail");

    check(sibling.put(different_actor, binary_key.data(), binary_key.size(),
                      binary_value.data(), binary_value.size()) == Status::ok,
          "peer actor allocation failed");
    check(sibling.get(owner, binary_key.data(), binary_key.size(),
                      bytes.data(), bytes.size()).status == Status::denied,
          "actor read across isolated Map owners");
    check(recreated.get(owner, binary_key.data(), binary_key.size(),
                        bytes.data(), bytes.size()).status == Status::absent,
          "two physical Maps with the same actor identity unexpectedly shared bytes");
    check(actor.try_retire(reincarnated) == Status::denied,
          "foreign incarnation retired live actor Map");
    check(actor.try_retire(owner) == Status::ok &&
          actor.try_retire(owner) == Status::ok,
          "actor retirement must scrub and be idempotent");
    check(actor.put(owner, k5, 1, k5, 1) == Status::retired &&
          actor.get(owner, k5, 1, bytes.data(), bytes.size()).status == Status::retired,
          "retired actor Map admitted a stale operation");
    check(sibling.get(different_actor, binary_key.data(), binary_key.size(),
                      bytes.data(), bytes.size()).status == Status::ok,
          "retiring an actor wiped its live sibling");
    check(sibling.try_retire(different_actor) == Status::ok, "sibling retire");
    check(recreated.try_retire(owner) == Status::ok, "recreated map retire");

    // Trusted host threading smoke: no value may be torn and competing
    // carrier calls must return busy promptly, without parking on the lock.
    using Concurrent = NativeActorBytesMap<8, 16, 32>;
    using CS = Concurrent::Status;
    const ActorHeapIdentity concurrency_owner{29, 4, 9};
    Concurrent concurrent(concurrency_owner);
    const unsigned char shared_key[] = {0, 255, 0x50};
    std::array<unsigned char, 32> all_one{}, all_two{};
    all_one.fill(1); all_two.fill(2);
    check(concurrent.put(concurrency_owner, shared_key, sizeof shared_key,
                         all_one.data(), all_one.size()) == CS::ok,
          "concurrent map initialized");
    std::atomic<bool> start{false};
    std::atomic<bool> correct{true};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i < 1500; ++i) {
            const auto& v = (i & 1) ? all_one : all_two;
            if (nonparking_retry([&] {
                    return concurrent.put(concurrency_owner, shared_key,
                        sizeof shared_key, v.data(), v.size());
                }) != CS::ok)
                correct.store(false);
        }
    });
    std::vector<std::thread> readers;
    for (unsigned t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            std::array<unsigned char, 32> output{};
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned i = 0; i < 1500; ++i) {
                Concurrent::ReadResult value{CS::busy, 0, 0};
                for (unsigned attempts = 0; attempts < 1000000; ++attempts) {
                    value = concurrent.get(concurrency_owner, shared_key,
                                           sizeof shared_key, output.data(), output.size());
                    if (value.status != CS::busy) break;
                    std::this_thread::yield();
                }
                if (value.status != CS::ok || value.length != output.size())
                    correct.store(false);
                const auto expected = output[0];
                if (expected != 1 && expected != 2) correct.store(false);
                for (auto c : output)
                    if (c != expected) correct.store(false);
            }
        });
    }
    start.store(true, std::memory_order_release);
    writer.join();
    for (auto& t : readers) t.join();
    check(correct.load(), "partial/torn read or lost concurrent admission");
    check(concurrent.try_retire(concurrency_owner) == CS::ok,
          "concurrent Map could not retire after carrier quiescence");
    std::cout << "native actor Bytes map host substrate passed\n";
}
