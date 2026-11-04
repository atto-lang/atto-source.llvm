// Linux M1: two actual .atto compiler-generated scalar guests on a
// MultiActorCarrier, via a C++ host adapter. Actor DECLARATION lowering,
// supervisor restart and native SIGSEGV recovery remain out of scope.
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
    using Entry = int (*)();
    Entry compiled_guest = nullptr;
    Entry budget_status = nullptr;
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
    if (!e.compiled_guest || !e.budget_status) {
        e.errors.fetch_add(1);
        return static_cast<MultiActorCarrier::Outcome>(255);
    }

    // The actual compiler-generated guest returns the native OS status.
    // A normal AttoLang function may ALSO return 75, therefore only the
    // separately exported TLS failure flag is a valid resource diagnosis.
    const int result = e.compiled_guest();
    const int exhausted = e.budget_status();
    if (result != 75 || exhausted != (e.failing ? 1 : 0)) {
        e.errors.fetch_add(1);
        return static_cast<MultiActorCarrier::Outcome>(255);
    }
    return e.failing ? MultiActorCarrier::Outcome::stack_budget_exhausted
                     : MultiActorCarrier::Outcome::complete;
}

static void cleanup(void* raw, MultiActorCarrier::Message message,
                    bool had_active) noexcept {
    auto& e = *static_cast<Evidence*>(raw);
    if (message.sequence != 1 ||
            e.worker_tag.load(std::memory_order_acquire) != tag() ||
            had_active != e.failing)
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
    std::fprintf(stderr, "atto LLVM compiled carrier failure at line %d: %s\n", \
                 __LINE__, #check); return 3; \
} } while (false)

template<class FunctionPointer>
static FunctionPointer resolve(void* image, const char* symbol) {
    void* raw = dlsym(image, symbol);
    FunctionPointer fn = nullptr;
    static_assert(sizeof(raw) == sizeof(fn), "Linux function pointer ABI unsupported");
    std::memcpy(&fn, &raw, sizeof(fn));
    return fn;
}

int main(int argc, char** argv) {
    CHECK(argc == 3 && argv[1] && argv[2]);
    void* failed_image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!failed_image) {
        std::fprintf(stderr, "atto compiled failing guest dlopen failed\n");
        return 2;
    }
    void* healthy_image = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    if (!healthy_image) {
        std::fprintf(stderr, "atto compiled normal guest dlopen failed\n");
        return 2;
    }

    Evidence failing;
    failing.failing = true;
    failing.compiled_guest = resolve<Evidence::Entry>(failed_image, "main");
    failing.budget_status = resolve<Evidence::Entry>(
        failed_image, "__atto_guest_frame_exhausted_status");
    Evidence healthy;
    healthy.compiled_guest = resolve<Evidence::Entry>(healthy_image, "main");
    healthy.budget_status = resolve<Evidence::Entry>(
        healthy_image, "__atto_guest_frame_exhausted_status");
    CHECK(failing.compiled_guest && failing.budget_status);
    CHECK(healthy.compiled_guest && healthy.budget_status);
    {
        MultiActorCarrier carrier(32);
        const auto a = carrier.register_actor(&failing, slice, cleanup);
        const auto b = carrier.register_actor(&healthy, slice, cleanup);
        const auto sender = carrier.producer();
        carrier.start();

        CHECK(sender.try_send(a, {1, 0}));
        CHECK(until([&] { return carrier.actor_stats(a).retired; }));
        const auto status_a = carrier.actor_stats(a);
        CHECK(status_a.stack_budget_exhausted && status_a.retired);
        CHECK(status_a.completed == 0 && !status_a.active);
        CHECK(failing.invoked.load() == 1);
        CHECK(failing.cleanups.load() == 1 && failing.errors.load() == 0);
        CHECK(!sender.try_send(a, {2, 0}));
        CHECK(!carrier.actor_stats({a.slot, a.generation + 1}).stack_budget_exhausted);

        // B's generated guest intentionally returns the SAME number 75 but
        // its typed TLS status is zero. It must not become a failed actor.
        CHECK(sender.try_send(b, {1, 0}));
        CHECK(until([&] { return carrier.actor_stats(b).completed == 1; }));
        const auto status_b = carrier.actor_stats(b);
        CHECK(status_b.completed == 1 && !status_b.stack_budget_exhausted);
        CHECK(healthy.invoked.load() == 1 && healthy.errors.load() == 0);
        CHECK(failing.worker_tag.load() != 0);
        CHECK(healthy.worker_tag.load() == failing.worker_tag.load());
        CHECK(carrier.stats().worker_thread_tag == failing.worker_tag.load());
        CHECK(carrier.stats().stack_budget_exhaustions == 1);
        CHECK(carrier.stats().protocol_failures == 0);
        CHECK(failing.observed_stack_bytes.load() >= 128 * 1024);
        CHECK(failing.observed_stack_bytes.load() <= 2 * 1024 * 1024);
        CHECK(healthy.observed_stack_bytes.load() ==
              failing.observed_stack_bytes.load());

        carrier.join();
        CHECK(carrier.stopped());
        CHECK(failing.cleanups.load() == 1);
        CHECK(healthy.cleanups.load() == 1);
        CHECK(failing.errors.load() == 0 && healthy.errors.load() == 0);
        CHECK(carrier.actor_stats(a).stack_budget_exhausted);
    }
    CHECK(dlclose(healthy_image) == 0);
    CHECK(dlclose(failed_image) == 0);
    std::puts("ATTO_TWO_COMPILED_GUESTS_SHARED_NATIVE_CARRIER_OK");
    return 0;
}
