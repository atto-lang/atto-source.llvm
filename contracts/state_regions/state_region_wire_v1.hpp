#pragma once
// v1 JSON/WASM interoperable *metadata*, never a runtime access capability.
// The native task-bound gate must still authenticate the live scheduler ticket.
#include <cstdint>
#include <string_view>

namespace ores::state::wire {
constexpr std::uint64_t kMaxJsonSafeId = 9007199254740991ULL;
enum class Kind : std::uint8_t { read, write };
enum class Action : std::uint8_t { borrow, release };

constexpr bool identity(std::uint64_t id) noexcept {
    return id != 0 && id <= kMaxJsonSafeId;
}
constexpr bool kind_valid(Kind k) noexcept {
    return k == Kind::read || k == Kind::write;
}
constexpr bool action_valid(Action a) noexcept {
    return a == Action::borrow || a == Action::release;
}
constexpr bool nonblank(std::string_view s) noexcept {
    for (const char c : s)
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t' &&
            c != '\v' && c != '\f') return true;
    return false;
}

// Compiler-side requirement is SYMBOLIC, not a minted task/region identity.
struct Requirement {
    std::string_view region_binding;
    bool current_logical_execution;
    bool generation_fence;
    std::uint64_t loan_id;
    std::string_view place;
    Kind kind;
    Action action;
    constexpr bool valid() const noexcept {
        return nonblank(region_binding) && current_logical_execution &&
            generation_fence && identity(loan_id) &&
            nonblank(place) && kind_valid(kind) && action_valid(action);
    }
    constexpr bool authorizes_release(const Requirement& r) const noexcept {
        return valid() && r.valid() && action == Action::borrow &&
            r.action == Action::release &&
            region_binding == r.region_binding &&
            current_logical_execution == r.current_logical_execution &&
            generation_fence == r.generation_fence && loan_id == r.loan_id &&
            place == r.place && kind == r.kind;
    }
};

// Receipt fields are host-supplied from AUTHENTICATED execution context.
// Comparing two guest-forged receipts MUST NOT grant state authority.
struct Provenance {
    std::uint64_t region_id;
    std::uint64_t region_generation;
    std::uint64_t runtime_domain;
    std::uint64_t execution_id;
    std::uint64_t execution_slot;
    std::uint64_t execution_generation;
    std::uint64_t loan_id;
    std::string_view place;
    Kind kind;
    Action action;
    constexpr bool valid() const noexcept {
        return identity(region_id) && identity(region_generation) &&
            identity(runtime_domain) && identity(execution_id) &&
            identity(execution_slot) && identity(execution_generation) &&
            identity(loan_id) && nonblank(place) &&
            kind_valid(kind) && action_valid(action);
    }
    constexpr bool authorizes_release(const Provenance& r) const noexcept {
        return valid() && r.valid() && action == Action::borrow &&
            r.action == Action::release &&
            region_id == r.region_id &&
            region_generation == r.region_generation &&
            runtime_domain == r.runtime_domain &&
            execution_id == r.execution_id &&
            execution_slot == r.execution_slot &&
            execution_generation == r.execution_generation &&
            loan_id == r.loan_id && place == r.place && kind == r.kind;
    }
    constexpr Provenance release_record() const noexcept {
        Provenance copy = *this;
        copy.action = Action::release;
        return copy;
    }
};
static_assert(identity(kMaxJsonSafeId));
static_assert(!identity(kMaxJsonSafeId + 1));
static_assert(!identity(0));
} // namespace ores::state::wire
