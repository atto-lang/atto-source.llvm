#include "oreslang/gpu/G1CudaDriver.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
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
        std::string ptx((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());
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
