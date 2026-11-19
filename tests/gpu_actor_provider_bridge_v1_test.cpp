#include "oreslang/runtime/GpuActorProviderBridgeV1.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::gpu_actor;
using Lane = oreslang::SupervisorFailureLane;
using Registry = GpuActorQuarantineRegistryV1;
using Port = ProviderPortV1;
using Bridge = GpuActorProviderBridgeV1;

namespace {
std::size_t checks=0;
void check(bool ok, const std::string& description) {
    if(!ok) throw std::runtime_error("GPU_PROVIDER_TEST_FAILED: "+description);
    ++checks;
}
template <class F> void denied(F&& fn, const char* code) {
    try { fn(); } catch(const std::exception& e) {
        if(std::string(e.what()).find(code)!=std::string::npos) {
            check(true,code); return;
        }
        throw;
    }
    throw std::runtime_error(std::string("GPU_PROVIDER_MISSING_DENIAL: ")+code);
}
Port::Binding binding(const std::string& actor,
                      const std::string& tenant="tenantA",
                      const std::string& device="gpu0",
                      std::uint64_t generation=1) {
    return {actor,generation,tenant,device,"queue0",5,7,101};
}

class MockPort : public Port {
public:
    explicit MockPort(Binding b) : Port(std::move(b)) {}
    EnqueueState enqueue_mode=EnqueueState::Submitted;
    EnqueueState reconcile_mode=EnqueueState::Unknown;
    Observation observation=Observation::Pending;
    EventHandle last_event;
    EventHandle injected_event;
    bool release_ok=false;
    bool throw_in_enqueue=false;
    bool throw_in_poll=false;
    bool throw_in_release=false;
    int enqueue_calls=0, poll_calls=0, reconcile_calls=0, release_calls=0;

    EnqueueResult make_report(EnqueueState mode, Turn turn) {
        if(mode==EnqueueState::Submitted) {
            const auto ev=injected_event?injected_event:issue_event(turn);
            last_event=ev;
            return {mode,ev};
        }
        return {mode,injected_event};
    }
    EnqueueResult enqueue(Turn turn) override {
        ++enqueue_calls;
        if(throw_in_enqueue) throw std::runtime_error("MOCK_PROVIDER_AMBIGUOUS_ENQUEUE");
        return make_report(enqueue_mode,turn);
    }
    EnqueueResult reconcile_unknown(Turn turn) override {
        ++reconcile_calls;
        return make_report(reconcile_mode,turn);
    }
    Observation observe(const EventHandle& event) override {
        ++poll_calls;
        if(throw_in_poll) throw std::runtime_error("MOCK_POLL_TRANSIENT");
        check(event==last_event,"provider observes same minted event");
        return observation;
    }
    bool try_release_buffer() override {
        ++release_calls;
        if (throw_in_release) throw std::runtime_error("MOCK_RELEASE_UNKNOWN");
        return release_ok;
    }
    EventHandle mint_for_test(Turn turn) { return issue_event(turn); }
};

class BlockedPort final : public MockPort {
public:
    explicit BlockedPort(Binding b) : MockPort(std::move(b)) {}
    std::mutex mu;
    std::condition_variable cv;
    bool entered=false;
    bool proceed=false;
    EnqueueResult enqueue(Turn turn) override {
        {
            std::unique_lock<std::mutex> guard(mu);
            entered=true; cv.notify_all();
            cv.wait(guard,[&]{return proceed;});
        }
        return MockPort::enqueue(turn);
    }
    void await_entered() {
        std::unique_lock<std::mutex> guard(mu);
        cv.wait(guard,[&]{return entered;});
    }
    void resume() {
        std::lock_guard<std::mutex> guard(mu);
        proceed=true; cv.notify_all();
    }
};

class BlockedReleasePort final : public MockPort {
public:
    explicit BlockedReleasePort(Binding b) : MockPort(std::move(b)) {}
    std::mutex mu;
    std::condition_variable cv;
    bool entered=false, proceed=false, blocked_once=false;
    bool try_release_buffer() override {
        {
            std::unique_lock<std::mutex> guard(mu);
            if(!blocked_once) {
                blocked_once=true; entered=true; cv.notify_all();
                cv.wait(guard,[&]{return proceed;});
            }
        }
        return MockPort::try_release_buffer();
    }
    void await_entered() {
        std::unique_lock<std::mutex> guard(mu);
        cv.wait(guard,[&]{return entered;});
    }
    void resume() {
        std::lock_guard<std::mutex> guard(mu);
        proceed=true; cv.notify_all();
    }
};

class BlockedReconcilePort final : public MockPort {
public:
    explicit BlockedReconcilePort(Binding b) : MockPort(std::move(b)) {}
    std::mutex mu;
    std::condition_variable cv;
    bool entered=false, proceed=false;
    EnqueueResult reconcile_unknown(Turn turn) override {
        {
            std::unique_lock<std::mutex> guard(mu);
            entered=true; cv.notify_all();
            cv.wait(guard,[&]{return proceed;});
        }
        return MockPort::reconcile_unknown(turn);
    }
    void await_entered() {
        std::unique_lock<std::mutex> guard(mu);
        cv.wait(guard,[&]{return entered;});
    }
    void resume() {
        std::lock_guard<std::mutex> guard(mu);
        proceed=true;cv.notify_all();
    }
};

std::shared_ptr<Lane> lane(std::size_t capacity=8) {
    return std::make_shared<Lane>(capacity);
}
std::shared_ptr<Registry> registry(std::size_t capacity=8) {
    return std::make_shared<Registry>(capacity);
}
std::unique_ptr<Bridge> actor(
    const std::shared_ptr<Registry>& r, const std::shared_ptr<Lane>& l,
    const std::shared_ptr<MockPort>& p, std::int64_t initial=0) {
    return std::make_unique<Bridge>(
        r,p,l,Lane::ActorKey{p->binding().actor_id,p->binding().actor_generation},
        "root_supervisor",4096,initial);
}
void drain_notice(const std::shared_ptr<Lane>& l, Lane::Kind kind) {
    auto message=l->poll();
    check(message.has_value(),"CONTROL notice emitted");
    check(message->kind==kind,"CONTROL failure kind");
    check(message->parent_id=="root_supervisor","CONTROL parent");
    l->ack(message->actor);
}

void successful_event_is_host_bound_and_single_commit() {
    auto r=registry(); auto l=lane(); auto p=std::make_shared<MockPort>(binding("success"));
    auto a=actor(r,l,p,10);
    auto turn=a->submit(7);
    check(turn.actor_generation==1 && turn.sequence==1,"turn identity");
    check(p->last_event && p->last_event->serial()>0,"provider-minted event");
    check(p->authentic_local_event(p->last_event,turn),"host event binding");
    check(a->committed_state()==10,"no early state publication");
    r->progress_all();
    check(a->committed_state()==10,"pending event not committed");
    p->observation=Port::Observation::CompleteOk;
    r->progress_all();
    check(a->committed_state()==17,"once complete publish state");
    r->progress_all();
    check(a->committed_state()==17,"duplicate poll not double commit");
    check(p->poll_calls==2,"one pending plus one terminal observation");
    denied([&]{a->submit(1); a->submit(2);},"GPU_PROVIDER_ACTOR_NOT_READY");
    // A new submitted turn is active. Retire only after its event.
    p->observation=Port::Observation::CompleteOk;
    r->progress_all();
    check(a->committed_state()==18,"next turn committed once");
    denied([&]{a->retire_gracefully();},"GPU_PROVIDER_RELEASE_NOT_PROVEN");
    check(!a->completed(),"cannot claim unproven physical release");
    p->release_ok=true;
    a->retire_gracefully();
    check(a->completed(),"graceful retire after release");
    check(l->pending_count()==0 && l->reserved_count()==0,"normal retirement has no failure");
    a.reset();
    check(r->retained_count()==0,"graceful registry slot reclaimed");
}

void forged_cross_provider_event_denied_and_quarantined() {
    auto r=registry(); auto l=lane();
    auto p=std::make_shared<MockPort>(binding("forgery"));
    auto attacker=std::make_shared<MockPort>(binding("forgery"));
    p->injected_event=attacker->mint_for_test({1,1});
    check(!p->authentic_local_event(p->injected_event,{1,1}),"foreign provider proof rejected");
    auto a=actor(r,l,p);
    denied([&]{(void)a->submit(12);},"GPU_PROVIDER_FOREIGN_OR_STALE_EVENT");
    check(a->committed_state()==0,"forged event did not commit");
    check(r->retained_count()==1,"untrusted enqueue not released");
    p->injected_event.reset();
    p->reconcile_mode=Port::EnqueueState::Submitted;
    a.reset();
    check(r->quarantined_count()==1,"actor detach retains unresolved event");
    r->progress_all(); // reconcile: valid local event, but host is stopping
    check(r->quarantined_count()==1,"reconciled event still waits for quiescence");
    p->observation=Port::Observation::CompleteOk;
    r->progress_all();
    check(r->quarantined_count()==1,"release still pending");
    p->release_ok=true;
    r->progress_all();
    check(r->retained_count()==0,"released and pruned after verified observation");
    drain_notice(l,Lane::Kind::HostFailure);
}

void stale_turn_event_cannot_reuse_completed_token() {
    auto r=registry(); auto l=lane();
    auto p=std::make_shared<MockPort>(binding("stale", "tenantA","gpu0",2));
    auto a=actor(r,l,p);
    auto first=a->submit(1);
    check(first.actor_generation==2 && first.sequence==1,"first generation-qualified turn");
    auto stale=p->last_event;
    p->observation=Port::Observation::CompleteOk;
    r->progress_all();
    check(a->committed_state()==1,"first event complete");
    p->injected_event=stale;
    denied([&]{(void)a->submit(3);},"GPU_PROVIDER_FOREIGN_OR_STALE_EVENT");
    check(a->committed_state()==1,"stale event cannot commit second turn");
    p->injected_event.reset();
    p->reconcile_mode=Port::EnqueueState::Submitted;
    a.reset();
    r->progress_all();
    r->progress_all();
    p->release_ok=true;
    r->progress_all();
    check(r->retained_count()==0,"stale event quarantine recovered");
    drain_notice(l,Lane::Kind::HostFailure);
}

void detached_actor_keeps_provider_and_buffer_quarantined() {
    auto r=registry(1); auto l=lane(1);
    auto p=std::make_shared<MockPort>(binding("detached"));
    auto a=actor(r,l,p);
    a->submit(50);
    a.reset();
    check(r->retained_count()==1 && r->quarantined_count()==1,
          "actor object gone but resource retained");
    r->progress_all();
    check(r->retained_count()==1,"pending device operation retained");
    p->observation=Port::Observation::CompleteOk;
    r->progress_all();
    check(r->retained_count()==1,"quiescent but physical buffer not released");
    check(p->release_calls==0,"first completion step is not release");
    r->progress_all();
    check(r->retained_count()==1,"unsuccessful provider release retains memory");
    p->release_ok=true;
    r->progress_all();
    check(r->retained_count()==0,"physical release proof permits cleanup");
    drain_notice(l,Lane::Kind::Cancel);
}

void unknown_enqueue_never_assumes_no_command() {
    auto r=registry(1); auto l=lane(1);
    auto p=std::make_shared<MockPort>(binding("unknown"));
    p->enqueue_mode=Port::EnqueueState::Unknown;
    auto a=actor(r,l,p);
    a->submit(10);
    a.reset();
    check(r->quarantined_count()==1,"unknown submission quarantined");
    r->progress_all();
    check(r->retained_count()==1,"no inferred pre-enqueue abort");
    p->reconcile_mode=Port::EnqueueState::DefinitelyNotSubmitted;
    r->progress_all();
    check(r->retained_count()==1,"definitively aborted, release still pending");
    p->release_ok=true;
    r->progress_all();
    check(r->retained_count()==0,"definite absence plus physical release");
    drain_notice(l,Lane::Kind::Cancel);
}

void device_loss_poison_all_matching_cohort_only() {
    auto r=registry(3); auto l=lane(3);
    auto p1=std::make_shared<MockPort>(binding("cohort-a"));
    auto p2=std::make_shared<MockPort>(binding("cohort-b"));
    auto p3=std::make_shared<MockPort>(binding("other","tenantB","gpu0"));
    auto a=actor(r,l,p1), b=actor(r,l,p2), c=actor(r,l,p3);
    a->submit(3); b->submit(9); c->submit(5);
    r->report_device_loss("tenantA","gpu0",5);
    p1->observation=Port::Observation::DeviceLost;
    p2->observation=Port::Observation::Unknown;
    p3->observation=Port::Observation::CompleteOk;
    r->progress_all();
    check(a->committed_state()==0 && b->committed_state()==0,
          "cohort staged state never published on loss");
    check(c->committed_state()==5,"other tenant unaffected");
    a.reset();b.reset();
    r->progress_all();
    check(r->quarantined_count()==2,"device loss alone never proves quiescence");
    p1->observation=Port::Observation::CompleteFailed;
    p2->observation=Port::Observation::CompleteFailed;
    r->progress_all();
    p1->release_ok=true; p2->release_ok=true;
    r->progress_all();
    check(r->retained_count()==1,"both damaged actors safely retired");
    for(int i=0;i<2;i++) drain_notice(l,Lane::Kind::HostFailure);
    p3->release_ok=true;c->retire_gracefully();c.reset();
    check(r->retained_count()==0,"surviving actor retires normally");
}

void registry_quota_rejects_new_actors_until_physical_release() {
    auto r=registry(1); auto l=lane(2);
    auto p1=std::make_shared<MockPort>(binding("quota-one"));
    auto a=actor(r,l,p1);a->submit(2);a.reset();
    auto p2=std::make_shared<MockPort>(binding("quota-two"));
    denied([&]{auto rejected=actor(r,l,p2);},"GPU_PROVIDER_QUARANTINE_CAPACITY_EXHAUSTED");
    p1->observation=Port::Observation::CompleteOk;
    r->progress_all();p1->release_ok=true;r->progress_all();
    check(r->retained_count()==0,"quarantine slot freed only with provider release");
    drain_notice(l,Lane::Kind::Cancel);
    auto b=actor(r,l,p2);
    p2->release_ok=true;
    b->retire_gracefully();
    b.reset();
    check(r->retained_count()==0,"quota reuse safe");
}

void exception_during_enqueue_remains_ambiguous() {
    auto r=registry(1); auto l=lane(1);
    auto p=std::make_shared<MockPort>(binding("exception"));
    p->throw_in_enqueue=true;
    auto a=actor(r,l,p);
    denied([&]{(void)a->submit(99);},"MOCK_PROVIDER_AMBIGUOUS_ENQUEUE");
    check(a->committed_state()==0,"exception may not commit");
    a.reset();
    check(r->quarantined_count()==1,"exception quarantine owns resources");
    r->progress_all();
    check(r->retained_count()==1,"unknown command not silently discarded");
    p->reconcile_mode=Port::EnqueueState::DefinitelyNotSubmitted;
    r->progress_all();p->release_ok=true;r->progress_all();
    check(r->retained_count()==0,"definitive no-enqueue recovered");
    drain_notice(l,Lane::Kind::HostFailure);
}

void cancel_racing_enqueued_submission_does_not_abort() {
    auto r=registry(1); auto l=lane(1);
    auto p=std::make_shared<BlockedPort>(binding("race"));
    std::shared_ptr<MockPort> as_mock=p;
    auto a=actor(r,l,as_mock);
    std::atomic<bool> submit_ok{false};
    std::thread producer([&] {
        try { (void)a->submit(2);submit_ok.store(true); }
        catch (...) { /* rejected by test assertion below */ }
    });
    p->await_entered();
    a->request_close(); // while port is inside enqueue
    check(a->committed_state()==0,"close does not commit");
    check(r->retained_count()==1,"close cannot release during enqueue");
    p->resume();producer.join();
    check(submit_ok.load(),"late durable enqueue was accepted into poisoned turn");
    a.reset();
    check(r->quarantined_count()==1,"post-submit cancellation retains event");
    p->observation=Port::Observation::Pending;
    r->progress_all();
    check(r->retained_count()==1,"pending fenced cancellation");
    p->observation=Port::Observation::CompleteOk;
    r->progress_all();
    p->release_ok=true;r->progress_all();
    check(r->retained_count()==0,"safe cancellation after event and release");
    drain_notice(l,Lane::Kind::Cancel);
}

void malformed_provider_reply_is_quarantined_not_lost() {
    for(const auto mode : {Port::EnqueueState::Unknown,
                           Port::EnqueueState::DefinitelyNotSubmitted}) {
        auto r=registry(1); auto l=lane(1);
        auto p=std::make_shared<MockPort>(binding("malformed"));
        p->enqueue_mode=mode;
        p->injected_event=p->mint_for_test({1,1});
        auto a=actor(r,l,p);
        denied([&]{(void)a->submit(19);},
               mode==Port::EnqueueState::Unknown
               ? "GPU_PROVIDER_UNKNOWN_HAS_EVENT" : "GPU_PROVIDER_SPURIOUS_EVENT");
        check(a->committed_state()==0,"malformed receipt never commits state");
        a.reset();
        check(r->quarantined_count()==1,"ambiguous malformed receipt retained");
        p->injected_event.reset();
        p->reconcile_mode=Port::EnqueueState::DefinitelyNotSubmitted;
        r->progress_all();
        check(r->retained_count()==1,"no release until explicit provider result");
        p->release_ok=true;r->progress_all();
        check(r->retained_count()==0,"malformed provider receipt can recover safely");
        drain_notice(l,Lane::Kind::HostFailure);
    }
}

void population_1024_mock_provider_turns() {
    constexpr int count=1024;
    auto r=registry(count);auto l=lane(count);
    std::vector<std::shared_ptr<MockPort>> providers;
    std::vector<std::unique_ptr<Bridge>> actors;
    providers.reserve(count);actors.reserve(count);
    for(int i=0;i<count;i++) {
        auto p=std::make_shared<MockPort>(binding("bulk-"+std::to_string(i)));
        p->observation=Port::Observation::CompleteOk;
        p->release_ok=true;
        actors.push_back(actor(r,l,p));
        providers.push_back(std::move(p));
    }
    for(int n=1;n<=4;n++) {
        for(auto& a:actors) (void)a->submit(n);
        r->progress_all();
    }
    for(auto& a:actors) {
        check(a->committed_state()==10,"1024 independent actor state");
        a->retire_gracefully();
        check(a->completed(),"provider release before actor retirement");
        a.reset();
    }
    check(r->retained_count()==0,"all 1024 registry slots returned");
    check(l->reserved_count()==0 && l->pending_count()==0,
          "all 1024 supervisor reservations returned without fake CONTROL");
}

void retirement_release_window_denies_concurrent_actor_turn() {
    auto r=registry(1);auto l=lane(1);
    auto p=std::make_shared<BlockedReleasePort>(binding("retire-race"));
    std::shared_ptr<MockPort> port=p;
    auto a=actor(r,l,port);
    p->release_ok=true;
    std::atomic<bool> retired{false};
    std::thread worker([&] {
        try {a->retire_gracefully(); retired.store(true);}
        catch(...) { /* verified below */ }
    });
    p->await_entered();
    denied([&] { (void)a->submit(1); },"GPU_PROVIDER_ACTOR_NOT_READY");
    check(a->committed_state()==0,"retire guard prevents mid-release mutation");
    p->resume();worker.join();
    check(retired.load(),"reserved retirement completed");
    check(a->completed(),"retired ledger cannot reenter actor");
    a.reset();
    check(r->retained_count()==0,"normal retired slot released");
    check(l->reserved_count()==0,"CONTROL reservation retired without failure");

    auto p2=std::make_shared<BlockedReleasePort>(binding("retire-vs-close"));
    std::shared_ptr<MockPort> port2=p2;
    auto b=actor(r,l,port2);
    p2->release_ok=true;
    std::atomic<bool> rejected{false};
    std::thread w2([&] {
        try {b->retire_gracefully();}
        catch(const std::logic_error& e) {
            rejected.store(std::string(e.what()).find("GPU_PROVIDER_RELEASE_NOT_PROVEN")
                            != std::string::npos);
        }
    });
    p2->await_entered();
    b->request_close(); // cancel races with provider release
    p2->resume();w2.join();
    check(rejected.load(),"inconsistent concurrent retirement rejected");
    denied([&] { (void)b->submit(1); },"GPU_PROVIDER_ACTOR_NOT_READY");
    r->progress_all();
    check(b->completed(),"cancelled actor retires only after provider release");
    check(p2->release_calls==1,"cancel-vs-retire must not release same buffer twice");
    b.reset();
    check(r->retained_count()==0,"retire-vs-close slot freed");
    drain_notice(l,Lane::Kind::Cancel);
}

void terminal_release_serializes_concurrent_progress_workers() {
    auto r=registry(1); auto l=lane(1);
    auto p=std::make_shared<BlockedReleasePort>(binding("terminal-release"));
    std::shared_ptr<MockPort> as_mock=p;
    auto a=actor(r,l,as_mock);
    (void)a->submit(31);
    a.reset();
    p->observation=Port::Observation::CompleteOk;
    r->progress_all(); // terminal device event; needs explicit buffer release
    check(r->retained_count()==1,"terminal actor still owns buffer");

    p->release_ok=true;
    std::atomic<bool> first_finished{false};
    std::thread releasing([&] {
        r->progress_all(); // blocks inside the native provider release
        first_finished.store(true);
    });
    p->await_entered();
    r->progress_all(); // competing poll must NOT enter the same provider release
    check(!first_finished.load(),"first release still in provider");
    check(r->retained_count()==1,"registry remains pinned throughout release");
    p->resume();
    releasing.join();
    check(first_finished.load(),"first provider release completed");
    check(p->release_calls==1,"exactly one provider release for one allocation");
    check(r->retained_count()==0,"terminal actor pruned after release");
    drain_notice(l,Lane::Kind::Cancel);
}

void unknown_enqueue_reconciliation_serializes_concurrent_pollers() {
    auto r=registry(1);auto l=lane(1);
    auto p=std::make_shared<BlockedReconcilePort>(binding("reconcile-race"));
    std::shared_ptr<MockPort> as_mock=p;
    p->enqueue_mode=Port::EnqueueState::Unknown;
    p->reconcile_mode=Port::EnqueueState::DefinitelyNotSubmitted;
    auto a=actor(r,l,as_mock);
    (void)a->submit(5);
    a.reset();
    std::thread first([&]{r->progress_all();});
    p->await_entered();
    r->progress_all(); // second poll cannot enter same native reconciliation
    check(p->reconcile_calls==0,"first reconciliation has not returned");
    check(r->retained_count()==1,"unresolved enqueue remains quarantined");
    p->resume();first.join();
    check(p->reconcile_calls==1,"one provider reconciliation at a time");
    check(r->retained_count()==1,"definite absence is not buffer release");
    p->release_ok=true;r->progress_all();
    check(r->retained_count()==0,"quiescent buffer released exactly once");
    drain_notice(l,Lane::Kind::Cancel);
}

void malformed_reconciliation_sends_host_failure_control() {
    auto r=registry(1);auto l=lane(1);
    auto p=std::make_shared<MockPort>(binding("bad-reconcile"));
    p->enqueue_mode=Port::EnqueueState::Unknown;
    auto a=actor(r,l,p);
    (void)a->submit(19);
    p->reconcile_mode=Port::EnqueueState::Unknown;
    p->injected_event=p->mint_for_test({1,1});
    r->progress_all();
    check(a->committed_state()==0,"malformed reconcile never publishes state");
    p->injected_event.reset();
    p->reconcile_mode=Port::EnqueueState::DefinitelyNotSubmitted;
    a.reset();
    r->progress_all(); // now provider proves no kernel was submitted
    p->release_ok=true;r->progress_all();
    check(r->retained_count()==0,"malformed reconcile preserved until release");
    drain_notice(l,Lane::Kind::HostFailure);
}

void release_exception_keeps_quarantine_and_retries() {
    auto r=registry(1);auto l=lane(1);
    auto p=std::make_shared<MockPort>(binding("release-exception"));
    p->throw_in_release=true;
    auto a=actor(r,l,p);
    (void)a->submit(9);
    a.reset();p->observation=Port::Observation::CompleteOk;
    r->progress_all(); // event quiescent, still terminal
    r->progress_all(); // provider throws; registry retains physical claim
    check(r->retained_count()==1,"native release exception retains buffer");
    check(p->release_calls==1,"first provider release attempted once");
    p->throw_in_release=false;p->release_ok=true;
    r->progress_all();
    check(p->release_calls==2,"retry only after exception unwound");
    check(r->retained_count()==0,"event/release proof allows final prune");
    drain_notice(l,Lane::Kind::Cancel);
}

void failed_actor_admission_unpins_empty_process_root() {
    auto r=registry(1);auto l=lane(2);
    auto p1=std::make_shared<MockPort>(binding("actor-duplicate"));
    // A competing supervisor already owns this actor incarnation.
    l->reserve({"actor-duplicate",1});
    const auto before=Registry::retained_registry_count();
    denied([&]{auto bad=actor(r,l,p1);},"duplicate supervision reservation");
    check(r->retained_count()==0,"failed admission erases placeholder entry");
    check(Registry::retained_registry_count()==before,
          "failed actor constructor must not leak a rooted empty registry");
    l->retire_gracefully({"actor-duplicate",1});
    auto a=actor(r,l,p1);
    p1->release_ok=true;
    a->retire_gracefully();a.reset();
    check(r->retained_count()==0,"same registry remains reusable after failed admission");
}

void supervisor_handle_loss_preserves_process_root_quarantine() {
    check(Registry::retained_registry_count()==0,
          "all prior actor registry roots were retired");
    auto r=registry(1); auto l=lane(1);
    auto p=std::make_shared<MockPort>(binding("root-retained"));
    auto a=actor(r,l,p);
    a->submit(13);
    std::weak_ptr<Registry> weak=r;
    r.reset(); // immediate supervisor lost its registry handle
    a.reset(); // actor wrapper is now also gone
    check(!weak.expired(),"process root retained unresolved work");
    check(Registry::retained_registry_count()==1,
          "one bounded process root pins the orphan registry");
    Registry::progress_process_root();
    check(!weak.expired(),"pending provider event cannot release root registry");
    p->observation=Port::Observation::CompleteOk;
    Registry::progress_process_root();
    check(!weak.expired(),"quiescent event alone is not physical buffer release");
    p->release_ok=true;
    Registry::progress_process_root();
    check(weak.expired(),"root registry released only after confirmed buffer release");
    check(Registry::retained_registry_count()==0,"process-root slot reclaimed");
    drain_notice(l,Lane::Kind::Cancel);
}

void forged_binding_and_invalid_admission_rejected() {
    auto r=registry(); auto l=lane();
    auto p=std::make_shared<MockPort>(binding("actual"));
    denied([&] {
        Bridge wrong(r,p,l,{"different",1},"root",4096);
    },"GPU_PROVIDER_ADMISSION_BINDING_MISMATCH");
    check(r->retained_count()==0,"bad actor binding not reserved");
    denied([&] { (void)registry(0); },"GPU_PROVIDER_INVALID_REGISTRY_CAPACITY");
    auto blank=binding("actual");blank.queue.clear();
    denied([&] { auto bad=std::make_shared<MockPort>(blank); },
           "GPU_PROVIDER_INVALID_BINDING");
}

} // namespace
int main() {
    successful_event_is_host_bound_and_single_commit();
    forged_cross_provider_event_denied_and_quarantined();
    stale_turn_event_cannot_reuse_completed_token();
    detached_actor_keeps_provider_and_buffer_quarantined();
    unknown_enqueue_never_assumes_no_command();
    device_loss_poison_all_matching_cohort_only();
    registry_quota_rejects_new_actors_until_physical_release();
    exception_during_enqueue_remains_ambiguous();
    cancel_racing_enqueued_submission_does_not_abort();
    forged_binding_and_invalid_admission_rejected();
    malformed_provider_reply_is_quarantined_not_lost();
    supervisor_handle_loss_preserves_process_root_quarantine();
    retirement_release_window_denies_concurrent_actor_turn();
    terminal_release_serializes_concurrent_progress_workers();
    unknown_enqueue_reconciliation_serializes_concurrent_pollers();
    malformed_reconciliation_sends_host_failure_control();
    release_exception_keeps_quarantine_and_retries();
    failed_actor_admission_unpins_empty_process_root();
    population_1024_mock_provider_turns();
    if(checks<3100) throw std::runtime_error("GPU_PROVIDER_INSUFFICIENT_CHECKS");
    std::cout<<"GPU_PROVIDER_BRIDGE_V1_HOST_PASS checks="<<checks
             <<" actors=1024 turns=4096 physical_gpu=false provider_observation=mock"<<std::endl;
}
