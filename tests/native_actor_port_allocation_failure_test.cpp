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
// arming the fault; the only expected new inside offer_owned is deque growth.
static thread_local bool fail_next_allocation = false;

void* operator new(std::size_t count) {
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc();
    }
    if (void* ptr = std::malloc(count == 0 ? 1 : count)) return ptr;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }

static void check(bool condition) {
    if (!condition) throw std::runtime_error("owned mailbox allocator fault invariant");
}

int main() {
    using oreslang::runtime::ActorMailboxCompletion;
    using oreslang::runtime::BoundedActorPort;
    auto ledger = std::make_shared<ActorMailboxCompletion>(512);
    BoundedActorPort<int> port(512, 512 * sizeof(int), ledger);
    bool injected = false;
    std::size_t committed = 0;
    for (int i = 0; i < 512; ++i) {
        auto owner = std::make_unique<int>(i);
        const auto pre_frames = port.used_frames();
        const auto pre_bytes = port.used_bytes();
        const auto pre_sequence = ledger->snapshot();
        fail_next_allocation = true;
        bool threw = false;
        try {
            check(port.offer_owned(owner, sizeof(int)));
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        fail_next_allocation = false;
        if (threw) {
            // This proves both global admission rollback and caller ownership.
            check(owner && *owner == i);
            check(port.used_frames() == pre_frames);
            check(port.used_bytes() == pre_bytes);
            check(ledger->snapshot() == pre_sequence);
            injected = true;
            // The failed attempt must not produce a sequence gap, even if
            // urgent/normal consumers already hold older receipts.
            check(port.offer_owned(owner, sizeof(int)));
            check(!owner);
            ++committed;
            break;
        }
        check(!owner);
        ++committed;
    }
    check(injected);
    check(port.used_frames() == committed);
    check(ledger->snapshot() == committed);
    for (std::size_t sequence = 1; sequence <= committed; ++sequence) {
        auto delivery = port.try_receive();
        check(delivery.has_value());
        check(delivery->sequence() == sequence);
        check(delivery->value() == static_cast<int>(sequence - 1));
        check(delivery->acknowledge_released());
        // BoundedActorPort is intentionally lower-level; caller records ACK.
        ledger->complete(delivery->sequence());
    }
    check(port.used_frames() == 0);
    check(port.used_bytes() == 0);
    check(ledger->watermark() == committed);
    port.close();
}
