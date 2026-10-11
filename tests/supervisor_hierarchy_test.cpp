#include "oreslang/runtime/SupervisorControlTurn.hpp"
#include <stdexcept>
#include <string>

using oreslang::SupervisorControlTurn;
using oreslang::SupervisorFailureLane;
#define VERIFY(expr) do { if (!(expr)) throw std::logic_error("failed: " #expr); } while (false)

struct Router final : SupervisorControlTurn::Hooks {
    SupervisorControlTurn* parent = nullptr;
    bool acknowledge = false;
    int panics = 0;
    int escalations = 0;
    int user_turns = 0;

    Decision on_throw(const SupervisorFailureLane::Failure&) override {
        return Decision::Escalate;
    }
    Decision on_raise(const SupervisorFailureLane::Failure&) override {
        return Decision::Escalate;
    }
    Decision on_panic(const SupervisorFailureLane::Failure&) override {
        ++panics;
        return acknowledge ? Decision::Acknowledge : Decision::Escalate;
    }
    Decision on_cancel(const SupervisorFailureLane::Failure&) override {
        return Decision::Escalate;
    }
    Decision on_host_failure(const SupervisorFailureLane::Failure&) override {
        return Decision::Escalate;
    }
    void on_escalate(const SupervisorFailureLane::Failure& failure) override {
        ++escalations;
        VERIFY(parent != nullptr);
        // Parent already holds a per-descendant CONTROL reservation. This
        // only queues the event; it must not run the parent's guest hook.
        VERIFY(parent->publish_child(
            failure.actor, failure.kind, failure.summary, failure.parent_id));
    }
    bool on_restart(const SupervisorFailureLane::Failure&) override {
        return false;
    }
    void on_user_turn() override { ++user_turns; }
};

int main() {
    using Kind = SupervisorFailureLane::Kind;
    using Key = SupervisorFailureLane::ActorKey;

    SupervisorControlTurn middle(2);
    SupervisorControlTurn grandparent(2);
    Key descendant{"descendant", 3};
    middle.reserve_child(descendant);
    grandparent.reserve_child(descendant);

    Router middleHooks, grandparentHooks;
    middleHooks.parent = &grandparent;
    grandparentHooks.acknowledge = true;

    // Child publishes onto middle's preallocated CONTROL; grandparent's
    // guest code does not execute on the failing child's carrier.
    VERIFY(middle.publish_child(descendant, Kind::Panic, "panic", "middle"));
    VERIFY(middle.urgent());
    VERIFY(grandparent.pending_count() == 0);
    VERIFY(middle.admit_turn(middleHooks, true) == SupervisorControlTurn::Admitted::Control);
    VERIFY(middleHooks.panics == 1);
    VERIFY(middleHooks.escalations == 1);
    VERIFY(middleHooks.user_turns == 0);
    VERIFY(middle.reserved_count() == 0);

    // Only an explicitly admitted grandparent logical turn executes its hook.
    VERIFY(grandparent.urgent());
    VERIFY(grandparentHooks.panics == 0);
    VERIFY(grandparent.admit_turn(grandparentHooks, true)
        == SupervisorControlTurn::Admitted::Control);
    VERIFY(grandparentHooks.panics == 1);
    VERIFY(grandparentHooks.escalations == 0);
    VERIFY(grandparent.reserved_count() == 0);
    VERIFY(grandparent.admit_turn(grandparentHooks, true)
        == SupervisorControlTurn::Admitted::User);
    VERIFY(grandparentHooks.user_turns == 1);
    return 0;
}
