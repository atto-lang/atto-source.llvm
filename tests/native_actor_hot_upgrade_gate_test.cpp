#include "oreslang/runtime/NativeActorHotUpgradeGate.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
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
    catch (const std::invalid_argument&) { rejected = true; }
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

int main() {
    actor_code_only_upgrade(Gate::Kind::trusted_shared);
    actor_code_only_upgrade(Gate::Kind::trusted_private);
    actor_code_only_upgrade(Gate::Kind::untrusted);
    terminal_close_preserves_old_pin();
}
