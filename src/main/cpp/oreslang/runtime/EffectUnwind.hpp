#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace oreslang {

/**
 * Typed native effect ABI. LLVM generated code should propagate these
 * discriminants as data/branches, not use C++ exception objects or GC.
 *
 * Checked-i64 overflow is now wired into the restricted terminal try/catch
 * compiler slice using EffectKind::Throw plus NumericErrorCode. General source
 * throw/raise/panic propagation is still incomplete; this remains the native
 * cleanup substrate for lexical defer/finally and actor fault reporting.
 */
enum class EffectKind : std::uint8_t {
    Return, Throw, Raise, Panic, Cancel, Fatal, ProcessExit
};

struct EffectStatus {
    EffectKind kind = EffectKind::Return;
    std::uint32_t code = 0;
};

// Forward declaration for the allocation-free I64EffectResult helpers below.
// Their inline definitions must not depend on a later undeclared symbol.
constexpr EffectStatus normalize_effect(EffectStatus status) noexcept;

/**
 * Stable source-language numeric Error ABI. These values match
 * oreslang.numeric-errors.v1 and are backend-neutral guest identities.
 * Host exception classes, POSIX signals and trap opcodes are never identity.
 */
enum class NumericErrorCode : std::uint32_t {
    ArithmeticOverflowError = 65537u,
    DivisionByZeroError = 65538u,
    InexactConversionError = 65539u,
    NumericRangeError = 65540u,
    NumericParseError = 65541u,
    NumericContextError = 65542u,
    NumericResourceError = 65543u,
    NonterminatingDecimalError = 65544u
};

constexpr EffectStatus arithmetic_overflow_effect() noexcept {
    return {EffectKind::Throw,
            static_cast<std::uint32_t>(NumericErrorCode::ArithmeticOverflowError)};
}

/**
 * Allocation-free i64 call-result carrier for native cross-frame effect
 * propagation. A normal return carries kind Return/code 0. Exceptional
 * results preserve the exact EffectStatus unchanged across ordinary frames.
 *
 * This is the ABI substrate only: general frontend/codegen call lowering still
 * returns scalar i64 today and must opt into this carrier before claiming full
 * cross-frame guest effect propagation.
 */
struct I64EffectResult {
    std::int64_t value = 0;
    EffectStatus effect{};
};

constexpr I64EffectResult i64_return(std::int64_t value) noexcept {
    return {value, {EffectKind::Return, 0}};
}

constexpr I64EffectResult i64_throw(
        std::uint32_t code) noexcept {
    return {0, {EffectKind::Throw, code}};
}

constexpr bool i64_has_effect(I64EffectResult result) noexcept {
    return normalize_effect(result.effect).kind != EffectKind::Return;
}

constexpr I64EffectResult i64_forward(
        I64EffectResult result) noexcept {
    result.effect = normalize_effect(result.effect);
    if (result.effect.kind != EffectKind::Return) result.value = 0;
    return result;
}

constexpr bool i64_catch_matches(
        I64EffectResult result, std::uint32_t code) noexcept {
    const auto status = normalize_effect(result.effect);
    return status.kind == EffectKind::Throw && status.code == code;
}

constexpr I64EffectResult i64_recover(
        I64EffectResult result,
        std::uint32_t code,
        std::int64_t recovered) noexcept {
    return i64_catch_matches(result, code)
        ? i64_return(recovered)
        : i64_forward(result);
}

// Only recoverable ordinary source throws may be captured by trap. Control
// signals and terminal failures must propagate even through deferred cleanup.
constexpr bool native_trap_admits(EffectKind kind) noexcept {
    return kind == EffectKind::Throw;
}

constexpr bool effect_kind_valid(EffectKind kind) noexcept {
    switch (kind) {
        case EffectKind::Return:
        case EffectKind::Throw:
        case EffectKind::Raise:
        case EffectKind::Panic:
        case EffectKind::Cancel:
        case EffectKind::Fatal:
        case EffectKind::ProcessExit:
            return true;
    }
    return false;
}

constexpr bool effect_is_terminal(EffectKind kind) noexcept {
    return kind == EffectKind::Raise || kind == EffectKind::Panic
        || kind == EffectKind::Cancel || kind == EffectKind::Fatal
        || kind == EffectKind::ProcessExit;
}

// FFI/IR consumers may hand in an out-of-range discriminant. Such a value
// is never an ordinary throw, success, or catchable effect.
constexpr EffectStatus normalize_effect(EffectStatus status) noexcept {
    return effect_kind_valid(status.kind) ? status
        : EffectStatus{EffectKind::Fatal, 0xFFFFFFFDu};
}

/**
 * One lexical unhandled-effect promotion at a non-actor boundary.
 * Throw escapes ordinary catch as Raise; an unhandled Raise becomes Panic.
 * Panic and non-exception control effects are already terminal at this layer.
 * The typed error code is preserved verbatim.
 */
constexpr EffectStatus promote_unhandled_non_actor(
        EffectStatus status) noexcept {
    status = normalize_effect(status);
    if (status.kind == EffectKind::Throw)
        status.kind = EffectKind::Raise;
    else if (status.kind == EffectKind::Raise)
        status.kind = EffectKind::Panic;
    return status;
}

/** Complete root/actor exceptional escalation to its terminal form. */
constexpr EffectStatus escalate_unhandled_exception(
        EffectStatus status) noexcept {
    status = promote_unhandled_non_actor(status);
    if (status.kind == EffectKind::Raise)
        status = promote_unhandled_non_actor(status);
    return status;
}

/**
 * Compiler-preallocated stack of lexical cleanup entries.
 * Never allocates on unwind, including the secondary failure log.
 */
template <std::size_t Capacity, std::size_t SecondaryCapacity = 8>
class EffectUnwind final {
public:
    // A pending cleanup stack is a unique lexical owner. Implicit copies or
    // moves would duplicate callback registrations and could run a deferred
    // resource release more than once (or leave stale context pointers).
    // Keep the frame at its compiler-allocated address for its entire scope.
    EffectUnwind() = default;
    EffectUnwind(const EffectUnwind&) = delete;
    EffectUnwind& operator=(const EffectUnwind&) = delete;
    EffectUnwind(EffectUnwind&&) = delete;
    EffectUnwind& operator=(EffectUnwind&&) = delete;

    using Cleanup = EffectStatus (*)(void*) noexcept;

    struct Summary {
        EffectStatus primary{};
        std::array<EffectStatus, SecondaryCapacity> secondary{};
        std::size_t secondary_count = 0;
        std::size_t overflow_count = 0;
    };

    [[nodiscard]] bool push(Cleanup cleanup, void* context) noexcept {
        if (cleanup == nullptr || count_ == Capacity || finished_) return false;
        stack_[count_++] = Entry{cleanup, context};
        return true;
    }

    // Completes once. Each defer/finally runs once in strict LIFO order.
    // The first terminal effect is authoritative (not the numerically largest
    // kind). A later ordinary cleanup throw supersedes an earlier ordinary
    // throw, preserving the earlier one as secondary diagnostics. This mirrors
    // the Java/GraalVM cleanup and trap contracts. In particular, a deferred
    // process exit cannot convert an already-failing ordinary throw into an
    // apparently successful exit status.
    [[nodiscard]] Summary finish(EffectStatus current = {}) noexcept {
        Summary result{};
        result.primary = normalize_effect(current);
        if (finished_) {
            result.primary = {EffectKind::Fatal, 0xFFFFFFFEu};
            return result;
        }
        finished_ = true;
        while (count_ != 0) {
            Entry cleanup = stack_[--count_];
            const EffectStatus next = normalize_effect(cleanup.callback(cleanup.context));
            if (next.kind == EffectKind::Return) continue;
            if (effect_is_terminal(result.primary.kind) ||
                (result.primary.kind == EffectKind::Throw &&
                 next.kind == EffectKind::ProcessExit)) {
                // A later panic/cancellation/fatal cleanup is diagnostic when
                // an earlier terminal control effect already owns the exit.
                append_secondary(result, next);
            } else {
                // Normal return yields to cleanup. For an ordinary throw the
                // later cleanup failure becomes primary, as on Java/GraalVM.
                append_secondary(result, result.primary);
                result.primary = next;
            }
        }
        return result;
    }

    std::size_t remaining() const noexcept { return count_; }

private:
    struct Entry {
        Cleanup callback = nullptr;
        void* context = nullptr;
    };

    static void append_secondary(Summary& result, EffectStatus status) noexcept {
        if (status.kind == EffectKind::Return) return;
        if (result.secondary_count < SecondaryCapacity) {
            result.secondary[result.secondary_count++] = status;
        } else {
            ++result.overflow_count;
        }
    }

    std::array<Entry, Capacity> stack_{};
    std::size_t count_ = 0;
    bool finished_ = false;
};

} // namespace oreslang
