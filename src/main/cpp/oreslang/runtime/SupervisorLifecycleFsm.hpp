#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace oreslang::runtime {

// Host CONTROL reducer only. Receipt issuers must authenticate runtime and
// physical quiescence; numeric tokens alone confer no authority. No callbacks,
// heap allocation or scheduler operations occur in successful transitions.
class SupervisorLifecycleFsm final {
public:
    static constexpr std::size_t max_children = 64;
    enum class Phase : std::uint8_t {
        running, restarting, before_hook, wait_children, wait_actors,
        after_hook, stopped, reclaimed
    };
    struct Child { std::uint64_t id = 0, incarnation = 0; };
    enum class Kind : std::uint8_t {
        restart_requested, restart_committed, shutdown_requested,
        before_finished, child_finished, child_reclaimed, actors_finished,
        after_finished, physical_release, deadline, coordinator_failed
    };
    struct Event {
        Kind kind;
        std::uint64_t epoch;
        Child child{};
        bool failed = false;
        std::array<Child, max_children> children{};
        std::size_t child_count = 0;
    };
    // Pointer-free record. One authoritative layout; storage adapters may
    // select AoS/SoA/AoSoA/SoAoS without synchronized shadow representations.
    struct State {
        Phase phase = Phase::running;
        std::uint64_t epoch = 1, revision = 0;
        std::array<Child, max_children> children{};
        std::size_t child_count = 0;
        std::uint64_t completed = 0, released_children = 0;
        bool before_done = false, actors_done = false, after_done = false;
        bool physical_released = false, failed = false, timed_out = false;
        bool admissions_open() const { return phase == Phase::running; }
        bool logically_stopped() const {
            return phase == Phase::stopped || phase == Phase::reclaimed;
        }
    };

    static State reduce(const State& s, const Event& e) {
        validate(s);
        if (e.epoch < s.epoch) return s;
        require(e.epoch == s.epoch, "future supervisor epoch");
        State n = s;
        switch (e.kind) {
        case Kind::restart_requested:
            if (s.phase == Phase::restarting) return s;
            require(s.phase == Phase::running, "restart after shutdown fence");
            n.phase = Phase::restarting;
            break;
        case Kind::restart_committed:
            require(s.phase == Phase::restarting && s.actors_done
                    && s.physical_released, "restart before retirement");
            require(s.epoch != std::numeric_limits<std::uint64_t>::max(), "epoch overflow");
            n = State{};
            n.epoch = s.epoch + 1;
            break;
        case Kind::shutdown_requested:
            if (s.phase != Phase::running && s.phase != Phase::restarting) return s;
            require(e.child_count <= max_children, "child capacity");
            for (std::size_t i = 0; i < e.child_count; ++i) {
                require(e.children[i].id && e.children[i].incarnation, "invalid child token");
                for (std::size_t j = 0; j < i; ++j)
                    require(e.children[j].id != e.children[i].id, "duplicate child id");
            }
            n.children = e.children;
            n.child_count = e.child_count;
            n.completed = n.released_children = 0;
            n.phase = Phase::before_hook;
            break;
        case Kind::before_finished:
            if (s.before_done) return s;
            require(s.phase == Phase::before_hook, "pre hook out of order");
            n.before_done = true;
            n.failed |= e.failed;
            n.phase = s.child_count ? Phase::wait_children : Phase::wait_actors;
            break;
        case Kind::child_finished:
        case Kind::child_reclaimed: {
            std::size_t slot = 0;
            while (slot < s.child_count && s.children[slot].id != e.child.id) ++slot;
            require(slot < s.child_count, "unknown child");
            if (e.child.incarnation < s.children[slot].incarnation) return s;
            require(e.child.incarnation == s.children[slot].incarnation, "future child incarnation");
            const auto bit = std::uint64_t{1} << slot;
            if (e.kind == Kind::child_finished) {
                if (s.completed & bit) return s;
                require(s.phase == Phase::wait_children, "child before pre hook");
                n.completed |= bit;
                n.failed |= e.failed;
                if (n.completed == mask(n)) n.phase = Phase::wait_actors;
            } else {
                if (s.released_children & bit) return s;
                require(s.completed & bit, "child release before logical stop");
                n.released_children |= bit;
            }
            break;
        }
        case Kind::actors_finished:
            if (s.actors_done) return s;
            n.actors_done = true;
            n.failed |= e.failed;
            break;
        case Kind::after_finished:
            if (s.after_done) return s;
            require(s.phase == Phase::after_hook, "post hook out of order");
            n.after_done = true;
            n.failed |= e.failed;
            n.phase = Phase::stopped;
            break;
        case Kind::physical_release:
            if (s.physical_released) return s;
            require(s.actors_done, "release before actors retire");
            n.physical_released = true;
            break;
        case Kind::deadline:
            if (s.timed_out || s.logically_stopped()) return s;
            require(s.phase != Phase::running && s.phase != Phase::restarting,
                    "deadline without shutdown");
            n.timed_out = true;
            break;
        case Kind::coordinator_failed:
            if (s.failed) return s;
            n.failed = true;
            break;
        default: throw std::logic_error("unknown supervisor event");
        }
        if (n.phase == Phase::wait_actors && n.actors_done) n.phase = Phase::after_hook;
        if (n.phase == Phase::stopped && n.physical_released
                && n.released_children == mask(n)) n.phase = Phase::reclaimed;
        require(s.revision != std::numeric_limits<std::uint64_t>::max(), "revision overflow");
        n.revision = s.revision + 1;
        validate(n);
        return n;
    }

private:
    static void require(bool ok, const char* message) {
        if (!ok) throw std::logic_error(message);
    }
    static std::uint64_t mask(const State& s) {
        return s.child_count == max_children ? ~std::uint64_t{0}
            : (std::uint64_t{1} << s.child_count) - 1;
    }
    static void validate(const State& s) {
        require(s.epoch && s.child_count <= max_children, "invalid state identity/capacity");
        require(static_cast<unsigned>(s.phase) <= static_cast<unsigned>(Phase::reclaimed), "invalid phase");
        require(!(s.completed & ~mask(s)) && !(s.released_children & ~s.completed), "invalid child receipts");
        require(!s.physical_released || s.actors_done, "release without actor done");
        if (s.phase == Phase::after_hook || s.logically_stopped())
            require(s.before_done && s.actors_done && s.completed == mask(s), "premature post/stop");
        if (s.logically_stopped()) require(s.after_done, "stop without post hook");
        if (s.phase == Phase::reclaimed)
            require(s.physical_released && s.released_children == mask(s), "premature reclaim");
    }
};
static_assert(std::is_trivially_copyable_v<SupervisorLifecycleFsm::State>);
} // namespace oreslang::runtime
