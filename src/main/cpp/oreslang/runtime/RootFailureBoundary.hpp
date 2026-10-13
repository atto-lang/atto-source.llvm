#pragma once
#include "oreslang/runtime/EffectUnwind.hpp"

namespace oreslang {

/**
 * Pure policy for a non-actor task / process-root exit boundary.
 * Nested ordinary calls must not use this converter. It is applied only
 * after lexical catch/trap/recover and cleanup have been exhausted.
 * Not yet wired into the Oreslang LLVM guest compiler.
 */
struct RootTerminalOutcome {
    EffectStatus terminal{};      // normalized terminal category
    EffectStatus original{};      // complete originating discriminator/code
    bool failed = false;
    int default_exit_status = 0; // explicit process.exit is separate
};

constexpr RootTerminalOutcome root_terminal_outcome(EffectStatus result) noexcept {
    switch (result.kind) {
        case EffectKind::Return:
            return {result, result, false, 0};
        case EffectKind::Throw:
        case EffectKind::Raise:
            return {{EffectKind::Panic, result.code}, result, true, 1};
        case EffectKind::Panic:
        case EffectKind::Cancel:
        case EffectKind::Fatal:
            return {result, result, true, 1};
    }
    // Invalid native/FFI effect values must never indicate successful exit.
    return {{EffectKind::Fatal, 0xffffffffu}, result, true, 1};
}

} // namespace oreslang
