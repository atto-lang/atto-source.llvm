#include "oreslang/runtime/NativeActorHotUpgradeGate.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace oreslang::runtime;
using Code = std::int64_t (*)(std::int64_t) noexcept;
using Gate = NativeActorHotUpgradeGate<Code>;

static std::int64_t compiled_version_one(std::int64_t n) noexcept { return n + 11; }
static std::int64_t compiled_version_two(std::int64_t n) noexcept { return n * 7; }
static std::int64_t compiled_version_three(std::int64_t n) noexcept { return n - 3; }
static void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
static Gate::Shape shape(Gate::Kind kind) {
    return {kind, "actor-msg-v1", "state-v1", "shared-cell-v1", "caps-v1"};
}
static std::int64_t dispatch(Gate& gate, ActorHeapIdentity identity, std::int64_t n) {
    // This is an actual native machine-code callback dispatch, not a string
    // label. The runtime-owned pin brackets every caller-visible invocation.
    auto turn = gate.pin(identity);
    return turn.code()(n);
}
static void one_actor(Gate::Kind kind) {
    const ActorHeapIdentity id{100, 900, 4};
    std::atomic<int> reclaimed{0};
    Gate gate(id, shape(kind), &compiled_version_one, [&] { ++reclaimed; }, 3);
    require(dispatch(gate, id, 7) == 18, "initial native AOT function not invoked");

    auto pinned_old_call = gate.pin(id);
    auto release = gate.freeze(id, 1);
    require(!gate.quiesced(release), "live native function lease not counted");
    require(pinned_old_call.code()(7) == 18, "pinned old executable changed");
    bool denied = false;
    try { (void)dispatch(gate, id, 7); }
    catch (const std::logic_error&) { denied = true; }
    require(denied, "guest dispatch entered after CONTROL freeze");
    pinned_old_call.close();
    require(gate.quiesced(release), "completed native callback not quiescent");

    auto next = gate.promote(release, shape(kind), &compiled_version_two,
                             [&] { ++reclaimed; });
    require(next.revision == 2 && reclaimed == 1, "new executable not atomically published");
    require(dispatch(gate, id, 7) == 49, "new native function pointer not executed");

    // Parallel callbacks enter the same actor code generation while the
    // supervisor owns the freeze barrier. This proves a lifetime boundary,
    // NOT single-flight actor state exclusion, which belongs to the scheduler.
    std::atomic<int> completed{0}, invalid{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&] {
            for (int j = 0; j < 200; ++j) {
                try {
                    if (dispatch(gate, id, 7) == 49) ++completed;
                    else ++invalid;
                } catch (...) { ++invalid; }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    require(completed == 1600 && invalid == 0,
            "native code callback dispatch lost generation consistency");

    auto frozen = gate.freeze(id, 2);
    require(gate.quiesced(frozen), "completed native callbacks leaked pins");
    require(gate.promote(frozen, shape(kind), &compiled_version_three,
                         [&] { ++reclaimed; }).revision == 3,
            "third compiled native revision could not publish");
    require(dispatch(gate, id, 7) == 4, "third compiled native body did not execute");
    gate.close();
    require(reclaimed == 3, "native callback code generations leaked on close");
}
int main() {
    one_actor(Gate::Kind::trusted_shared);
    one_actor(Gate::Kind::trusted_private);
    one_actor(Gate::Kind::untrusted);
}
