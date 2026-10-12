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

    // Higher-priority cleanup escalation is retained without skipping others.
    std::vector<int> order2;
    Step panic{&order2, 1, {EffectKind::Panic, 50}};
    Step ordinary{&order2, 2, {EffectKind::Throw, 60}};
    oreslang::EffectUnwind<2> second;
    CHECK(second.push(run_step, &panic));
    CHECK(second.push(run_step, &ordinary));
    const auto x = second.finish({EffectKind::Raise, 40});
    CHECK(x.primary.kind == EffectKind::Panic);
    CHECK(x.primary.code == 50);
    CHECK(x.secondary_count == 2);
    CHECK(x.secondary[0].code == 60);
    CHECK(x.secondary[1].code == 40);
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
    return 0;
}
