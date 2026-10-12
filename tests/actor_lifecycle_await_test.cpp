#include "oreslang/runtime/ActorLifecycleAwait.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::NativeActorLifecycleAwait;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    {
        NativeActorGroup parent(9101, 1), children(9102, 1);
        auto parent_ref = parent.adopt(9101, 1, 1, [] {});
        auto child = children.adopt(9102, 1, 1, [] {});
        check(parent.mark_ready(parent_ref), "parent not ready");
        auto frame = NativeActorLifecycleAwait::wait_ready(child);
        check(frame->phase() == NativeActorLifecycleAwait::Phase::pending,
              "ready frame began resolved");
        int resumed = 0;
        auto resume = [&](const NativeActorLifecycleAwait::Result& result) {
            check(result.success && result.error.empty(), "ready observed failure");
            ++resumed;
        };
        check(!frame->try_schedule(parent, parent_ref, resume),
              "unresolved ready future resumed early");
        check(parent.enqueue_control(parent_ref, [] {}),
              "could not fill reserved parent CONTROL");
        check(children.mark_ready(child), "child not initialized");
        check(frame->phase() == NativeActorLifecycleAwait::Phase::ready,
              "child READY did not settle frame");
        check(resumed == 0, "child carrier invoked guest resume inline");
        check(!frame->try_schedule(parent, parent_ref, resume),
              "full CONTROL silently accepted second continuation");
        check(frame->phase() == NativeActorLifecycleAwait::Phase::ready,
              "full CONTROL erased required retry");
        check(parent.run_control_turn(parent_ref) == NativeActorGroup::ControlTurn::executed,
              "could not drain reserved parent CONTROL");
        check(frame->try_schedule(parent, parent_ref, resume),
              "retried READY continuation not enqueued");
        check(frame->phase() == NativeActorLifecycleAwait::Phase::scheduled,
              "frame did not pin scheduled state");
        check(resumed == 0, "enqueue executed guest inline");
        check(parent.run_control_turn(parent_ref) == NativeActorGroup::ControlTurn::executed,
              "scheduler refused READY continuation");
        check(resumed == 1, "READY did not resume exactly once");
        check(frame->phase() == NativeActorLifecycleAwait::Phase::completed,
              "frame not completed on CONTROL turn");
        check(!frame->try_schedule(parent, parent_ref, resume),
              "duplicate READY resumption accepted");
        check(children.finalized(child), "child not quiesced");
        check(parent.finalized(parent_ref), "parent not quiesced");
    }
    {
        NativeActorGroup parent(9201, 1);
        auto supervisor = parent.adopt(9201, 1, 1, [] {});
        check(parent.mark_ready(supervisor), "second parent not ready");
        std::shared_ptr<NativeActorLifecycleAwait> frame;
        std::optional<NativeActorGroup::RuntimeFinalizer> finalizer;
        {
            NativeActorGroup children(9202, 1);
            auto child = children.adopt(9202, 1, 1, [] {});
            check(children.mark_ready(child), "second child not ready");
            finalizer.emplace(children.runtime_finalizer(child));
            frame = NativeActorLifecycleAwait::wait_done(child);
            check(frame->phase() == NativeActorLifecycleAwait::Phase::pending,
                  "DONE settled before shutdown");
            children.close();
            check(frame->phase() == NativeActorLifecycleAwait::Phase::pending,
                  "close manufactured DONE");
        }
        int resumed = 0;
        check(finalizer->finalized(), "late runtime finalization not admitted");
        check(frame->phase() == NativeActorLifecycleAwait::Phase::ready,
              "post-group-destruction DONE frame not settled");
        check(frame->try_schedule(parent, supervisor, [&](const auto& result) {
            check(result.success && result.error.empty(),
                  "late DONE completed with failure");
            ++resumed;
        }), "post-group DONE did not enqueue continuation");
        check(resumed == 0, "late physical ACK invoked guest on runtime thread");
        check(parent.run_control_turn(supervisor) == NativeActorGroup::ControlTurn::executed,
              "post-group DONE did not resume on parent CONTROL");
        check(resumed == 1, "post-group DONE lost/doubled continuation");
        check(!finalizer->finalized(), "duplicate late ACK succeeded");
        check(parent.finalized(supervisor), "second parent not finalized");
    }
    {
        NativeActorGroup parent(9301, 1), children(9302, 1);
        auto supervisor = parent.adopt(9301, 1, 1, [] {});
        auto child = children.adopt(9302, 1, 1, [] {});
        check(parent.mark_ready(supervisor), "cancel parent not ready");
        auto frame = NativeActorLifecycleAwait::wait_ready(child);
        children.close();
        check(frame->phase() == NativeActorLifecycleAwait::Phase::ready,
              "failed startup not observed");
        bool guest_executed = false;
        check(frame->try_schedule(parent, supervisor, [&](const auto&) {
            guest_executed = true;
        }), "failed-startup continuation not queued");
        frame->cancel();
        check(parent.run_control_turn(supervisor) == NativeActorGroup::ControlTurn::executed,
              "cancelled frame left CONTROL slot stranded");
        check(!guest_executed, "cancelled continuation ran guest callback");
        check(frame->phase() == NativeActorLifecycleAwait::Phase::cancelled,
              "cancelled frame was resurrected");
        check(children.finalized(child), "cancelled child not finalized");
        check(parent.finalized(supervisor), "cancel parent not finalized");
    }
    {
        // Weak subscriptions do not retain abandoned frames or create cycles.
        NativeActorGroup group(9401, 1);
        auto child = group.adopt(9401, 1, 1, [] {});
        std::weak_ptr<NativeActorLifecycleAwait> observer;
        {
            auto pending = NativeActorLifecycleAwait::wait_done(child);
            observer = pending;
            pending->cancel();
        }
        check(observer.expired(), "child Future callback retained abandoned frame");
        check(group.finalized(child), "weak subscription blocked actor teardown");
    }
}
