#ifndef ORESLANG_NATIVE_TEST_REGISTRY_DOMAIN_EXHAUSTION
#error This isolated test requires ORESLANG_NATIVE_TEST_REGISTRY_DOMAIN_EXHAUSTION
#endif
#include "oreslang/runtime/ActorResourceRegistry.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace oreslang::runtime {
struct ActorResourceRegistryDomainTestAccess {
    static void seed(std::uint64_t next) {
        ActorResourceRegistry::next_registry_domain_.store(next, std::memory_order_relaxed);
    }
    static std::uint64_t next() {
        return ActorResourceRegistry::next_registry_domain_.load(std::memory_order_relaxed);
    }
    static std::uint64_t domain(const ActorResourceRegistry& registry) {
        return registry.registry_domain_;
    }
};
}
using Registry = oreslang::runtime::ActorResourceRegistry;
using Access = oreslang::runtime::ActorResourceRegistryDomainTestAccess;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        constexpr unsigned workers = 12, attempts = 128, remaining = 8;
        constexpr auto terminal = std::numeric_limits<std::uint64_t>::max();
        // A fresh isolated executable owns the counter. Never run a seed
        // hook in a runtime that may retain tickets from previous domains.
        Access::seed(terminal - remaining);
        std::array<std::vector<std::unique_ptr<Registry>>, workers> issued;
        std::array<std::exception_ptr, workers> errors{};
        std::array<unsigned, workers> exhausted{};
        std::array<std::thread, workers> threads;
        std::atomic<unsigned> ready{0};
        std::atomic<bool> start{false};
        for (unsigned worker = 0; worker < workers; ++worker) {
            threads[worker] = std::thread([&, worker] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                try {
                    issued[worker].reserve(remaining);
                    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
                        try { issued[worker].push_back(std::make_unique<Registry>(41, 7)); }
                        catch (const std::overflow_error&) { ++exhausted[worker]; }
                    }
                } catch (...) { errors[worker] = std::current_exception(); }
            });
        }
        while (ready.load(std::memory_order_acquire) != workers) std::this_thread::yield();
        start.store(true, std::memory_order_release);
        for (auto& thread : threads) thread.join();
        for (const auto& error : errors) if (error) std::rethrow_exception(error);
        std::vector<std::uint64_t> domains;
        unsigned denied = 0;
        for (unsigned worker = 0; worker < workers; ++worker) {
            denied += exhausted[worker];
            for (const auto& registry : issued[worker]) domains.push_back(Access::domain(*registry));
        }
        require(domains.size() == remaining, "wrong number of admitted registries");
        require(denied == workers * attempts - remaining, "wrong exhausted-constructor count");
        std::sort(domains.begin(), domains.end());
        for (unsigned i = 0; i < remaining; ++i)
            require(domains[i] == terminal - remaining + i, "duplicate, missing or terminal domain");
        require(Access::next() == terminal, "constructor counter wrapped");
        // Last admitted registries must still issue and drain real tickets
        // even after all subsequent constructions are permanently denied.
        for (auto& batch : issued) for (auto& registry : batch) {
            const auto ticket = registry->acquire_callback();
            const auto handoff = registry->acquire_handoff();
            registry->retire();
            require(registry->try_reclaim().status == Registry::Reclaim::busy, "lost terminal registry pin");
            registry->release_callback(ticket);
            require(registry->try_reclaim().status == Registry::Reclaim::busy, "lost terminal handoff pin");
            registry->release_handoff(handoff);
            require(registry->try_reclaim().status == Registry::Reclaim::complete, "terminal registry failed to drain");
        }
        for (unsigned i = 0; i < 1024; ++i) {
            bool rejected = false;
            try { Registry extra(41, 7); }
            catch (const std::overflow_error&) { rejected = true; }
            require(rejected && Access::next() == terminal, "exhausted constructor recycled identity");
        }
        for (auto& batch : issued) batch.clear();
        // Zero is also a fail-closed state. All prior registries and tickets
        // are gone; this second injection is confined to this executable.
        Access::seed(0);
        bool rejected = false;
        try { Registry invalid(41, 7); }
        catch (const std::overflow_error&) { rejected = true; }
        require(rejected && Access::next() == 0, "zero domain was issued or advanced");
        std::puts("PASS registry constructors: 12-thread terminal exhaustion, 8 unique admissions, 1528 denials, 1024 exhausted retries and zero rejection");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL registry exhaustion: %s\n", error.what());
        return 1;
    }
}
