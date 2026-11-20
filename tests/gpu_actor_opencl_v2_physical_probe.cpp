// Physical-GPU-required native actor-provider integration probe.
// This is not a compiled AttoLang source actor. No CPU-device fallback.
#include "oreslang/runtime/GpuActorOpenClProviderV2.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
}
int main() {
    using namespace oreslang;
    auto gpu = opencl::SystemApi::try_create_gpu();
    if (!gpu) {
        std::cerr << "GPU_ACTOR_V2_NO_GPU_RUNTIME no-compatible-GPU; no CPU fallback\n";
        return 77;
    }

    try {
        const std::string device_name = gpu->device_name();
        std::shared_ptr<opencl::Api> api(std::move(gpu));
        const dop::Plan plan(dop::Layout::AoS, 4, 2, 4, 16, 1);
        const dop::Kernel kernel(
            plan,dop::Element::F32,dop::Op::ScaleBias,1.5f,-2.0f);
        std::vector<unsigned char> host(static_cast<std::size_t>(plan.byte_length()),0);
        for(std::uint64_t row=0;row<plan.rows();row++)
            for(std::uint32_t col=0;col<plan.fields();col++){
                float v=static_cast<float>(row*10+col)+0.25f;
                std::memcpy(host.data()+plan.byte_offset(row,col),&v,sizeof(v));
            }

        const std::string tenant="gpu-actor-native-probe";
        const gpu_actor::ProviderPortV1::Binding binding{
            "vector-actor",1,tenant,device_name,"1",1,1,1};
        auto port=std::make_shared<gpu_actor::OpenClActorProviderPortV2>(
            binding,api,
            opencl::ProviderIdentity{tenant,device_name,1,1,4},
            opencl::Limits{plan.byte_length()*4,plan.byte_length()*4,2,1},
            dop::GpuTarget{64,
                std::min<std::uint64_t>(gpu->maximum_allocation_bytes(),
                    1024*1024ULL),1024,true},
            kernel,host);
        auto registry=std::make_shared<gpu_actor::GpuActorQuarantineRegistryV1>(1);
        auto lane=std::make_shared<SupervisorFailureLane>(1);
        auto actor=std::make_unique<gpu_actor::GpuActorProviderBridgeV1>(
            registry,port,lane,SupervisorFailureLane::ActorKey{"vector-actor",1},
            "gpu-supervisor",plan.byte_length(),41);
        const auto turn=actor->submit(1);
        require(turn.actor_generation==1 && turn.sequence==1,
                "GPU_ACTOR_V2_INVALID_TURN");
        require(actor->committed_state()==41,"GPU_ACTOR_V2_EARLY_COMMIT");
        for(int attempts=0;attempts<10000
                && actor->committed_state()!=42;attempts++){
            registry->progress_all();
            if(actor->committed_state()!=42)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(actor->committed_state()!=42) {
            actor->request_close();
            throw std::runtime_error("GPU_ACTOR_V2_PHYSICAL_EVENT_UNVERIFIED");
        }
        require(port->native_opencl_success_observed(),"GPU_ACTOR_V2_NO_NATIVE_SUCCESS");
        const auto out=port->verified_output();
        for(std::uint64_t row=0;row<plan.rows();row++)
            for(std::uint32_t col=0;col<plan.fields();col++){
                const auto off=plan.byte_offset(row,col);
                float x=0,y=0;
                std::memcpy(&x,host.data()+off,sizeof(x));
                std::memcpy(&y,out.data()+off,sizeof(y));
                const float expected=x*1.5f-2.0f;
                require(std::isfinite(y)&&std::fabs(y-expected)<1e-5f,
                        "GPU_ACTOR_V2_GPU_NUMERICAL_ORACLE_FAILED");
            }
        actor->request_close();
        registry->progress_all();
        require(actor->completed(),"GPU_ACTOR_V2_PROGRAM_RELEASE_UNVERIFIED");
        actor.reset();
        require(registry->retained_count()==0,"GPU_ACTOR_V2_QUARANTINE_LEAK");
        auto control=lane->poll();
        require(control && control->kind==SupervisorFailureLane::Kind::Cancel,
                "GPU_ACTOR_V2_SUPERVISOR_NOTICE_MISSING");
        lane->ack(control->actor);
        require(lane->reserved_count()==0,"GPU_ACTOR_V2_CONTROL_SLOT_LEAK");

        std::cout<<"GPU_ACTOR_V2_NATIVE_OPENCL_PASS device="<<device_name
                 <<" typed_kernel=dop_f32_scale_bias"
                 <<" actor_committed=42 oracle_items=8"
                 <<" CPU_FALLBACK=false compiled_atto_guest=false"
                 <<" HARDWARE_ATTESTATION=EXTERNAL_REQUIRED\n";
        return 0;
    } catch(const std::exception& ex) {
        std::cerr<<"GPU_ACTOR_V2_PHYSICAL_PROBE_FAILED "<<ex.what()<<"\n";
        return 1;
    }
}
