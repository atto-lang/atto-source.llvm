// Real host-runtime BoundedActorPort / PriorityActorInbox regression tests.
// These are NOT AttoLang guest source or AOT compilation evidence.
#include "oreslang/runtime/BoundedActorPort.hpp"
#include "oreslang/runtime/PriorityActorInbox.hpp"
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

using oreslang::runtime::BoundedActorPort;
using oreslang::runtime::PriorityActorInbox;

#define CHECK(x) do { if (!(x)) throw std::runtime_error("selective port contract: " #x); } while (false)

struct MoveOnlyMessage {
    std::unique_ptr<int> value;
    explicit MoveOnlyMessage(int v) : value(std::make_unique<int>(v)) {}
    MoveOnlyMessage(MoveOnlyMessage&&) noexcept = default;
    MoveOnlyMessage& operator=(MoveOnlyMessage&&) = delete;
    MoveOnlyMessage(const MoveOnlyMessage&) = delete;
    MoveOnlyMessage& operator=(const MoveOnlyMessage&) = delete;
};

template <typename F> void expect_reject(F&& f) {
    bool rejected = false;
    try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

static void bounded_port_selective() {
    using Port = BoundedActorPort<MoveOnlyMessage>;
    using Status = Port::SelectiveStatus;
    constexpr std::uint32_t payment = 11, inventory = 17, shutdown = 23;
    Port p(3, 48);
    MoveOnlyMessage a(100), b(200), c(300), overflow(400);
    CHECK(p.offer(std::move(a), 16, payment));
    CHECK(p.offer(std::move(b), 16, inventory));
    CHECK(p.offer(std::move(c), 16, shutdown));
    CHECK(p.used_frames() == 3 && p.used_bytes() == 48);
    CHECK(!p.offer(std::move(overflow), 16, shutdown));
    CHECK(*overflow.value == 400); // failed offer did not move payload

    // Budgeted scan yields WITHOUT consuming even a single unmatched message.
    auto yielded = p.try_receive_tagged(shutdown, 2);
    CHECK(yielded.status == Status::Continue && !yielded.delivery);
    CHECK(yielded.inspected == 2 && yielded.cursor.through_sequence == 2);
    CHECK(p.used_frames() == 3 && p.used_bytes() == 48);
    expect_reject([&] { (void)p.try_receive_tagged(payment, 2, yielded.cursor); });
    expect_reject([&] { (void)p.try_receive_tagged(shutdown, 0, yielded.cursor); });
    Port other(1, 16);
    expect_reject([&] { (void)other.try_receive_tagged(shutdown, 2, yielded.cursor); });

    auto selected = p.try_receive_tagged(shutdown, 1, yielded.cursor);
    CHECK(selected.status == Status::Matched && selected.delivery);
    CHECK(selected.inspected == 1 && *selected.delivery->value().value == 300);
    // Even checked-out receives hold bytes until physical ACK.
    CHECK(p.used_frames() == 3 && p.used_bytes() == 48);
    CHECK(selected.delivery->acknowledge_released());
    CHECK(!selected.delivery->acknowledge_released());
    CHECK(p.used_frames() == 2 && p.used_bytes() == 32);

    // The original nonmatching prefix has not changed order or identity.
    auto first = p.try_receive();
    auto second = p.try_receive();
    CHECK(first && second);
    CHECK(first->id() == 1 && second->id() == 2);
    CHECK(*first->value().value == 100);
    CHECK(*second->value().value == 200);
    CHECK(first->acknowledge_released());
    CHECK(second->acknowledge_released());
    CHECK(p.used_frames() == 0 && p.used_bytes() == 0);
    auto no_match = p.try_receive_tagged(shutdown, 4);
    CHECK(no_match.status == Status::NoMatch && !no_match.delivery);
    p.close();
    other.close();

    // Owned transfer avoids a payload move and retains tagged identity.
    Port owned_port(2, 32);
    auto owned = std::make_unique<MoveOnlyMessage>(55);
    CHECK(owned_port.offer_owned(owned, 16, inventory));
    CHECK(!owned);
    auto delivery = owned_port.try_receive_tagged(inventory, 2);
    CHECK(delivery.status == Status::Matched && delivery.delivery);
    CHECK(*delivery.delivery->value().value == 55);
    CHECK(delivery.delivery->acknowledge_released());
    owned_port.close();

    // Old cursor is tied to the physical port and cannot be replayed on
    // a replacement incarnation even if its message tag is identical.
    Port current(2, 32);
    MoveOnlyMessage first_pending(1), second_pending(2);
    CHECK(current.offer(std::move(first_pending), 8, payment));
    CHECK(current.offer(std::move(second_pending), 8, inventory));
    auto token = current.try_receive_tagged(shutdown, 1);
    CHECK(token.status == Status::Continue);
    Port replacement(2, 32);
    expect_reject([&] { (void)replacement.try_receive_tagged(shutdown, 1, token.cursor); });
    auto r1 = current.try_receive(), r2 = current.try_receive();
    CHECK(r1 && r2 && r1->acknowledge_released() && r2->acknowledge_released());
    current.close();
    replacement.close();
}

static void actual_priority_inbox_lane() {
    using Inbox = PriorityActorInbox<MoveOnlyMessage>;
    using Status = Inbox::SelectiveStatus;
    using Lane = Inbox::Lane;
    Inbox inbox({2, 32}, {4, 64}, {2, 32}, 8);
    MoveOnlyMessage a(1), b(2), c(3), d(4);
    CHECK(inbox.offer(Lane::Normal, std::move(a), 8, 11));
    CHECK(inbox.offer(Lane::Normal, std::move(b), 8, 17));
    CHECK(inbox.offer(Lane::Normal, std::move(c), 8, 23));
    CHECK(inbox.offer(Lane::Urgent, std::move(d), 8, 23));
    const auto snapshot = inbox.snapshot();
    CHECK(snapshot == 4);
    auto pause = inbox.try_receive_tagged(Lane::Normal, 23, 2);
    CHECK(pause.status == Status::Continue && pause.inspected == 2);
    CHECK(inbox.used_frames(Lane::Normal) == 3);
    auto match = inbox.try_receive_tagged(Lane::Normal, 23, 1, pause.cursor);
    CHECK(match.status == Status::Matched && match.delivery);
    CHECK(match.delivery->lane() == Lane::Normal);
    CHECK(*match.delivery->value().value == 3);
    CHECK(match.delivery->acknowledge_released());
    CHECK(inbox.snapshot() == snapshot);  // dequeue never fabricates a publication
    CHECK(inbox.used_frames(Lane::Normal) == 2);
    CHECK(inbox.watermark() == 0);   // Earlier unconsumed sequences remain pending

    // Ordinary weighted priority dequeue remains intact; unmatched Normal
    // prefix is not reordered or charged differently by selective scanning.
    auto urgent = inbox.try_receive();
    CHECK(urgent && urgent->lane() == Lane::Urgent);
    CHECK(*urgent->value().value == 4);
    CHECK(urgent->acknowledge_released());
    auto first = inbox.try_receive();
    auto second = inbox.try_receive();
    CHECK(first && second);
    CHECK(first->lane() == Lane::Normal && second->lane() == Lane::Normal);
    CHECK(*first->value().value == 1 && *second->value().value == 2);
    CHECK(first->acknowledge_released() && second->acknowledge_released());
    CHECK(inbox.watermark() == snapshot);
    CHECK(inbox.completed_through(snapshot));
    inbox.close();
}

int main() {
    bounded_port_selective();
    actual_priority_inbox_lane();
}
