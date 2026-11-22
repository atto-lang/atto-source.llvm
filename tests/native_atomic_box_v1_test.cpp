#include "oreslang/runtime/NativeAtomicBoxV1.hpp"
#include "oreslang/runtime/NativeBoxArenaV1.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace oreslang::runtime::native_ownership_v1;

#define REQUIRE(expr) do { if (!(expr)) throw std::runtime_error("failed: " #expr); } while (false)

static_assert(SupportedAtomicScalar<bool>);
static_assert(SupportedAtomicScalar<std::uint8_t>);
static_assert(SupportedAtomicScalar<std::int32_t>);
static_assert(SupportedAtomicScalar<std::uint64_t>);
static_assert(!SupportedAtomicScalar<float>);
static_assert(!SupportedAtomicScalar<double>);
static_assert(!std::is_copy_constructible_v<AtomicScalar<std::int64_t>>);
static_assert(!std::is_move_constructible_v<AtomicScalar<std::int64_t>>);
static_assert(!std::is_copy_constructible_v<UniqueBox<int>>);
static_assert(std::is_nothrow_move_constructible_v<UniqueBox<int>>);
static_assert(std::is_nothrow_destructible_v<UniqueBox<int>>);

struct Token {
    std::atomic<int>* drops;
    explicit Token(std::atomic<int>* counter) noexcept : drops(counter) {}
    ~Token() noexcept { drops->fetch_add(1, std::memory_order_relaxed); }
};
static_assert(!std::is_constructible_v<UniqueAllocationTicket<Token>,
               Token*, void*, UniqueAllocationTicket<Token>::Destroy,
               std::uint64_t, std::uint32_t>);
static_assert(!std::is_copy_constructible_v<UniqueAllocationTicket<Token>>);
static_assert(!std::is_move_constructible_v<UniqueAllocationTicket<Token>>);
static_assert(std::is_nothrow_destructible_v<UniqueAllocationTicket<Token>>);

static void ordering_and_failure_checks() {
    AtomicScalar<std::int64_t> value(3);
    std::int64_t result = -1;
    REQUIRE(value.load(&result) == AtomicStatus::Ok && result == 3);
    REQUIRE(value.load(nullptr) == AtomicStatus::InvalidOutput);
    REQUIRE(value.load(&result, Ordering::Release) == AtomicStatus::InvalidOrder);
    REQUIRE(value.load(&result, Ordering::AcqRel) == AtomicStatus::InvalidOrder);
    REQUIRE(value.store(4, Ordering::Acquire) == AtomicStatus::InvalidOrder);
    REQUIRE(value.store(4, Ordering::AcqRel) == AtomicStatus::InvalidOrder);
    REQUIRE(value.load(&result) == AtomicStatus::Ok && result == 3);
    REQUIRE(value.store(4, Ordering::Release) == AtomicStatus::Ok);
    REQUIRE(value.load(&result, Ordering::Acquire) == AtomicStatus::Ok && result == 4);

    const Ordering orders[] = {
        Ordering::Relaxed, Ordering::Acquire, Ordering::Release,
        Ordering::AcqRel, Ordering::SeqCst,
        static_cast<Ordering>(255)
    };
    for (auto success : orders) {
        for (auto failure : orders) {
            std::int64_t expected = -200;
            const auto status = value.compare_exchange(&expected, 9, success, failure);
            if (!is_cas_order(success, failure)) {
                REQUIRE(status == AtomicStatus::InvalidOrder);
                REQUIRE(expected == -200);
            } else {
                REQUIRE(status == AtomicStatus::Mismatch);
                REQUIRE(expected == 4);
            }
        }
    }
    std::int64_t expected = 4;
    REQUIRE(value.compare_exchange(&expected, 10, Ordering::AcqRel, Ordering::Acquire)
            == AtomicStatus::Ok);
    REQUIRE(value.compare_exchange(nullptr, 11, Ordering::SeqCst, Ordering::SeqCst)
            == AtomicStatus::InvalidOutput);
    REQUIRE(value.load(&result) == AtomicStatus::Ok && result == 10);
    REQUIRE(value.swap(12, &result, Ordering::AcqRel) == AtomicStatus::Ok && result == 10);
    REQUIRE(value.fetch_add(3, &result) == AtomicStatus::Ok && result == 12);
    REQUIRE(value.fetch_sub(2, &result) == AtomicStatus::Ok && result == 15);
    REQUIRE(value.load(&result) == AtomicStatus::Ok && result == 13);
    REQUIRE(value.fetch_and(7, &result) == AtomicStatus::Ok && result == 13);
    REQUIRE(value.fetch_or(8, &result) == AtomicStatus::Ok && result == 5);
    REQUIRE(value.fetch_xor(3, &result) == AtomicStatus::Ok && result == 13);
    REQUIRE(value.load(&result) == AtomicStatus::Ok && result == 14);

    AtomicScalar<bool> flag(false);
    bool previous = true;
    REQUIRE(flag.swap(true, &previous) == AtomicStatus::Ok && !previous);
    REQUIRE(flag.load(&previous) == AtomicStatus::Ok && previous);
    // Source-facing integer atomics must never publish a wrapped result.
    const auto limit_max = std::numeric_limits<std::int64_t>::max();
    const auto limit_min = std::numeric_limits<std::int64_t>::min();
    AtomicScalar<std::int64_t> checked_max(limit_max);
    result = 17;
    REQUIRE(checked_max.fetch_add(1, &result) == AtomicStatus::Overflow);
    REQUIRE(result == 17); // rejected operation cannot write output either
    REQUIRE(checked_max.load(&result) == AtomicStatus::Ok && result == limit_max);
    AtomicScalar<std::int64_t> checked_min(limit_min);
    result = 19;
    REQUIRE(checked_min.fetch_sub(1, &result, Ordering::AcqRel)
            == AtomicStatus::Overflow);
    REQUIRE(result == 19);
    REQUIRE(checked_min.load(&result) == AtomicStatus::Ok && result == limit_min);
    // Negative delta can also overflow signed subtraction.
    REQUIRE(checked_max.fetch_sub(-1, &result) == AtomicStatus::Overflow);
    REQUIRE(checked_max.load(&result) == AtomicStatus::Ok && result == limit_max);
    AtomicScalar<std::uint8_t> checked_byte(255);
    std::uint8_t previous_byte = 7;
    REQUIRE(checked_byte.fetch_add(1, &previous_byte) == AtomicStatus::Overflow);
    REQUIRE(previous_byte == 7);
    REQUIRE(checked_byte.load(&previous_byte) == AtomicStatus::Ok &&
            previous_byte == 255);
}

static void concurrent_atomic_contention() {
    constexpr int threads = 10;
    constexpr int iterations = 20000;
    AtomicScalar<std::int64_t> count(0);
    std::vector<std::thread> workers;
    for (int lane = 0; lane < threads; ++lane) {
        workers.emplace_back([&] {
            for (int step = 0; step < iterations; ++step) {
                std::int64_t previous = -1;
                REQUIRE(count.fetch_add(1, &previous, Ordering::AcqRel) == AtomicStatus::Ok);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    std::int64_t got = -1;
    REQUIRE(count.load(&got, Ordering::Acquire) == AtomicStatus::Ok);
    REQUIRE(got == threads * iterations);

    // CAS retry path is independent from fetch_add and handles contention.
    AtomicScalar<std::int64_t> cas_count(0);
    workers.clear();
    for (int lane = 0; lane < threads; ++lane) {
        workers.emplace_back([&] {
            for (int step = 0; step < iterations; ++step) {
                std::int64_t current = 0;
                REQUIRE(cas_count.load(&current, Ordering::Acquire) == AtomicStatus::Ok);
                while (true) {
                    auto expected = current;
                    const auto status = cas_count.compare_exchange(
                        &expected, current + 1, Ordering::AcqRel, Ordering::Acquire, true);
                    if (status == AtomicStatus::Ok) break;
                    REQUIRE(status == AtomicStatus::Mismatch);
                    current = expected;
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    REQUIRE(cas_count.load(&got) == AtomicStatus::Ok && got == threads * iterations);

    // Release-store and acquire-load must publish non-atomic payload.
    struct Publication { int payload = 0; AtomicScalar<bool> ready{false}; };
    Publication publication;
    std::thread receiver([&] {
        bool ready = false;
        do { REQUIRE(publication.ready.load(&ready, Ordering::Acquire) == AtomicStatus::Ok); }
        while (!ready);
        REQUIRE(publication.payload == 42);
    });
    publication.payload = 42;
    REQUIRE(publication.ready.store(true, Ordering::Release) == AtomicStatus::Ok);
    receiver.join();
}

static void unique_box_lifecycle() {
    std::atomic<int> drops{0};
    FixedBoxArena<Token, 4> arena;
    REQUIRE(arena.domain() != 0);
    auto invalid_index = arena.issue_at(4, &drops);
    REQUIRE(!invalid_index.valid());
    UniqueBox<Token> a, rival;
    auto ticket = arena.issue_at(0, &drops);
    REQUIRE(ticket.valid());
    auto same_slot = arena.issue_at(0, &drops);
    REQUIRE(!same_slot.valid());
    REQUIRE(UniqueBox<Token>::adopt(ticket, a));
    REQUIRE(!UniqueBox<Token>::adopt(ticket, rival));
    REQUIRE(!ticket.valid());
    REQUIRE(arena.issue_at(0, &drops).valid() == false);
    REQUIRE(a.get_checked(arena.domain(), 1) != nullptr);
    REQUIRE(a.get_checked(arena.domain(), 2) == nullptr);
    REQUIRE(a.get_checked(arena.domain() + 1, 1) == nullptr);
    auto other_ticket = arena.issue_at(1, &drops);
    REQUIRE(other_ticket.valid());
    REQUIRE(!UniqueBox<Token>::adopt(other_ticket, a));
    REQUIRE(other_ticket.valid());
    REQUIRE(UniqueBox<Token>::adopt(other_ticket, rival));

    UniqueBox<Token> moved(std::move(a));
    REQUIRE(!a.is_owned() && a.get_checked(arena.domain(), 1) == nullptr);
    REQUIRE(moved.get_checked(arena.domain(), 1) != nullptr);
    UniqueBox<Token>* self_alias = &moved;
    moved = std::move(*self_alias);
    REQUIRE(drops.load() == 0);

    moved = std::move(rival); // slot 0 first drop
    REQUIRE(drops.load() == 1);
    REQUIRE(!rival.is_owned() && rival.owner_generation() == 0);
    moved.reset(); // slot 1 drop
    REQUIRE(drops.load() == 2);
    moved.reset();
    REQUIRE(drops.load() == 2);

    // Slot 0 is available again but has a new, never-aliased generation.
    auto reuse = arena.issue_at(0, &drops);
    REQUIRE(reuse.valid());
    REQUIRE(UniqueBox<Token>::adopt(reuse, moved));
    REQUIRE(moved.owner_generation() == 2);
    REQUIRE(moved.get_checked(arena.domain(), 1) == nullptr);
    REQUIRE(moved.get_checked(arena.domain(), 2) != nullptr);
    std::thread worker([owned = std::move(moved), domain = arena.domain()] () mutable {
        REQUIRE(owned.get_checked(domain, 2) != nullptr);
        owned.reset();
    });
    worker.join();
    REQUIRE(!moved.is_owned());
    REQUIRE(drops.load() == 3);

    // Unconsumed ticket owns the live allocation and drops it upon scope exit.
    {
        auto unused = arena.issue_at(2, &drops);
        REQUIRE(unused.valid());
        REQUIRE(!arena.issue_at(2, &drops).valid());
    }
    REQUIRE(drops.load() == 4);
    auto after_reject = arena.issue_at(2, &drops);
    REQUIRE(after_reject.valid());
    REQUIRE(UniqueBox<Token>::adopt(after_reject, moved));
    REQUIRE(moved.owner_generation() == 2);
    REQUIRE(!arena.try_close()); // cannot falsely certify all allocations drained
    moved.reset();
    REQUIRE(drops.load() == 5);
    REQUIRE(arena.try_close());
    REQUIRE(!arena.issue_at(0, &drops).valid());
}

static void concurrent_ticket_admission() {
    constexpr int lanes = 16;
    std::atomic<int> drops{0};
    FixedBoxArena<Token, 1> arena;
    auto ticket = arena.issue_at(0, &drops);
    std::array<UniqueBox<Token>, lanes> destinations;
    std::array<bool, lanes> results{};
    std::vector<std::thread> workers;
    for (int i = 0; i < lanes; ++i) {
        workers.emplace_back([&, i] {
            results[static_cast<std::size_t>(i)] =
                UniqueBox<Token>::adopt(ticket, destinations[static_cast<std::size_t>(i)]);
        });
    }
    for (auto& worker : workers) worker.join();
    int admitted = 0;
    for (int i = 0; i < lanes; ++i) {
        if (results[static_cast<std::size_t>(i)]) ++admitted;
        if (destinations[static_cast<std::size_t>(i)].is_owned())
            REQUIRE(results[static_cast<std::size_t>(i)]);
    }
    REQUIRE(admitted == 1);
    REQUIRE(drops.load() == 0);
    // A second issuance cannot overlap an outstanding claimed owner.
    REQUIRE(!arena.issue_at(0, &drops).valid());
    for (auto& box : destinations) box.reset();
    REQUIRE(drops.load() == 1);
    REQUIRE(arena.try_close());
}

static void concurrent_allocator_issuance() {
    constexpr int lanes = 16;
    std::atomic<int> drops{0}, issued{0}, arrived{0};
    std::atomic<bool> start{false};
    FixedBoxArena<Token, 1> arena;
    std::vector<std::thread> workers;
    for (int i = 0; i < lanes; ++i) {
        workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            auto t = arena.issue_at(0, &drops);
            if (t.valid()) issued.fetch_add(1, std::memory_order_relaxed);
            arrived.fetch_add(1, std::memory_order_release);
            while (arrived.load(std::memory_order_acquire) < lanes)
                std::this_thread::yield();
            // Losing tickets do not free the winner's live allocation.
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    REQUIRE(issued.load() == 1);
    REQUIRE(drops.load() == 1); // winner's unclaimed ticket dropped once
    REQUIRE(arena.try_close());
}

// A close must never report success while an allocator lease can still
// be published. Every execution order is valid; false success is not.
static void concurrent_issue_and_retire() {
    for (int round = 0; round < 64; ++round) {
        std::atomic<int> drops{0};
        FixedBoxArena<Token, 1> arena;
        UniqueBox<Token> held;
        std::atomic<bool> start{false};
        std::atomic<bool> issued{false};
        std::atomic<bool> closed{false};
        std::thread issuer([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            auto ticket = arena.issue_at(0, &drops);
            if (ticket.valid()) {
                REQUIRE(UniqueBox<Token>::adopt(ticket, held));
                issued.store(true, std::memory_order_release);
            }
        });
        std::thread closer([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            closed.store(arena.try_close(), std::memory_order_release);
        });
        start.store(true, std::memory_order_release);
        issuer.join();
        closer.join();
        REQUIRE(!closed.load(std::memory_order_acquire) ||
                !issued.load(std::memory_order_acquire));
        const bool had_owner = held.is_owned();
        held.reset();
        REQUIRE(drops.load(std::memory_order_acquire) == (had_owner ? 1 : 0));
        REQUIRE(arena.try_close());
        REQUIRE(!arena.issue_at(0, &drops).valid());
    }
}

struct ReentrantToken {
    UniqueBox<ReentrantToken>* owner;
    std::atomic<int>* drops;
    ReentrantToken(UniqueBox<ReentrantToken>* b, std::atomic<int>* count) noexcept
        : owner(b), drops(count) {}
    ~ReentrantToken() noexcept {
        owner->reset();
        drops->fetch_add(1, std::memory_order_relaxed);
    }
};
static void reentrant_destructor_revocation() {
    std::atomic<int> drops{0};
    UniqueBox<ReentrantToken> box;
    FixedBoxArena<ReentrantToken, 1> arena;
    auto ticket = arena.issue_at(0, &box, &drops);
    REQUIRE(ticket.valid());
    REQUIRE(UniqueBox<ReentrantToken>::adopt(ticket, box));
    box.reset();
    REQUIRE(drops.load() == 1 && !box.is_owned());
    box.reset();
    REQUIRE(drops.load() == 1);
    REQUIRE(arena.try_close());
}

int main() {
    ordering_and_failure_checks();
    concurrent_atomic_contention();
    unique_box_lifecycle();
    concurrent_ticket_admission();
    concurrent_allocator_issuance();
    concurrent_issue_and_retire();
    reentrant_destructor_revocation();
    std::puts("ATTO_NATIVE_ATOMIC_BOX_V1_PASS");
}
