#include "oreslang/runtime/GpuActorOpenClRecoveryWorkerV1.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
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
    bool reject_prepare=false, fail_kernel=false, fail_upload=false, fail_download=false;
    bool throw_after_upload_accept=false, throw_after_kernel_accept=false;
    bool throw_after_download_accept=false;
    bool delayed=false, device_loss=false, allow_loss_quiescence=false;
    bool fail_buffer_release_once=false;
    bool block_native_finish=false;
    bool finish_entered=false;
    bool finish_released=false;
    std::mutex native_mu;
    std::condition_variable native_cv;
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
        if (throw_after_upload_accept)
            throw std::runtime_error("GPU_V2_FAKE_UPLOAD_ACCEPTED_EXCEPTION");
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
        if (throw_after_kernel_accept)
            throw std::runtime_error("GPU_V2_FAKE_KERNEL_ACCEPTED_EXCEPTION");
        return next_event();
    }
    Handle enqueue_download(Handle from,unsigned char* dst,std::size_t n) override {
        ++downloads;
        if (fail_download) throw std::runtime_error("GPU_V2_FAKE_DOWNLOAD_FAILED");
        const auto& x=static_cast<Buffer*>(from)->bytes;
        std::copy(x.begin(),x.begin()+static_cast<std::ptrdiff_t>(n),dst);
        if (throw_after_download_accept)
            throw std::runtime_error("GPU_V2_FAKE_DOWNLOAD_ACCEPTED_EXCEPTION");
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
        std::unique_lock<std::mutex> guard(native_mu);
        if(block_native_finish) {
            finish_entered=true;
            native_cv.notify_all();
            native_cv.wait(guard,[&]{return finish_released;});
        }
        return allow_loss_quiescence;
    }
    void await_native_finish_entry() {
        std::unique_lock<std::mutex> guard(native_mu);
        native_cv.wait(guard,[&]{return finish_entered;});
    }
    void release_native_finish() {
        std::lock_guard<std::mutex> guard(native_mu);
        finish_released=true;
        native_cv.notify_all();
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
void event_info_query_failure_never_forges_terminal_completion() {
    using S=oreslang::opencl::SystemApi;
    check(S::classify_event_query(0,0)==EventStatus::Complete,
          "successful CL_COMPLETE is terminal");
    check(S::classify_event_query(0,2)==EventStatus::Pending,
          "running native event is pending");
    check(S::classify_event_query(0,-5)==EventStatus::Failed,
          "native command error is terminal");
    check(S::classify_event_query(-2,0)==EventStatus::DeviceLost,
          "CL_DEVICE_NOT_AVAILABLE quarantines device loss");
    check(S::classify_event_query(-58,0)==EventStatus::Pending,
          "CL_INVALID_EVENT status query cannot release GPU buffers");
    check(S::classify_event_query(-30,-5)==EventStatus::Pending,
          "failed query cannot trust command execution status");
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
    check(p->observe(result.event)==Port::Observation::Unknown,"retired provider cannot replay completion");
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

void gpu_loss_requires_native_quiescence_before_cancel_retirement() {
    Inputs in;
    auto api=std::make_shared<FakeOpenCl>();
    api->device_loss=true;
    auto p=make_port(api,in);
    auto reg=std::make_shared<Registry>(1);
    auto lane=std::make_shared<Lane>(1);
    auto a=std::make_unique<GpuActorProviderBridgeV1>(
        reg,p,lane,Lane::ActorKey{"actor",3},"root",in.plan.byte_length(),5);
    (void)a->submit(55);
    reg->progress_all(); // native OpenCL reports device loss, not completion
    check(a->committed_state()==5,"no actor state commit on device loss");
    check(!a->completed(),"device-loss notice is not physical quiescence");
    reg->progress_all(); // no native device-loss quiescence proof available
    check(a->committed_state()==5,"no false event completion while device is live");
    check(!a->completed(),"inflight buffer remains quarantined");
    api->allow_loss_quiescence=true;
    oreslang::gpu_actor::OpenClActorRecoveryWorkerV1 worker(2);
    check(worker.request(p,reg,{3,1},
          oreslang::gpu_actor::OpenClActorRecoveryWorkerV1::Kind::DeviceLoss),
          "device-loss recovery admitted on worker");
    worker.await_idle();
    check(worker.stats().proven==1,"native device-loss quiescence proven off scheduler");
    reg->progress_all(); // V1 consumes the failed native completion
    check(a->committed_state()==5,"recovery never publishes failed GPU state");
    reg->progress_all(); // retire prepared OpenCL program only after proof
    check(a->completed(),"device-loss actor now physically retired");
    a.reset();
    check(reg->retained_count()==0,"device-loss quarantine drained");
    auto msg=lane->poll();
    check(msg && msg->kind==Lane::Kind::HostFailure,"supervisor gets device-loss CONTROL");
    lane->ack(msg->actor);
    check(lane->reserved_count()==0,"CONTROL slot reclaimed after device loss");
}

void blocking_native_finish_does_not_park_actor_scheduler() {
    using Worker=oreslang::gpu_actor::OpenClActorRecoveryWorkerV1;
    Inputs in;
    auto api=std::make_shared<FakeOpenCl>();
    api->fail_upload=true;
    api->block_native_finish=true;
    api->allow_loss_quiescence=true;
    auto p=make_port(api,in);
    auto reg=std::make_shared<Registry>(1);
    auto lane=std::make_shared<Lane>(1);
    auto actor=std::make_unique<GpuActorProviderBridgeV1>(
        reg,p,lane,Lane::ActorKey{"actor",3},"root",in.plan.byte_length(),11);
    denied([&]{(void)actor->submit(99);},"GPU_V2_FAKE_UPLOAD_FAILED");
    check(actor->committed_state()==11,"unknown submission not committed");

    Worker worker(1);
    check(worker.request(p,reg,{3,1},Worker::Kind::UnknownBegin),
          "worker accepted exact generation-qualified operation");
    api->await_native_finish_entry(); // worker is blocked inside mock clFinish
    check(!worker.request(p,reg,{3,1},Worker::Kind::UnknownBegin),
          "duplicate native recovery request coalesced");
    denied([&]{
        (void)worker.request(p,reg,{3,1},Worker::Kind::DeviceLoss);
    },"GPU_RECOVERY_QUEUE_CAPACITY_EXHAUSTED");
    denied([&]{
        (void)worker.request(p,reg,{999,1},Worker::Kind::UnknownBegin);
    },"GPU_RECOVERY_FOREIGN_OR_STALE_TURN");

    // The only time bound here is an actor SCHEDULER responsiveness test.
    // It does not assert/assume any physical device quiescence.
    auto scheduler=std::async(std::launch::async,[&] {
        reg->progress_all();
        return actor->committed_state();
    });
    const bool scheduler_ready=scheduler.wait_for(
            std::chrono::milliseconds(1000))==std::future_status::ready;
    api->release_native_finish();
    check(scheduler_ready,"actor scheduler was blocked by recovery mutex");
    check(scheduler.get()==11,"poll cannot forge completion while worker blocked");
    worker.await_idle();
    check(worker.stats().proven==1,"worker received explicit native queue proof");
    reg->progress_all();
    reg->progress_all();
    check(actor->committed_state()==11,"quiescent failure never publishes staged 99");
    check(reg->retained_count()==1,"wrapper retained pending explicit program release");
    reg->progress_all();
    check(actor->completed(),"verified recovery and cleanup close actor");
    actor.reset();
    check(reg->retained_count()==0,"root slot freed only after native release");
    auto ctrl=lane->poll();
    check(ctrl && ctrl->kind==Lane::Kind::HostFailure,
          "recovered host failure sent to supervisor CONTROL");
    lane->ack(ctrl->actor);
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

void ambiguous_enqueue_is_retained_not_falsely_aborted(bool after_accept) {
    Inputs in;
    auto api=std::make_shared<FakeOpenCl>();
    auto p=make_port(api,in);
    api->fail_upload=!after_accept;
    api->throw_after_upload_accept=after_accept;
    auto reg=std::make_shared<Registry>(1);
    auto lane=std::make_shared<Lane>(1);
    auto actor=std::make_unique<GpuActorProviderBridgeV1>(
        reg,p,lane,Lane::ActorKey{"actor",3},"root",in.plan.byte_length());
    denied([&]{(void)actor->submit(7);},
           after_accept ? "GPU_V2_FAKE_UPLOAD_ACCEPTED_EXCEPTION"
                        : "GPU_V2_FAKE_UPLOAD_FAILED");
    check(actor->committed_state()==0,"ambiguous enqueue cannot publish state");
    actor.reset();
    check(reg->quarantined_count()==1,"ambiguity pins actor and owned device context");
    reg->progress_all();
    check(reg->retained_count()==1,"unknown cannot be mistaken for completed");
    check(p->reconcile_unknown({3,1}).state==Port::EnqueueState::Unknown,
          "no fabricated preenqueue proof");
    check(api->buffer_releases==0,"enqueue error must not release live device buffers");
    using Worker=oreslang::gpu_actor::OpenClActorRecoveryWorkerV1;
    Worker worker(1);
    check(worker.request(p,reg,{3,1},Worker::Kind::UnknownBegin),
          "worker admits uncertain upload only via native capability");
    worker.await_idle();
    check(worker.stats().deferred==1,"no queue proof means deferred recovery");
    check(api->buffer_releases==0,"still no buffer release before native proof");
    api->allow_loss_quiescence=true;
    check(worker.request(p,reg,{3,1},Worker::Kind::UnknownBegin),
          "requeue exact operation after proof becomes available");
    worker.await_idle();
    check(worker.stats().proven==1,"worker proves native queue quiescence");
    reg->progress_all(); // provider-issued failed/quiescent local receipt
    reg->progress_all(); // cancellation after failed native operation
    reg->progress_all(); // physical kernel program release
    check(reg->retained_count()==0,"root quarantine retires only after native release");
    auto msg=lane->poll();
    check(msg && msg->kind==Lane::Kind::HostFailure,
          "ambiguous native upload reports supervisor host failure");
    lane->ack(msg->actor);
    check(lane->reserved_count()==0,"recovered upload CONTROL lane retired");
}

void ambiguous_kernel_or_download_never_releases_before_proof(bool download,
                                                               bool after_accept) {
    Inputs in;
    auto api=std::make_shared<FakeOpenCl>();
    api->fail_kernel=!download && !after_accept;
    api->fail_download=download && !after_accept;
    api->throw_after_kernel_accept=!download && after_accept;
    api->throw_after_download_accept=download && after_accept;
    auto p=make_port(api,in);
    auto reg=std::make_shared<Registry>(1);
    auto lane=std::make_shared<Lane>(1);
    auto a=std::make_unique<GpuActorProviderBridgeV1>(
        reg,p,lane,Lane::ActorKey{"actor",3},"root",in.plan.byte_length(),17);
    (void)a->submit(40);
    reg->progress_all(); // upload -> kernel, or attempted kernel throws
    if (download) reg->progress_all(); // kernel -> attempted download throws
    check(a->committed_state()==17,"no speculative state on uncertain native enqueue");
    check(api->buffer_releases==0,"uncertain kernel/download retains physical buffers");
    reg->progress_all(); // acknowledge uncertain native enqueue as supervisor failure
    check(!a->completed(),"unknown physical event not an actor completion");
    using Worker=oreslang::gpu_actor::OpenClActorRecoveryWorkerV1;
    Worker worker(1);
    check(worker.request(p,reg,{3,1},Worker::Kind::UnknownKernel),
          "worker admits uncertain kernel/download");
    worker.await_idle();
    check(worker.stats().deferred==1,"worker cannot invent quiescence");
    check(api->buffer_releases==0,"no buffers freed before native queue quiescence");
    api->allow_loss_quiescence=true;
    check(worker.request(p,reg,{3,1},Worker::Kind::UnknownKernel),
          "worker retries exact quarantined operation");
    worker.await_idle();
    check(worker.stats().proven==1,"trusted worker obtains queue-finish proof");
    reg->progress_all(); // terminal failed event, no state commit
    check(a->committed_state()==17,"failed GPU operation never publishes staged value");
    reg->progress_all(); // release prepared program
    check(a->completed(),"physical buffers+program retired before actor close");
    check(api->buffer_releases==2,"physical cleanup occurs exactly once");
    a.reset();
    check(reg->retained_count()==0,"no residual quota or orphaned GPU actor");
    auto msg=lane->poll();
    check(msg && msg->kind==Lane::Kind::HostFailure,
          "uncertain native GPU submission reaches supervisor CONTROL");
    lane->ack(msg->actor);
}

} // namespace

int main() {
    actual_provider_api_pipeline_mocked();
    event_info_query_failure_never_forges_terminal_completion();
    actor_bridge_commits_only_after_kernel_download();
    gpu_loss_requires_native_quiescence_before_cancel_retirement();
    blocking_native_finish_does_not_park_actor_scheduler();
    negative_authority_and_missing_gpu_fail_closed();
    // Keep one unresolved ambiguous provider operation deliberately rooted:
    // a failed enqueue API call is not proof no device command was accepted.
    ambiguous_enqueue_is_retained_not_falsely_aborted(false);
    ambiguous_enqueue_is_retained_not_falsely_aborted(true);
    ambiguous_kernel_or_download_never_releases_before_proof(false,false);
    ambiguous_kernel_or_download_never_releases_before_proof(false,true);
    ambiguous_kernel_or_download_never_releases_before_proof(true,false);
    ambiguous_kernel_or_download_never_releases_before_proof(true,true);
    if(checks<115) throw std::runtime_error("GPU_V2_TEST_COVERAGE_UNDERFLOW");
    std::cout<<"GPU_ACTOR_OPENCL_V2_HOST_MOCK_PASS checks="<<checks
             <<" physical_gpu=false device_events=fake_api"<<std::endl;
}
