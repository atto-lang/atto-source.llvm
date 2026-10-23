#include "oreslang/runtime/EffectSupervisorBridge.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("check failed: " #condition); } while(false)

using oreslang::EffectKind;
using oreslang::EffectStatus;
using Lane = oreslang::SupervisorFailureLane;

struct EventHandler : Lane::Handler {
    std::vector<Lane::Kind> observed;
    std::vector<Lane::Kind> origins;
    std::vector<std::uint32_t> origin_codes;
    void add(const Lane::Failure& failure) {
        observed.push_back(failure.kind);
        origins.push_back(failure.origin_kind);
        origin_codes.push_back(failure.origin_code);
    }
    void on_throw(const Lane::Failure& f) override { add(f); }
    void on_raise(const Lane::Failure& f) override { add(f); }
    void on_panic(const Lane::Failure& f) override { add(f); }
    void on_cancel(const Lane::Failure& f) override { add(f); }
    void on_host_failure(const Lane::Failure& f) override { add(f); }
};

int main() {
    using Bridge = oreslang::EffectSupervisorBridge;
    Lane lane(7);
    auto key = [](int n) -> Lane::ActorKey { return {"worker-" + std::to_string(n), 1}; };
    constexpr EffectKind effects[] = {
        EffectKind::Throw, EffectKind::Raise, EffectKind::Panic,
        EffectKind::Cancel, EffectKind::Fatal
    };
    constexpr Lane::Kind expected[] = {
        Lane::Kind::Panic, Lane::Kind::Panic, Lane::Kind::Panic,
        Lane::Kind::Cancel, Lane::Kind::HostFailure
    };
    constexpr Lane::Kind original[] = {
        Lane::Kind::Throw, Lane::Kind::Raise, Lane::Kind::Panic,
        Lane::Kind::Cancel, Lane::Kind::HostFailure
    };
    // Publication cannot consume user mailbox capacity and never runs guest
    // handlers inline; each pre-reserved slot is published at most once.
    for (int i = 0; i < 5; ++i) {
        lane.reserve(key(i));
        CHECK(Bridge::publish_terminal(lane, key(i), {effects[i], 11u},
                                       "actor failure", "parent"));
        CHECK(!Bridge::publish_terminal(lane, key(i), {effects[i], 12u},
                                        "duplicate", "parent"));
    }
    EventHandler handler;
    for (int i = 0; i < 5; ++i) CHECK(lane.dispatch_one(handler));
    CHECK(handler.observed == std::vector<Lane::Kind>(expected, expected + 5));
    CHECK(handler.origins == std::vector<Lane::Kind>(original, original + 5));
    CHECK(handler.origin_codes == std::vector<std::uint32_t>(5, 11u));
    CHECK(lane.reserved_count() == 0);

    // Normal return leaves the reservation intact; lifecycle owner retires it.
    lane.reserve(key(5));
    CHECK(!Bridge::publish_terminal(lane, key(5), {EffectKind::Return, 42u}, "ok"));
    CHECK(lane.pending_count() == 0);
    lane.retire_gracefully(key(5));

    // An actual unwind where a panic in defer wins over a plain throw
    // produces exactly one typed parent CONTROL event.
    lane.reserve(key(6));
    oreslang::EffectUnwind<1> frame;
    const auto cleanup = [](void*) noexcept -> EffectStatus {
        return {EffectKind::Panic, 99u};
    };
    CHECK(frame.push(cleanup, nullptr));
    const auto terminal = frame.finish({EffectKind::Throw, 1u});
    CHECK(terminal.primary.kind == EffectKind::Panic);
    CHECK(terminal.secondary_count == 1);
    CHECK(terminal.secondary[0].kind == EffectKind::Throw);
    CHECK(Bridge::publish_terminal(lane, key(6), terminal.primary, "defer panic"));
    auto delivered = lane.poll();
    CHECK(delivered.has_value());
    CHECK(delivered->kind == Lane::Kind::Panic);
    CHECK(delivered->origin_kind == Lane::Kind::Panic);
    CHECK(delivered->origin_code == 99u);
    CHECK(delivered->summary == "defer panic");
    lane.ack(key(6));

    // All 249 unknown uint8 native/FFI effect tags must escalate through
    // the reserved CONTROL slot as fatal HostFailure. Throwing C++ across
    // the failing carrier would strand the child reservation permanently.
    for (unsigned raw = 7; raw < 256; ++raw) {
        const auto actor = key(700 + static_cast<int>(raw));
        lane.reserve(actor);
        CHECK(Bridge::publish_terminal(lane, actor,
                {static_cast<EffectKind>(raw), 10u}, "forged return"));
        CHECK(lane.pending_count() == 1);
        const auto event = lane.poll();
        CHECK(event.has_value());
        CHECK(event->kind == Lane::Kind::HostFailure);
        CHECK(event->origin_kind == Lane::Kind::HostFailure);
        CHECK(event->origin_code == 0xFFFFFFFDu);
        CHECK(event->summary == "invalid native effect discriminant");
        lane.ack(actor);
        CHECK(lane.pending_count() == 0);
    }
    // A valid ProcessExit(0) is actor-terminal and cannot be confused
    // with a successful Return; keep its numeric payload.
    lane.reserve(key(7));
    CHECK(Bridge::publish_terminal(lane, key(7),
            {EffectKind::ProcessExit, 0u}, "process exit from child"));
    const auto process_exit = lane.poll();
    CHECK(process_exit.has_value());
    CHECK(process_exit->kind == Lane::Kind::HostFailure);
    CHECK(process_exit->origin_kind == Lane::Kind::HostFailure);
    CHECK(process_exit->origin_code == 0u);
    lane.ack(key(7));

    // Promotion is tested through the SupervisorControlTurn overload too.
    // A malformed (terminal, origin) pair is never accepted.
    oreslang::SupervisorControlTurn controlled(2);
    controlled.reserve_child(key(8));
    CHECK(Bridge::publish_terminal(controlled, key(8),
            {EffectKind::Throw, 33u}, "uncaught throw", "root"));
    CHECK(controlled.pending_count() == 1);

    Lane invalid_pair(1);
    invalid_pair.reserve(key(9));
    bool bad_origin = false;
    try {
        invalid_pair.publish_preallocated(key(9), Lane::Kind::Raise, "invalid",
                                          "root", Lane::Kind::Throw);
    } catch (const std::invalid_argument&) { bad_origin = true; }
    CHECK(bad_origin && invalid_pair.pending_count() == 0);
    CHECK(invalid_pair.publish_preallocated(
            key(9), Lane::Kind::Panic, "uncaught raise", "root", Lane::Kind::Raise));
    auto restored = invalid_pair.poll();
    CHECK(restored.has_value());
    CHECK(restored->kind == Lane::Kind::Panic);
    CHECK(restored->origin_kind == Lane::Kind::Raise);
    CHECK(restored->origin_code == 0u);
    invalid_pair.ack(key(9));

    // Invalid effect values returned by native/FFI cleanup cannot masquerade
    // as a valid terminal effect or strand a reserved supervisor slot.
    const auto corrupt_cleanup = [](void*) noexcept -> EffectStatus {
        return {static_cast<EffectKind>(0xff), 17u};
    };
    oreslang::EffectUnwind<1> corrupted_frame;
    CHECK(corrupted_frame.push(corrupt_cleanup, nullptr));
    const auto corrupted = corrupted_frame.finish({EffectKind::Throw, 43u});
    CHECK(corrupted.primary.kind == EffectKind::Fatal);
    CHECK(corrupted.primary.code == 0xFFFFFFFDu);
    CHECK(corrupted.secondary_count == 1);
    CHECK(corrupted.secondary[0].kind == EffectKind::Throw);
    CHECK(corrupted.secondary[0].code == 43u);

    oreslang::EffectUnwind<0> invalid_initial;
    const auto malformed = invalid_initial.finish(
            {static_cast<EffectKind>(0xff), 51u});
    CHECK(malformed.primary.kind == EffectKind::Fatal);
    CHECK(malformed.primary.code == 0xFFFFFFFDu);

    Lane fallback(1);
    fallback.reserve(key(10));
    CHECK(Bridge::publish_terminal(fallback, key(10),
            corrupted.primary, "invalid cleanup tag"));
    const auto fail_closed = fallback.poll();
    CHECK(fail_closed && fail_closed->kind == Lane::Kind::HostFailure);
    CHECK(fail_closed->origin_kind == Lane::Kind::HostFailure);
    CHECK(fail_closed->origin_code == 0xFFFFFFFDu);
    fallback.ack(key(10));
    return 0;
}
