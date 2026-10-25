#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/ActorOwnedGraphHeap.hpp"
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <thread>

using oreslang::runtime::ActorHeapIdentity;
using oreslang::runtime::ActorOwnedGraphHeap;
using Graph = ActorOwnedGraphHeap<32, sizeof(std::uint64_t), 1>;
enum : std::size_t { Actors = 2, Messages = 32 };

static void require(bool yes) { if (!yes) std::abort(); }

struct Lane {
    const ActorHeapIdentity owner;
    Graph graph;
    std::optional<Graph::Handle> last;
    std::atomic<unsigned> executing{0};
    std::atomic<unsigned> faults{0};
    std::uint64_t processed = 0;
    explicit Lane(ActorHeapIdentity id) : owner(id), graph(id) {}
};
struct Fixture {
    OresNativeActorScheduler* scheduler = nullptr;
    std::array<OresNativeActorRef, Actors> refs{};
    Lane lanes[Actors]{Lane(ActorHeapIdentity{120, 201, 1}),
                       Lane(ActorHeapIdentity{120, 202, 1})};
    std::atomic<unsigned> bad_slots{0};
};

static void callback(void* raw, std::size_t slot, std::uint64_t payload) {
    auto& fixture = *static_cast<Fixture*>(raw);
    if (slot >= Actors) { ++fixture.bad_slots; return; }
    Lane& lane = fixture.lanes[slot];
    if (lane.executing.fetch_add(1) != 0) ++lane.faults;
    try {
        if (!ores_native_actor_scheduler_is_current_actor(
                fixture.scheduler, fixture.refs[slot])) ++lane.faults;
        if (ores_native_actor_scheduler_is_current_actor(
                fixture.scheduler, fixture.refs[1 - slot])) ++lane.faults;
        if (payload == 0 || payload > Messages) ++lane.faults;
        else {
            std::uint64_t bytes = payload + 1000 * slot;
            auto node = lane.graph.allocate(lane.owner, &bytes, sizeof(bytes));
            if (!node) ++lane.faults;
            else {
                if (lane.last && !lane.graph.link(lane.owner, *lane.last, *node))
                    ++lane.faults;
                lane.last = *node;
                ++lane.processed;
            }
        }
        if (payload == 1) {
            bool denied = false;
            try {
                std::uint64_t byte = payload;
                (void)lane.graph.allocate(fixture.lanes[1 - slot].owner,
                                          &byte, sizeof(byte));
            } catch (const std::logic_error&) {
                denied = true;
            }
            if (!denied) ++lane.faults;
        }
    } catch (...) {
        ++lane.faults; // C++ exceptions must never unwind into a C callback
    }
    std::this_thread::yield();
    lane.executing.fetch_sub(1);
}

int main() {
    for (unsigned round = 0; round < 8; ++round) {
        Fixture f{};
        f.scheduler = ores_native_actor_scheduler_open(
            Actors, Messages + 1, 2, callback, &f);
        require(f.scheduler != nullptr);
        for (std::size_t i = 0; i < Actors; ++i) {
            f.refs[i] = ores_native_actor_scheduler_spawn(f.scheduler, i);
            require(f.refs[i].scheduler_domain != 0);
        }
        for (std::size_t payload = 1; payload <= Messages; ++payload) {
            const std::size_t first = (payload + round) % Actors;
            for (std::size_t k = 0; k < Actors; ++k) {
                const std::size_t lane = (first + k) % Actors;
                require(ores_native_actor_scheduler_try_send(
                    f.scheduler, f.refs[lane], payload) == ORES_NATIVE_SEND_ACCEPTED);
            }
        }
        for (const auto& ref : f.refs)
            require(ores_native_actor_scheduler_stop(f.scheduler, ref));
        ores_native_actor_scheduler_retire(f.scheduler);
        require(ores_native_actor_scheduler_drain(f.scheduler));
        for (std::size_t i = 0; i < Actors; ++i) {
            require(ores_native_actor_scheduler_poll_done(f.scheduler, f.refs[i]));
            require(f.lanes[i].faults.load() == 0);
            require(f.lanes[i].executing.load() == 0);
            require(f.lanes[i].processed == Messages);
            require(f.lanes[i].graph.live_nodes(f.lanes[i].owner) == Messages);
            require(f.lanes[i].graph.live_edges(f.lanes[i].owner) == Messages - 1);
            std::uint64_t last = 0;
            require(f.lanes[i].last.has_value());
            require(f.lanes[i].graph.copy_out(f.lanes[i].owner,
                *f.lanes[i].last, &last, sizeof(last)) == sizeof(last));
            require(last >= 1 + 1000 * i && last <= Messages + 1000 * i);
        }
        require(f.bad_slots.load() == 0);
        require(ores_native_actor_scheduler_completed(f.scheduler) ==
                Actors * Messages);
        require(ores_native_actor_scheduler_try_destroy(f.scheduler));
    }
    std::puts("ACTOR_SCHEDULER_GRAPH_BRIDGE_PASS");
}
