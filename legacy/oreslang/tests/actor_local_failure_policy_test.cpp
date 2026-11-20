#include "oreslang/runtime/ActorLocalFailurePolicy.hpp"
#include "oreslang/runtime/ActorCheckedFrameBudget.hpp"
#include <new>
#include <stdexcept>
#include <initializer_list>

using namespace oreslang;
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error("failed: " #expr); } while(false)

struct Counts { int throws = 0; int raises = 0; int panics = 0; std::uint32_t panic_code = 0; };

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

static ActorLocalHookReply accept_raise(void* user, EffectStatus failure) noexcept {
    auto* counters = static_cast<Counts*>(user);
    ++counters->raises;
    if (failure.kind != EffectKind::Raise)
        return {ActorLocalResolution::Escalate, {EffectKind::Fatal, 103}};
    return {ActorLocalResolution::Handled, {EffectKind::Return, 0}};
}

static ActorLocalHookReply panic_in_hook(void* user, EffectStatus) noexcept {
    auto* counters = static_cast<Counts*>(user);
    ++counters->throws;
    return {ActorLocalResolution::Handled, {EffectKind::Panic, 202}};
}

static void observe_panic(void* user, EffectStatus failure) noexcept {
    auto* counters = static_cast<Counts*>(user);
    ++counters->panics;
    counters->panic_code = failure.code;
}

static ActorLocalHookReply invalid_in_hook(void*, EffectStatus) noexcept {
    return {ActorLocalResolution::Handled, {static_cast<EffectKind>(255), 42}};
}

static ActorLocalHookReply exit_in_hook(void*, EffectStatus) noexcept {
    return {ActorLocalResolution::Handled, {EffectKind::ProcessExit, 0}};
}


static void verify_checked_frame_budget() {
    using Limit = ActorFrameOverflowLimit;
    ActorCheckedFrameBudget failed(64, 2);
    auto outer = failed.enter_frame(24);
    auto inner = failed.enter_frame(32);
    CHECK(failed.active_frames() == 2 && failed.used_bytes() == 56);
    bool out_of_order = false;
    try { failed.leave_frame(outer); }
    catch (const std::logic_error&) { out_of_order = true; }
    CHECK(out_of_order);
    bool frame_quota = false;
    try { failed.enter_frame(1); }
    catch (const ActorCheckedFrameBudget::Overflow& e) {
        frame_quota = e.limit() == Limit::FrameCount;
    }
    CHECK(frame_quota && failed.terminal_overflow() == Limit::FrameCount);
    failed.leave_frame(inner);
    failed.leave_frame(outer);
    CHECK(failed.used_bytes() == 0 && failed.active_frames() == 0);
    CHECK(failed.terminal_overflow() == Limit::FrameCount);
    bool cannot_recover = false;
    try { failed.enter_frame(1); }
    catch (const ActorCheckedFrameBudget::Overflow& e) {
        cannot_recover = e.limit() == Limit::FrameCount;
    }
    CHECK(cannot_recover);

    // Placement-new creates another actor at the SAME memory address.
    // A token minted for the retired incarnation must still be rejected.
    alignas(ActorCheckedFrameBudget) unsigned char reused_storage[
        sizeof(ActorCheckedFrameBudget)] = {};
    auto* old_budget = new (reused_storage) ActorCheckedFrameBudget(64, 2);
    auto retired_token = old_budget->enter_frame(8);
    old_budget->leave_frame(retired_token);
    old_budget->~ActorCheckedFrameBudget();
    auto* new_budget = new (reused_storage) ActorCheckedFrameBudget(64, 2);
    auto live_token = new_budget->enter_frame(8);
    bool rejected_aba = false;
    try { new_budget->leave_frame(retired_token); }
    catch (const std::logic_error&) { rejected_aba = true; }
    CHECK(rejected_aba);
    CHECK(new_budget->active_frames() == 1 && new_budget->used_bytes() == 8);
    new_budget->leave_frame(live_token);
    new_budget->~ActorCheckedFrameBudget();

    ActorCheckedFrameBudget byte_limit(48, 4);
    auto initial = byte_limit.enter_frame(32);
    bool bytes_exhausted = false;
    try { byte_limit.enter_frame(17); }
    catch (const ActorCheckedFrameBudget::Overflow& e) {
        bytes_exhausted = e.limit() == Limit::ByteCapacity;
    }
    CHECK(bytes_exhausted && byte_limit.terminal_overflow() == Limit::ByteCapacity);
    byte_limit.leave_frame(initial);
    CHECK(byte_limit.used_bytes() == 0);
    ActorCheckedFrameBudget healthy(48, 2);
    auto first = healthy.enter_frame(16);
    bool foreign = false;
    try { byte_limit.leave_frame(first); }
    catch (const std::logic_error&) { foreign = true; }
    CHECK(foreign);
    healthy.leave_frame(first);
    bool replay = false;
    try { healthy.leave_frame(first); }
    catch (const std::logic_error&) { replay = true; }
    CHECK(replay);
    auto replacement = healthy.enter_frame(16);
    bool stale_generation = false;
    try { healthy.leave_frame(first); }
    catch (const std::logic_error&) { stale_generation = true; }
    CHECK(stale_generation);
    healthy.leave_frame(replacement);
    CHECK(!healthy.terminal_overflow() && healthy.used_bytes() == 0);
    bool invalid_budget = false;
    try { ActorCheckedFrameBudget bad(0, 1); }
    catch (const std::invalid_argument&) { invalid_budget = true; }
    CHECK(invalid_budget);
}

int main() {
    verify_checked_frame_budget();
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

    // No on_panic RESOLUTION exists at this boundary, even when both local
    // callbacks are nonnull. The separate safe-lane observer below cannot
    // consume the parent's PANIC CONTROL notice.
    for (const auto kind : {EffectKind::Panic, EffectKind::Cancel,
                             EffectKind::Fatal, EffectKind::Return}) {
        auto x = dispatch_actor_local_unhandled(
                {kind, 55}, &counts, accept_throw, escalate_raise);
        CHECK(!x.hook_invoked && !x.handled);
        CHECK(x.outgoing.kind == kind && x.outgoing.code == 55);
    }
    CHECK(counts.throws == 1 && counts.raises == 1);

    // Unlike PANIC, an unhandled RAISE may be handled at a checked actor
    // on_raise boundary after lexical recover had the first opportunity.
    auto handled_raise = dispatch_actor_local_unhandled(
            {EffectKind::Raise, 76}, &counts, accept_throw, accept_raise);
    CHECK(handled_raise.hook_invoked && handled_raise.handled);
    CHECK(handled_raise.outgoing.kind == EffectKind::Return);
    CHECK(counts.raises == 2 && counts.throws == 1);

    // Only a safely unwound, dedicated observer lane may call on_panic.
    // It sees the panic but cannot convert it to success or handle it.
    auto observed = observe_actor_panic_on_safe_lane(
            {EffectKind::Panic, 77}, &counts, observe_panic);
    CHECK(observed.hook_invoked && !observed.handled);
    CHECK(observed.outgoing.kind == EffectKind::Panic &&
          observed.outgoing.code == 77);
    CHECK(counts.panics == 1 && counts.panic_code == 77);
    CHECK(counts.throws == 1 && counts.raises == 2);

    // Missing observers and non-panic effects bypass local observation.
    for (const auto kind : {EffectKind::Throw, EffectKind::Raise,
                            EffectKind::Cancel, EffectKind::Fatal,
                            EffectKind::ProcessExit, EffectKind::Return}) {
        auto skipped = observe_actor_panic_on_safe_lane(
                {kind, 78}, &counts, observe_panic);
        CHECK(!skipped.hook_invoked && !skipped.handled);
        CHECK(skipped.outgoing.kind == kind && skipped.outgoing.code == 78);
    }
    auto absent = observe_actor_panic_on_safe_lane(
            {EffectKind::Panic, 79}, &counts, nullptr);
    CHECK(!absent.hook_invoked && !absent.handled);
    CHECK(absent.outgoing.kind == EffectKind::Panic && absent.outgoing.code == 79);
    CHECK(counts.panics == 1);

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
    auto invalid = dispatch_actor_local_unhandled(
            {EffectKind::Throw, 16}, nullptr, invalid_in_hook, nullptr);
    CHECK(!invalid.handled && invalid.outgoing.kind == EffectKind::Fatal);
    auto bad_input = dispatch_actor_local_unhandled(
            {static_cast<EffectKind>(255), 17}, nullptr, accept_throw, escalate_raise);
    CHECK(!bad_input.hook_invoked && bad_input.outgoing.kind == EffectKind::Fatal);
    auto exit = dispatch_actor_local_unhandled(
            {EffectKind::Throw, 18}, nullptr, exit_in_hook, nullptr);
    CHECK(!exit.handled && exit.outgoing.kind == EffectKind::Throw && exit.outgoing.code == 18);
    return 0;
}
