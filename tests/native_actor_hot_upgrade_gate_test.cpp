#include "oreslang/runtime/NativeActorHotUpgradeGate.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::runtime;
using Gate = NativeActorHotUpgradeGate<std::string>;

static void check(bool yes, const char* why) {
    if (!yes) throw std::runtime_error(why);
}
template<class Fn>
static void denied(Fn fn, const char* why) {
    bool rejected = false;
    try { fn(); }
    catch (const std::logic_error&) { rejected = true; }
    check(rejected, why);
}
static Gate::Shape shape(Gate::Kind kind) {
    return {kind, "ActorRef<Message,Reply,Error>/receive-v1",
            "field-schema-v1", "shared-cell-layout-v1", "capabilities-v1"};
}

static void actor_code_only_upgrade(Gate::Kind kind) {
    std::atomic<int> cleaned{0};
    const ActorHeapIdentity id{91, 4321, 17};
    Gate gate(id, shape(kind), "AOT-v1", [&] { ++cleaned; }, 3);
    const ActorHeapIdentity foreign{92, 4321, 17};
    const ActorHeapIdentity stale_epoch{91, 4321, 16};
    denied([&] { (void)gate.pin(foreign); }, "foreign runtime gained actor code pin");
    denied([&] { (void)gate.pin(stale_epoch); }, "stale incarnation gained actor code pin");

    auto old_turn = gate.pin(id);
    check(old_turn.code() == "AOT-v1" && old_turn.descriptor().revision == 1,
          "initial AOT code generation is wrong");
    auto ticket = gate.freeze(id, 1);
    denied([&] { (void)gate.pin(id); }, "new DATA turn admitted during freeze");
    denied([&] { (void)gate.freeze(id, 1); }, "second simultaneous freeze accepted");
    check(!gate.quiesced(ticket), "running turn was omitted from quiescence");

    denied([&] { (void)gate.promote(ticket, shape(kind), "jit-v2",
                                  [&] { ++cleaned; }); },
           "code promotion admitted while old turn was active");
    check(cleaned == 1, "rejected active-turn candidate not reclaimed exactly once");
    check(old_turn.code() == "AOT-v1", "failed upgrade mutated old pinned code");
    old_turn.close();
    check(gate.quiesced(ticket), "quiescence not observed after all turn leases drain");

    auto incompatible = shape(kind);
    incompatible.state_schema = "field-schema-v2";
    denied([&] { (void)gate.promote(ticket, incompatible, "bad-state-v2",
                                  [&] { ++cleaned; }); },
           "implicit actor state migration accepted");
    auto unsafe = shape(kind);
    unsafe.shared_cell_abi = "changed-shared-cell-layout";
    denied([&] { (void)gate.promote(ticket, unsafe, "bad-shared-v2",
                                  [&] { ++cleaned; }); },
           "shared-cell ABI drift accepted");
    auto widened = shape(kind);
    widened.capability_abi = "privileged-capability";
    denied([&] { (void)gate.promote(ticket, widened, "bad-capability-v2",
                                  [&] { ++cleaned; }); },
           "capability ABI drift accepted");
    auto moved = shape(kind == Gate::Kind::trusted_private
                       ? Gate::Kind::trusted_shared : Gate::Kind::trusted_private);
    denied([&] { (void)gate.promote(ticket, moved, "bad-placement-v2",
                                  [&] { ++cleaned; }); },
           "actor placement changed during code-only upgrade");
    check(cleaned == 5, "rejected candidate cleanup leaked or double-called");
    check(gate.quiesced(ticket), "rejected candidate unsealed actor");

    auto revision = gate.promote(ticket, shape(kind), "JIT-v2",
                                 [&] { ++cleaned; });
    check(revision.revision == 2 && cleaned == 6,
          "AOT-to-JIT upgrade did not commit or reclaim old AOT image");
    auto v2 = gate.pin(id);
    check(v2.code() == "JIT-v2", "new actor turn received stale code");

    // Every suspended continuation/FFI user holds a pin. Freezing cannot
    // migrate the actor while that lease is held, even across carriers.
    auto suspended = gate.freeze(id, 2);
    std::thread producer([pin = std::move(v2)]() mutable {
        check(pin.code() == "JIT-v2", "continuation's code pin was overwritten");
        pin.close();
    });
    producer.join();
    check(gate.quiesced(suspended), "suspended code lease failed to drain");
    gate.abort(suspended);
    auto fresh = gate.pin(id);
    check(fresh.code() == "JIT-v2", "aborted release did not resume DATA turns");
    fresh.close();

    denied([&] { (void)gate.freeze(stale_epoch, 2); },
           "stale actor incarnation froze live actor");
    denied([&] { (void)gate.freeze(id, 1); },
           "stale expected revision froze actor");
    auto ticket2 = gate.freeze(id, 2);
    check(gate.promote(ticket2, shape(kind), "AOT-v3",
                       [&] { ++cleaned; }).revision == 3,
          "JIT-to-AOT upgrade failed");
    gate.close();
    denied([&] { (void)gate.pin(id); }, "actor gate admitted work after close");
    check(cleaned == 8, "actor code images were not reclaimed on close");
}

static void terminal_close_preserves_old_pin() {
    std::atomic<int> cleaned{0};
    const ActorHeapIdentity id{99, 7, 1};
    Gate gate(id, shape(Gate::Kind::trusted_shared), "v1",
              [&] { ++cleaned; });
    auto pin = gate.pin(id);
    gate.close();
    check(cleaned == 0 && pin.code() == "v1",
          "closing actor prematurely reclaimed executable code");
    pin.close();
    check(cleaned == 1, "last old-code pin did not reclaim image");
}

static void racing_turn_admission_vs_upgrade_freeze() {
    // Linearization proof under racing admission: a pin admitted before the
    // freeze must be counted, or a later pin must be rejected. Never neither.
    for (std::uint64_t iteration = 1; iteration <= 200; ++iteration) {
        const ActorHeapIdentity id{55, 8800 + iteration, 1};
        std::atomic<int> reclaimed{0};
        Gate gate(id, shape(Gate::Kind::trusted_private), "v1",
                  [&] { ++reclaimed; });
        std::atomic<bool> start{false}, release{false};
        std::atomic<bool> entered{false}, rejected{false};
        std::thread caller([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            try {
                auto pin = gate.pin(id);
                entered.store(true, std::memory_order_release);
                while (!release.load(std::memory_order_acquire))
                    std::this_thread::yield();
                check(pin.descriptor().revision == 1,
                      "racing turn read a revised code generation");
                pin.close();
            } catch (const std::logic_error&) {
                rejected.store(true, std::memory_order_release);
            }
        });
        start.store(true, std::memory_order_release);
        auto ticket = gate.freeze(id, 1);
        const bool quiet_at_freeze = gate.quiesced(ticket);
        release.store(true, std::memory_order_release);
        caller.join();
        check(entered.load() != rejected.load(),
              "racing turn was neither admitted nor rejected exactly once");
        check(!quiet_at_freeze || rejected.load(),
              "freeze falsely reported quiescence with an active turn");
        check(gate.quiesced(ticket), "racing actor pin did not drain");
        check(gate.promote(ticket, shape(Gate::Kind::trusted_private), "v2",
                           [&] { ++reclaimed; }).revision == 2,
              "race-free upgrade did not publish");
        gate.close();
        check(reclaimed == 2, "racing upgrade leaked/reclaimed code twice");
    }
}

static void abandoned_freeze_rolls_back_without_reclaiming_live_code() {
    std::atomic<int> reclaims{0};
    const ActorHeapIdentity id{111, 222, 333};
    Gate gate(id, shape(Gate::Kind::trusted_private), "v1",
              [&] { ++reclaims; });
    {
        auto forgotten = gate.freeze(id, 1);
        denied([&] { (void)gate.pin(id); }, "frozen actor admitted DATA before ticket died");
    }
    auto resumed = gate.pin(id);
    check(resumed.code() == "v1" && reclaims == 0,
          "RAII ticket cleanup discarded live code or failed to resume admission");
    resumed.close();
    {
        auto first = gate.freeze(id, 1);
        auto transferred = std::move(first);
        denied([&] { gate.abort(first); }, "moved-from token retained CONTROL authority");
        check(gate.quiesced(transferred), "move invalidated the surviving freeze token");
    }
    auto again = gate.pin(id);
    again.close();
    auto live = gate.freeze(id, 1);
    // An abandoned token from a past epoch must not undo a new freeze.
    gate.abort(live);
    auto later = gate.freeze(id, 1);
    denied([&] { (void)gate.pin(id); }, "newer freeze bypassed");
    check(gate.promote(later, shape(Gate::Kind::trusted_private), "v2",
                       [&] { ++reclaims; }).revision == 2,
          "RAII abort damaged a later valid publication");
    gate.close();
    check(reclaims == 2, "admission recovery leaked code revisions");
}

struct TrackedActorImage final {
    std::shared_ptr<std::atomic<int>> live;
    explicit TrackedActorImage(std::shared_ptr<std::atomic<int>> value)
        : live(std::move(value)) { ++*live; }
    TrackedActorImage(const TrackedActorImage&) = delete;
    TrackedActorImage& operator=(const TrackedActorImage&) = delete;
    TrackedActorImage(TrackedActorImage&&) noexcept = default;
    TrackedActorImage& operator=(TrackedActorImage&&) noexcept = default;
    ~TrackedActorImage() noexcept { if (live) --*live; }
};
static void actor_rejection_reclaims_after_wrapper_destructor() {
    using TrackedGate = NativeActorHotUpgradeGate<TrackedActorImage>;
    const ActorHeapIdentity id{200, 1, 9};
    const TrackedGate::Shape actor_shape{
        TrackedGate::Kind::untrusted, "msg", "state", "shared", "cap"};
    std::atomic<int> calls{0}, early_unloads{0};
    auto alive = [] { return std::make_shared<std::atomic<int>>(0); };
    auto cleanup = [&](std::shared_ptr<std::atomic<int>> counter) {
        return [&, counter] {
            ++calls;
            if (counter->load() != 0) ++early_unloads;
        };
    };
    const auto old = alive();
    TrackedGate gate(id, actor_shape, TrackedActorImage(old), cleanup(old));
    auto pinned = gate.pin(id);
    auto ticket = gate.freeze(id, 1);
    const auto denied_code = alive();
    denied([&] {
        (void)gate.promote(ticket, actor_shape, TrackedActorImage(denied_code),
                           cleanup(denied_code));
    }, "promoted while old code had a running pin");
    check(denied_code->load() == 0 && calls == 1 && early_unloads == 0,
          "gate rejection unloaded library before code wrapper destructor");
    pinned.close();
    const auto new_code = alive();
    check(gate.promote(ticket, actor_shape, TrackedActorImage(new_code),
                       cleanup(new_code)).revision == 2,
          "valid tracked image could not be published");
    check(old->load() == 0 && new_code->load() == 1,
          "old tracked generation was not retired after publication");
    gate.close();
    check(new_code->load() == 0 && calls == 3 && early_unloads == 0,
          "successful code upgrade unloaded a live wrapper or leaked image");
}

static void actor_pin_outlives_gate_instance() {
    const ActorHeapIdentity id{201, 2, 10};
    std::atomic<int> cleanup{0};
    std::optional<Gate::Pin> outside;
    {
        Gate gate(id, shape(Gate::Kind::trusted_shared), "persistent",
                  [&] { ++cleanup; });
        outside.emplace(gate.pin(id));
    }
    check(cleanup == 0 && outside->code() == "persistent",
          "gate destructor unloaded an image with a live continuation pin");
    outside.reset();
    check(cleanup == 1, "orphaned continuation pin leaked retired code");
}


struct FailingActivationCopy {
    std::atomic<bool>* fail_copy;
    Gate::Ticket* ticket;
    std::atomic<int>* cleanups;
    FailingActivationCopy(std::atomic<bool>& f, Gate::Ticket& t, std::atomic<int>& c)
        : fail_copy(&f), ticket(&t), cleanups(&c) {}
    FailingActivationCopy(const FailingActivationCopy& other)
        : fail_copy(other.fail_copy), ticket(other.ticket),
          cleanups(other.cleanups) {
        if (fail_copy->load()) throw std::runtime_error("injected callback copy failure");
    }
    void operator()() const {
        ++*cleanups;
        ticket->abandon();
    }
};
static void abandoned_publication_restores_admission_on_exception() {
    const ActorHeapIdentity id{321, 77, 1};
    std::atomic<int> old_cleanup{0}, rejected_cleanup{0};
    std::atomic<bool> fail_copy{false};
    Gate gate(id, shape(Gate::Kind::trusted_private), "v1",
              [&] { ++old_cleanup; });
    auto ticket = gate.freeze(id, 1);
    std::function<void()> cleanup(
        FailingActivationCopy(fail_copy, ticket, rejected_cleanup));
    fail_copy.store(true);
    bool thrown = false;
    try {
        (void)gate.promote(ticket, shape(Gate::Kind::trusted_private),
                           "v2", std::move(cleanup));
    } catch (const std::runtime_error&) { thrown = true; }
    check(thrown, "fault injection did not fail publication");
    check(rejected_cleanup == 1 && old_cleanup == 0,
          "rejected candidate reclaimed incorrectly");
    auto resumed = gate.pin(id);
    check(resumed.code() == "v1",
          "abandoned publication leaked freeze or damaged old image");
    resumed.close();
    auto retry = gate.freeze(id, 1);
    check(gate.promote(retry, shape(Gate::Kind::trusted_private), "v3",
                       [&] { ++old_cleanup; }).revision == 2,
          "failed publication prevented valid retry");
    gate.close();
    check(old_cleanup == 2, "retry leaked code generations");
}
static void abandon_during_successful_publication_is_terminal_safe() {
    const ActorHeapIdentity id{321, 78, 1};
    std::atomic<int> reclaimed{0};
    Gate::Ticket* active = nullptr;
    Gate gate(id, shape(Gate::Kind::trusted_shared), "old", [&] {
        ++reclaimed;
        if (active) active->abandon();
    });
    auto ticket = gate.freeze(id, 1);
    active = &ticket;
    check(gate.promote(ticket, shape(Gate::Kind::trusted_shared),
                       "new", [&] { ++reclaimed; }).revision == 2,
          "abandoned successful publish was not committed");
    auto pin = gate.pin(id);
    check(pin.code() == "new", "successful release left DATA frozen");
    pin.close();
    gate.close();
    check(reclaimed == 2, "successful abandoned ticket double reclaimed code");
}

int main() {
    actor_code_only_upgrade(Gate::Kind::trusted_shared);
    actor_code_only_upgrade(Gate::Kind::trusted_private);
    actor_code_only_upgrade(Gate::Kind::untrusted);
    terminal_close_preserves_old_pin();
    racing_turn_admission_vs_upgrade_freeze();
    abandoned_freeze_rolls_back_without_reclaiming_live_code();
    actor_rejection_reclaims_after_wrapper_destructor();
    actor_pin_outlives_gate_instance();
    abandoned_publication_restores_admission_on_exception();
    abandon_during_successful_publication_is_terminal_safe();
}
