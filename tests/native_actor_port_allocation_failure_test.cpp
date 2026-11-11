#include "oreslang/runtime/BoundedActorPort.hpp"
#include "oreslang/runtime/ActorMailboxCompletion.hpp"

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

// Thread-local, one-shot injected allocator fault: no global OOM and no
// other thread can consume our fault. All payloads are allocated before
// arming the fault: offer_owned must NOT allocate during queue publication.
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
    auto ledger = std::make_shared<ActorMailboxCompletion>(512);
    BoundedActorPort<int> port(512, 512 * sizeof(int), ledger);
    // Physical slot memory was allocated by the constructor. The shared
    // completion ledger was initialized before any injection. Neither
    // offer_owned nor a full-capacity rejection may allocate.
    for (int i = 0; i < 512; ++i) {
        auto owner = std::make_unique<int>(i);
        const auto before = ledger->snapshot();
        fail_next_allocation = true;
        bool published = false;
        try { published = port.offer_owned(owner, sizeof(int), 11); }
        catch (...) { fail_next_allocation = false; throw; }
        check(published && !owner);
        check(fail_next_allocation); // injected operator new never called
        fail_next_allocation = false;
        check(ledger->snapshot() == before + 1);
        check(port.used_frames() == static_cast<std::size_t>(i + 1));
    }
    auto rejected_owner = std::make_unique<int>(777);
    fail_next_allocation = true;
    check(!port.offer_owned(rejected_owner, sizeof(int), 11));
    check(fail_next_allocation && rejected_owner && *rejected_owner == 777);
    fail_next_allocation = false;
    check(ledger->snapshot() == 512);
    for (int i = 0; i < 512; ++i) {
        auto delivery = port.try_receive();
        check(delivery && delivery->sequence() == static_cast<std::uint64_t>(i + 1));
        check(delivery->id() == static_cast<std::uint64_t>(i + 1));
        check(delivery->value() == i);
        check(delivery->acknowledge_released());
        ledger->complete(delivery->sequence());
    }
    check(port.used_frames() == 0 && port.used_bytes() == 0);
    check(ledger->watermark() == 512);

    // offer(T&&) is allowed to allocate for its standalone payload owner,
    // but an allocation failure MUST roll back every reservation. The
    // producer still owns the original T and no sequence is published.
    int retry = 1234;
    fail_next_allocation = true;
    bool injected_oom = false;
    try { (void)port.offer(std::move(retry), sizeof(int), 17); }
    catch (const std::bad_alloc&) { injected_oom = true; }
    fail_next_allocation = false;
    check(injected_oom && retry == 1234);
    check(port.used_frames() == 0 && port.used_bytes() == 0);
    check(ledger->snapshot() == 512);
    check(port.offer(std::move(retry), sizeof(int), 17));
    auto last = port.try_receive_tagged(17, 2);
    check(last.status == BoundedActorPort<int>::SelectiveStatus::Matched);
    check(last.delivery && last.delivery->id() == 513);
    check(last.delivery->sequence() == 513 && last.delivery->value() == 1234);
    check(last.delivery->acknowledge_released());
    ledger->complete(last.delivery->sequence());
    check(ledger->completed_through(513));
    port.close();
}
