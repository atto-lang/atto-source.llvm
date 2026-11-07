#include "oreslang/gpu/G1CudaDriver.hpp"
#include "oreslang/gpu/G2PtxInput.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::gpu::g2;

static void completion(SaxpyOperation& operation) {
    const auto timeout=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while (!operation.poll()) {
        if (std::chrono::steady_clock::now()>timeout)
            throw std::runtime_error("GPU-G2-004: physical device event timeout");
        // Dedicated test runner only, never an actor/scheduler carrier.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
int main(int argc, char* argv[]) {
    if (argc != 2) { std::cerr<<"usage: cuda-g2-hardware <verified.ptx>\n"; return 2; }
    try {
        std::ifstream stream(argv[1],std::ios::binary);
        if(!stream)throw std::runtime_error("GPU-G2-004: cannot open offline PTX");
        const std::string ptx = read_bounded_ptx(stream);
        CudaDriver driver(0);
        std::cerr<<"GPU-G2 HARDWARE: device="<<driver.device_identity()
                 <<" driverVersion="<<driver.driver_version()<<"\n";
        std::uint64_t generation=1000;
        int verified=0;
        for(const auto n:std::vector<std::size_t>{0,1,255,256,257,1024,4097,16385}) {
            std::vector<float> x(n),y(n);
            for(std::size_t i=0;i<n;++i) {
                x[i]=static_cast<float>(static_cast<int>(i%101)-50)/7.0f;
                y[i]=static_cast<float>(static_cast<int>(i%59)-29)/11.0f;
            }
            constexpr float scale=-1.25f;
            SaxpyOperation op(driver,"gpu-g1-hardware-ci",generation,16*1024*1024);
            generation+=3;
            op.submit(ptx,scale,x,y);
            completion(op);
            if(op.state()!=State::Completed)throw std::runtime_error("hardware computation failed");
            const auto& out=op.output();
            if(out.size()!=n)throw std::runtime_error("output length mismatch");
            for(std::size_t i=0;i<n;++i) {
                const auto expected=scale*x[i]+y[i];
                const auto abs_err=std::abs(out[i]-expected);
                if(!std::isfinite(out[i]) || abs_err>0.00002f*(1.0f+std::abs(expected)))
                    throw std::runtime_error("GPU-G2-004: numerical oracle mismatch");
            }
            op.close(); ++verified;
            std::cerr<<"GPU-G2 HARDWARE PASS: n="<<n<<" elements\n";
        }
        // Reproducible physical-GPU differential sweep: diverse padded tails,
        // small/large launch extents, workgroup sizes, and signs/magnitudes.
        std::mt19937_64 rng(0xa77020261106ULL);
        for(int trial=0;trial<64;++trial) {
            const auto n=trial<16? std::size_t(trial*33+1):
                std::size_t(rng()%8193);
            const auto group=std::uint32_t(1u<<(rng()%9));
            const auto scale=float(int(rng()%33)-16)/5.0f;
            std::vector<float> x(n),y(n);
            for(std::size_t i=0;i<n;++i) {
                x[i]=float(int(rng()%2003)-1001)/13.0f;
                y[i]=float(int(rng()%997)-498)/7.0f;
            }
            SaxpyOperation op(driver,"gpu-g1-hardware-ci",generation,16*1024*1024);
            generation+=3;
            op.submit(ptx,scale,x,y,group);
            completion(op);
            if(op.state()!=State::Completed || op.output().size()!=n)
                throw std::runtime_error("GPU-G2-004: randomized kernel did not complete");
            for(std::size_t i=0;i<n;++i) {
                const auto expected=scale*x[i]+y[i];
                const auto actual=op.output()[i];
                if(!std::isfinite(actual) ||
                    std::abs(actual-expected)>0.00002f*(1.0f+std::abs(expected)))
                    throw std::runtime_error("GPU-G2-004: seeded numerical oracle mismatch");
            }
            op.close(); ++verified;
        }
        // Invalid launches must fail *before* allocating or scheduling.
        {
            SaxpyOperation op(driver,"gpu-g1-hardware-ci",generation,16*1024*1024);
            generation+=3;
            bool rejected=false;
            try { op.submit(ptx,1.0f,{1.0f},{2.0f},0); }
            catch(const std::invalid_argument&) { rejected=true; }
            if(!rejected || op.has_live_device_handles() || op.state()!=State::Ready)
                throw std::runtime_error("GPU-G2-004: invalid block size admitted");
            ++verified;
        }
        {
            SaxpyOperation op(driver,"gpu-g1-hardware-ci",generation,sizeof(float));
            generation+=3;
            bool rejected=false;
            try { op.submit(ptx,1.0f,{1.0f,2.0f},{3.0f,4.0f}); }
            catch(const std::invalid_argument&) { rejected=true; }
            if(!rejected || op.has_live_device_handles() || op.state()!=State::Ready)
                throw std::runtime_error("GPU-G2-004: byte quota bypassed");
            ++verified;
        }
        { // A cancelled future/request must not free in-flight device buffers.
            SaxpyOperation op(driver,"gpu-g1-hardware-ci",generation,16*1024*1024);
            std::vector<float> x(4097,1.0f),y(4097,-2.0f);
            op.submit(ptx,3.0f,x,y);
            op.request_cancel();
            completion(op);
            if(op.state()!=State::Cancelled || op.has_live_device_handles())
                throw std::runtime_error("cancelled GPU buffers not quiescent");
            op.close(); ++verified;
        }
        std::cout<<"PASS "<<verified<<" PHYSICAL CUDA SAXPY/cancellation cases\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"GPU-G2 hardware failure: "<<e.what()<<'\n'; return 2;
    }
}
