// Legacy host-turn coverage migrated to production registry capabilities.
// There is deliberately NO V0 Boolean-completion test macro or direct
// access to HostTurnBridgeV0. All source-like turns enter the V1 registry.
#include "oreslang/runtime/GpuActorProviderBridgeV1.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Registry = oreslang::gpu_actor::GpuActorQuarantineRegistryV1;
using Port = oreslang::gpu_actor::ProviderPortV1;
using Bridge = oreslang::gpu_actor::GpuActorProviderBridgeV1;
using Lane = oreslang::SupervisorFailureLane;

namespace {
std::size_t checks = 0;
void check(bool ok,const char* label) {
    if(!ok) throw std::runtime_error(std::string("GPU_V0_REGISTRY_MIGRATION_FAILED: ")+label);
    ++checks;
}
template<class F> void denied(F&& action,const char* expected) {
    try {action();}
    catch (const std::exception& e) {
        if (std::string(e.what()).find(expected)!=std::string::npos) {
            check(true,"expected rejection");
            return;
        }
        throw;
    }
    throw std::runtime_error(std::string("GPU_V0_MIGRATION_MISSING_DENIAL: ")+expected);
}
class TrustedFixturePort final : public Port {
public:
    explicit TrustedFixturePort(Binding binding): Port(std::move(binding)) {}
    Observation observed=Observation::Pending;
    bool release_proven=false;
    EventHandle event;
    EnqueueResult enqueue(Turn turn) override {
        event=issue_event(turn);
        return {EnqueueState::Submitted,event};
    }
    EnqueueResult reconcile_unknown(Turn) override {
        return {EnqueueState::Unknown,nullptr};
    }
    Observation observe(const EventHandle& next) override {
        if(next!=event) return Observation::Unknown;
        return observed;
    }
    bool try_release_buffer() override {return release_proven;}
};
Port::Binding binding(std::string name,std::uint64_t generation=3) {
    return {std::move(name),generation,"tenant","mock-gpu","queue",1,1,1};
}
void population_through_registry() {
    constexpr int actors=64;
    constexpr int turns=4;
    auto registry=std::make_shared<Registry>(actors);
    auto lane=std::make_shared<Lane>(actors);
    std::vector<std::shared_ptr<TrustedFixturePort>> ports;
    std::vector<std::unique_ptr<Bridge>> members;
    ports.reserve(actors);
    members.reserve(actors);
    for(int i=0;i<actors;++i) {
        auto id="actor-"+std::to_string(i);
        auto port=std::make_shared<TrustedFixturePort>(binding(id));
        members.push_back(std::make_unique<Bridge>(
            registry,port,lane,Lane::ActorKey{id,3},"parent",4096,0));
        ports.push_back(std::move(port));
    }
    for(int turn=1;turn<=turns;++turn) {
        for(int i=0;i<actors;++i) {
            const auto identity=members[i]->submit(turn);
            check(identity.sequence==static_cast<std::uint64_t>(turn),"sequence bound");
            check(identity.actor_generation==3,"generation bound");
            check(ports[i]->event!=nullptr,"trusted port event minted");
            check(members[i]->committed_state()==turn*(turn-1)/2,
                  "stage invisible until event completion");
            ports[i]->observed=Port::Observation::CompleteOk;
        }
        registry->progress_all();
    }
    for(int i=0;i<actors;++i) {
        check(members[i]->committed_state()==10,"one commit per actual completion");
        ports[i]->release_proven=true;
        members[i]->retire_gracefully();
        check(members[i]->completed(),"backend release required for retirement");
        members[i].reset();
    }
    check(registry->retained_count()==0,"all registry leases retired");
    check(lane->reserved_count()==0,"all CONTROL slots returned");
    check(lane->pending_count()==0,"normal retire does not fabricate failure");
    check(Registry::retained_registry_count()==0,"root quarantine empty");
}
void cancellation_releases_only_after_event() {
    auto registry=std::make_shared<Registry>(1);
    auto lane=std::make_shared<Lane>(1);
    auto p=std::make_shared<TrustedFixturePort>(binding("cancel"));
    auto actor=std::make_unique<Bridge>(registry,p,lane,Lane::ActorKey{"cancel",3},
                                       "parent",4096,7);
    actor->submit(42);
    actor->request_close();
    registry->progress_all();
    check(actor->committed_state()==7,"cancel does not commit pending GPU state");
    check(!actor->completed(),"pending event prevents retirement");
    denied([&]{actor->submit(1);},"GPU_PROVIDER_ACTOR_NOT_READY");
    p->observed=Port::Observation::CompleteOk;
    registry->progress_all();
    check(actor->committed_state()==7,"cancelled completed kernel never commits");
    check(!actor->completed(),"event alone not physical buffer release");
    p->release_proven=true;
    registry->progress_all();
    check(actor->completed(),"release after quiescence");
    actor.reset();
    check(registry->retained_count()==0,"cancelled lease reclaimed");
    auto control=lane->poll();
    check(control && control->kind==Lane::Kind::Cancel,"supervisor CANCEL CONTROL");
    lane->ack(control->actor);
    check(lane->reserved_count()==0,"supervisor cancellation reservation freed");
}
void stale_identity_and_overflow_denied() {
    auto registry=std::make_shared<Registry>(1);
    auto lane=std::make_shared<Lane>(1);
    auto p=std::make_shared<TrustedFixturePort>(binding("checked"));
    denied([&]{Bridge invalid(registry,p,lane,Lane::ActorKey{"checked",4},
                              "parent",4096);},"GPU_PROVIDER_ADMISSION_BINDING_MISMATCH");
    auto actor=std::make_unique<Bridge>(registry,p,lane,Lane::ActorKey{"checked",3},
                                       "parent",4096,INT64_MAX);
    denied([&]{actor->submit(1);},"GPU_ACTOR_STAGE_OVERFLOW");
    check(actor->committed_state()==INT64_MAX,"overflow leaves state intact");
    p->release_proven=true;
    actor->retire_gracefully();
    actor.reset();
    check(registry->retained_count()==0,"overflow actor gracefully retired");
}
}

int main() {
    population_through_registry();
    cancellation_releases_only_after_event();
    stale_identity_and_overflow_denied();
    if(checks<1000) throw std::runtime_error("GPU_V0_REGISTRY_MIGRATION_COVERAGE_SHORTFALL");
    std::cout<<"GPU_ACTOR_V0_REGISTRY_MIGRATION_PASS checks="<<checks
             <<" logical_actors=64 physical_gpu=false direct_v0_boolean_calls=0"
             <<std::endl;
}
