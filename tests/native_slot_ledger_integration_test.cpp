#include "oreslang/runtime/ActorMailboxCompletion.hpp"
#include "oreslang/runtime/PreallocatedSelectiveSlots.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#define CHECK(x) do { if (!(x)) throw std::runtime_error("slot-ledger integration: " #x); } while (false)

using oreslang::runtime::ActorMailboxCompletion;
using Slots = oreslang::runtime::PreallocatedSelectiveSlots<int>;

int main() {
    auto ledger = std::make_shared<ActorMailboxCompletion>(3);
    Slots normal(2), urgent(2);
    auto publish = [&](Slots& queue, int value, std::uint32_t tag,
                       std::uint64_t local_id) {
        auto payload = std::make_unique<int>(value);
        CHECK(ledger->reserve_prepare());
        std::uint64_t result = 0;
        result = ledger->publish([&](std::uint64_t sequence) {
            CHECK(queue.push(payload, sizeof(int), tag, sequence, local_id));
        });
        CHECK(!payload);
        return result;
    };
    CHECK(publish(normal, 10, 11, 1) == 1);
    CHECK(publish(urgent, 20, 23, 1) == 2);
    CHECK(publish(normal, 30, 11, 2) == 3);
    CHECK(ledger->snapshot() == 3);
    CHECK(!ledger->reserve_prepare()); // completion window is saturated

    // Selective receipt of sequence 2 does not refund the earlier gap.
    auto second = urgent.select(23, 1);
    CHECK(second.status == Slots::Status::Matched && second.frame);
    CHECK(second.frame->sequence == 2 && second.frame->id == 1);
    CHECK(*second.frame->payload == 20);
    CHECK(ledger->watermark() == 0);
    second.frame->payload.reset(); // physical owner release, then ledger ACK
    ledger->complete(second.frame->sequence);
    CHECK(ledger->watermark() == 0);
    CHECK(!ledger->reserve_prepare());
    second.frame.reset();

    auto first = normal.pop_front();
    CHECK(first && first->sequence == 1 && first->id == 1);
    CHECK(*first->payload == 10);
    first->payload.reset();
    ledger->complete(first->sequence);
    CHECK(ledger->watermark() == 2);
    first.reset();

    // A contiguous ACK opens exactly two completion-window slots.
    CHECK(publish(urgent, 40, 23, 2) == 4);
    auto newest = urgent.select(23, 1);
    CHECK(newest.status == Slots::Status::Matched && newest.frame);
    CHECK(newest.frame->sequence == 4 && newest.frame->id == 2);
    newest.frame->payload.reset();
    ledger->complete(newest.frame->sequence);
    CHECK(ledger->watermark() == 2);
    newest.frame.reset();

    auto third = normal.pop_front();
    CHECK(third && third->id == 2 && third->sequence == 3);
    third->payload.reset();
    ledger->complete(third->sequence);
    third.reset();
    CHECK(ledger->watermark() == 4 && ledger->completed_through(4));
    CHECK(normal.size() == 0 && urgent.size() == 0);
    CHECK(ledger->drain_status(4) == ActorMailboxCompletion::DrainStatus::Delivered);
}
