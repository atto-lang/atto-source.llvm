#include "oreslang/runtime/ActorLocalFailurePolicy.hpp"
#include <stdexcept>

using namespace oreslang;
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error("failed: " #expr); } while(false)

struct Counts { int throws = 0; int raises = 0; };

static ActorLocalHookReply accept_throw(void* user, EffectStatus failure) noexcept {
    auto* counters = static_cast<Counts*>(user);
    ++counters->throws;
    if (failure.kind != EffectKind::Throw) {
        return {ActorLocalResolution::Escalate, {EffectKind::Fatal, 100}};
    }
    return {ActorLocalResolution::Handled, {EffectKind::Return, 0}};
}

static ActorLocalHookReply escalate_raise(void* user, EffectStatus failure) noexcept {
    auto* counters = static_cast<Counts*>(user);
    ++counters->raises;
    if (failure.kind != EffectKind::Raise) {
        return {ActorLocalResolution::Escalate, {EffectKind::Fatal, 101}};
    }
    return {ActorLocalResolution::Escalate, {EffectKind::Return, 0}};
}

static ActorLocalHookReply panic_in_hook(void* user, EffectStatus) noexcept {
    auto* counters = static_cast<Counts*>(user);
    ++counters->throws;
    return {ActorLocalResolution::Handled, {EffectKind::Panic, 202}};
}

int main() {
    Counts counts;
    auto a = dispatch_actor_local_unhandled(
            {EffectKind::Throw, 11}, &counts, accept_throw, escalate_raise);
    CHECK(a.hook_invoked && a.handled);
    CHECK(a.outgoing.kind == EffectKind::Return);
    CHECK(counts.throws == 1 && counts.raises == 0);

    auto b = dispatch_actor_local_unhandled(
            {EffectKind::Raise, 12}, &counts, accept_throw, escalate_raise);
    CHECK(b.hook_invoked && !b.handled);
    CHECK(b.outgoing.kind == EffectKind::Raise && b.outgoing.code == 12);
    CHECK(counts.raises == 1);

    // No on_panic exists at this ABI boundary, even when both local
    // callbacks are nonnull. The parent supervisor owns PANIC CONTROL.
    for (const auto kind : {EffectKind::Panic, EffectKind::Cancel,
                             EffectKind::Fatal, EffectKind::Return}) {
        auto x = dispatch_actor_local_unhandled(
                {kind, 55}, &counts, accept_throw, escalate_raise);
        CHECK(!x.hook_invoked && !x.handled);
        CHECK(x.outgoing.kind == kind && x.outgoing.code == 55);
    }
    CHECK(counts.throws == 1 && counts.raises == 1);

    auto c = dispatch_actor_local_unhandled(
            {EffectKind::Throw, 13}, &counts, nullptr, nullptr);
    CHECK(!c.hook_invoked && !c.handled && c.outgoing.code == 13);

    // A hook that panics cannot claim HANDLED and hide that panic.
    auto d = dispatch_actor_local_unhandled(
            {EffectKind::Throw, 14}, &counts, panic_in_hook, nullptr);
    CHECK(d.hook_invoked && !d.handled);
    CHECK(d.outgoing.kind == EffectKind::Panic && d.outgoing.code == 202);

    auto e = dispatch_actor_local_unhandled(
            {EffectKind::Raise, 15}, &counts, panic_in_hook, panic_in_hook);
    CHECK(e.hook_invoked && !e.handled);
    CHECK(e.outgoing.kind == EffectKind::Panic && e.outgoing.code == 202);
    return 0;
}
