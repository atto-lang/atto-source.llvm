// Linux M1: compiler-generated scalar .atto guest code executed *inside*
// the native shared carrier through an explicit host adapter. This is NOT
// native lowering of an AttoLang actor declaration or supervisor restart.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "oreslang/runtime/MultiActorCarrier.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <pthread.h>
#include <thread>

using namespace oreslang::runtime;
using namespace std::chrono_literals;

struct Evidence {
    using Guest = int (*)();
    Guest compiled_guest = nullptr;
    bool failing = false;
    std::atomic<int> invoked{0};
    std::atomic<int> cleanups{0};
    std::atomic<int> errors{0};
    std::atomic<std::uint64_t> worker_tag{0};
    std::atomic<std::size_t> observed_stack_bytes{0};
};

static std::uint64_t tag() noexcept {
    return static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

static MultiActorCarrier::Outcome slice(
        void* raw, MultiActorCarrier::Message message,
        MultiActorCarrier::Cause cause, std::uint64_t fuel) noexcept {
    auto& e = *static_cast<Evidence*>(raw);
    if (message.sequence != 1 ||
            cause != MultiActorCarrier::Cause::initial ||
            fuel != 32) e.errors.fetch_add(1);

    e.worker_tag.store(tag(), std::memory_order_release);
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) {
        e.errors.fetch_add(1);
    } else {
        std::size_t bytes = 0;
        if (pthread_attr_getstacksize(&attributes, &bytes) != 0)
            e.errors.fetch_add(1);
        else e.observed_stack_bytes.store(bytes);
        pthread_attr_destroy(&attributes);
    }
    e.invoked.fetch_add(1);
    if (!e.failing) return MultiActorCarrier::Outcome::complete;
    if (!e.compiled_guest) {
        e.errors.fetch_add(1);
        return static_cast<MultiActorCarrier::Outcome>(255);
    }
    // This entry is emitted by the REAL .atto LLVM compiler. The wrapper
    // reports the generated TLS budget flag as native process status 75.
    const int result = e.compiled_guest();
    if (result != 75) {
        e.errors.fetch_add(1);
        return static_cast<MultiActorCarrier::Outcome>(255);
    }
    return MultiActorCarrier::Outcome::stack_budget_exhausted;
}

static void cleanup(void* raw, MultiActorCarrier::Message message,
                    bool had_active) noexcept {
    auto& e = *static_cast<Evidence*>(raw);
    if (message.sequence != 1 || !had_active ||
            e.worker_tag.load(std::memory_order_acquire) != tag())
        e.errors.fetch_add(1);
    e.cleanups.fetch_add(1);
}

template<class Predicate>
static bool until(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < end) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

#define CHECK(check) do { if (!(check)) { \
    std::fprintf(stderr, "atto LLVM carrier proof failure at line %d: %s\n", \
                 __LINE__, #check); return 3; \
} } while (false)

int main(int argc, char** argv) {
    CHECK(argc == 2 && argv[1]);
    void* image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!image) {
        std::fprintf(stderr, "atto LLVM scalar guest dlopen failed\n");
        return 2;
    }
    void* raw = dlsym(image, "main");
    Evidence::Guest guest = nullptr;
    static_assert(sizeof(raw) == sizeof(guest), "Linux function-pointer ABI unsupported");
    std::memcpy(&guest, &raw, sizeof(guest));
    CHECK(guest != nullptr);

    Evidence bad;
    bad.failing = true;
    bad.compiled_guest = guest;
    Evidence healthy;
    {
        MultiActorCarrier carrier(32);
        const auto failing = carrier.register_actor(&bad, slice, cleanup);
        const auto sibling = carrier.register_actor(&healthy, slice, cleanup);
        const auto sender = carrier.producer();
        carrier.start();

        CHECK(sender.try_send(failing, {1, 0}));
        CHECK(until([&] { return carrier.actor_stats(failing).retired; }));
        const auto failure = carrier.actor_stats(failing);
        CHECK(failure.stack_budget_exhausted);
        CHECK(failure.completed == 0);
        CHECK(!failure.active);
        CHECK(bad.invoked.load() == 1);
        CHECK(bad.cleanups.load() == 1);
        CHECK(bad.errors.load() == 0);
        CHECK(!sender.try_send(failing, {2, 0}));
        CHECK(!carrier.actor_stats(
            {failing.slot, failing.generation + 1}).stack_budget_exhausted);

        // An independent actor is admitted only after A's worker-side cleanup.
        CHECK(sender.try_send(sibling, {1, 0}));
        CHECK(until([&] { return carrier.actor_stats(sibling).completed == 1; }));
        CHECK(!carrier.actor_stats(sibling).stack_budget_exhausted);
        CHECK(healthy.invoked.load() == 1);
        CHECK(healthy.errors.load() == 0);
        CHECK(bad.worker_tag.load() != 0);
        CHECK(healthy.worker_tag.load() == bad.worker_tag.load());
        CHECK(carrier.stats().worker_thread_tag == bad.worker_tag.load());
        CHECK(carrier.stats().stack_budget_exhaustions == 1);
        CHECK(carrier.stats().protocol_failures == 0);

        // This proof must actually use a small physical carrier stack.
        CHECK(bad.observed_stack_bytes.load() >= 128 * 1024);
        CHECK(bad.observed_stack_bytes.load() <= 2 * 1024 * 1024);
        CHECK(healthy.observed_stack_bytes.load() ==
              bad.observed_stack_bytes.load());
        carrier.join();
        CHECK(carrier.stopped());
    }
    CHECK(dlclose(image) == 0);
    std::puts("ATTO_SOURCE_COMPILED_GUEST_ON_SHARED_NATIVE_CARRIER_OK");
    return 0;
}
