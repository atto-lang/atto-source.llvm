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
    Return, Throw, Raise, Panic, Cancel, Fatal
};

struct EffectStatus {
    EffectKind kind = EffectKind::Return;
    std::uint32_t code = 0;
};

constexpr bool effect_kind_valid(EffectKind kind) noexcept {
    switch (kind) {
        case EffectKind::Return: case EffectKind::Throw:
        case EffectKind::Raise: case EffectKind::Panic:
        case EffectKind::Cancel: case EffectKind::Fatal:
            return true;
    }
    return false;
}

constexpr unsigned effect_rank(EffectKind kind) noexcept {
    switch (kind) {
        case EffectKind::Return: return 0;
        case EffectKind::Throw: return 1;
        case EffectKind::Raise: return 2;
        case EffectKind::Panic: return 3;
        case EffectKind::Cancel: return 4;
        case EffectKind::Fatal: return 5;
    }
    return 5;
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

    // Completes once. Each defer/finally runs once in strict LIFO order even
    // when another cleanup fails. Higher-priority control effects cannot be
    // downgraded by ordinary cleanup throws.
    Summary finish(EffectStatus current = {}) noexcept {
        Summary result{};
        // Invalid native tags can arrive via FFI or corrupted compiler
        // output. Normalize before any cleanup callback executes; never
        // propagate a fabricated effect discriminator as guest-visible data.
        result.primary = effect_kind_valid(current.kind)
                ? current : EffectStatus{EffectKind::Fatal, 0xFFFFFFFDu};
        if (finished_) {
            result.primary = {EffectKind::Fatal, 0xFFFFFFFEu};
            return result;
        }
        finished_ = true;
        while (count_ != 0) {
            Entry cleanup = stack_[--count_];
            const EffectStatus reported = cleanup.callback(cleanup.context);
            // Fail closed if guest/FFI code manufactures an invalid tag.
            const EffectStatus next = effect_kind_valid(reported.kind)
                    ? reported : EffectStatus{EffectKind::Fatal, 0xFFFFFFFDu};
            if (next.kind == EffectKind::Return) continue;
            if (effect_rank(next.kind) > effect_rank(result.primary.kind)) {
                append_secondary(result, result.primary);
                result.primary = next;
            } else {
                append_secondary(result, next);
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
