#include "oreslang/runtime/BoundedActorPort.hpp"
#include "oreslang/runtime/ActorMailboxCompletion.hpp"

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

// Thread-local, one-shot injected allocator fault. Owned message publication
// must no longer allocate after construction of the fixed-capacity slot pool.
// The ordinary offer(T&&) still allocates its T handle outside the port lock
// and must roll back preparation/ledger credits if that allocation fails.
static thread_local bool fail_next_allocation = false;

__attribute__((noinline)) void* operator new(std::size_t count) {
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc();
    }
    if (void* ptr = std::malloc(count == 0 ? 1 : count)) return ptr;
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void* ptr) noexcept { std::free(ptr); }
__attribute__((noinline)) void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }

static void check(bool condition) {
    if (!condition) throw std::runtime_error("owned mailbox allocator fault invariant");
}

int main() {
    using oreslang::runtime::ActorMailboxCompletion;
    using oreslang::runtime::BoundedActorPort;
    auto ledger = std::make_shared<ActorMailboxCompletion>(16);
    BoundedActorPort<int> port(8, 8 * sizeof(int), ledger);

    // The fixed slot pool is allocated at construction, before arming the
    // allocator fault. No allocation is allowed within offer_owned().
    for (int i = 0; i < 8; ++i) {
        auto owner = std::make_unique<int>(i);
        fail_next_allocation = true;
        const bool accepted = port.offer_owned(owner, sizeof(int));
        const bool did_not_allocate = fail_next_allocation;
        fail_next_allocation = false;
        check(accepted && did_not_allocate && !owner);
        check(ledger->snapshot() == static_cast<std::size_t>(i+1));
    }
    check(port.used_frames() == 8);
    auto extra = std::make_unique<int>(101);
    fail_next_allocation = true;
    check(!port.offer_owned(extra, sizeof(int))); // capacity denial
    check(fail_next_allocation && extra && *extra == 101);
    fail_next_allocation = false;

    for (int i = 0; i < 8; ++i) {
        auto delivery = port.try_receive();
        check(delivery && delivery->sequence() == static_cast<std::uint64_t>(i+1));
        check(delivery->id() == static_cast<std::uint64_t>(i+1));
        check(delivery->value() == i);
        check(delivery->acknowledge_released());
        ledger->complete(delivery->sequence());
    }
    check(port.used_frames() == 0 && port.used_bytes() == 0);
    check(ledger->watermark() == 8);

    // Non-owned offer allocates outside the mailbox lock and must restore
    // physical credits AND the ledger reservation on allocation failure.
    int value = 111;
    fail_next_allocation = true;
    bool threw = false;
    try { (void)port.offer(std::move(value), sizeof(int)); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_next_allocation = false;
    check(threw && value == 111);
    check(port.used_frames() == 0 && port.used_bytes() == 0);
    check(ledger->snapshot() == 8);
    check(port.offer(std::move(value), sizeof(int)));
    auto retry = port.try_receive();
    check(retry && retry->sequence() == 9 && retry->id() == 9);
    check(retry->value() == 111 && retry->acknowledge_released());
    ledger->complete(retry->sequence());
    check(ledger->watermark() == 9);
    port.close();
}
