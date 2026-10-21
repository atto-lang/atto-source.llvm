#include "oreslang/runtime/BoundedActorPort.hpp"

#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

using oreslang::runtime::BoundedActorPort;
static bool fail_next_allocation = false;

void* operator new(std::size_t size) {
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

struct MoveOnly {
    std::unique_ptr<int> item;
    explicit MoveOnly(int n) : item(std::make_unique<int>(n)) {}
    MoveOnly(MoveOnly&&) noexcept = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
};
static void check(bool ok) { if (!ok) throw std::runtime_error("port invariant broken"); }
template<class F> void rejected(F f) {
    try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error("expected bounded port failure");
}
int main() {
    using Delivery = BoundedActorPort<MoveOnly>::Delivery;
    static_assert(std::is_move_constructible<Delivery>::value,
                  "delivery must move into optional/consumer ownership");
    static_assert(!std::is_move_assignable<Delivery>::value,
                  "live delivery leases must not be overwriteable");

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

    // Any fallible allocation during admission must occur before the
    // sender is moved. A failed offer therefore leaves caller ownership and
    // quota accounting intact, and a subsequent retry can succeed.
    BoundedActorPort<MoveOnly> allocation_failure(1, 32);
    MoveOnly retryable(99);
    fail_next_allocation = true;
    bool allocation_rejected = false;
    try {
        (void)allocation_failure.offer(std::move(retryable), 16);
    } catch (const std::bad_alloc&) {
        allocation_rejected = true;
    }
    check(allocation_rejected);
    check(retryable.item != nullptr && *retryable.item == 99);
    check(allocation_failure.used_frames() == 0
          && allocation_failure.used_bytes() == 0);
    check(allocation_failure.offer(std::move(retryable), 16));
    check(retryable.item == nullptr);
    auto retried = allocation_failure.try_receive();
    check(retried && *retried->value().item == 99);
    check(retried->acknowledge_released());
    allocation_failure.close();

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
}
