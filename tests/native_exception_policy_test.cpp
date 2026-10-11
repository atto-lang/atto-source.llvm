#include "oreslang/runtime/ExceptionPolicy.hpp"

#include <array>
// These conformance checks must execute even in CMake Release (-DNDEBUG).
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <optional>
#include <string>
#include <utility>

using oreslang::runtime::Failure;
using oreslang::runtime::FailureKind;
using oreslang::runtime::bounded_control_diagnostic;
using oreslang::runtime::combine_failure;
using oreslang::runtime::failure_priority;
using oreslang::runtime::max_control_diagnostic_bytes;

int main() {
    static_assert(failure_priority(FailureKind::HostFatal) ==
                  failure_priority(FailureKind::ProcessExit));
    static_assert(failure_priority(FailureKind::Cancelled) >
                  failure_priority(FailureKind::Panic));
    static_assert(failure_priority(FailureKind::Panic) >
                  failure_priority(FailureKind::Raise));
    static_assert(failure_priority(FailureKind::Raise) >
                  failure_priority(FailureKind::Throw));

    bool rejected_unknown_kind = false;
    try {
        (void)Failure::make(static_cast<FailureKind>(0xff), "forged failure tag");
    } catch (const std::invalid_argument&) {
        rejected_unknown_kind = true;
    }
    assert(rejected_unknown_kind);

    // All 36 rank pairs share the Java OresExceptionPolicy regression lattice.
    constexpr std::array<FailureKind, 6> kinds = {
        FailureKind::Throw, FailureKind::Raise, FailureKind::Panic,
        FailureKind::Cancelled, FailureKind::ProcessExit, FailureKind::HostFatal
    };
    for (const auto pending_kind : kinds) {
        for (const auto cleanup_kind : kinds) {
            const auto result = combine_failure(
                Failure::make(pending_kind, "body"),
                Failure::make(cleanup_kind, "cleanup"));
            const bool cleanup_wins =
                failure_priority(cleanup_kind) > failure_priority(pending_kind);
            assert(result.kind() == (cleanup_wins ? cleanup_kind : pending_kind));
            assert(result.suppressed().size() == 1);
            assert(result.suppressed()[0].kind() ==
                   (cleanup_wins ? pending_kind : cleanup_kind));
        }
    }

    auto pending_panic = combine_failure(
        Failure::make(FailureKind::Panic, "body invariant"),
        Failure::make(FailureKind::Throw, "defer error"));
    assert(pending_panic.kind() == FailureKind::Panic);
    assert(pending_panic.suppressed().size() == 1);
    assert(pending_panic.suppressed()[0].kind() == FailureKind::Throw);

    auto cleanup_panic = combine_failure(
        Failure::make(FailureKind::Throw, "body error"),
        Failure::make(FailureKind::Panic, "defer invariant"));
    assert(cleanup_panic.kind() == FailureKind::Panic);
    assert(cleanup_panic.suppressed().size() == 1);
    assert(cleanup_panic.suppressed()[0].kind() == FailureKind::Throw);

    auto equal_priority = combine_failure(
        Failure::make(FailureKind::Raise, "first"),
        Failure::make(FailureKind::Raise, "second"));
    assert(equal_priority.diagnostic() == "first");
    assert(equal_priority.suppressed().size() == 1);
    assert(equal_priority.suppressed()[0].diagnostic() == "second");

    auto cancellation = combine_failure(
        Failure::make(FailureKind::Cancelled, "structured cancellation"),
        Failure::make(FailureKind::Panic, "cleanup panic"));
    assert(cancellation.kind() == FailureKind::Cancelled);
    assert(cancellation.suppressed()[0].kind() == FailureKind::Panic);

    auto explicit_exit = combine_failure(
        Failure::make(FailureKind::ProcessExit, "exit 2"),
        Failure::make(FailureKind::Panic, "defer panic"));
    assert(explicit_exit.kind() == FailureKind::ProcessExit);
    assert(explicit_exit.suppressed()[0].kind() == FailureKind::Panic);

    auto host_fatal = combine_failure(
        Failure::make(FailureKind::Raise, "escalation"),
        Failure::make(FailureKind::HostFatal, "OOM"));
    assert(host_fatal.kind() == FailureKind::HostFatal);
    assert(host_fatal.suppressed()[0].kind() == FailureKind::Raise);

    // A chain of cleanup failures cannot silently erase earlier failures,
    // even when a stronger error occurs in the middle of unwinding.
    std::optional<Failure> chain = Failure::make(FailureKind::Throw, "body");
    chain = combine_failure(std::move(chain), Failure::make(FailureKind::Raise, "defer 3"));
    chain = combine_failure(std::move(chain), Failure::make(FailureKind::Panic, "defer 2"));
    chain = combine_failure(std::move(chain), Failure::make(FailureKind::Throw, "defer 1"));
    assert(chain && chain->kind() == FailureKind::Panic);
    assert(chain->suppressed().size() == 2);
    assert(chain->suppressed()[0].kind() == FailureKind::Raise);
    assert(chain->suppressed()[0].suppressed()[0].kind() == FailureKind::Throw);
    assert(chain->suppressed()[1].kind() == FailureKind::Throw);

    std::optional<Failure> none;
    auto first = combine_failure(std::move(none), Failure::make(FailureKind::Throw, "first"));
    assert(first && first->diagnostic() == "first");
    auto unchanged = combine_failure(std::move(first), std::optional<Failure>{});
    assert(unchanged && unchanged->diagnostic() == "first");

    const std::string rocket = "\xf0\x9f\x9a\xa8";
    const auto long_unicode = rocket + std::string(300, 'x') + rocket;
    assert(bounded_control_diagnostic(long_unicode) == long_unicode);
    std::string many_rockets;
    for (int i = 0; i < 300; ++i) many_rockets += rocket;
    const auto bounded = bounded_control_diagnostic(many_rockets);
    assert(bounded.size() == max_control_diagnostic_bytes);
    assert(bounded == many_rockets.substr(0, 256 * 4));

    const std::string malformed = std::string("valid:") + "\xed\xa0\x80" + "!";
    assert(bounded_control_diagnostic(malformed) == std::string("valid:") + std::string(3, char(63)) + "!");
    assert(bounded_control_diagnostic(std::string(2048, 'x')).size() ==
           max_control_diagnostic_bytes);

    // Malformed UTF-8 and a truncated final scalar are always normalized
    // before crossing the native supervisor CONTROL boundary.
    const std::string truncated_sequence = std::string("ok") + "\xf0\x9f";
    assert(bounded_control_diagnostic(truncated_sequence) == "ok??");
    const std::string overlong_encoding = std::string("\xc0\xaf", 2);
    assert(bounded_control_diagnostic(overlong_encoding) == "??");
    const std::string invalid_scalar = std::string("\xf4\x90\x80\x80", 4);
    assert(bounded_control_diagnostic(invalid_scalar) == "????");
    const auto near_boundary = bounded_control_diagnostic(
        std::string(1023, 'x') + rocket + "tail");
    assert(near_boundary == std::string(1023, 'x'));
    assert(near_boundary.size() < max_control_diagnostic_bytes);
    const auto invalid_at_boundary = bounded_control_diagnostic(
        std::string(1023, 'x') + std::string("\xed\xa0\x80", 3) + rocket);
    assert(invalid_at_boundary == std::string(1023, 'x') + "?");
    assert(invalid_at_boundary.size() == max_control_diagnostic_bytes);
    return 0;
}
