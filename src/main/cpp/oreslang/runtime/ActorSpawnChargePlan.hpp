#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace oreslang::spawn_charge {
// Fixed-profile host calculation only; pool authorization and dispatch are external.
enum class Affinity { none, sticky, pinned };
struct Policy {
    bool serial;
    std::int32_t carrier_cap, default_carriers;
    std::int64_t live_stack_slots, heap_limit, stack_limit, mailbox_messages_limit, mailbox_byte_charge;
};
struct Defaults { Affinity affinity; std::int64_t heap_bytes, stack_bytes, mailbox_messages; };
struct Request {
    std::optional<std::int32_t> carriers;
    std::optional<Affinity> affinity;
    std::optional<std::int64_t> heap_bytes, stack_bytes, mailbox_messages;
};
struct Plan {
    std::int32_t carrier_cap;
    Affinity affinity;
    std::int64_t stack_bytes, heap, stacks, mailbox_bytes, messages;
};
inline Plan checked(const Policy& p, std::int32_t carriers, Affinity affinity,
                    std::int64_t heap, std::int64_t stack, std::int64_t messages) {
    if (carriers <= 0 || carriers > p.carrier_cap ||
        (affinity != Affinity::none && affinity != Affinity::sticky && affinity != Affinity::pinned) ||
        (affinity == Affinity::pinned && carriers != 1))
        throw std::invalid_argument("E_SPAWN_POLICY");
    if (heap <= 0 || heap > p.heap_limit || stack <= 0 || stack > p.stack_limit ||
        messages <= 0 || messages > p.mailbox_messages_limit ||
        p.live_stack_slots <= 0 || stack > std::numeric_limits<std::int64_t>::max() / p.live_stack_slots)
        throw std::invalid_argument("E_SPAWN_QUOTA");
    return {carriers, affinity, stack, heap, stack * p.live_stack_slots, p.mailbox_byte_charge, messages};
}
inline Plan resolve(const Policy& p, const Defaults& d, const Request& r) {
    if (p.carrier_cap <= 0 || p.default_carriers <= 0 || p.default_carriers > p.carrier_cap ||
        p.live_stack_slots <= 0 || p.heap_limit <= 0 || p.stack_limit <= 0 ||
        p.mailbox_messages_limit <= 0 || p.mailbox_byte_charge <= 0 ||
        (p.serial && (p.carrier_cap != 1 || p.default_carriers != 1)))
        throw std::invalid_argument("E_SPAWN_POLICY");
    (void)checked(p, p.default_carriers, d.affinity, d.heap_bytes, d.stack_bytes, d.mailbox_messages);
    return checked(p, r.carriers.value_or(p.default_carriers), r.affinity.value_or(d.affinity),
        r.heap_bytes.value_or(d.heap_bytes), r.stack_bytes.value_or(d.stack_bytes),
        r.mailbox_messages.value_or(d.mailbox_messages));
}
} // namespace oreslang::spawn_charge
