#include "oreslang/runtime/EffectUnwind.hpp"
#include <stdexcept>
#include <vector>
#include <cstdint>
#include <type_traits>

using oreslang::EffectKind;
using oreslang::EffectStatus;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("failed: " #condition); } while(false)

struct Step {
    std::vector<int>* order;
    int id;
    EffectStatus effect;
};

static EffectStatus run_step(void* value) noexcept {
    auto* s = static_cast<Step*>(value);
    s->order->push_back(s->id);
    return s->effect;
}

struct CountedStep {
    int* executions;
    EffectStatus effect;
};

static EffectStatus run_counted(void* value) noexcept {
    auto* step = static_cast<CountedStep*>(value);
    ++*step->executions;
    return step->effect;
}

// The unwind frame is a unique lexical owner; copying or moving it would
// duplicate callbacks or invalidate contexts and violate exactly-once cleanup.
static_assert(std::is_default_constructible_v<oreslang::EffectUnwind<1>>);
static_assert(!std::is_copy_constructible_v<oreslang::EffectUnwind<1>>);
static_assert(!std::is_copy_assignable_v<oreslang::EffectUnwind<1>>);
static_assert(!std::is_move_constructible_v<oreslang::EffectUnwind<1>>);
static_assert(!std::is_move_assignable_v<oreslang::EffectUnwind<1>>);

int main() {
    using Frame = oreslang::EffectUnwind<3, 2>;
    std::vector<int> order;
    Step a{&order, 1, {EffectKind::Throw, 11}};
    Step b{&order, 2, {EffectKind::Raise, 22}};
    Step c{&order, 3, {EffectKind::Throw, 33}};

    Frame frame;
    CHECK(frame.push(run_step, &a));
    CHECK(frame.push(run_step, &b));
    CHECK(frame.push(run_step, &c));
    CHECK(!frame.push(run_step, &a));
    auto outcome = frame.finish({EffectKind::Panic, 99});
    CHECK(order == std::vector<int>({3, 2, 1}));
    CHECK(outcome.primary.kind == EffectKind::Panic);
    CHECK(outcome.primary.code == 99);
    CHECK(outcome.secondary_count == 2);
    CHECK(outcome.overflow_count == 1);
    CHECK(outcome.secondary[0].code == 33);
    CHECK(outcome.secondary[1].code == 22);
    CHECK(frame.remaining() == 0);
    CHECK(frame.finish().primary.kind == EffectKind::Fatal);

    // An already pending Raise remains authoritative even if a deferred
    // callback panics. Numeric severity must not reorder terminal failures.
    std::vector<int> order2;
    Step panic{&order2, 1, {EffectKind::Panic, 50}};
    Step ordinary{&order2, 2, {EffectKind::Throw, 60}};
    oreslang::EffectUnwind<2> second;
    CHECK(second.push(run_step, &panic));
    CHECK(second.push(run_step, &ordinary));
    const auto x = second.finish({EffectKind::Raise, 40});
    CHECK(x.primary.kind == EffectKind::Raise);
    CHECK(x.primary.code == 40);
    CHECK(x.secondary_count == 2);
    CHECK(x.secondary[0].code == 60);
    CHECK(x.secondary[1].code == 50);
    CHECK(order2 == std::vector<int>({2, 1}));

    // Cancellation cannot be turned into a normal throw during unwind.
    Step weaker{&order2, 3, {EffectKind::Throw, 1}};
    oreslang::EffectUnwind<1> cancelled;
    CHECK(cancelled.push(run_step, &weaker));
    const auto z = cancelled.finish({EffectKind::Cancel, 7});
    CHECK(z.primary.kind == EffectKind::Cancel);
    CHECK(z.primary.code == 7);
    CHECK(z.secondary_count == 1);
    CHECK(z.secondary[0].kind == EffectKind::Throw);

    // Ordinary failure cleanup follows Java's last-failing-defer-wins rule,
    // retaining both the original body error and the earlier failing defer.
    std::vector<int> ordinary_order;
    ordinary_order.reserve(2);
    Step last{&ordinary_order, 1, {EffectKind::Throw, 31}};
    Step first{&ordinary_order, 2, {EffectKind::Throw, 32}};
    oreslang::EffectUnwind<2> ordinary_frame;
    CHECK(ordinary_frame.push(run_step, &last));
    CHECK(ordinary_frame.push(run_step, &first));
    const auto ordinary_result = ordinary_frame.finish({EffectKind::Throw, 30});
    CHECK(ordinary_result.primary.kind == EffectKind::Throw);
    CHECK(ordinary_result.primary.code == 31);
    CHECK(ordinary_result.secondary_count == 2);
    CHECK(ordinary_result.secondary[0].code == 30);
    CHECK(ordinary_result.secondary[1].code == 32);
    CHECK(ordinary_order == std::vector<int>({2, 1}));

    constexpr EffectKind kinds[] = {
        EffectKind::Return, EffectKind::Throw, EffectKind::Raise,
        EffectKind::Panic, EffectKind::Cancel, EffectKind::Fatal,
        EffectKind::ProcessExit
    };
    // Exhaustively check every pair of initial and cleanup effect kinds.
    // This is an executable finite-state precedence model (7 * 7 cases).
    for (const EffectKind pending : kinds) {
        for (const EffectKind deferred : kinds) {
            std::vector<int> executed;
            executed.reserve(1); // no allocations within noexcept callbacks
            Step only{&executed, 42, {deferred, 200}};
            oreslang::EffectUnwind<1> pair;
            CHECK(pair.push(run_step, &only));
            const auto actual = pair.finish({pending, 100});
            const bool preserve_pending =
                oreslang::effect_is_terminal(pending) ||
                (pending == EffectKind::Throw &&
                 deferred == EffectKind::ProcessExit);
            const EffectKind expected = deferred == EffectKind::Return
                || preserve_pending ? pending : deferred;
            CHECK(actual.primary.kind == expected);
            const bool cleanup_becomes_primary =
                deferred != EffectKind::Return && !preserve_pending;
            CHECK(actual.primary.code == (cleanup_becomes_primary ? 200u : 100u));
            CHECK(actual.secondary_count ==
                ((pending != EffectKind::Return && deferred != EffectKind::Return) ? 1u : 0u));
            if (actual.secondary_count != 0) {
                CHECK(actual.secondary[0].kind ==
                    (preserve_pending ? deferred : pending));
            }
            CHECK(executed == std::vector<int>({42}));
            CHECK(pair.remaining() == 0);
        }
    }
    // A deferred exit(0) cannot downgrade an already-failing Oreslang
    // function. But on normal return an explicit exit takes effect.
    std::vector<int> exit_order;
    Step exit_action{&exit_order, 1, {EffectKind::ProcessExit, 0}};
    oreslang::EffectUnwind<1> prior_throw;
    CHECK(prior_throw.push(run_step, &exit_action));
    const auto preserved = prior_throw.finish({EffectKind::Throw, 77});
    CHECK(preserved.primary.kind == EffectKind::Throw);
    CHECK(preserved.primary.code == 77);
    CHECK(preserved.secondary_count == 1);
    CHECK(preserved.secondary[0].kind == EffectKind::ProcessExit);
    CHECK(preserved.secondary[0].code == 0);

    oreslang::EffectUnwind<1> normal_return;
    CHECK(normal_return.push(run_step, &exit_action));
    const auto exited = normal_return.finish();
    CHECK(exited.primary.kind == EffectKind::ProcessExit);
    CHECK(exited.primary.code == 0);

    // Unknown native/FFI tags must never be interpreted as a normal return
    // or converted to Option.None. Normalize them to a terminal host failure.
    constexpr auto forged = static_cast<EffectKind>(0xff);
    CHECK(!oreslang::effect_kind_valid(forged));
    CHECK(!oreslang::native_trap_admits(forged));
    oreslang::EffectUnwind<0> invalid_pending;
    const auto incoming = invalid_pending.finish({forged, 12});
    CHECK(incoming.primary.kind == EffectKind::Fatal);
    CHECK(incoming.primary.code == 0xFFFFFFFDu);

    std::vector<int> invalid_order;
    Step invalid_callback{&invalid_order, 1, {forged, 42}};
    oreslang::EffectUnwind<1> invalid_cleanup;
    CHECK(invalid_cleanup.push(run_step, &invalid_callback));
    const auto outgoing = invalid_cleanup.finish({EffectKind::Throw, 17});
    CHECK(outgoing.primary.kind == EffectKind::Fatal);
    CHECK(outgoing.primary.code == 0xFFFFFFFDu);
    CHECK(outgoing.secondary_count == 1);
    CHECK(outgoing.secondary[0].kind == EffectKind::Throw);
    CHECK(outgoing.secondary[0].code == 17);

    // Exhaustively validate all possible one-byte effect ABI tags. Only the
    // seven admitted tags are valid; the remaining 249 become terminal Fatal.
    for (unsigned raw = 0; raw < 256; ++raw) {
        const auto kind = static_cast<EffectKind>(raw);
        const bool valid = raw < 7;
        CHECK(oreslang::effect_kind_valid(kind) == valid);
        CHECK(oreslang::native_trap_admits(kind) == (raw == 1));
        const auto normalized = oreslang::normalize_effect({kind, 0xCAFEu});
        CHECK(normalized.kind == (valid ? kind : EffectKind::Fatal));
        CHECK(normalized.code == (valid ? 0xCAFEu : 0xFFFFFFFDu));
    }

    // Rejected registration must never displace a valid cleanup, and a
    // completed frame cannot be pushed again or unwound twice.
    int executed_count = 0;
    CountedStep counted{&executed_count, {EffectKind::Return, 0}};
    oreslang::EffectUnwind<1> once;
    CHECK(!once.push(nullptr, &counted));
    CHECK(once.push(run_counted, &counted));
    CHECK(!once.push(run_counted, &counted));
    const auto finished = once.finish();
    CHECK(finished.primary.kind == EffectKind::Return);
    CHECK(finished.secondary_count == 0);
    CHECK(executed_count == 1);
    CHECK(once.remaining() == 0);
    CHECK(!once.push(run_counted, &counted));
    const auto repeated = once.finish();
    CHECK(repeated.primary.kind == EffectKind::Fatal);
    CHECK(repeated.primary.code == 0xFFFFFFFEu);
    CHECK(executed_count == 1);

    // Zero diagnostic slots must still preserve the primary failure and
    // report all suppressed failures as bounded, allocation-free overflow.
    int fail_count = 0;
    CountedStep fails{&fail_count, {EffectKind::Throw, 55}};
    oreslang::EffectUnwind<3, 0> bounded;
    CHECK(bounded.push(run_counted, &fails));
    CHECK(bounded.push(run_counted, &fails));
    CHECK(bounded.push(run_counted, &fails));
    const auto no_slots = bounded.finish({EffectKind::Throw, 44});
    CHECK(no_slots.primary.kind == EffectKind::Throw);
    CHECK(no_slots.primary.code == 55);
    CHECK(no_slots.secondary_count == 0);
    CHECK(no_slots.overflow_count == 3);
    CHECK(fail_count == 3);

    CHECK(oreslang::native_trap_admits(EffectKind::Throw));
    for (const EffectKind kind : kinds)
        if (kind != EffectKind::Throw)
            CHECK(!oreslang::native_trap_admits(kind));

    return 0;
}
