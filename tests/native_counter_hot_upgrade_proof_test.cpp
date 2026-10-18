#include "oreslang/runtime/NativeCounterUpgradeProof.hpp"
#include <openssl/evp.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

using Proof = oreslang::runtime::NativeCounterUpgradeProof;
using Wire = oreslang::runtime::ActorCheckpointEnvelopeV1;
static Wire::Hash sha256(const std::uint8_t* bytes, std::size_t size) {
    Wire::Hash result{};
    unsigned int produced = 0;
    if (EVP_Digest(bytes, size, result.data(), &produced, EVP_sha256(), nullptr) != 1
            || produced != result.size()) throw std::runtime_error("SHA256 failed");
    return result;
}
static std::string hash(char c) { return std::string(64, c); }
static Proof::Image image(char code) {
    return {hash(code), hash('1'), hash('2'), hash('3')};
}
static void require(bool ok) {
    if (!ok) throw std::runtime_error("native counter upgrade invariant failed");
}
template <class F> static bool rejects(F&& f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}
static std::int64_t v1(std::int64_t current, std::int64_t delta) {
    return current + delta;
}
static std::int64_t v2(std::int64_t current, std::int64_t delta) {
    return current + (delta < 0 ? -delta : delta);
}
static std::int64_t identity(std::int64_t value) { return value; }
static std::int64_t reject_restore(std::int64_t) {
    throw std::runtime_error("candidate initialization failed");
}
static std::atomic<bool> entered{false};
static std::atomic<bool> release{false};
static std::int64_t blocked(std::int64_t current, std::int64_t delta) {
    entered.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    return current + delta;
}

int main() {
    const Wire::ActorId id = {
        0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x43, 0x21,
        0x81, 0x23, 0, 0, 0, 0, 0, 1};

    {
        Proof actor(42, id, image('a'), 1, v1, sha256);
        actor.deliver(41);
        require(actor.value() == 42);
        require(actor.epoch() == 1);
        auto wrong = image('b');
        wrong.capability_sha256 = hash('9');
        require(rejects([&] { (void) actor.upgrade(wrong, v2, identity); }));
        require(actor.image().code_sha256 == hash('a'));
        require(rejects([&] { (void) actor.upgrade(image('b'), v2, reject_restore); }));
        require(actor.image().code_sha256 == hash('a'));
        require(actor.value() == 42);
        require(actor.upgrade(image('b'), v2, identity));
        require(actor.epoch() == 2);
        require(actor.value() == 42);
        actor.deliver(1);
        require(actor.value() == 43);
        actor.deliver(-2);
        require(actor.value() == 45);
        require(rejects([&] { (void) actor.upgrade(image('b'), v1, identity); }));
    }

    {
        entered.store(false);
        release.store(false);
        Proof actor(43, id, image('a'), 1, blocked, sha256);
        std::exception_ptr thread_error;
        std::thread old_turn([&] {
            try { actor.deliver(41); } catch (...) { thread_error = std::current_exception(); }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!entered.load(std::memory_order_acquire)
                && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        try {
            require(entered.load());
            require(!actor.upgrade(image('b'), v2, identity));
            require(actor.epoch() == 1);
        } catch (...) {
            release.store(true, std::memory_order_release);
            old_turn.join();
            throw;
        }
        release.store(true, std::memory_order_release);
        old_turn.join();
        if (thread_error) std::rethrow_exception(thread_error);
        require(actor.value() == 42);
        require(actor.upgrade(image('b'), v2, identity));
        actor.deliver(1);
        require(actor.value() == 43);
    }
    return 0;
}
