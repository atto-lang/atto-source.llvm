// One OS process, many tenant VMs. Real cages, real signals, real concurrency.
// Run one case by name: tenant_vm_test <case>
#include "guests.h"
#include "oreslang/runtime/TenantVm.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <deque>
#include <sched.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace oreslang::runtime::vm;
using Bytes = std::vector<std::uint8_t>;

#if defined(TENANT_VM_TSAN_BUILD)
#define SPIN_OP FAULT_SPIN_IN_HOST_CALLS
#else
#define SPIN_OP FAULT_SPIN
#endif

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const auto va = (a); const auto vb = (b); if (!(va == vb)) { std::fprintf(stderr, "  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); ++failures; } } while (0)
#define CHECK_STATUS(actual, want) do { const Status va = (actual); if (va != (want)) { std::fprintf(stderr, "  FAIL %s:%d: got %s, want %s\n", __FILE__, __LINE__, to_string(va), to_string(want)); ++failures; } } while (0)
#define CHECK_REASON(actual, want) do { const FailReason va = (actual); if (va != (want)) { std::fprintf(stderr, "  FAIL %s:%d: got %s, want %s\n", __FILE__, __LINE__, to_string(va), to_string(want)); ++failures; } } while (0)
#define REQUIRE_OK(r) do { if ((r).status != Status::Ok) { std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, to_string((r).status)); ++failures; return; } } while (0)

using Ms = std::chrono::milliseconds;
long long ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<Ms>(std::chrono::steady_clock::now() - t).count();
}

VmSpec spec(const std::string& id) {
    VmSpec s;
    s.tenant_id = id;
    s.module = &ores_vm_test_module;
    s.limits.turn_deadline_ms = 2000;
    return s;
}
Bytes words(std::initializer_list<std::int64_t> values) {
    Bytes b(values.size() * 8);
    std::size_t i = 0;
    for (std::int64_t v : values) { std::memcpy(b.data() + i, &v, 8); i += 8; }
    return b;
}
std::int64_t word(const Bytes& b) {
    std::int64_t v = -999999;
    if (b.size() == 8) std::memcpy(&v, b.data(), 8);
    return v;
}
std::int64_t resident_kib() {
    FILE* f = popen(("ps -o rss= -p " + std::to_string(getpid())).c_str(), "r");
    long long kib = 0;
    if (f) { if (std::fscanf(f, "%lld", &kib) != 1) kib = 0; pclose(f); }
    return kib;
}

// ---------------------------------------------------------------------------

// The stated requirement: 1000 VMs in one OS process, each with its own actors and state.
void thousand_vms_in_one_process() {
    HostLimits limits;
    limits.carriers = 8;
    TenantVmHost host(limits);
    constexpr int kVms = 1000, kActorsPerVm = 100;
    const auto base_kib = resident_kib();
    std::vector<VmHandle> vms;
    std::vector<std::vector<VmActor>> actors(kVms);
    auto t = std::chrono::steady_clock::now();
    for (int i = 0; i < kVms; ++i) {
        auto v = host.open(spec("tenant-" + std::to_string(i)));
        REQUIRE_OK(v);
        vms.push_back(v.handle);
        actors[i].reserve(kActorsPerVm);
        for (int a = 0; a < kActorsPerVm; ++a) {
            auto spawned = host.spawn_actor(v.handle, KIND_COUNTER);
            REQUIRE_OK(spawned);
            actors[i].push_back(spawned.actor);
        }
    }
    const auto opened = ms_since(t);
    t = std::chrono::steady_clock::now();
    constexpr int kRounds = 3;
    for (int round = 1; round <= kRounds; ++round) {
        std::vector<std::future<TurnResult>> futures;
        futures.reserve(kVms * kActorsPerVm);
        for (int i = 0; i < kVms; ++i)
            for (int a = 0; a < kActorsPerVm; ++a)
                futures.push_back(host.call(actors[i][a], words({i * 1000 + a})));
        std::size_t n = 0;
        for (int i = 0; i < kVms; ++i)
            for (int a = 0; a < kActorsPerVm; ++a) {
                const auto r = futures[n++].get();
                CHECK_STATUS(r.status, Status::Ok);
                CHECK_EQ(word(r.payload), std::int64_t{i * 1000 + a} * round);
            }
    }
    const auto ran = ms_since(t);
    std::printf("  %d VMs, %d actors: open+spawn %lldms, %d turns %lldms, process pid %d, RSS +%.1f MiB, %.1f KiB/actor\n",
                kVms, kVms * kActorsPerVm, static_cast<long long>(opened), kVms * kActorsPerVm * kRounds,
                static_cast<long long>(ran), getpid(), (resident_kib() - base_kib) / 1024.0,
                static_cast<double>(resident_kib() - base_kib) / (kVms * kActorsPerVm));
    CHECK_EQ(host.stats().live_vms, std::size_t{kVms});
    // Over the limit is a typed refusal.
    CHECK_STATUS(host.open(spec("one-too-many")).status, Status::TenantQuota);
}

void equal_offsets_are_different_memory() {
    TenantVmHost host;
    auto a = host.open(spec("a"));
    auto b = host.open(spec("b"));
    REQUIRE_OK(a); REQUIRE_OK(b);
    auto actor_a = host.spawn_actor(a.handle, KIND_MEMORY);
    auto actor_b = host.spawn_actor(b.handle, KIND_MEMORY);
    REQUIRE_OK(actor_a); REQUIRE_OK(actor_b);
    const auto offset_a = word(host.call(actor_a.actor, words({MEM_STATE_OFFSET, 0, 0})).get().payload);
    const auto offset_b = word(host.call(actor_b.actor, words({MEM_STATE_OFFSET, 0, 0})).get().payload);
    CHECK_EQ(offset_a, offset_b); // same layout, same numbers

    constexpr std::int64_t kSecret = 0x5EC12E7;
    CHECK_STATUS(host.call(actor_a.actor, words({MEM_WRITE_STATE, kSecret, 0})).get().status, Status::Ok);
    CHECK_EQ(word(host.call(actor_a.actor, words({MEM_READ_STATE, 0, 0})).get().payload), kSecret);
    CHECK_EQ(word(host.call(actor_b.actor, words({MEM_READ_STATE, 0, 0})).get().payload), 0);
    CHECK_EQ(word(host.call(actor_b.actor, words({MEM_READ_OFFSET, offset_a, 0})).get().payload), 0);
}

// Actors inside one VM message each other. Nothing can name another tenant's actor.
void messaging_stays_inside_the_vm() {
    TenantVmHost host;
    auto a = host.open(spec("a"));
    auto b = host.open(spec("b"));
    REQUIRE_OK(a); REQUIRE_OK(b);
    auto relay_a = host.spawn_actor(a.handle, KIND_RELAY);
    auto counter_a = host.spawn_actor(a.handle, KIND_COUNTER);
    auto padding_b = host.spawn_actor(b.handle, KIND_RELAY);
    auto counter_b = host.spawn_actor(b.handle, KIND_COUNTER);
    REQUIRE_OK(relay_a); REQUIRE_OK(counter_a); REQUIRE_OK(padding_b); REQUIRE_OK(counter_b);
    CHECK_EQ(counter_a.actor.ref(), counter_b.actor.ref()); // identical refs, different VMs

    const auto delivered = host.call(relay_a.actor, words({static_cast<std::int64_t>(counter_a.actor.ref()), 41})).get();
    CHECK_EQ(word(delivered.payload), 1);
    bool arrived = false;
    for (int i = 0; i < 2000 && !arrived; ++i) {
        arrived = word(host.call(counter_a.actor, words({0})).get().payload) == 41;
        if (!arrived) std::this_thread::sleep_for(Ms(1));
    }
    CHECK(arrived);
    // That same ref, used in tenant A, reached A's counter and never B's.
    CHECK_EQ(word(host.call(counter_b.actor, words({0})).get().payload), 0);
    // Unknown refs are refused rather than delivered anywhere.
    CHECK_EQ(word(host.call(relay_a.actor, words({0x7777, 1})).get().payload), 0);
    CHECK_EQ(word(host.call(relay_a.actor, words({static_cast<std::int64_t>((std::uint64_t{5} << 32) | counter_a.actor.ref()), 1})).get().payload), 0);
}

struct FaultCase { const char* name; int kind; Bytes message; FailReason want; std::uint32_t deadline_ms; };

// Every fault ends exactly one VM. The carrier survives and the neighbour never notices.
void faults_end_only_that_vm() {
    HostLimits limits;
    limits.carriers = 2; // few, so a lost carrier would show up at once
    TenantVmHost host(limits);
    auto neighbour = host.open(spec("neighbour"));
    REQUIRE_OK(neighbour);
    auto innocent = host.spawn_actor(neighbour.handle, KIND_COUNTER);
    REQUIRE_OK(innocent);
    std::int64_t value = 0;

    const std::vector<FaultCase> cases = {
        {"null-offset", KIND_FAULT, words({FAULT_NULL_OFFSET}), FailReason::MemoryFault, 2000},
        {"past-committed-memory", KIND_MEMORY, words({MEM_READ_OFFSET, 0x70000000, 0}), FailReason::MemoryFault, 2000},
        {"write-past-committed", KIND_MEMORY, words({MEM_WRITE_OFFSET, 0x70000000, 1}), FailReason::MemoryFault, 2000},
        {"farthest-possible-access", KIND_MEMORY, words({MEM_FARTHEST_ACCESS, 0, 0}), FailReason::MemoryFault, 2000},
        {"stack-overflow", KIND_FAULT, words({FAULT_STACK}), FailReason::StackOverflow, 2000},
        {"stack-overflow-in-host-calls", KIND_FAULT, words({FAULT_STACK_IN_HOST_CALLS}), FailReason::StackOverflow, 20000},
        {"trap-instruction", KIND_FAULT, words({FAULT_TRAP}), FailReason::Trap, 2000},
#if !defined(TENANT_VM_TSAN_BUILD) // TSan defers async signals to its next interceptor, which a pure spin never reaches
        {"spin-no-checks", KIND_FAULT, words({FAULT_SPIN}), FailReason::TurnDeadline, 100},
#endif
        {"spin-in-host-calls", KIND_FAULT, words({FAULT_SPIN_IN_HOST_CALLS}), FailReason::TurnDeadline, 100},
    };
    // Twice each, and on few carriers: recovery must leave the thread fully reusable,
    // including the signal stack after an overflow.
    for (int round = 0; round < 3; ++round) {
        for (const FaultCase& c : cases) {
            auto s = spec(c.name);
            s.limits.turn_deadline_ms = c.deadline_ms;
            auto v = host.open(s);
            REQUIRE_OK(v);
            auto actor = host.spawn_actor(v.handle, static_cast<std::uint32_t>(c.kind));
            REQUIRE_OK(actor);
            const auto start = std::chrono::steady_clock::now();
            const auto result = host.call(actor.actor, c.message).get();
            CHECK_STATUS(result.status, Status::TenantFailed);
            const VmTerminal terminal = host.wait_closed(v.handle);
            if (round == 0) std::printf("  %-26s %-14s %4lldms\n", c.name, to_string(terminal.reason), static_cast<long long>(ms_since(start)));
            CHECK(terminal.phase == Phase::Failed);
            CHECK_REASON(terminal.reason, c.want);
            // Authority is gone, and the id is free again.
            CHECK_STATUS(host.call(actor.actor, c.message).get().status, Status::StaleHandle);
            CHECK_STATUS(host.spawn_actor(v.handle, 0).status, Status::StaleHandle);
            value += 1;
            CHECK_EQ(word(host.call(innocent.actor, words({1})).get().payload), value);
        }
    }
    // Arithmetic traps only on some architectures; either result must leave the host fine.
    auto d = host.open(spec("divide"));
    REQUIRE_OK(d);
    auto divider = host.spawn_actor(d.handle, KIND_FAULT);
    REQUIRE_OK(divider);
    const Status divided = host.call(divider.actor, words({FAULT_DIVIDE})).get().status;
    CHECK(divided == Status::Ok || divided == Status::TenantFailed);
    host.kill(d.handle);
    host.wait_closed(d.handle);
    value += 1;
    CHECK_EQ(word(host.call(innocent.actor, words({1})).get().payload), value);
    std::printf("  host stats: %llu traps contained, %llu carrier interrupts\n",
                static_cast<unsigned long long>(host.stats().traps_contained),
                static_cast<unsigned long long>(host.stats().kills_delivered));
}

// A fault in one actor takes down the other actors of that VM while they are mid-turn,
// and nothing outside it. Turns on other carriers get interrupted, not waited for.
void a_fault_stops_all_of_that_vm_running_in_parallel() {
    HostLimits limits;
    limits.carriers = 8;
    TenantVmHost host(limits);
    auto neighbour = host.open(spec("neighbour"));
    REQUIRE_OK(neighbour);
    auto innocent = host.spawn_actor(neighbour.handle, KIND_COUNTER);
    REQUIRE_OK(innocent);
    auto s = spec("doomed");
    s.limits.max_parallel_turns = 4;
    s.limits.turn_deadline_ms = 60000; // these spinners would outlive the test by themselves
    auto v = host.open(s);
    REQUIRE_OK(v);
    std::vector<std::future<TurnResult>> spinners;
    std::vector<VmActor> keep;
    for (int i = 0; i < 3; ++i) {
        auto a = host.spawn_actor(v.handle, KIND_FAULT);
        REQUIRE_OK(a);
        keep.push_back(a.actor);
        spinners.push_back(host.call(a.actor, words({SPIN_OP})));
    }
    std::this_thread::sleep_for(Ms(50));
    auto bomber = host.spawn_actor(v.handle, KIND_FAULT);
    REQUIRE_OK(bomber);
    const auto start = std::chrono::steady_clock::now();
    CHECK_STATUS(host.call(bomber.actor, words({FAULT_TRAP})).get().status, Status::TenantFailed);
    for (auto& f : spinners) CHECK_STATUS(f.get().status, Status::TenantFailed);
    CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::Trap);
    CHECK(ms_since(start) < 1000);
    CHECK_EQ(word(host.call(innocent.actor, words({7})).get().payload), 7);
}

void host_can_kill_a_spinning_vm() {
    TenantVmHost host;
    auto s = spec("spinner");
    s.limits.turn_deadline_ms = 60000;
    auto v = host.open(s);
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_FAULT);
    REQUIRE_OK(a);
    auto spinning = host.call(a.actor, words({SPIN_OP}));
    std::this_thread::sleep_for(Ms(30));
    const auto start = std::chrono::steady_clock::now();
    CHECK_STATUS(host.kill(v.handle), Status::Ok);
    CHECK_STATUS(spinning.get().status, Status::TenantFailed);
    const auto closed = host.wait_closed(v.handle);
    CHECK_REASON(closed.reason, FailReason::HostKill);
    std::printf("  spinning VM killed in %lldms\n", static_cast<long long>(ms_since(start)));
    CHECK(ms_since(start) < 500);
}

// Host calls run outside guest mode. A kill arriving during one must wait for its return,
// with the host's locks released, and must then still take effect.
void kills_during_host_calls_leave_locks_free() {
    HostLimits limits;
    limits.carriers = 4;
    TenantVmHost host(limits);
    for (int i = 0; i < 40; ++i) {
        auto s = spec("churn-" + std::to_string(i));
        s.limits.turn_deadline_ms = 60000;
        auto v = host.open(s);
        REQUIRE_OK(v);
        auto a = host.spawn_actor(v.handle, KIND_FAULT);
        REQUIRE_OK(a);
        auto spinning = host.call(a.actor, words({FAULT_SPIN_IN_HOST_CALLS}));
        std::this_thread::sleep_for(std::chrono::microseconds(200 * (i % 7)));
        host.kill(v.handle);
        CHECK_STATUS(spinning.get().status, Status::TenantFailed);
        host.wait_closed(v.handle);
    }
    // Locks were all released: the host still works.
    auto last = host.open(spec("after"));
    REQUIRE_OK(last);
    auto c = host.spawn_actor(last.handle, KIND_COUNTER);
    REQUIRE_OK(c);
    CHECK_EQ(word(host.call(c.actor, words({3})).get().payload), 3);
}

// Fairness is per tenant, not per actor: a noisy tenant cannot starve a quiet one.
void one_tenant_cannot_starve_another() {
    HostLimits limits;
    limits.carriers = 2;
    TenantVmHost host(limits);
    auto noisy_spec = spec("noisy");
    noisy_spec.limits.max_parallel_turns = 2;
    noisy_spec.limits.max_inflight = 1u << 20;
    noisy_spec.limits.turn_deadline_ms = 60000;
    auto noisy = host.open(noisy_spec);
    auto quiet = host.open(spec("quiet"));
    REQUIRE_OK(noisy); REQUIRE_OK(quiet);
    std::vector<VmActor> noise;
    for (int i = 0; i < 2000; ++i) {
        auto a = host.spawn_actor(noisy.handle, KIND_FAULT);
        REQUIRE_OK(a);
        noise.push_back(a.actor);
    }
    auto q = host.spawn_actor(quiet.handle, KIND_COUNTER);
    REQUIRE_OK(q);
    std::vector<std::future<TurnResult>> flood;
    for (auto& a : noise)
        for (int i = 0; i < 8; ++i) flood.push_back(host.call(a, words({FAULT_SLEEPLESS_BRIEFLY})));
    std::vector<long long> latency;
    for (int i = 0; i < 200; ++i) {
        const auto t = std::chrono::steady_clock::now();
        CHECK_STATUS(host.call(q.actor, words({1})).get().status, Status::Ok);
        latency.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count());
    }
    std::sort(latency.begin(), latency.end());
    const auto noisy_left = host.snapshot(noisy.handle).inflight;
    std::printf("  quiet tenant beside %zu queued noisy turns: p50 %lldus, worst %lldus\n", noisy_left,
                latency[latency.size() / 2], latency.back());
    CHECK(noisy_left > 1000); // the noise was still there for the whole measurement
#if defined(TENANT_VM_TIMING_ASSERTS)
    CHECK(latency[latency.size() / 2] < 100000); // bounded by turn length, not by queue length
#endif
    host.kill(noisy.handle);
    for (auto& f : flood) f.get();
    host.wait_closed(noisy.handle);
}

void memory_is_bounded_per_vm() {
    TenantVmHost host;
    auto s = spec("small");
    s.limits.heap_bytes = (7u << 20) / 2; // room for three 1 MiB actors, not four
    auto v = host.open(s);
    REQUIRE_OK(v);
    // Budget refusals are typed and leave the VM healthy.
    auto big = host.spawn_actor(v.handle, KIND_LARGE);
    auto big2 = host.spawn_actor(v.handle, KIND_LARGE);
    REQUIRE_OK(big); REQUIRE_OK(big2);
    CHECK_STATUS(host.spawn_actor(v.handle, KIND_LARGE).status, Status::Ok);
    CHECK_STATUS(host.spawn_actor(v.handle, KIND_LARGE).status, Status::HeapBudget);
    CHECK_EQ(word(host.call(big.actor, words({9})).get().payload), 9);
    CHECK(host.snapshot(v.handle).committed_bytes <= s.limits.heap_bytes + 65536);

    // The actor heap is bump-allocated within its own budget.
    auto heap_owner = host.open(spec("heap"));
    REQUIRE_OK(heap_owner);
    auto m = host.spawn_actor(heap_owner.handle, KIND_MEMORY);
    REQUIRE_OK(m);
    const auto first = word(host.call(m.actor, words({MEM_ALLOC, 100, 16})).get().payload);
    const auto second = word(host.call(m.actor, words({MEM_ALLOC, 100, 16})).get().payload);
    CHECK(first != 0 && second >= first + 100);
    CHECK_EQ(word(host.call(m.actor, words({MEM_ALLOC, 8192, 16})).get().payload), 0); // over 4 KiB
    CHECK_EQ(word(host.call(m.actor, words({MEM_ALLOC, 16, 3})).get().payload), 0);    // bad alignment
    const auto count = word(host.call(m.actor, words({MEM_ALLOC_UNTIL_REFUSED, 64, 0})).get().payload);
    CHECK(count > 0 && count < 64); // stopped by its budget

    // Actor-count and input limits.
    auto few_spec = spec("few");
    few_spec.limits.max_actors = 2;
    few_spec.limits.mailbox_capacity = 1;
    auto few = host.open(few_spec);
    REQUIRE_OK(few);
    auto one = host.spawn_actor(few.handle, KIND_COUNTER);
    REQUIRE_OK(one);
    CHECK_STATUS(host.spawn_actor(few.handle, KIND_COUNTER).status, Status::Ok);
    CHECK_STATUS(host.spawn_actor(few.handle, KIND_COUNTER).status, Status::ActorQuota);
    CHECK_STATUS(host.spawn_actor(few.handle, 99).status, Status::BadKind);
    CHECK_STATUS(host.call(one.actor, Bytes(1024)).get().status, Status::PayloadTooLarge);

    // Host-level admission covers what all VMs may commit.
    HostLimits tight;
    tight.max_admitted_heap_bytes = 100;
    TenantVmHost small_host(tight);
    auto huge = spec("huge");
    huge.limits.heap_bytes = 101;
    CHECK_STATUS(small_host.open(huge).status, Status::HeapQuota);
    huge.limits.heap_bytes = 100;
    auto fits = small_host.open(huge);
    REQUIRE_OK(fits);
    CHECK_STATUS(small_host.kill(fits.handle), Status::Ok);
    small_host.wait_closed(fits.handle);
    auto again = spec("huge-2");
    again.limits.heap_bytes = 100;
    CHECK_STATUS(small_host.open(again).status, Status::Ok); // charge returned at retirement
}

void cpu_budget_is_enforced() {
    TenantVmHost host;
    auto s = spec("cpu");
    s.limits.cpu_budget_ms = 20;
    auto v = host.open(s);
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_FAULT);
    REQUIRE_OK(a);
    for (int i = 0; i < 1000; ++i) {
        const Status status = host.call(a.actor, words({FAULT_SLEEPLESS_BRIEFLY})).get().status;
        if (status != Status::Ok) break;
    }
    CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::CpuBudget);
}

// Actor slots and memory come back, unlike the single-use slots of NativeActorScheduler.
void actor_churn_reuses_slots_and_scrubs_memory() {
    TenantVmHost host;
    auto s = spec("churn");
    s.limits.max_actors = 4;
    auto v = host.open(s);
    REQUIRE_OK(v);
    const auto start_committed = host.snapshot(v.handle).committed_bytes;
    VmActor previous;
    bool have_previous = false;
    for (int i = 0; i < 20000; ++i) {
        auto a = host.spawn_actor(v.handle, KIND_MEMORY);
        REQUIRE_OK(a);
        // A fresh actor must see zeroes, even when it reuses a previous actor's memory.
        CHECK_EQ(word(host.call(a.actor, words({MEM_READ_STATE, 0, 0})).get().payload), 0);
        CHECK_STATUS(host.call(a.actor, words({MEM_WRITE_STATE, 0x5EC12E7, 0})).get().status, Status::Ok);
        CHECK_STATUS(host.stop_actor(a.actor), Status::Ok);
        CHECK_STATUS(host.call(a.actor, words({MEM_READ_STATE, 0, 0})).get().status, Status::UnknownActor);
        if (have_previous) CHECK_STATUS(host.call(previous, words({MEM_READ_STATE, 0, 0})).get().status, Status::UnknownActor);
        previous = a.actor;
        have_previous = true;
    }
    const auto snapshot = host.snapshot(v.handle);
    CHECK_EQ(snapshot.live_actors, std::size_t{0});
    CHECK_EQ(snapshot.used_bytes, std::uint64_t{0});
    CHECK(snapshot.committed_bytes <= start_committed + (std::uint64_t{2} << 20)); // no growth from churn
}

// Native host-side proof of tenant memory-budget recycling, not compiled
// AttoLang guest zero-GC or untrusted code isolation. Exercise one tenant
// for >100 reincarnations while a second tenant stays alive.
void tenant_recycling_restores_heap_quota_and_fences_aliases() {
    HostLimits limits;
    limits.max_vms = 2;
    limits.max_admitted_heap_bytes = std::uint64_t{128} << 20;
    TenantVmHost host(limits);
    auto survivor = host.open(spec("healthy"));
    REQUIRE_OK(survivor);
    auto survivor_actor = host.spawn_actor(survivor.handle, KIND_MEMORY);
    REQUIRE_OK(survivor_actor);
    constexpr std::int64_t kSentinel = 0x5EC12E7;
    CHECK_STATUS(host.call(survivor_actor.actor, words({MEM_WRITE_STATE, kSentinel, 0})).get().status, Status::Ok);
    const auto retained_heap_charge = host.stats().admitted_heap_bytes;
    CHECK(retained_heap_charge != 0);
    std::uint64_t previous_generation = 0;
    VmActor stale_actor;
    bool have_stale = false;
    for (int round = 0; round < 128; ++round) {
        auto cycle = host.open(spec("churn"));
        REQUIRE_OK(cycle);
        CHECK(cycle.handle.generation() > previous_generation);
        previous_generation = cycle.handle.generation();
        auto current = host.spawn_actor(cycle.handle, KIND_MEMORY);
        REQUIRE_OK(current);
        CHECK_EQ(word(host.call(current.actor, words({MEM_READ_STATE, 0, 0})).get().payload), 0);
        CHECK_STATUS(host.call(current.actor, words({MEM_WRITE_STATE, kSentinel + round, 0})).get().status, Status::Ok);
        CHECK_EQ(word(host.call(current.actor, words({MEM_READ_STATE, 0, 0})).get().payload), kSentinel + round);
        if (have_stale)
            CHECK_STATUS(host.call(stale_actor, words({MEM_READ_STATE, 0, 0})).get().status, Status::StaleHandle);
        stale_actor = current.actor;
        have_stale = true;
        CHECK_STATUS(host.kill(cycle.handle), Status::Ok);
        host.wait_closed(cycle.handle); // physical unmap/retirement must precede quota return
        CHECK_STATUS(host.call(current.actor, words({MEM_READ_STATE, 0, 0})).get().status, Status::StaleHandle);
        CHECK_EQ(host.stats().live_vms, std::size_t{1});
        CHECK_EQ(host.stats().admitted_heap_bytes, retained_heap_charge);
        CHECK_EQ(word(host.call(survivor_actor.actor, words({MEM_READ_STATE, 0, 0})).get().payload), kSentinel);
    }
    CHECK_EQ(host.stats().live_vms, std::size_t{1});
    CHECK_STATUS(host.kill(survivor.handle), Status::Ok);
    host.wait_closed(survivor.handle);
    CHECK_EQ(host.stats().admitted_heap_bytes, std::uint64_t{0});
    CHECK_EQ(host.stats().live_vms, std::size_t{0});
}

void guests_can_spawn_within_their_vm() {
    TenantVmHost host;
    auto s = spec("spawning");
    s.limits.max_actors = 3;
    auto v = host.open(s);
    REQUIRE_OK(v);
    auto spawner = host.spawn_actor(v.handle, KIND_SPAWNER);
    REQUIRE_OK(spawner);
    const auto child = word(host.call(spawner.actor, words({KIND_COUNTER})).get().payload);
    CHECK(child != 0);
    CHECK(word(host.call(spawner.actor, words({KIND_COUNTER})).get().payload) != 0);
    CHECK_EQ(word(host.call(spawner.actor, words({KIND_COUNTER})).get().payload), 0); // quota
    CHECK_EQ(word(host.call(spawner.actor, words({99})).get().payload), 0);
    CHECK_EQ(host.snapshot(v.handle).live_actors, std::size_t{3});
}

void shutdown_drains_then_retires() {
    TenantVmHost host;
    auto v = host.open(spec("drain"));
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_COUNTER);
    auto b = host.spawn_actor(v.handle, KIND_COUNTER);
    REQUIRE_OK(a); REQUIRE_OK(b);
    std::vector<std::future<TurnResult>> accepted;
    for (int i = 0; i < 32; ++i) accepted.push_back(host.call(a.actor, words({1})));
    CHECK_STATUS(host.begin_shutdown(v.handle), Status::Ok);
    const Status refused = host.call(b.actor, words({1})).get().status;
    CHECK(refused == Status::NotRunning || refused == Status::StaleHandle);
    for (auto& f : accepted) CHECK_STATUS(f.get().status, Status::Ok);
    const VmTerminal terminal = host.wait_closed(v.handle);
    CHECK(terminal.phase == Phase::Stopped);
    CHECK_STATUS(host.begin_shutdown(v.handle), Status::StaleHandle);
    CHECK(host.wait_closed(v.handle).phase == Phase::Stopped); // stable
}

void authority_is_bound_to_host_and_generation() {
    TenantVmHost one, two;
    auto a = one.open(spec("same"));
    auto b = two.open(spec("same"));
    REQUIRE_OK(a); REQUIRE_OK(b);
    CHECK_EQ(a.handle.generation(), b.handle.generation()); // numbers collide by construction
    auto actor_a = one.spawn_actor(a.handle, KIND_COUNTER);
    REQUIRE_OK(actor_a);
    CHECK_STATUS(two.spawn_actor(a.handle, 0).status, Status::ForeignHandle);
    CHECK_STATUS(two.call(actor_a.actor, words({1})).get().status, Status::ForeignHandle);
    CHECK_STATUS(two.kill(a.handle), Status::ForeignHandle);
    CHECK_STATUS(one.call(VmActor{}, words({1})).get().status, Status::ForeignHandle);
    CHECK_STATUS(one.spawn_actor(VmHandle{}, 0).status, Status::ForeignHandle);
    CHECK_STATUS(one.open(spec("same")).status, Status::DuplicateTenant);
    CHECK_EQ(word(one.call(actor_a.actor, words({2})).get().payload), 2);

    // Restart: same id, new generation, fresh memory, and old refs stay dead.
    CHECK_STATUS(one.kill(a.handle), Status::Ok);
    one.wait_closed(a.handle);
    auto restarted = one.open(spec("same"));
    REQUIRE_OK(restarted);
    CHECK(restarted.handle.generation() > a.handle.generation());
    auto fresh = one.spawn_actor(restarted.handle, KIND_COUNTER);
    REQUIRE_OK(fresh);
    CHECK_EQ(fresh.actor.ref(), actor_a.actor.ref());
    CHECK_STATUS(one.call(actor_a.actor, words({1})).get().status, Status::StaleHandle);
    CHECK_EQ(word(one.call(fresh.actor, words({5})).get().payload), 5);

    for (const char* id : {"", ".", "..", "a/b", "a b"}) {
        auto bad = spec("x");
        bad.tenant_id = id;
        CHECK_STATUS(one.open(bad).status, Status::InvalidArgument);
    }
    auto no_module = spec("x");
    no_module.module = nullptr;
    CHECK_STATUS(one.open(no_module).status, Status::InvalidArgument);
}

// Following the addressing rule, a guest cannot reach a neighbour at any offset. The next
// case is the control: breaking the rule reads other memory directly.
void the_addressing_rule_is_what_isolates() {
    TenantVmHost host;
    auto victim = host.open(spec("victim"));
    auto attacker = host.open(spec("attacker"));
    REQUIRE_OK(victim); REQUIRE_OK(attacker);
    auto v = host.spawn_actor(victim.handle, KIND_MEMORY);
    auto a = host.spawn_actor(attacker.handle, KIND_MEMORY);
    REQUIRE_OK(v); REQUIRE_OK(a);
    constexpr std::int64_t kSecret = 0x5EC12E7;
    CHECK_STATUS(host.call(v.actor, words({MEM_WRITE_STATE, kSecret, 0})).get().status, Status::Ok);
    const auto state = word(host.call(a.actor, words({MEM_STATE_OFFSET, 0, 0})).get().payload);

    // Inside the rule: sweep every committed offset. Nothing found but its own memory.
    const auto committed = host.snapshot(attacker.handle).committed_bytes;
    int found = 0;
    for (std::int64_t offset = 4096 * 16; offset + 8 <= static_cast<std::int64_t>(committed) && offset < (1 << 20); offset += 4096 * 16)
        if (word(host.call(a.actor, words({MEM_READ_OFFSET, offset, 0})).get().payload) == kSecret) ++found;
    CHECK_EQ(found, 0);
    CHECK_EQ(word(host.call(a.actor, words({MEM_READ_OFFSET, state, 0})).get().payload), 0);

}

// A rule-breaking access that lands on unmapped memory is caught and classified, so it costs
// only that VM. One that lands on mapped memory is not caught, which the doc states plainly.
void rule_breaking_read_is_contained_only_if_it_faults() {
    TenantVmHost host;
    auto v = host.open(spec("breaker"));
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_MEMORY);
    REQUIRE_OK(a);
    CHECK_STATUS(host.call(a.actor, words({MEM_RULE_BREAKING_READ, 0x10, 0})).get().status, Status::TenantFailed);
    CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::WildAccess);
    auto o = host.open(spec("reader"));
    REQUIRE_OK(o);
    auto r = host.spawn_actor(o.handle, KIND_MEMORY);
    REQUIRE_OK(r);
    // A mapped host address: the read succeeds, with host data, and nothing notices.
    static std::int64_t host_secret = 0x1234567;
    const auto stolen = host.call(r.actor, words({MEM_RULE_BREAKING_READ, static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(&host_secret)), 0})).get();
    CHECK_EQ(word(stolen.payload), host_secret);
    std::printf("  rule-breaking guest read host memory: %s\n", word(stolen.payload) == host_secret ? "yes, as documented" : "no");
}

void concurrent_open_call_fault_kill_stress() {
    HostLimits limits;
    limits.carriers = 8;
    TenantVmHost host(limits);
    auto stable = host.open(spec("stable"));
    REQUIRE_OK(stable);
    auto counter = host.spawn_actor(stable.handle, KIND_COUNTER);
    REQUIRE_OK(counter);
    std::atomic<std::int64_t> expected{0};
    std::atomic<int> survivors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 150; ++i) {
                auto s = spec("s" + std::to_string(t) + "-" + std::to_string(i));
                s.limits.turn_deadline_ms = 50;
                auto v = host.open(s);
                if (v.status != Status::Ok) continue;
                auto c = host.spawn_actor(v.handle, KIND_COUNTER);
                auto f = host.spawn_actor(v.handle, KIND_FAULT);
                if (c.status != Status::Ok || f.status != Status::Ok) continue;
                auto good = host.call(c.actor, words({1}));
                switch (i % 4) {
                    case 0: (void)host.call(f.actor, words({FAULT_TRAP})); break;
                    case 1: (void)host.call(f.actor, words({FAULT_NULL_OFFSET})); break;
                    case 2: (void)host.call(f.actor, words({SPIN_OP})); host.kill(v.handle); break;
                    case 3: host.begin_shutdown(v.handle); break;
                }
                const Status status = good.get().status;
                if (status != Status::Ok && status != Status::TenantFailed) std::abort();
                if (host.wait_closed(v.handle).phase == Phase::Stopped) ++survivors;
                expected.fetch_add(1);
                if (word(host.call(counter.actor, words({1})).get().payload) < 1) std::abort();
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(word(host.call(counter.actor, words({0})).get().payload), expected.load());
    CHECK_EQ(host.stats().live_vms, std::size_t{1});
    std::printf("  %lld VMs opened and ended, %d by clean shutdown\n", static_cast<long long>(expected.load()), survivors.load());
}

void destructor_retires_everything() {
    for (int i = 0; i < 3; ++i) {
        TenantVmHost host;
        std::vector<std::future<TurnResult>> left;
        std::vector<VmHandle> keep;
        for (int v = 0; v < 8; ++v) {
            auto s = spec("v" + std::to_string(v));
            s.limits.turn_deadline_ms = 60000;
            auto opened = host.open(s);
            REQUIRE_OK(opened);
            keep.push_back(opened.handle);
            auto a = host.spawn_actor(opened.handle, KIND_FAULT);
            REQUIRE_OK(a);
            left.push_back(host.call(a.actor, words({SPIN_OP}))); // even while spinning
        }
    }
}

// A fault outside guest code is a bug in the host. It must stay fatal, not pass for a guest
// fault, and handing it on must not cost later guest faults their handler.
void a_host_fault_stays_fatal() {
    TenantVmHost host;
    const pid_t child = fork();
    if (child == 0) {
        volatile std::uintptr_t unmapped = 8;
        *reinterpret_cast<volatile int*>(unmapped) = 1;
        _exit(0); // reached only if the fault was swallowed
    }
    int status = 0;
    CHECK_EQ(waitpid(child, &status, 0), child);
#if defined(TENANT_VM_TIMING_ASSERTS)
    CHECK(WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS));
#else
    // Under a sanitizer the previous handler is the sanitizer: it reports, then exits nonzero.
    CHECK(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0));
#endif
    // The parent's handler is untouched: guest faults still end only their VM.
    auto v = host.open(spec("after"));
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_FAULT);
    REQUIRE_OK(a);
    CHECK_STATUS(host.call(a.actor, words({FAULT_TRAP})).get().status, Status::TenantFailed);
    CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::Trap);
}


// ------------------------------------------------------------------- audit findings
//
// Each of these reproduced against the first version of this host. Most were process-fatal,
// so they ran in a forked child there: any one tenant ended all of them.

template <class Body>
void must_not_kill_the_process(const char* what, Body body) {
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        body();
        std::fflush(stdout);
        std::fflush(stderr);
        _exit(failures ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "  FAIL %s ended the process, status %d\n", what, status);
        ++failures;
    }
}

// A send from the unmapped first page, or past what is committed, made the host fault while
// copying, and a fault in host code is fatal to everyone.
void guest_send_cannot_fault_the_host() {
    must_not_kill_the_process("send from bad offsets", [] {
        TenantVmHost host;
        auto v = host.open(spec("a"));
        REQUIRE_OK(v);
        auto a = host.spawn_actor(v.handle, KIND_HOSTILE);
        REQUIRE_OK(a);
        const long page = sysconf(_SC_PAGESIZE);
        const std::int64_t bad[][2] = {{0, 8}, {1, 8}, {8, 8}, {page - 8, 8}, {page - 8, 16}, {0, 4096}, {0x7FFFFFFF, 64},
                                       {0xFFFFFFFFLL, 64}, {0xFFFFFFF0LL, 32}};
        for (const auto& b : bad) {
            auto r = host.call(a.actor, words({HOSTILE_SEND, -1, b[0], b[1]})).get();
            CHECK_STATUS(r.status, Status::Ok);
            CHECK_EQ(word(r.payload), 0);
        }
        CHECK_STATUS(host.snapshot(v.handle).status, Status::Ok);
    });
}

// Alignment could carry the output offset past the end of a small scratch area. The capacity
// then wrapped around, and the guest could make the host read a reply from unmapped memory.
void guest_reply_cannot_fault_the_host() {
    must_not_kill_the_process("reply claim", [] {
        TenantVmHost host;
        auto v = host.open(spec("a"));
        REQUIRE_OK(v);
        auto a = host.spawn_actor(v.handle, KIND_TINY_SCRATCH);
        REQUIRE_OK(a);
        auto r = host.call(a.actor, {}).get();
        CHECK_STATUS(r.status, Status::Ok); // offered nothing, so it claimed nothing
        CHECK(r.payload.empty());
    });
}

// One tenant could make the host hold 4 GiB of mail, and enough tenants could exhaust it.
void queued_mail_is_bounded_per_vm_and_per_host() {
    constexpr std::int64_t kMessage = 32768;
    const std::uint64_t cost = kMessage + 256; // payload plus the overhead the quota charges
    // One turn floods one mailbox that could hold 64. Nothing drains during a turn, because
    // max_parallel_turns is 1, so the count this returns is exactly what the quota admitted.
    const auto flood = [&](TenantVmHost& host, const std::string& id, std::uint64_t vm_quota) {
        VmSpec s = spec(id);
        s.limits.max_parallel_turns = 1;
        s.limits.max_queued_bytes = vm_quota;
        s.limits.turn_deadline_ms = 20000;
        auto v = host.open(s);
        auto flooder = host.spawn_actor(v.handle, KIND_HOSTILE_LARGE_MAIL);
        auto sink = host.spawn_actor(v.handle, KIND_HOSTILE_LARGE_MAIL);
        const auto sent = word(host.call(flooder.actor, words({HOSTILE_FLOOD_SEND, static_cast<std::int64_t>(sink.actor.ref()), kMessage})).get().payload);
        return std::make_pair(v.handle, sent);
    };
    HostLimits limits;
    limits.carriers = 2;
    {
        TenantVmHost host(limits);
        const std::uint64_t quota = std::uint64_t{1} << 20;
        auto first = flood(host, "vm-quota", quota);
        CHECK_EQ(first.second, static_cast<std::int64_t>(quota / cost)); // 31, in a mailbox of 64
        host.kill(first.first);
        host.wait_closed(first.first);
        CHECK_EQ(host.stats().queued_bytes, std::uint64_t{0}); // a dead VM gives all of it back
    }
    {
        limits.max_queued_bytes = std::uint64_t{1} << 20;
        TenantVmHost host(limits);
        auto capped = flood(host, "host-quota", std::uint64_t{64} << 20); // its own quota is no limit here
        CHECK_EQ(capped.second, static_cast<std::int64_t>(limits.max_queued_bytes / cost));
        host.kill(capped.first);
        host.wait_closed(capped.first);
        // A host call is charged too, and refused without blocking when the quota is spent.
        VmSpec s = spec("host-call");
        s.limits.max_parallel_turns = 1;
        auto v = host.open(s);
        REQUIRE_OK(v);
        auto busy = host.spawn_actor(v.handle, KIND_FAULT);
        auto queued = host.spawn_actor(v.handle, KIND_HOSTILE_LARGE_MAIL);
        REQUIRE_OK(busy);
        REQUIRE_OK(queued);
        auto holding = host.call(busy.actor, words({SPIN_OP})); // holds the VM's only turn
        std::this_thread::sleep_for(Ms(20));
        std::vector<std::future<TurnResult>> admitted;
        Status last = Status::Ok;
        for (int i = 0; i < 64 && last == Status::Ok; ++i) {
            auto f = host.call(queued.actor, Bytes(kMessage));
            if (f.wait_for(Ms(0)) == std::future_status::ready) last = f.get().status;
            else admitted.push_back(std::move(f));
        }
        CHECK_STATUS(last, Status::Busy);
        CHECK(host.stats().queued_bytes <= limits.max_queued_bytes);
        host.kill(v.handle);
        host.wait_closed(v.handle);
        for (auto& f : admitted) CHECK_STATUS(f.get().status, Status::TenantFailed);
        holding.get();
        CHECK_EQ(host.stats().queued_bytes, std::uint64_t{0});
    }
}

// Fairness used to count turns, so a tenant with turns 200 times longer got 200 times the CPU.
void fairness_is_measured_in_cpu_not_turns() {
    HostLimits limits;
    limits.carriers = 1;
    TenantVmHost host(limits);
    VmSpec long_turns = spec("long"), short_turns = spec("short");
    long_turns.limits.max_parallel_turns = short_turns.limits.max_parallel_turns = 1;
    long_turns.limits.turn_deadline_ms = short_turns.limits.turn_deadline_ms = 30000;
    auto l = host.open(long_turns);
    auto s = host.open(short_turns);
    REQUIRE_OK(l);
    REQUIRE_OK(s);
    auto la = host.spawn_actor(l.handle, KIND_BURN);
    auto sa = host.spawn_actor(s.handle, KIND_BURN);
    std::atomic<bool> stop{false};
    const auto pump = [&](VmActor actor, std::int64_t iterations) {
        // A sliding window, so the tenant is always ready. A work-conserving scheduler rightly
        // gives away any gap between batches, which would measure the batching and not fairness.
        std::deque<std::future<TurnResult>> window;
        while (!stop.load()) {
            while (window.size() < 32) window.push_back(host.call(actor, words({iterations})));
            window.front().get();
            window.pop_front();
        }
        for (auto& f : window) f.get();
    };
    std::thread a(pump, la.actor, 100000000), b(pump, sa.actor, 500000);
    // Measured while both compete: once stopped, each drains its window alone. Skip the start too.
    std::this_thread::sleep_for(Ms(1000));
    const auto l0 = host.snapshot(l.handle), s0 = host.snapshot(s.handle);
    std::this_thread::sleep_for(Ms(3000));
    auto ls = host.snapshot(l.handle), ss = host.snapshot(s.handle);
    stop.store(true);
    a.join();
    b.join();
    ls.cpu_ns -= l0.cpu_ns;
    ss.cpu_ns -= s0.cpu_ns;
    ls.turns -= l0.turns;
    ss.turns -= s0.turns;
    const double ratio = static_cast<double>(ls.cpu_ns) / static_cast<double>(std::max<std::uint64_t>(ss.cpu_ns, 1));
    std::printf("  CPU share long:short = %.2f:1, turns %llu vs %llu\n", ratio,
                static_cast<unsigned long long>(ls.turns), static_cast<unsigned long long>(ss.turns));
#if defined(TENANT_VM_TIMING_ASSERTS)
    CHECK(ratio < 3.0); // was about 190:1
    CHECK(ss.turns > ls.turns * 10);
#endif
}

// A wall-clock deadline let a neighbour get an innocent tenant killed by crowding the CPU.
void a_crowded_host_does_not_cost_an_innocent_turn() {
#if defined(__linux__) && defined(TENANT_VM_TIMING_ASSERTS)
    cpu_set_t one, previous;
    CPU_ZERO(&one);
    CPU_SET(0, &one);
    sched_getaffinity(0, sizeof previous, &previous);
    sched_setaffinity(0, sizeof one, &one); // carriers inherit it, so they all fight for one core
    {
        HostLimits limits;
        limits.carriers = 7;
        TenantVmHost host(limits);
        // Calibrate on this machine: a turn that needs about 120 ms of CPU when it has a core alone.
        // The margin below is wide on purpose: the same work can cost 2-3 times the CPU on a slow
        // core of an asymmetric machine, and then it really would use up its deadline.
        VmSpec calibrating = spec("calibrating");
        calibrating.limits.turn_deadline_ms = 30000;
        auto c = host.open(calibrating);
        REQUIRE_OK(c);
        auto ca = host.spawn_actor(c.handle, KIND_BURN);
        const auto calibration = std::chrono::steady_clock::now();
        constexpr std::int64_t kProbe = 100000000;
        CHECK_STATUS(host.call(ca.actor, words({kProbe})).get().status, Status::Ok);
        const auto probe_ms = std::max<long long>(ms_since(calibration), 1);
        const std::int64_t iterations = kProbe * 120 / probe_ms;

        std::vector<VmHandle> neighbours;
        std::vector<VmActor> spinners;
        for (int i = 0; i < 6; ++i) {
            VmSpec n = spec("neighbour" + std::to_string(i));
            n.limits.turn_deadline_ms = 60000;
            auto v = host.open(n);
            REQUIRE_OK(v);
            neighbours.push_back(v.handle);
            spinners.push_back(host.spawn_actor(v.handle, KIND_FAULT).actor);
        }
        std::vector<std::future<TurnResult>> spins;
        for (auto& a : spinners) spins.push_back(host.call(a, words({SPIN_OP})));
        std::this_thread::sleep_for(Ms(50));
        // Under a 300 ms deadline, while getting about a seventh of one core.
        VmSpec innocent = spec("innocent");
        innocent.limits.turn_deadline_ms = 300;
        auto v = host.open(innocent);
        REQUIRE_OK(v);
        auto a = host.spawn_actor(v.handle, KIND_BURN);
        const auto start = std::chrono::steady_clock::now();
        CHECK_STATUS(host.call(a.actor, words({iterations})).get().status, Status::Ok);
        const auto wall = ms_since(start);
        std::printf("  120 ms of CPU took %lld ms of wall time under a 300 ms deadline\n", wall);
        CHECK(wall > 300); // otherwise this did not test what it claims to
        CHECK_STATUS(host.snapshot(v.handle).status, Status::Ok);
        for (auto& n : neighbours) host.kill(n);
        for (auto& s : spins) s.get();
    }
    sched_setaffinity(0, sizeof previous, &previous);
#endif
}

void a_real_spinner_still_dies_on_cpu_deadline() {
    TenantVmHost host;
    VmSpec s = spec("spinner");
    s.limits.turn_deadline_ms = 100;
    auto v = host.open(s);
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_FAULT);
    const auto start = std::chrono::steady_clock::now();
    CHECK_STATUS(host.call(a.actor, words({SPIN_OP})).get().status, Status::TenantFailed);
    CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::TurnDeadline);
    CHECK(ms_since(start) < 2000);
}

// Someone replacing the fault handler later means a guest fault would end the whole process.
void open_refuses_when_the_fault_handler_was_replaced() {
    TenantVmHost host;
    struct sigaction ours {}, foreign {};
    foreign.sa_handler = SIG_DFL;
    sigemptyset(&foreign.sa_mask);
    sigaction(SIGSEGV, &foreign, &ours);
    CHECK_STATUS(host.open(spec("refused")).status, Status::SignalHandlerLost);
    sigaction(SIGSEGV, &ours, nullptr);
    auto v = host.open(spec("accepted"));
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_FAULT);
    CHECK_STATUS(host.call(a.actor, words({FAULT_NULL_OFFSET})).get().status, Status::TenantFailed);
    CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::MemoryFault);
}

// A host call must not leak a C++ exception through guest frames, and creating the host must
// not leave threads behind when it fails part way.
void a_failed_host_start_leaves_nothing_running() {
    HostLimits limits;
    limits.carriers = 4;
    limits.carrier_stack_bytes = std::size_t{1} << 60; // cannot be mapped
    bool threw = false;
    try {
        TenantVmHost host(limits);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    TenantVmHost working; // and a later one is unaffected
    auto v = working.open(spec("after"));
    REQUIRE_OK(v);
}

void wild_access_is_counted_and_can_be_fatal() {
    {
        TenantVmHost host;
        auto v = host.open(spec("wild"));
        REQUIRE_OK(v);
        auto a = host.spawn_actor(v.handle, KIND_MEMORY);
        CHECK_STATUS(host.call(a.actor, words({MEM_RULE_BREAKING_READ, 8, 0})).get().status, Status::TenantFailed);
        CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::WildAccess);
        CHECK_EQ(host.stats().wild_accesses, std::uint64_t{1});
    }
#if defined(TENANT_VM_TIMING_ASSERTS) // sanitizers intercept abort
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        HostLimits strict;
        strict.abort_on_wild_access = true;
        TenantVmHost host(strict);
        auto v = host.open(spec("wild"));
        auto a = host.spawn_actor(v.handle, KIND_MEMORY);
        host.call(a.actor, words({MEM_RULE_BREAKING_READ, 8, 0})).get();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
#endif
}

void instance_memory_is_per_actor_and_outside_the_cage() {
    TenantVmHost host;
    auto v = host.open(spec("a"));
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_INSTANCE);
    auto b = host.spawn_actor(v.handle, KIND_INSTANCE);
    REQUIRE_OK(a);
    REQUIRE_OK(b);
    CHECK_EQ(word(host.call(a.actor, {}).get().payload), 1);
    CHECK_EQ(word(host.call(a.actor, {}).get().payload), 2);
    CHECK_EQ(word(host.call(b.actor, {}).get().payload), 1);
    CHECK_STATUS(host.stop_actor(a.actor), Status::Ok);
    auto again = host.spawn_actor(v.handle, KIND_INSTANCE);
    REQUIRE_OK(again);
    CHECK_EQ(word(host.call(again.actor, {}).get().payload), 1); // not what the last one left

    // It counts against the VM, so a flood of actors is bounded in host memory too.
    VmSpec small = spec("small");
    small.limits.heap_bytes = 4096; // room for 64 instances of 64 bytes, at most
    small.limits.max_actors = 100000;
    auto w = host.open(small);
    REQUIRE_OK(w);
    int spawned = 0;
    while (host.spawn_actor(w.handle, KIND_INSTANCE).status == Status::Ok) ++spawned;
    CHECK(spawned <= 64);
}

void heap_free_reuses_blocks_and_tolerates_abuse() {
    TenantVmHost host;
    auto v = host.open(spec("a"));
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_HOSTILE);
    REQUIRE_OK(a);
    // Far more bytes than the 64 KiB heap, in a loop: only freeing can make this succeed.
    CHECK_EQ(word(host.call(a.actor, words({HOSTILE_ALLOC_FREE_LOOP, 100000, 4096})).get().payload), 1);
    const std::int64_t block = word(host.call(a.actor, words({HOSTILE_ALLOC, 64, 16})).get().payload);
    CHECK(block > 0);
    // Freeing a block twice, or memory that is not a heap block, is refused or harms only itself.
    CHECK_EQ(word(host.call(a.actor, words({HOSTILE_FREE, block, 64, 16})).get().payload), 1);
    CHECK_EQ(word(host.call(a.actor, words({HOSTILE_FREE, 0, 64, 16})).get().payload), 0);
    CHECK_EQ(word(host.call(a.actor, words({HOSTILE_FREE, 0x7FFFFFF0, 64, 16})).get().payload), 0);
    CHECK_EQ(word(host.call(a.actor, words({HOSTILE_FREE, block, 64, 3})).get().payload), 0); // not a power of two
    CHECK_EQ(word(host.call(a.actor, words({HOSTILE_FREE, block, 0, 16})).get().payload), 0);
    // Poisoned free-list links are checked when followed: the next allocation stays in this
    // actor's heap, whatever was written, and nothing the host reads is out of range.
    const std::int64_t poisons[] = {0, 8, 0x7FFFFFFF, -1, 0xFFFFFFF0LL, static_cast<std::int64_t>(host.snapshot(v.handle).committed_bytes)};
    for (std::int64_t poison : poisons) {
        auto r = host.call(a.actor, words({HOSTILE_POISON_FREE_LIST, poison})).get();
        CHECK_STATUS(r.status, Status::Ok);
    }
    CHECK_STATUS(host.snapshot(v.handle).status, Status::Ok);
}

// Random host calls from many hostile actors at once. A failure here is a process death, or a
// sanitizer report, not a value that differs. Deterministic, so a failing seed can be replayed.
void hostile_host_calls_fuzz() {
    const int rounds = std::getenv("TENANT_VM_FUZZ_ROUNDS") ? std::atoi(std::getenv("TENANT_VM_FUZZ_ROUNDS")) : 40;
    std::uint64_t seed = std::getenv("TENANT_VM_FUZZ_SEED") ? std::strtoull(std::getenv("TENANT_VM_FUZZ_SEED"), nullptr, 10) : 0x5EEDFACE;
    const auto next = [&seed] {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    // Values worth trying: boundaries of everything the host checks, then noise.
    const auto interesting = [&](std::uint64_t base) -> std::uint64_t {
        const long page = sysconf(_SC_PAGESIZE);
        const std::uint64_t picks[] = {0, 1, 8, 15, 16, 17, 63, 64, 4095, 4096, 4097, static_cast<std::uint64_t>(page) - 1,
                                       static_cast<std::uint64_t>(page), static_cast<std::uint64_t>(page) + 1, 65535, 65536,
                                       0x7FFFFFFF, 0x80000000ULL, 0xFFFFFFFFULL, 0x100000000ULL, ~std::uint64_t{0},
                                       std::uint64_t{1} << 63, base, base + 8, base + 4096};
        const std::uint64_t r = next();
        return r & 1 ? picks[(r >> 1) % (sizeof picks / sizeof picks[0])] : next();
    };
    std::uint64_t calls = 0, turns = 0;
    TenantVmHost host;
    auto bystander = host.open(spec("bystander"));
    REQUIRE_OK(bystander);
    auto counter = host.spawn_actor(bystander.handle, KIND_COUNTER);
    REQUIRE_OK(counter);
    std::int64_t expected = 0;
    for (int round = 0; round < rounds; ++round) {
        VmSpec s = spec("fuzz" + std::to_string(round));
        s.limits.heap_bytes = std::uint64_t{4} << 20;
        s.limits.max_actors = 64;
        s.limits.max_queued_bytes = std::uint64_t{1} << 20;
        auto v = host.open(s);
        REQUIRE_OK(v);
        std::vector<VmActor> actors;
        for (int i = 0; i < 8; ++i) {
            auto a = host.spawn_actor(v.handle, KIND_HOSTILE);
            REQUIRE_OK(a);
            actors.push_back(a.actor);
        }
        std::vector<std::future<TurnResult>> pending;
        for (int batch = 0; batch < 40; ++batch) {
            const VmActor& a = actors[next() % actors.size()];
            const std::uint64_t target = next() & 1 ? actors[next() % actors.size()].ref() : next();
            std::vector<std::int64_t> script = {HOSTILE_SCRIPT, 0, 0, 0};
            for (int i = 0; i < 40; ++i) {
                script.push_back(static_cast<std::int64_t>(next()));
                const std::uint64_t base = next() % 0x400000;
                script.push_back(static_cast<std::int64_t>(next() & 1 ? target : interesting(base)));
                script.push_back(static_cast<std::int64_t>(interesting(base)));
                script.push_back(static_cast<std::int64_t>(interesting(base)));
                ++calls;
            }
            Bytes input(script.size() * 8);
            std::memcpy(input.data(), script.data(), input.size());
            pending.push_back(host.call(a, std::move(input)));
            ++turns;
        }
        for (auto& f : pending) f.get();
        // Sometimes end a VM with its work still in flight.
        if (next() & 1) host.kill(v.handle);
        else host.begin_shutdown(v.handle);
        host.wait_closed(v.handle);
        // The whole time, a tenant that did nothing wrong has been fine.
        expected += 1;
        CHECK_EQ(word(host.call(counter.actor, words({1})).get().payload), expected);
    }
    CHECK_EQ(host.stats().wild_accesses, std::uint64_t{0});
    CHECK_EQ(host.stats().queued_bytes, std::uint64_t{0}); // every VM has ended, and the bystander is idle
    std::printf("  %llu hostile host calls in %llu turns over %d VMs\n", static_cast<unsigned long long>(calls),
                static_cast<unsigned long long>(turns), rounds);
}

// ------------------------------------------------------------------- restart

#if defined(TENANT_VM_TSAN_BUILD)
#define RESTART_SPIN_OP RESTART_SPIN_IN_HOST_CALLS
#else
#define RESTART_SPIN_OP RESTART_SPIN
#endif

// A confined fault ends one turn. That actor goes on from what it committed, with its mail, and
// its siblings never notice.
void a_confined_fault_restarts_only_that_actor() {
    TenantVmHost host;
    VmSpec s = spec("t");
    s.limits.max_parallel_turns = 1; // so mail queues up behind the failing turn
    auto v = host.open(s);
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_RESTARTABLE);
    auto sibling = host.spawn_actor(v.handle, KIND_RESTARTABLE);
    REQUIRE_OK(a);
    REQUIRE_OK(sibling);
    CHECK_EQ(word(host.call(sibling.actor, words({RESTART_ADD, 7})).get().payload), 7);
    CHECK_EQ(word(host.call(a.actor, words({RESTART_ADD, 10})).get().payload), 10);

    auto failed = host.call(a.actor, words({RESTART_ADD_THEN_TRAP, 1000}));
    auto after1 = host.call(a.actor, words({RESTART_ADD, 1})); // queued behind it
    auto after2 = host.call(a.actor, words({RESTART_ADD, 2}));
    const auto f = failed.get();
    CHECK_STATUS(f.status, Status::ActorRestarted);
    CHECK_REASON(f.failure, FailReason::Trap);
    CHECK_EQ(word(after1.get().payload), 11); // not 1011: the failed turn did not commit. Mail carried over, in order.
    CHECK_EQ(word(after2.get().payload), 13);
    CHECK_EQ(word(host.call(sibling.actor, words({RESTART_ADD, 0})).get().payload), 7);

    const auto snapshot = host.snapshot(v.handle);
    CHECK(snapshot.phase == Phase::Running);
    CHECK_EQ(snapshot.actor_restarts, std::uint64_t{1});
    CHECK_EQ(snapshot.mail_lost, std::uint64_t{0}); // that turn had a caller, who was told
    CHECK_EQ(host.stats().actor_restarts, std::uint64_t{1});

    // Running out of stack and running out of CPU are confined too.
    VmSpec t = spec("t2");
    t.limits.turn_deadline_ms = 50;
    auto w = host.open(t);
    REQUIRE_OK(w);
    auto b = host.spawn_actor(w.handle, KIND_RESTARTABLE);
    REQUIRE_OK(b);
    CHECK_EQ(word(host.call(b.actor, words({RESTART_ADD, 5})).get().payload), 5);
    auto overflow = host.call(b.actor, words({RESTART_STACK})).get();
    CHECK_STATUS(overflow.status, Status::ActorRestarted);
    CHECK_REASON(overflow.failure, FailReason::StackOverflow);
    auto spun = host.call(b.actor, words({RESTART_SPIN_OP})).get();
    CHECK_STATUS(spun.status, Status::ActorRestarted);
    CHECK_REASON(spun.failure, FailReason::TurnDeadline);
    CHECK_EQ(word(host.call(b.actor, words({RESTART_ADD, 1})).get().payload), 6);

    // A restart gives the heap back, or a crash loop could use it all up.
    auto heap = host.spawn_actor(w.handle, KIND_RESTARTABLE);
    REQUIRE_OK(heap);
    int blocks = 0;
    while (blocks < 8 && word(host.call(heap.actor, words({RESTART_READ_HEAP_OK})).get().payload) == 1) ++blocks;
    CHECK(blocks >= 1 && blocks < 8); // it does run out
    CHECK_STATUS(host.call(heap.actor, words({RESTART_ADD_THEN_TRAP, 0})).get().status, Status::ActorRestarted);
    CHECK_EQ(word(host.call(heap.actor, words({RESTART_READ_HEAP_OK})).get().payload), 1);
}

// Spinning in one actor costs its turn. Before this it cost every actor in the VM.
void a_deadline_stops_one_turn_not_its_siblings() {
    TenantVmHost host;
    VmSpec s = spec("t");
    s.limits.turn_deadline_ms = 50;
    auto v = host.open(s);
    REQUIRE_OK(v);
    auto spinner = host.spawn_actor(v.handle, KIND_RESTARTABLE);
    auto worker = host.spawn_actor(v.handle, KIND_RESTARTABLE);
    REQUIRE_OK(spinner);
    REQUIRE_OK(worker);
    auto spin = host.call(spinner.actor, words({RESTART_SPIN_OP}));
    std::int64_t total = 0;
    for (int i = 0; i < 2000; ++i) {
        const auto r = host.call(worker.actor, words({RESTART_ADD, 1})).get(); // runs alongside it
        CHECK_STATUS(r.status, Status::Ok);
        total += 1;
    }
    CHECK_STATUS(spin.get().status, Status::ActorRestarted);
    CHECK_EQ(word(host.call(worker.actor, words({RESTART_ADD, 0})).get().payload), total);
    CHECK(host.snapshot(v.handle).phase == Phase::Running);
}

// A crash loop has to end somewhere, and stored state can be what causes it. It ends that actor.
void repeated_failures_end_only_that_actor() {
    TenantVmHost host;
    VmSpec s = spec("loop");
    s.limits.restart_budget = 3;
    s.limits.restart_window_ms = 60000;
    s.limits.restart_backoff_ms = 1;
    s.limits.mailbox_capacity = 16;
    auto v = host.open(s);
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_RESTARTABLE);
    auto sibling = host.spawn_actor(v.handle, KIND_RESTARTABLE);
    REQUIRE_OK(a);
    REQUIRE_OK(sibling);
    CHECK_STATUS(host.call(sibling.actor, words({RESTART_ADD, 7})).get().status, Status::Ok);
    // Successes between failures do not refill the budget: it is failures per window.
    for (int i = 0; i < 3; ++i) {
        CHECK_STATUS(host.call(a.actor, words({RESTART_ADD_THEN_TRAP, 1})).get().status, Status::ActorRestarted);
        CHECK_STATUS(host.call(a.actor, words({RESTART_ADD, 1})).get().status, Status::Ok);
    }
    // Mail queued behind the last failure is reported, not dropped.
    auto last = host.call(a.actor, words({RESTART_ADD_THEN_TRAP, 1}));
    auto queued = host.call(a.actor, words({RESTART_ADD, 1}));
    const TurnResult ended = last.get();
    CHECK_STATUS(ended.status, Status::ActorTerminated);
    CHECK_REASON(ended.failure, FailReason::Trap);
    CHECK_STATUS(queued.get().status, Status::ActorTerminated);
    CHECK_STATUS(host.call(a.actor, words({RESTART_ADD, 1})).get().status, Status::ActorTerminated);
    CHECK_STATUS(host.stop_actor(a.actor), Status::ActorTerminated);
    // The rest of the VM goes on, and the ended actor gave its memory and quota back.
    const VmSnapshot snapshot = host.snapshot(v.handle);
    CHECK(snapshot.phase == Phase::Running);
    CHECK_EQ(snapshot.live_actors, std::size_t{1});
    CHECK_EQ(snapshot.inflight, std::size_t{0});
    CHECK_EQ(word(host.call(sibling.actor, words({RESTART_ADD, 0})).get().payload), std::int64_t{7});
    auto replacement = host.spawn_actor(v.handle, KIND_RESTARTABLE);
    REQUIRE_OK(replacement);
    // A stale ref stays ended even when its slot is used again.
    CHECK_STATUS(host.call(a.actor, words({RESTART_ADD, 1})).get().status, Status::ActorTerminated);
    // And a clean finish still says what happened on the way.
    host.begin_shutdown(v.handle);
    const VmTerminal terminal = host.wait_closed(v.handle);
    CHECK(terminal.phase == Phase::Stopped);
    CHECK_EQ(terminal.actor_restarts, std::uint64_t{3});
    CHECK_EQ(terminal.actors_terminated, std::uint64_t{1});
    CHECK_EQ(terminal.mail_abandoned, std::uint64_t{1});
}

// The budget counts failures in a window, so old failures stop counting.
void the_restart_budget_refills_with_time() {
    TenantVmHost host;
    VmSpec s = spec("window");
    s.limits.restart_budget = 1;
    s.limits.restart_window_ms = 50;
    s.limits.restart_backoff_ms = 0;
    auto v = host.open(s);
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_RESTARTABLE);
    REQUIRE_OK(a);
    for (int i = 0; i < 4; ++i) {
        CHECK_STATUS(host.call(a.actor, words({RESTART_ADD_THEN_TRAP, 1})).get().status, Status::ActorRestarted);
        std::this_thread::sleep_for(std::chrono::milliseconds(70));
    }
    CHECK_STATUS(host.call(a.actor, words({RESTART_ADD_THEN_TRAP, 1})).get().status, Status::ActorRestarted);
    CHECK_STATUS(host.call(a.actor, words({RESTART_ADD_THEN_TRAP, 1})).get().status, Status::ActorTerminated);
}

// A crash loop waits between tries, doubling, and its mail stays in order through the wait.
void a_restart_waits_before_the_next_turn() {
    TenantVmHost host;
    VmSpec s = spec("backoff");
    s.limits.restart_budget = 3;
    s.limits.restart_window_ms = 60000;
    s.limits.restart_backoff_ms = 40;
    auto v = host.open(s);
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_RESTARTABLE);
    auto sibling = host.spawn_actor(v.handle, KIND_RESTARTABLE);
    REQUIRE_OK(a);
    REQUIRE_OK(sibling);
    CHECK_STATUS(host.call(a.actor, words({RESTART_ADD, 5})).get().status, Status::Ok);
    auto failing = host.call(a.actor, words({RESTART_ADD_THEN_TRAP, 1}));
    auto first = host.call(a.actor, words({RESTART_ADD, 1}));
    auto second = host.call(a.actor, words({RESTART_ADD, 1}));
    CHECK_STATUS(failing.get().status, Status::ActorRestarted);
    const auto start = std::chrono::steady_clock::now();
    // The sibling is not held back while this actor waits.
    CHECK_STATUS(host.call(sibling.actor, words({RESTART_ADD, 1})).get().status, Status::Ok);
    CHECK(ms_since(start) < 30);
    CHECK_EQ(word(first.get().payload), std::int64_t{6}); // from stored state, and the failed turn left nothing
    CHECK(ms_since(start) >= 30);
    CHECK_EQ(word(second.get().payload), std::int64_t{7});
    // A VM can still stop cleanly with an actor waiting.
    CHECK_STATUS(host.call(a.actor, words({RESTART_ADD_THEN_TRAP, 1})).get().status, Status::ActorRestarted);
    auto waiting = host.call(a.actor, words({RESTART_ADD, 1}));
    host.begin_shutdown(v.handle);
    CHECK_EQ(word(waiting.get().payload), std::int64_t{8});
    CHECK(host.wait_closed(v.handle).phase == Phase::Stopped);
}

// Restart is for faults shown to be confined. Everything else still ends the VM.
void an_unconfined_fault_still_fails_the_vm() {
    TenantVmHost host;
    // A memory fault in a guest that bounds-checks should be impossible, so it says the checks failed.
    auto v = host.open(spec("bad-read"));
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_RESTARTABLE);
    REQUIRE_OK(a);
    CHECK_STATUS(host.call(a.actor, words({RESTART_BAD_READ})).get().status, Status::TenantFailed);
    CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::MemoryFault);
    // A kind that does not claim confinement is not given it, even for a checked trap.
    auto w = host.open(spec("unflagged"));
    REQUIRE_OK(w);
    auto b = host.spawn_actor(w.handle, KIND_RESTART_UNFLAGGED);
    REQUIRE_OK(b);
    CHECK_STATUS(host.call(b.actor, words({RESTART_ADD_THEN_TRAP, 1})).get().status, Status::TenantFailed);
    CHECK_REASON(host.wait_closed(w.handle).reason, FailReason::Trap);
    // A kill is a kill, however restartable the actor is.
    VmSpec s = spec("killed");
    s.limits.turn_deadline_ms = 60000;
    auto x = host.open(s);
    REQUIRE_OK(x);
    auto c = host.spawn_actor(x.handle, KIND_RESTARTABLE);
    REQUIRE_OK(c);
    auto spin = host.call(c.actor, words({RESTART_SPIN_OP}));
    std::this_thread::sleep_for(Ms(20));
    CHECK_STATUS(host.kill(x.handle), Status::Ok);
    CHECK_STATUS(spin.get().status, Status::TenantFailed);
    CHECK_REASON(host.wait_closed(x.handle).reason, FailReason::HostKill);
}

// A fire-and-forget message that fails has no caller to tell, so the VM keeps count, and a clean stop
// still says so. A failed run must not look like a clean finish.
void a_clean_stop_still_reports_failures() {
    TenantVmHost host;
    auto v = host.open(spec("t"));
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, KIND_RESTARTABLE);
    REQUIRE_OK(a);
    CHECK_STATUS(host.call(a.actor, words({RESTART_ADD_THEN_TRAP, 1})).get().status, Status::ActorRestarted);
    CHECK_STATUS(host.begin_shutdown(v.handle), Status::Ok);
    const VmTerminal terminal = host.wait_closed(v.handle);
    CHECK(terminal.phase == Phase::Stopped);
    CHECK_EQ(terminal.actor_restarts, std::uint64_t{1});
    // A stopping actor that fails on its last message is released all the same.
    auto w = host.open(spec("stopping"));
    REQUIRE_OK(w);
    auto b = host.spawn_actor(w.handle, KIND_RESTARTABLE);
    REQUIRE_OK(b);
    auto last = host.call(b.actor, words({RESTART_ADD_THEN_TRAP, 1}));
    CHECK_STATUS(host.stop_actor(b.actor), Status::Ok);
    CHECK_STATUS(last.get().status, Status::ActorRestarted);
    CHECK_EQ(host.snapshot(w.handle).live_actors, std::size_t{0});
}

// Many actors failing, stopping and being killed at once, for the sanitizers to look at.
void restart_stress() {
    TenantVmHost host;
    std::vector<std::thread> threads;
    std::atomic<std::uint64_t> restarted{0};
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&, t] {
            std::uint64_t seed = 0xABC + t;
            const auto next = [&seed] {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                return seed;
            };
            for (int round = 0; round < 40; ++round) {
                VmSpec s = spec("stress-" + std::to_string(t) + "-" + std::to_string(round));
                s.limits.turn_deadline_ms = 5;
                s.limits.restart_budget = 4;
                s.limits.restart_backoff_ms = 1;
                auto v = host.open(s);
                if (v.status != Status::Ok) continue;
                std::vector<VmActor> actors;
                for (int i = 0; i < 6; ++i) actors.push_back(host.spawn_actor(v.handle, KIND_RESTARTABLE).actor);
                std::vector<std::future<TurnResult>> pending;
                for (int i = 0; i < 60; ++i) {
                    const auto op = std::array<std::int64_t, 5>{RESTART_ADD, RESTART_ADD, RESTART_ADD_THEN_TRAP, RESTART_SPIN_OP, RESTART_STACK}[next() % 5];
                    pending.push_back(host.call(actors[next() % actors.size()], words({op, 1})));
                    if (next() % 16 == 0) host.stop_actor(actors[next() % actors.size()]);
                }
                if (next() & 1) host.kill(v.handle);
                else host.begin_shutdown(v.handle);
                for (auto& f : pending) {
                    if (f.get().status == Status::ActorRestarted) restarted.fetch_add(1);
                }
                host.wait_closed(v.handle);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    CHECK(restarted.load() > 0);
    CHECK_EQ(host.stats().queued_bytes, std::uint64_t{0});
    std::printf("  %llu turns failed and restarted, across 240 VMs ended in every way\n", static_cast<unsigned long long>(restarted.load()));
}

struct Case { const char* name; void (*run)(); };
const Case cases[] = {
    {"equal_offsets_are_different_memory", equal_offsets_are_different_memory},
    {"messaging_stays_inside_the_vm", messaging_stays_inside_the_vm},
    {"faults_end_only_that_vm", faults_end_only_that_vm},
    {"a_fault_stops_all_of_that_vm_running_in_parallel", a_fault_stops_all_of_that_vm_running_in_parallel},
    {"host_can_kill_a_spinning_vm", host_can_kill_a_spinning_vm},
    {"kills_during_host_calls_leave_locks_free", kills_during_host_calls_leave_locks_free},
    {"one_tenant_cannot_starve_another", one_tenant_cannot_starve_another},
    {"memory_is_bounded_per_vm", memory_is_bounded_per_vm},
    {"cpu_budget_is_enforced", cpu_budget_is_enforced},
    {"actor_churn_reuses_slots_and_scrubs_memory", actor_churn_reuses_slots_and_scrubs_memory},
    {"tenant_recycling_restores_heap_quota_and_fences_aliases", tenant_recycling_restores_heap_quota_and_fences_aliases},
    {"guests_can_spawn_within_their_vm", guests_can_spawn_within_their_vm},
    {"shutdown_drains_then_retires", shutdown_drains_then_retires},
    {"authority_is_bound_to_host_and_generation", authority_is_bound_to_host_and_generation},
    {"the_addressing_rule_is_what_isolates", the_addressing_rule_is_what_isolates},
    {"rule_breaking_read_is_contained_only_if_it_faults", rule_breaking_read_is_contained_only_if_it_faults},
    {"concurrent_open_call_fault_kill_stress", concurrent_open_call_fault_kill_stress},
    {"destructor_retires_everything", destructor_retires_everything},
    {"a_host_fault_stays_fatal", a_host_fault_stays_fatal},
    {"guest_send_cannot_fault_the_host", guest_send_cannot_fault_the_host},
    {"guest_reply_cannot_fault_the_host", guest_reply_cannot_fault_the_host},
    {"queued_mail_is_bounded_per_vm_and_per_host", queued_mail_is_bounded_per_vm_and_per_host},
    {"fairness_is_measured_in_cpu_not_turns", fairness_is_measured_in_cpu_not_turns},
    {"a_crowded_host_does_not_cost_an_innocent_turn", a_crowded_host_does_not_cost_an_innocent_turn},
    {"a_real_spinner_still_dies_on_cpu_deadline", a_real_spinner_still_dies_on_cpu_deadline},
    {"open_refuses_when_the_fault_handler_was_replaced", open_refuses_when_the_fault_handler_was_replaced},
    {"a_failed_host_start_leaves_nothing_running", a_failed_host_start_leaves_nothing_running},
    {"wild_access_is_counted_and_can_be_fatal", wild_access_is_counted_and_can_be_fatal},
    {"instance_memory_is_per_actor_and_outside_the_cage", instance_memory_is_per_actor_and_outside_the_cage},
    {"heap_free_reuses_blocks_and_tolerates_abuse", heap_free_reuses_blocks_and_tolerates_abuse},
    {"hostile_host_calls_fuzz", hostile_host_calls_fuzz},
    {"a_confined_fault_restarts_only_that_actor", a_confined_fault_restarts_only_that_actor},
    {"a_deadline_stops_one_turn_not_its_siblings", a_deadline_stops_one_turn_not_its_siblings},
    {"repeated_failures_end_only_that_actor", repeated_failures_end_only_that_actor},
    {"the_restart_budget_refills_with_time", the_restart_budget_refills_with_time},
    {"a_restart_waits_before_the_next_turn", a_restart_waits_before_the_next_turn},
    {"an_unconfined_fault_still_fails_the_vm", an_unconfined_fault_still_fails_the_vm},
    {"a_clean_stop_still_reports_failures", a_clean_stop_still_reports_failures},
    {"restart_stress", restart_stress},
    {"thousand_vms_in_one_process", thousand_vms_in_one_process},
};
} // namespace

int main(int argc, char** argv) {
    int ran = 0, failed = 0;
    for (const Case& c : cases) {
        if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
        std::printf("[ RUN  ] %s\n", c.name);
        std::fflush(stdout);
        const int before = failures;
        const auto start = std::chrono::steady_clock::now();
        c.run();
        const bool pass = failures == before;
        std::printf("[ %s ] %s (%lldms)\n", pass ? " OK " : "FAIL", c.name, ms_since(start));
        std::fflush(stdout);
        ++ran;
        if (!pass) ++failed;
    }
    std::printf("%d cases, %d failed\n", ran, failed);
    return ran == 0 || failed != 0;
}
