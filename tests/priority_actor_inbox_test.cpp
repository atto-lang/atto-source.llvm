#include "oreslang/runtime/PriorityActorInbox.hpp"
#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/SupervisorControlTurn.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

using oreslang::runtime::PriorityActorInbox;
using oreslang::runtime::NativeActorGroup;
using oreslang::SupervisorControlTurn;
using oreslang::SupervisorFailureLane;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("priority inbox check: " #condition); } while (false)

using Inbox = PriorityActorInbox<int>;
using Lane = Inbox::Lane;
static_assert(!std::is_move_assignable_v<Inbox::Delivery>,
              "a checked-out priority delivery may not be overwritten");

struct SupervisorHooks final : SupervisorControlTurn::Hooks {
    int terminal = 0;
    int user = 0;
    SupervisorControlTurn::Decision on_throw(const SupervisorFailureLane::Failure&) override {
        throw std::logic_error("terminal Throw must be promoted");
    }
    SupervisorControlTurn::Decision on_raise(const SupervisorFailureLane::Failure&) override {
        throw std::logic_error("terminal Raise must be promoted");
    }
    SupervisorControlTurn::Decision on_panic(const SupervisorFailureLane::Failure&) override {
        ++terminal; return SupervisorControlTurn::Decision::Acknowledge;
    }
    SupervisorControlTurn::Decision on_cancel(const SupervisorFailureLane::Failure&) override {
        ++terminal; return SupervisorControlTurn::Decision::Acknowledge;
    }
    SupervisorControlTurn::Decision on_host_failure(const SupervisorFailureLane::Failure&) override {
        ++terminal; return SupervisorControlTurn::Decision::Acknowledge;
    }
    void on_escalate(const SupervisorFailureLane::Failure&) override {
        throw std::logic_error("unexpected escalation");
    }
    bool on_restart(const SupervisorFailureLane::Failure&) override { return false; }
    void on_user_turn() override { ++user; }
};

static void bounded_lanes_and_deterministic_fairness() {
    Inbox inbox({16, 128}, {8, 64}, {2, 16});
    for (int n = 0; n < 16; ++n) CHECK(inbox.offer(Lane::Urgent, int(n), 8));
    for (int n = 0; n < 8; ++n) CHECK(inbox.offer(Lane::Normal, 100 + n, 8));
    for (int n = 0; n < 2; ++n) CHECK(inbox.offer(Lane::Bulk, 200 + n, 8));
    CHECK(!inbox.offer(Lane::Urgent, 999, 8));
    CHECK(!inbox.offer(Lane::Normal, 999, 8));
    CHECK(!inbox.offer(Lane::Bulk, 999, 8));
    CHECK(inbox.used_bytes(Lane::Urgent) == 128);
    CHECK(inbox.used_bytes(Lane::Normal) == 64);
    CHECK(inbox.used_bytes(Lane::Bulk) == 16);
    int seen_urgent = 0, seen_normal = 0, seen_bulk = 0;
    for (int turn = 0; turn < 26; ++turn) {
        auto item = inbox.try_receive();
        CHECK(item.has_value());
        const int weight_slot = turn % 13;
        const Lane expected = weight_slot < 8 ? Lane::Urgent
                : weight_slot < 12 ? Lane::Normal : Lane::Bulk;
        CHECK(item->lane() == expected);
        if (expected == Lane::Urgent) CHECK(item->value() == seen_urgent++);
        if (expected == Lane::Normal) CHECK(item->value() == 100 + seen_normal++);
        if (expected == Lane::Bulk) CHECK(item->value() == 200 + seen_bulk++);
        CHECK(inbox.used_frames(expected) > 0); // dequeue never refunds
        CHECK(item->charged_bytes() == 8);
        CHECK(item->acknowledge_released());
        CHECK(!item->acknowledge_released());
    }
    CHECK(seen_urgent == 16 && seen_normal == 8 && seen_bulk == 2);
    CHECK(!inbox.try_receive());
    CHECK(inbox.used_frames(Lane::Urgent) == 0);
    CHECK(inbox.used_frames(Lane::Normal) == 0);
    CHECK(inbox.used_frames(Lane::Bulk) == 0);
    inbox.close();
}

static void sustained_urgent_flood_cannot_starve_normal_or_bulk() {
    Inbox inbox({16, 128}, {8, 64}, {2, 16});
    for (int i = 0; i < 16; ++i) CHECK(inbox.offer(Lane::Urgent, int(i), 8));
    CHECK(inbox.offer(Lane::Normal, 1000, 8));
    CHECK(inbox.offer(Lane::Bulk, 2000, 8));
    int normal_at = -1, bulk_at = -1;
    for (int tick = 1; tick <= 52; ++tick) {
        auto message = inbox.try_receive();
        CHECK(message.has_value());
        Lane lane = message->lane();
        CHECK(message->acknowledge_released());
        if (lane == Lane::Urgent) CHECK(inbox.offer(Lane::Urgent, 42, 8));
        if (lane == Lane::Normal && normal_at == -1) normal_at = tick;
        if (lane == Lane::Bulk && bulk_at == -1) bulk_at = tick;
    }
    CHECK(normal_at > 0 && normal_at <= 9);
    CHECK(bulk_at > 0 && bulk_at <= 13);
    // Drain remaining admitted URGENT messages and prove exact refund.
    while (auto item = inbox.try_receive()) CHECK(item->acknowledge_released());
    CHECK(inbox.used_frames(Lane::Urgent) == 0);
    CHECK(inbox.used_frames(Lane::Normal) == 0);
    CHECK(inbox.used_frames(Lane::Bulk) == 0);
    inbox.close();
}

static void seal_rejects_all_lanes_but_keeps_admitted_receipts_drainable() {
    Inbox inbox({1, 8}, {1, 8}, {1, 8});
    CHECK(inbox.offer(Lane::Urgent, 1, 8));
    CHECK(inbox.offer(Lane::Normal, 2, 8));
    CHECK(inbox.offer(Lane::Bulk, 3, 8));
    bool outstanding = false;
    try { inbox.close(); }
    catch (const std::logic_error&) { outstanding = true; }
    CHECK(outstanding);
    for (const auto lane : {Lane::Urgent, Lane::Normal, Lane::Bulk}) {
        bool rejected = false;
        try { (void)inbox.offer(lane, 55, 8); }
        catch (const std::logic_error&) { rejected = true; }
        CHECK(rejected);
    }
    int count = 0;
    while (auto receipt = inbox.try_receive()) {
        CHECK(receipt->acknowledge_released());
        ++count;
    }
    CHECK(count == 3);
    inbox.close();

    Inbox bad({1, 8}, {1, 8}, {1, 8});
    bool invalid = false;
    try { (void)bad.offer(static_cast<Lane>(255), 1, 8); }
    catch (const std::invalid_argument&) { invalid = true; }
    CHECK(invalid);
    bad.close();
}

static void supervisor_control_and_actor_single_flight_precede_user_priority() {
    Inbox inbox({1, 8}, {1, 8}, {1, 8});
    CHECK(inbox.offer(Lane::Urgent, 1, 8));
    CHECK(inbox.offer(Lane::Normal, 2, 8));
    CHECK(inbox.offer(Lane::Bulk, 3, 8));
    SupervisorControlTurn supervisor(1);
    SupervisorHooks hooks;
    SupervisorFailureLane::ActorKey child{"child", 4};
    supervisor.reserve_child(child);
    CHECK(supervisor.publish_child(child, SupervisorFailureLane::Kind::Panic, "broken"));
    CHECK(supervisor.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(hooks.terminal == 1 && hooks.user == 0);
    CHECK(inbox.used_frames(Lane::Urgent) == 1);
    CHECK(inbox.used_frames(Lane::Normal) == 1);
    CHECK(inbox.used_frames(Lane::Bulk) == 1);

    NativeActorGroup group(601, 1);
    auto actor = group.adopt(601, 12, 2, [] {});
    CHECK(group.mark_ready(actor));
    int control = 0, user = 0;
    CHECK(group.enqueue_control(actor, [&] { ++control; }));
    CHECK(group.run_user_turn(actor, [&] { ++user; }) ==
          NativeActorGroup::UserTurn::busy);
    CHECK(group.run_control_turn(actor) == NativeActorGroup::ControlTurn::executed);
    CHECK(control == 1 && user == 0);
    CHECK(group.run_user_turn(actor, [&] {
        auto message = inbox.try_receive();
        CHECK(message && message->lane() == Lane::Urgent);
        CHECK(message->acknowledge_released());
        ++user;
    }) == NativeActorGroup::UserTurn::executed);
    CHECK(user == 1);
    for (int i = 0; i < 2; ++i) {
        auto message = inbox.try_receive();
        CHECK(message && message->acknowledge_released());
    }
    CHECK(group.finalized(actor));
    actor.get_done().get();
    inbox.close();
}


static void failed_queue_publication_retains_sequence_and_window_credit() {
    oreslang::runtime::ActorMailboxCompletion ledger(2);
    CHECK(ledger.reserve_prepare());
    bool failed = false;
    try {
        (void)ledger.publish([](std::uint64_t) {
            throw std::runtime_error("injected publication failure");
        });
    } catch (const std::runtime_error&) { failed = true; }
    CHECK(failed);
    CHECK(ledger.snapshot() == 0);
    // The failed queue insertion still owns a *preparing* reservation until
    // rollback. Nothing may be considered delivered or completed.
    CHECK(ledger.reserve_prepare());
    CHECK(!ledger.reserve_prepare()); // finite admission window
    ledger.cancel_prepare();
    const auto first = ledger.publish([](std::uint64_t seq) { CHECK(seq == 1); });
    CHECK(first == 1 && ledger.snapshot() == 1);
    CHECK(!ledger.completed_through(1));
    ledger.complete(first);
    CHECK(ledger.completed_through(1));
    bool duplicate = false;
    try { ledger.complete(first); }
    catch (const std::logic_error&) { duplicate = true; }
    CHECK(duplicate);
}

static void global_drain_never_passes_overtaken_normal() {
    Inbox inbox({4, 32}, {4, 32}, {4, 32});
    CHECK(inbox.offer(Lane::Normal, 101, 8));
    const auto first_barrier = inbox.snapshot();
    CHECK(first_barrier == 1);
    CHECK(inbox.offer(Lane::Urgent, 201, 8));
    const auto both_barrier = inbox.snapshot();
    CHECK(both_barrier == 2);
    auto urgent = inbox.try_receive();
    CHECK(urgent && urgent->lane() == Lane::Urgent);
    CHECK(urgent->sequence() == 2);
    CHECK(urgent->acknowledge_released());
    CHECK(inbox.watermark() == 0);
    CHECK(!inbox.completed_through(first_barrier));
    CHECK(!inbox.completed_through(both_barrier));
    auto normal = inbox.try_receive();
    CHECK(normal && normal->lane() == Lane::Normal);
    CHECK(normal->sequence() == 1);
    CHECK(normal->acknowledge_released());
    CHECK(inbox.watermark() == 2);
    CHECK(inbox.completed_through(first_barrier));
    CHECK(inbox.completed_through(both_barrier));
    inbox.close();
}

static void stalled_ack_bounds_global_reordering_and_recovers() {
    // Even if URGENT keeps ACKing, a missing earlier NORMAL must not grow an
    // unbounded sparse completion set. Max 3 unfinished sequence positions.
    Inbox inbox({2, 16}, {2, 16}, {2, 16}, 3);
    CHECK(inbox.offer(Lane::Normal, 100, 8));
    auto held = inbox.try_receive();
    CHECK(held && held->lane() == Lane::Normal && held->sequence() == 1);
    for (int n = 0; n < 2; ++n) {
        CHECK(inbox.offer(Lane::Urgent, int(n), 8));
        auto urgent = inbox.try_receive();
        CHECK(urgent && urgent->lane() == Lane::Urgent);
        CHECK(urgent->sequence() == static_cast<std::uint64_t>(n + 2));
        CHECK(urgent->acknowledge_released());
    }
    CHECK(inbox.watermark() == 0 && inbox.snapshot() == 3);
    int refused = 42;
    CHECK(!inbox.offer(Lane::Bulk, std::move(refused), 8));
    CHECK(refused == 42); // denied before move, including global window denial
    CHECK(inbox.used_frames(Lane::Bulk) == 0);
    CHECK(held->acknowledge_released());
    CHECK(inbox.watermark() == 3);
    CHECK(inbox.offer(Lane::Bulk, 999, 8));
    CHECK(inbox.snapshot() == 4);
    auto bulk = inbox.try_receive();
    CHECK(bulk && bulk->sequence() == 4);
    CHECK(bulk->acknowledge_released());
    CHECK(inbox.completed_through(4));
    inbox.close();
}

struct FailOnceMove {
    static inline bool fail_move = false;
    int payload;
    explicit FailOnceMove(int v) : payload(v) {}
    FailOnceMove(FailOnceMove&& other) {
        if (fail_move) throw std::runtime_error("injected move fault");
        payload = other.payload;
    }
    FailOnceMove(const FailOnceMove&) = delete;
    FailOnceMove& operator=(const FailOnceMove&) = delete;
};

static void failed_admission_never_consumes_global_identity() {
    PriorityActorInbox<FailOnceMove> inbox({2, 16}, {2, 16}, {2, 16}, 2);
    FailOnceMove producer(77);
    FailOnceMove::fail_move = true;
    bool threw = false;
    try { (void)inbox.offer(PriorityActorInbox<FailOnceMove>::Lane::Normal,
                            std::move(producer), 8); }
    catch (const std::runtime_error&) { threw = true; }
    FailOnceMove::fail_move = false;
    CHECK(threw);
    CHECK(producer.payload == 77);
    CHECK(inbox.snapshot() == 0 && inbox.watermark() == 0);
    CHECK(inbox.used_frames(PriorityActorInbox<FailOnceMove>::Lane::Normal) == 0);
    CHECK(inbox.offer(PriorityActorInbox<FailOnceMove>::Lane::Normal,
                      std::move(producer), 8));
    auto accepted = inbox.try_receive();
    CHECK(accepted && accepted->sequence() == 1);
    CHECK(accepted->value().payload == 77);
    CHECK(accepted->acknowledge_released());
    CHECK(inbox.completed_through(1));
    inbox.close();
}

static void concurrent_publish_has_unique_global_sequences_and_no_credit_leaks() {
    constexpr int PER_LANE = 500, LANES = 3, TOTAL = PER_LANE * LANES;
    Inbox inbox({16, 128}, {16, 128}, {16, 128}, 64);
    const std::array<Lane, 3> lanes{Lane::Urgent, Lane::Normal, Lane::Bulk};
    std::atomic<int> delivered{0};
    std::array<std::thread, LANES> producers;
    for (int p = 0; p < LANES; ++p) {
        producers[p] = std::thread([&, p] {
            for (int n = 0; n < PER_LANE; ++n) {
                int payload = p * PER_LANE + n;
                while (!inbox.offer(lanes[p], std::move(payload), 8))
                    std::this_thread::yield();
            }
        });
    }
    std::vector<unsigned char> sequences(TOTAL + 1, 0);
    std::array<int, LANES> next_value{0, PER_LANE, 2 * PER_LANE};
    for (int i = 0; i < TOTAL; ++i) {
        auto message = inbox.try_receive();
        if (!message) { --i; std::this_thread::yield(); continue; }
        const auto sequence = message->sequence();
        CHECK(sequence > 0 && sequence <= TOTAL);
        CHECK(sequences[sequence] == 0);
        sequences[sequence] = 1;
        const auto lane = message->lane();
        const int producer_index = lane == Lane::Urgent ? 0 :
                                   lane == Lane::Normal ? 1 : 2;
        CHECK(message->value() == next_value[producer_index]++);
        CHECK(message->acknowledge_released());
        ++delivered;
    }
    for (auto& producer : producers) producer.join();
    CHECK(delivered == TOTAL);
    CHECK(inbox.snapshot() == TOTAL && inbox.watermark() == TOTAL);
    for (std::size_t n = 1; n < sequences.size(); ++n) CHECK(sequences[n]);
    CHECK(inbox.used_frames(Lane::Urgent) == 0);
    CHECK(inbox.used_frames(Lane::Normal) == 0);
    CHECK(inbox.used_frames(Lane::Bulk) == 0);
    inbox.close();
}

int main() {
    bounded_lanes_and_deterministic_fairness();
    sustained_urgent_flood_cannot_starve_normal_or_bulk();
    seal_rejects_all_lanes_but_keeps_admitted_receipts_drainable();
    supervisor_control_and_actor_single_flight_precede_user_priority();
    failed_queue_publication_retains_sequence_and_window_credit();
    global_drain_never_passes_overtaken_normal();
    stalled_ack_bounds_global_reordering_and_recovers();
    failed_admission_never_consumes_global_identity();
    concurrent_publish_has_unique_global_sequences_and_no_credit_leaks();
}
