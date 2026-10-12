#include "oreslang/runtime/EffectUnwind.hpp"
#include <stdexcept>
#include <vector>
#include <cstdint>

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
        EffectKind::Panic, EffectKind::Cancel, EffectKind::Fatal
    };
    // Exhaustively check every pair of initial and cleanup effect kinds.
    // This is an executable finite-state precedence model (6 * 6 cases).
    for (const EffectKind pending : kinds) {
        for (const EffectKind deferred : kinds) {
            std::vector<int> executed;
            executed.reserve(1); // no allocations within noexcept callbacks
            Step only{&executed, 42, {deferred, 200}};
            oreslang::EffectUnwind<1> pair;
            CHECK(pair.push(run_step, &only));
            const auto actual = pair.finish({pending, 100});
            const EffectKind expected = deferred == EffectKind::Return
                || oreslang::effect_is_terminal(pending) ? pending : deferred;
            CHECK(actual.primary.kind == expected);
            CHECK(actual.primary.code == (expected == deferred
                 && deferred != EffectKind::Return ? 200u : 100u));
            CHECK(actual.secondary_count ==
                ((pending != EffectKind::Return && deferred != EffectKind::Return) ? 1u : 0u));
            if (actual.secondary_count != 0) {
                CHECK(actual.secondary[0].kind ==
                    (oreslang::effect_is_terminal(pending) ? deferred : pending));
            }
            CHECK(executed == std::vector<int>({42}));
            CHECK(pair.remaining() == 0);
        }
    }
    CHECK(oreslang::native_trap_admits(EffectKind::Throw));
    for (const EffectKind kind : kinds)
        if (kind != EffectKind::Throw)
            CHECK(!oreslang::native_trap_admits(kind));

    return 0;
}
