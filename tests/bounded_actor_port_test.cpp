#include "oreslang/runtime/BoundedActorPort.hpp"

#include <memory>
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
}
