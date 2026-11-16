// Real host-runtime BoundedActorPort / PriorityActorInbox regression tests.
// These are NOT AttoLang guest source or AOT compilation evidence.
#include "oreslang/runtime/BoundedActorPort.hpp"
#include "oreslang/runtime/PriorityActorInbox.hpp"
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <thread>
#include <deque>
#include <random>
#include <algorithm>
#include <utility>

using oreslang::runtime::BoundedActorPort;
using oreslang::runtime::PriorityActorInbox;

#define CHECK(x) do { if (!(x)) throw std::runtime_error("selective port contract: " #x); } while (false)

static_assert(!std::is_aggregate_v<BoundedActorPort<int>::SelectiveCursor>,
              "tagged selective resume cursors must not expose forgeable fields");

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
    CHECK(yielded.inspected == 2 && yielded.cursor.through_sequence() == 2);
    CHECK(p.used_frames() == 3 && p.used_bytes() == 48);
    expect_reject([&] { (void)p.try_receive_tagged(payment, 2, yielded.cursor); });
    expect_reject([&] { (void)p.try_receive_tagged(shutdown, 0, yielded.cursor); });
    Port other(1, 16);
    expect_reject([&] { (void)other.try_receive_tagged(shutdown, 2, yielded.cursor); });

    auto selected = p.try_receive_tagged(shutdown, 1, yielded.cursor);
    CHECK(selected.status == Status::Matched && selected.delivery);
    CHECK(selected.inspected == 1 && *selected.delivery->value().value == 300);
    // Dequeue is custody transfer, not admission credit refund.
    // Regression: despite a free physical queue slot, an unacknowledged
    // selected receipt must keep both frame and byte windows saturated.
    CHECK(p.used_frames() == 3 && p.used_bytes() == 48);
    MoveOnlyMessage while_inflight(410);
    CHECK(!p.offer(std::move(while_inflight), 16, payment));
    CHECK(while_inflight.value && *while_inflight.value == 410);
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
    // A completed no-match scan has no retained cursor; new publication
    // must be visible on the next authorized scan, not hidden forever.
    MoveOnlyMessage late(500), legacy(600);
    CHECK(p.offer(std::move(legacy), 8)); // tag 0: FIFO-only legacy traffic
    expect_reject([&] { (void)p.try_receive_tagged(0, 8); });
    CHECK(p.offer(std::move(late), 8, shutdown));
    auto after_empty = p.try_receive_tagged(shutdown, 2);
    CHECK(after_empty.status == Status::Matched && after_empty.delivery);
    CHECK(*after_empty.delivery->value().value == 500);
    CHECK(after_empty.delivery->acknowledge_released());
    auto legacy_fifo = p.try_receive();
    CHECK(legacy_fifo && *legacy_fifo->value().value == 600);
    CHECK(legacy_fifo->acknowledge_released());
    CHECK(p.used_frames() == 0 && p.used_bytes() == 0);
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

// A real bound NativeActorGroup owns the turn; privileged CONTROL is polled
// before each selective data scan. Stop/retirement then aborts only unmatched
// queued work, never re-ACKs the already checked-out matching receipt.
static void bound_actor_turn_selective() {
    using Inbox = PriorityActorInbox<MoveOnlyMessage>;
    using Lane = Inbox::Lane;
    using Status = Inbox::SelectiveDispatch;
    oreslang::runtime::NativeActorGroup group(12034, 1);
    auto actor = group.adopt(12034, 55, 1, [] {});
    auto finalizer = group.runtime_finalizer(actor);
    CHECK(group.mark_ready(actor));
    Inbox inbox({2, 32}, {4, 64}, {2, 32}, 8);
    inbox.bind_target(actor);

    MoveOnlyMessage a(10), b(20), c(30);
    CHECK(inbox.offer_to(actor, Lane::Normal, std::move(a), 8, 11));
    CHECK(inbox.offer_to(actor, Lane::Normal, std::move(b), 8, 17));
    CHECK(inbox.offer_to(actor, Lane::Normal, std::move(c), 8, 23));
    // The low-level lane probe cannot bypass an owned actor's CONTROL
    // and user-turn gates, even when the caller holds the target inbox.
    bool bypass_rejected = false;
    try {
        (void)inbox.try_receive_tagged(Lane::Normal, 23, 1);
    } catch (const std::logic_error&) { bypass_rejected = true; }
    CHECK(bypass_rejected && inbox.used_frames(Lane::Normal) == 3);
    // The legacy FIFO probe must also fail closed: accepting an ordinary
    // receive would allow guest code to bypass supervisor CONTROL entirely.
    bool fifo_bypass_rejected = false;
    try {
        (void)inbox.try_receive();
    } catch (const std::logic_error&) { fifo_bypass_rejected = true; }
    CHECK(fifo_bypass_rejected && inbox.used_frames(Lane::Normal) == 3);

    Inbox::SelectiveCursor cursor;
    bool processed = false;
    auto consumer = [&](Inbox::Delivery& delivery) {
        CHECK(!processed);
        processed = true;
        CHECK(delivery.lane() == Lane::Normal);
        CHECK(*delivery.value().value == 30);
        CHECK(delivery.acknowledge_released());
    };
    // A queued supervisor CONTROL turn must win without inspecting the
    // user mailbox, even with an already-published matching tag.
    bool control_executed = false;
    CHECK(group.enqueue_control(actor, [&] { control_executed = true; }));
    CHECK(inbox.dispatch_next_tagged(group, actor, Lane::Normal, 23, 1,
                                    cursor, consumer) == Status::Control);
    CHECK(control_executed && !processed);
    CHECK(inbox.used_frames(Lane::Normal) == 3);
    CHECK(inbox.dispatch_next_tagged(group, actor, Lane::Normal, 23, 1,
                                    cursor, consumer) == Status::Resume);
    CHECK(!processed && inbox.used_frames(Lane::Normal) == 3);
    // CONTROL arriving between two bounded scan slices must preempt the
    // continuation before it can inspect or check out the matching DATA.
    bool mid_scan_control_executed = false;
    CHECK(group.enqueue_control(actor, [&] { mid_scan_control_executed = true; }));
    CHECK(inbox.dispatch_next_tagged(group, actor, Lane::Normal, 23, 1,
                                    cursor, consumer) == Status::Control);
    CHECK(mid_scan_control_executed && !processed);
    CHECK(inbox.used_frames(Lane::Normal) == 3);
    CHECK(inbox.dispatch_next_tagged(group, actor, Lane::Normal, 23, 1,
                                    cursor, consumer) == Status::Resume);
    CHECK(inbox.dispatch_next_tagged(group, actor, Lane::Normal, 23, 1,
                                    cursor, consumer) == Status::User);
    CHECK(processed && inbox.used_frames(Lane::Normal) == 2);
    CHECK(inbox.watermark() == 0);
    group.close();
    CHECK(inbox.dispatch_next_tagged(group, actor, Lane::Normal, 23, 1,
                                    cursor, consumer) == Status::Retired);
    CHECK(finalizer.ready_for_actor_turn_quiescence());
    CHECK(inbox.retire_queued_after_quiescence(finalizer, actor) == 2);
    CHECK(inbox.watermark() == 3);
    CHECK(inbox.drain_status(3) == Inbox::DrainStatus::Aborted);
    CHECK(finalizer.ready_for_resource_reclamation());
    CHECK(finalizer.finalized());
    actor.get_done().get();
    inbox.close();
}

// Independent fixed-seed FIFO oracle: the production port must select the
// first matching tag, never disturb unmatched order, and never refund credits
// until a receipt acknowledges. This is native host modeling, NOT guest AOT.
static void randomized_tagged_dequeue() {
    using Port = BoundedActorPort<MoveOnlyMessage>;
    using Status = Port::SelectiveStatus;
    Port p(16, 256);
    std::deque<std::pair<std::uint32_t, int>> expected;
    std::mt19937 rng(20261110);
    int next_value = 1;
    for (int turn = 0; turn < 8000; ++turn) {
        const auto op = rng() % 3;
        if (op == 0) {
            const std::uint32_t tag = 1 + rng() % 4;
            MoveOnlyMessage m(next_value);
            const bool admitted = p.offer(std::move(m), 8, tag);
            CHECK(admitted == (expected.size() < 16));
            if (admitted) {
                expected.emplace_back(tag, next_value);
                ++next_value;
            } else CHECK(m.value && *m.value == next_value);
        } else if (op == 1) {
            const auto tag = std::uint32_t(1 + rng() % 4);
            const auto hit = std::find_if(expected.begin(), expected.end(),
                [&](const auto& entry) { return entry.first == tag; });
            const bool should_match = hit != expected.end();
            bool matched = false, terminal = false;
            Port::SelectiveCursor cursor;
            for (int budget_turn = 0; budget_turn < 18 && !terminal; ++budget_turn) {
                auto selected = p.try_receive_tagged(tag, 1, cursor);
                CHECK(selected.inspected <= 1);
                if (selected.status == Status::Continue) {
                    CHECK(!selected.delivery);
                    cursor = std::move(selected.cursor);
                    continue;
                }
                terminal = true;
                if (selected.status == Status::Matched) {
                    CHECK(should_match && selected.delivery);
                    CHECK(*selected.delivery->value().value == hit->second);
                    CHECK(selected.delivery->acknowledge_released());
                    CHECK(!selected.delivery->acknowledge_released());
                    expected.erase(hit);
                    matched = true;
                } else CHECK(selected.status == Status::NoMatch && !should_match);
            }
            CHECK(terminal && matched == should_match);
        } else {
            auto received = p.try_receive();
            CHECK(bool(received) == !expected.empty());
            if (received) {
                CHECK(*received->value().value == expected.front().second);
                expected.pop_front();
                CHECK(received->acknowledge_released());
            }
        }
        CHECK(p.used_frames() == expected.size());
        CHECK(p.used_bytes() == expected.size() * 8);
    }
    while (auto r = p.try_receive()) {
        CHECK(!expected.empty());
        CHECK(*r->value().value == expected.front().second);
        expected.pop_front();
        CHECK(r->acknowledge_released());
    }
    CHECK(expected.empty() && p.used_frames() == 0 && p.used_bytes() == 0);
    p.close();
}

// Selective ACK may complete a newer sequence before an earlier queued
// receipt. As in every other port test, the consumer reports each receipt's
// global completion explicitly after its physical ACK; the port itself only
// refunds frame/byte credit. The global completion window must stay bounded
// by the contiguous watermark, even after physical frame credits are
// refunded out of order.
static void selective_ack_does_not_skip_global_completion_window() {
    using Port = BoundedActorPort<MoveOnlyMessage>;
    using Ledger = oreslang::runtime::ActorMailboxCompletion;
    auto ledger = std::make_shared<Ledger>(3);
    Port port(4, 64, ledger);
    MoveOnlyMessage one(1), two(2), three(3), four(4);
    CHECK(port.offer(std::move(one), 8, 11));
    CHECK(port.offer(std::move(two), 8, 17));
    CHECK(port.offer(std::move(three), 8, 23));
    CHECK(ledger->snapshot() == 3);
    auto newest = port.try_receive_tagged(23, 3);
    CHECK(newest.status == Port::SelectiveStatus::Matched && newest.delivery);
    CHECK(newest.delivery->sequence() == 3);
    CHECK(newest.delivery->acknowledge_released());
    ledger->complete(newest.delivery->sequence());
    CHECK(port.used_frames() == 2);
    CHECK(ledger->watermark() == 0);
    CHECK(!port.offer(std::move(four), 8, 23));
    CHECK(four.value && *four.value == 4);
    auto first = port.try_receive();
    CHECK(first && first->sequence() == 1);
    CHECK(first->acknowledge_released());
    ledger->complete(first->sequence());
    CHECK(ledger->watermark() == 1);
    CHECK(port.offer(std::move(four), 8, 23));
    CHECK(ledger->snapshot() == 4);
    auto fourth = port.try_receive_tagged(23, 4);
    CHECK(fourth.status == Port::SelectiveStatus::Matched && fourth.delivery);
    CHECK(fourth.delivery->sequence() == 4);
    CHECK(fourth.delivery->acknowledge_released());
    ledger->complete(fourth.delivery->sequence());
    CHECK(ledger->watermark() == 1);
    auto second = port.try_receive();
    CHECK(second && second->sequence() == 2);
    CHECK(second->acknowledge_released());
    ledger->complete(second->sequence());
    CHECK(ledger->watermark() == 4);
    CHECK(ledger->completed_through(4));
    CHECK(port.used_frames() == 0 && port.used_bytes() == 0);
    port.close();
}

int main() {
    selective_ack_does_not_skip_global_completion_window();
    bounded_port_selective();
    actual_priority_inbox_lane();
    bound_actor_turn_selective();
    randomized_tagged_dequeue();
}
