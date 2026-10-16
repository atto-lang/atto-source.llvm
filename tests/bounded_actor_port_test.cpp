#include "oreslang/runtime/BoundedActorPort.hpp"

#include <memory>
#include <iostream>
#include <new>
#include <functional>
#include <utility>
#include <stdexcept>

using oreslang::runtime::BoundedActorPort;
struct MoveOnly {
    std::unique_ptr<int> item;
    explicit MoveOnly(int n) : item(std::make_unique<int>(n)) {}
    MoveOnly(MoveOnly&&) noexcept = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
};

// An unacknowledged lease must never be overwritten by move assignment:
// doing so would drop its payload without releasing its queued/checked-out
// frame and byte credits. Move construction is still needed for optional.
using OwnedDelivery = BoundedActorPort<MoveOnly>::Delivery;
static_assert(std::is_move_constructible_v<OwnedDelivery>);
static_assert(!std::is_move_assignable_v<OwnedDelivery>);
static_assert(!std::is_copy_constructible_v<OwnedDelivery>);

struct AllocFaultMoveOnly {
    std::unique_ptr<int> item;
    static inline bool fail_next_allocation = false;
    explicit AllocFaultMoveOnly(int value) : item(std::make_unique<int>(value)) {}
    AllocFaultMoveOnly(AllocFaultMoveOnly&&) noexcept = default;
    AllocFaultMoveOnly& operator=(AllocFaultMoveOnly&&) noexcept = default;
    AllocFaultMoveOnly(const AllocFaultMoveOnly&) = delete;
    AllocFaultMoveOnly& operator=(const AllocFaultMoveOnly&) = delete;
    static void* operator new(std::size_t size) {
        if (fail_next_allocation) throw std::bad_alloc();
        return ::operator new(size);
    }
    static void operator delete(void* ptr) noexcept { ::operator delete(ptr); }
};

// A payload destructor is arbitrary host code and may re-enter port
// observability. It must never execute under the non-recursive port mutex.
struct ReentrantPayload {
    std::function<void()> cleanup;
    explicit ReentrantPayload(std::function<void()> fn) : cleanup(std::move(fn)) {}
    ReentrantPayload(ReentrantPayload&&) noexcept = default;
    ReentrantPayload(const ReentrantPayload&) = delete;
    ~ReentrantPayload() { if (cleanup) cleanup(); }
};

static void check(bool ok) { if (!ok) throw std::runtime_error("port invariant broken"); }
template<class F> void rejected(F f) {
    try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error("expected bounded port failure");
}
int main() {
    BoundedActorPort<MoveOnly> port(1, 64);
    MoveOnly first(42), second(77);
    check(port.offer(std::move(first), 32));
    check(first.item == nullptr);
    check(port.used_frames() == 1 && port.used_bytes() == 32);
    check(!port.offer(std::move(second), 32));
    check(second.item != nullptr); // failed admission retained caller ownership
    auto delivery = port.try_receive();
    check(delivery.has_value() && *delivery->value().item == 42);
    check(port.used_frames() == 1);
    check(!port.offer(std::move(second), 32)); // receiving does not refund
    rejected([&] { port.close(); });
    check(delivery->acknowledge_released());
    check(!delivery->acknowledge_released());
    rejected([&] { (void)delivery->value(); });
    check(port.used_frames() == 0 && port.used_bytes() == 0);
    // close seals admission, including after earlier close failure
    rejected([&] { (void)port.offer(std::move(second), 32); });

    BoundedActorPort<MoveOnly> abandoned(1, 16);
    MoveOnly forgotten(1);
    check(abandoned.offer(std::move(forgotten), 16));
    { auto frame = abandoned.try_receive(); check(frame.has_value()); }
    rejected([&] { abandoned.close(); }); // missing post-fence ack retains quota

    BoundedActorPort<MoveOnly> clean(2, 16);
    MoveOnly a(2), b(3);
    check(clean.offer(std::move(a), 8));
    check(clean.offer(std::move(b), 8));
    auto x = clean.try_receive(), y = clean.try_receive();
    check(x && y);
    check(x->id() != y->id());
    check(x->acknowledge_released() && y->acknowledge_released());
    clean.close();

    // Moving the lease into a new owner is valid, unlike overwriting a
    // different live lease; exactly one receiver can acknowledge the credit.
    BoundedActorPort<MoveOnly> movedPort(1, 16);
    MoveOnly movable(91);
    check(movedPort.offer(std::move(movable), 16));
    auto originalDelivery = movedPort.try_receive();
    check(originalDelivery.has_value());
    OwnedDelivery movedDelivery(std::move(*originalDelivery));
    check(*movedDelivery.value().item == 91);
    rejected([&] { (void)originalDelivery->value(); });
    check(movedPort.used_frames() == 1 && movedPort.used_bytes() == 16);
    check(movedDelivery.acknowledge_released());
    check(!movedDelivery.acknowledge_released());
    check(movedPort.used_frames() == 0 && movedPort.used_bytes() == 0);
    movedPort.close();

    // Force the owned-envelope allocation to fail after queue slot
    // reservation. No byte credit, frame ID, or sender ownership is lost.
    BoundedActorPort<AllocFaultMoveOnly> faultPort(1, 16);
    AllocFaultMoveOnly failSender(61);
    AllocFaultMoveOnly::fail_next_allocation = true;
    bool allocationFailed = false;
    try {
        (void)faultPort.offer(std::move(failSender), 8);
    } catch (const std::bad_alloc&) {
        allocationFailed = true;
    }
    AllocFaultMoveOnly::fail_next_allocation = false;
    check(allocationFailed);
    check(failSender.item && *failSender.item == 61);
    check(faultPort.used_frames() == 0 && faultPort.used_bytes() == 0);
    check(!faultPort.try_receive().has_value());
    check(faultPort.offer(std::move(failSender), 8));
    check(!failSender.item);
    auto faultDelivery = faultPort.try_receive();
    check(faultDelivery && faultDelivery->id() == 1);
    check(*faultDelivery->value().item == 61);
    check(faultDelivery->acknowledge_released());
    check(faultPort.used_frames() == 0 && faultPort.used_bytes() == 0);
    faultPort.close();
    BoundedActorPort<ReentrantPayload> reentrant(1, 32);
    int cleanup_calls = 0;
    ReentrantPayload callback([&] {
        ++cleanup_calls;
        if (reentrant.used_frames() != 0 || reentrant.used_bytes() != 0)
            throw std::logic_error("reentrant cleanup observed unreleased credits");
    });
    check(reentrant.offer(std::move(callback), 8));
    auto reentrantDelivery = reentrant.try_receive();
    check(reentrantDelivery && reentrantDelivery->acknowledge_released());
    check(cleanup_calls == 1 && !reentrantDelivery->acknowledge_released());
    reentrant.close();

    std::cout << "PASS native owned actor port allocation rollback and acknowledged transfer\\n";
}
