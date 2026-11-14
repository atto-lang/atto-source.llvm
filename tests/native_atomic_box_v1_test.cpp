#include "oreslang/runtime/NativeAtomicBoxV1.hpp"

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

struct Token { std::atomic<int>* drops; };
static void destroy_token(Token* token) noexcept {
    token->drops->fetch_add(1, std::memory_order_relaxed);
}

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
    // Explicitly wrapping atomic integer arithmetic, even for signed i64.
    AtomicScalar<std::int64_t> wrapping(std::numeric_limits<std::int64_t>::max());
    REQUIRE(wrapping.fetch_add(1, &result) == AtomicStatus::Ok);
    REQUIRE(result == std::numeric_limits<std::int64_t>::max());
    REQUIRE(wrapping.load(&result) == AtomicStatus::Ok &&
            result == std::numeric_limits<std::int64_t>::min());
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

static_assert(!std::is_copy_constructible_v<UniqueAllocationTicket<Token>>);
static_assert(!std::is_move_constructible_v<UniqueAllocationTicket<Token>>);
static_assert(std::is_nothrow_destructible_v<UniqueAllocationTicket<Token>>);

static void unique_box_lifecycle() {
    std::atomic<int> drops{0};
    Token first{&drops}, second{&drops}, third{&drops};
    UniqueBox<Token> a;
    {
        UniqueAllocationTicket<Token> null_pointer(nullptr, destroy_token, 41, 3);
        UniqueAllocationTicket<Token> null_destroy(&first, nullptr, 41, 3);
        REQUIRE(!UniqueBox<Token>::adopt(null_pointer, a));
        REQUIRE(!UniqueBox<Token>::adopt(null_destroy, a));
        REQUIRE(drops.load() == 0);
    }
    {
        Token invalid{&drops};
        UniqueAllocationTicket<Token> zero_domain(&invalid, destroy_token, 0, 3);
        REQUIRE(!UniqueBox<Token>::adopt(zero_domain, a));
        // An unconsumed ticket still owns the resource and destroys it here.
    }
    REQUIRE(drops.load() == 1);
    {
        Token invalid{&drops};
        UniqueAllocationTicket<Token> zero_generation(&invalid, destroy_token, 41, 0);
        REQUIRE(!UniqueBox<Token>::adopt(zero_generation, a));
    }
    REQUIRE(drops.load() == 2);

    UniqueAllocationTicket<Token> first_ticket(&first, destroy_token, 41, 3);
    UniqueBox<Token> rival;
    REQUIRE(UniqueBox<Token>::adopt(first_ticket, a));
    REQUIRE(!UniqueBox<Token>::adopt(first_ticket, rival));
    REQUIRE(!rival.is_owned()); // cannot create a second owner from same ticket
    REQUIRE(a.get_checked(41, 3) == &first);
    REQUIRE(a.get_checked(41, 4) == nullptr);
    REQUIRE(a.get_checked(42, 3) == nullptr);

    UniqueAllocationTicket<Token> second_ticket(&second, destroy_token, 41, 3);
    REQUIRE(!UniqueBox<Token>::adopt(second_ticket, a));
    // Failed occupied-destination adoption must leave ticket available.
    REQUIRE(UniqueBox<Token>::adopt(second_ticket, rival));
    REQUIRE(!UniqueBox<Token>::adopt(second_ticket, a));
    UniqueBox<Token> moved(std::move(a));
    REQUIRE(!a.is_owned() && a.get_checked(41, 3) == nullptr);
    REQUIRE(moved.get_checked(41, 3) == &first);
    UniqueBox<Token>* self_alias = &moved;
    moved = std::move(*self_alias);
    REQUIRE(drops.load() == 2);

    moved = std::move(rival); // first resource dropped once
    REQUIRE(drops.load() == 3);
    REQUIRE(!rival.is_owned() && rival.owner_generation() == 0);
    moved.reset(); // second resource dropped once
    REQUIRE(drops.load() == 4);
    moved.reset();
    REQUIRE(drops.load() == 4);
    REQUIRE(!moved.is_owned());

    UniqueAllocationTicket<Token> third_ticket(&third, destroy_token, 41, 3);
    REQUIRE(UniqueBox<Token>::adopt(third_ticket, moved));
    std::thread worker([owned = std::move(moved)] () mutable {
        REQUIRE(owned.get_checked(41, 3) != nullptr);
        owned.reset();
    });
    worker.join();
    REQUIRE(!moved.is_owned());
    REQUIRE(drops.load() == 5);
}

// Even two concurrent trusted host attempts to consume the *same* allocation
// ticket cannot mint two box owners. This is not permission to mint two
// different tickets for the same physical allocation.
static void concurrent_ticket_admission() {
    std::atomic<int> drops{0};
    Token token{&drops};
    UniqueAllocationTicket<Token> ticket(&token, destroy_token, 77, 9);
    std::array<UniqueBox<Token>, 16> destinations;
    std::array<bool, 16> results{};
    std::vector<std::thread> workers;
    for (int i = 0; i < 16; ++i) {
        workers.emplace_back([&, i] {
            results[static_cast<std::size_t>(i)] =
                UniqueBox<Token>::adopt(ticket, destinations[static_cast<std::size_t>(i)]);
        });
    }
    for (auto& worker : workers) worker.join();
    int admitted = 0;
    for (int i = 0; i < 16; ++i) {
        if (results[static_cast<std::size_t>(i)]) ++admitted;
        if (destinations[static_cast<std::size_t>(i)].is_owned())
            REQUIRE(results[static_cast<std::size_t>(i)]);
    }
    REQUIRE(admitted == 1);
    REQUIRE(drops.load() == 0);
    for (auto& box : destinations) box.reset();
    REQUIRE(drops.load() == 1);
}

struct ReentrantToken {
    UniqueBox<ReentrantToken>* owner;
    std::atomic<int>* drops;
};
static void destroy_reentrant(ReentrantToken* token) noexcept {
    token->owner->reset();
    token->drops->fetch_add(1, std::memory_order_relaxed);
}
static void reentrant_destructor_revocation() {
    std::atomic<int> drops{0};
    UniqueBox<ReentrantToken> box;
    ReentrantToken token{&box, &drops};
    UniqueAllocationTicket<ReentrantToken> ticket(&token, destroy_reentrant, 91, 2);
    REQUIRE(UniqueBox<ReentrantToken>::adopt(ticket, box));
    box.reset();
    REQUIRE(drops.load() == 1 && !box.is_owned());
    box.reset();
    REQUIRE(drops.load() == 1);
}

int main() {
    ordering_and_failure_checks();
    concurrent_atomic_contention();
    unique_box_lifecycle();
    concurrent_ticket_admission();
    reentrant_destructor_revocation();
    std::puts("ATTO_NATIVE_ATOMIC_BOX_V1_PASS");
}
