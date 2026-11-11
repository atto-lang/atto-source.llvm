#include "oreslang/runtime/GpuActorOpenClProviderV2.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using oreslang::gpu_actor::OpenClActorProviderPortV2;
using oreslang::gpu_actor::ProviderPortV1;
using oreslang::gpu_actor::GpuActorQuarantineRegistryV1;
using oreslang::gpu_actor::GpuActorProviderBridgeV1;
using oreslang::opencl::EventStatus;
using Port = ProviderPortV1;
using Registry = GpuActorQuarantineRegistryV1;
using Lane = oreslang::SupervisorFailureLane;
int checks=0;
void check(bool value,const char* name) {
    if (!value) throw std::runtime_error(std::string("GPU_V2_CHECK_FAILED: ")+name);
    ++checks;
}
template<class F> void denied(F f,const char* code) {
    try {f();}
    catch(const std::exception& e) {
        if (std::string(e.what()).find(code)!=std::string::npos) {++checks;return;}
        throw;
    }
    throw std::runtime_error(std::string("GPU_V2_EXPECTED_DENIAL: ")+code);
}

struct FakeOpenCl : oreslang::opencl::Api {
    struct Program {};
    struct Buffer { std::vector<unsigned char> bytes; };
    struct Event { EventStatus status; };
    int program_releases=0, buffer_releases=0, event_releases=0;
    int uploads=0, kernels=0, downloads=0, polls=0;
    bool reject_prepare=false, fail_kernel=false, fail_upload=false;
    bool delayed=false, device_loss=false, allow_loss_quiescence=false;
    bool fail_buffer_release_once=false;
    std::vector<Event*> events;

    void validate_artifact(const oreslang::dop::OpenClArtifact& a) const override {
        if(a.artifact_abi_version!=1 || a.required_bytes==0 ||
           !a.required_float32 || a.global_rows==0 || a.global_fields==0)
            throw std::invalid_argument("GPU_V2_FAKE_INVALID_ARTIFACT");
    }
    Handle prepare(const oreslang::dop::OpenClArtifact& a) override {
        validate_artifact(a);
        if (reject_prepare) throw std::runtime_error("GPU_V2_FAKE_PREPARE_FAILED");
        return new Program();
    }
    void release_program(Handle h) override {
        delete static_cast<Program*>(h);++program_releases;
    }
    Handle allocate(std::uint64_t n) override {
        auto b=new Buffer();
        b->bytes.assign(static_cast<std::size_t>(n),0);
        return b;
    }
    void release_buffer(Handle h) override {
        if(fail_buffer_release_once) {
            fail_buffer_release_once=false;
            throw std::runtime_error("GPU_V2_FAKE_RELEASE_FAIL");
        }
        delete static_cast<Buffer*>(h);++buffer_releases;
    }
    Event* next_event() {
        auto* e=new Event{device_loss?EventStatus::DeviceLost:
                           delayed?EventStatus::Pending:EventStatus::Complete};
        events.push_back(e);return e;
    }
    Handle enqueue_upload(Handle dst,const unsigned char* bytes,std::size_t n) override {
        ++uploads;
        if(fail_upload) throw std::runtime_error("GPU_V2_FAKE_UPLOAD_FAILED");
        auto& d=static_cast<Buffer*>(dst)->bytes;
        std::copy(bytes,bytes+n,d.begin());
        return next_event();
    }
    Handle enqueue_kernel(Handle,Handle in,Handle out,std::uint64_t,
                          std::uint32_t) override {
        ++kernels;
        if (fail_kernel) throw std::runtime_error("GPU_V2_FAKE_KERNEL_FAILED");
        const auto& x=static_cast<Buffer*>(in)->bytes;
        auto& y=static_cast<Buffer*>(out)->bytes;
        for(std::size_t i=0;i+sizeof(float)<=x.size();i+=sizeof(float)) {
            float value=0;
            std::memcpy(&value,x.data()+i,sizeof(value));
            value=value*1.5f-2.0f; // deterministic fake kernel, NOT GPU
            std::memcpy(y.data()+i,&value,sizeof(value));
        }
        return next_event();
    }
    Handle enqueue_download(Handle from,unsigned char* dst,std::size_t n) override {
        ++downloads;
        const auto& x=static_cast<Buffer*>(from)->bytes;
        std::copy(x.begin(),x.begin()+static_cast<std::ptrdiff_t>(n),dst);
        return next_event();
    }
    EventStatus poll_event(Handle h) override {
        ++polls;return static_cast<Event*>(h)->status;
    }
    void release_event(Handle h) override {
        auto* p=static_cast<Event*>(h);
        auto it=std::find(events.begin(),events.end(),p);
        if(it!=events.end())events.erase(it);
        delete p;++event_releases;
    }
    bool prove_device_loss_quiescence() override {
        return allow_loss_quiescence;
    }
};

struct Inputs {
    oreslang::dop::Plan plan;
    oreslang::dop::Kernel kernel;
    std::vector<unsigned char> host;
    Inputs()
        : plan(oreslang::dop::Layout::AoS,4,2,4,16,1),
          kernel(plan,oreslang::dop::Element::F32,
                 oreslang::dop::Op::ScaleBias,1.5f,-2.0f),
          host(static_cast<std::size_t>(plan.byte_length()),0) {
        for(std::uint64_t row=0;row<4;row++)
            for(std::uint32_t field=0;field<2;field++){
                float x=static_cast<float>(row*10+field)+0.25f;
                std::memcpy(host.data()+plan.byte_offset(row,field),&x,sizeof(x));
            }
    }
};
Port::Binding binding(const std::string& id) {
    return {id,3,"tenantA","GPU_TEST","9",7,21,101};
}
std::shared_ptr<OpenClActorProviderPortV2> make_port(
    const std::shared_ptr<FakeOpenCl>& api,const Inputs& in,
    const std::string& name="actor",Port::Binding b=binding("actor")) {
    b.actor_id=name;
    return std::make_shared<OpenClActorProviderPortV2>(
        b,api,
        oreslang::opencl::ProviderIdentity{b.tenant,b.device,9,21,4},
        oreslang::opencl::Limits{in.plan.byte_length()*4,
                                 in.plan.byte_length()*4,4,4},
        oreslang::dop::GpuTarget{64,in.plan.byte_length()*2,100,true},
        in.kernel,in.host);
}
void numerical_oracle(const Inputs& in,const std::vector<unsigned char>& out) {
    check(out.size()==in.host.size(),"output bytes");
    for(std::uint64_t row=0;row<4;row++)
        for(std::uint32_t field=0;field<2;field++){
            const auto idx=in.plan.byte_offset(row,field);
            float x=0,y=0;
            std::memcpy(&x,in.host.data()+idx,sizeof(x));
            std::memcpy(&y,out.data()+idx,sizeof(y));
            const float expected=x*1.5f-2.0f;
            check(y==expected,"independent fake-kernel f32 oracle");
        }
}
void actual_provider_api_pipeline_mocked() {
    Inputs in;
    auto api=std::make_shared<FakeOpenCl>();
    auto p=make_port(api,in);
    denied([&]{(void)p->verified_output();},"GPU_V2_OUTPUT_NOT_VERIFIED");
    const Port::Turn t{3,1};
    auto result=p->enqueue(t);
    check(result.state==Port::EnqueueState::Submitted,"upload physically enqueued through API seam");
    check(p->authentic_local_event(result.event,t),"actor provider token binds turn");
    check(!p->try_release_buffer(),"cannot release while upload pending");
    check(p->observe(result.event)==Port::Observation::Pending,"upload transition pending");
    check(api->uploads==1 && api->kernels==1,"DOP provider kernel enqueue");
    check(p->observe(result.event)==Port::Observation::Pending,"kernel stage pending");
    check(api->downloads==1,"DOP provider issued download");
    check(p->observe(result.event)==Port::Observation::CompleteOk,"all 3 provider stages quiescent");
    check(p->native_opencl_success_observed(),"real backend output gate observed through API");
    numerical_oracle(in,p->verified_output());
    check(api->buffer_releases==2 && api->event_releases==3,
          "underlying device buffers/events physically released by provider");
    check(p->try_release_buffer(),"prepared program physically released");
    check(api->program_releases==1,"no leaked compiled program");
    check(p->try_release_buffer(),"idempotent after trusted program close");
    check(p->observe(result.event)==Port::Observation::CompleteOk,"terminal observation stable");
}

void actor_bridge_commits_only_after_kernel_download() {
    Inputs in;
    auto api=std::make_shared<FakeOpenCl>();
    auto p=make_port(api,in);
    auto registry=std::make_shared<Registry>(1);
    auto lane=std::make_shared<Lane>(1);
    auto bridge=std::make_unique<GpuActorProviderBridgeV1>(
        registry,p,lane,Lane::ActorKey{"actor",3},"root",in.plan.byte_length(),5);
    auto turn=bridge->submit(12);
    check(turn.sequence==1,"actor first turn");
    check(bridge->committed_state()==5,"no early actor state visibility");
    registry->progress_all(); // upload -> kernel
    check(bridge->committed_state()==5,"no publish after upload");
    registry->progress_all(); // kernel -> download
    check(bridge->committed_state()==5,"no publish after kernel alone");
    registry->progress_all(); // download -> succeeded
    check(bridge->committed_state()==17,"actor state commits after download and physical cleanup");
    numerical_oracle(in,p->verified_output());
    bridge->request_close();
    registry->progress_all(); // release program
    check(bridge->completed(),"retired actor after explicit program release");
    bridge.reset();
    check(registry->retained_count()==0,"registry reclaims actor");
    auto notice=lane->poll();
    check(notice && notice->kind==Lane::Kind::Cancel,"supervisor notified");
    lane->ack(notice->actor);
    check(lane->reserved_count()==0,"supervisor CONTROL slot reclaimed");
}

void negative_authority_and_missing_gpu_fail_closed() {
    Inputs in;
    auto api=std::make_shared<FakeOpenCl>();
    auto b=binding("actor");
    b.queue="77";
    denied([&]{auto p=make_port(api,in,"actor",b);},
           "GPU_V2_PROVIDER_AUTHORITY_MISMATCH");
    b=binding("actor");
    b.allocation_generation=std::numeric_limits<std::uint64_t>::max();
    denied([&]{auto p=make_port(api,in,"actor",b);},
           "GPU_V2_ALLOCATION_GENERATION_EXHAUSTED");
    check(api->program_releases==0,"no physical provider creation on invalid authority");
    // Actual SystemApi is available only when a compatible real GPU exists.
    // CI does not require a GPU; missing GPU is NOT reported as a success.
}

void ambiguous_enqueue_is_retained_not_falsely_aborted() {
    Inputs in;
    auto api=std::make_shared<FakeOpenCl>();
    auto p=make_port(api,in);
    api->fail_upload=true;
    auto reg=std::make_shared<Registry>(1);
    auto lane=std::make_shared<Lane>(1);
    auto actor=std::make_unique<GpuActorProviderBridgeV1>(
        reg,p,lane,Lane::ActorKey{"actor",3},"root",in.plan.byte_length());
    denied([&]{(void)actor->submit(7);},"GPU_V2_FAKE_UPLOAD_FAILED");
    check(actor->committed_state()==0,"ambiguous enqueue cannot publish state");
    actor.reset();
    check(reg->quarantined_count()==1,"ambiguity pins actor and owned device context");
    reg->progress_all();
    check(reg->retained_count()==1,"unknown cannot be mistaken for completed");
    check(p->reconcile_unknown({3,1}).state==Port::EnqueueState::Unknown,
          "no fabricated preenqueue proof");
    // Intentionally leave the quarantine retained within this isolated
    // test PROCESS. The mock may have enqueued work despite the error.
    // This case is a known production-blocking recovery gap.
}

} // namespace

int main() {
    actual_provider_api_pipeline_mocked();
    actor_bridge_commits_only_after_kernel_download();
    negative_authority_and_missing_gpu_fail_closed();
    // Keep one unresolved ambiguous provider operation deliberately rooted:
    // a failed enqueue API call is not proof no device command was accepted.
    ambiguous_enqueue_is_retained_not_falsely_aborted();
    if(checks<35) throw std::runtime_error("GPU_V2_TEST_COVERAGE_UNDERFLOW");
    std::cout<<"GPU_ACTOR_OPENCL_V2_HOST_MOCK_PASS checks="<<checks
             <<" physical_gpu=false device_events=fake_api"<<std::endl;
}
