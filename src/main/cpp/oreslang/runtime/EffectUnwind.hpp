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
 * NOT yet wired into the Oreslang frontend/code generator. This is the native
 * cleanup substrate for lexical defer/finally and later actor fault reporting.
 */
enum class EffectKind : std::uint8_t {
    Return, Throw, Raise, Panic, Cancel, Fatal, ProcessExit
};

struct EffectStatus {
    EffectKind kind = EffectKind::Return;
    std::uint32_t code = 0;
};

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
 * Compiler-preallocated stack of lexical cleanup entries.
 * Never allocates on unwind, including the secondary failure log.
 */
template <std::size_t Capacity, std::size_t SecondaryCapacity = 8>
class EffectUnwind final {
public:
    using Cleanup = EffectStatus (*)(void*) noexcept;

    struct Summary {
        EffectStatus primary{};
        std::array<EffectStatus, SecondaryCapacity> secondary{};
        std::size_t secondary_count = 0;
        std::size_t overflow_count = 0;
    };

    bool push(Cleanup cleanup, void* context) noexcept {
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
    Summary finish(EffectStatus current = {}) noexcept {
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
