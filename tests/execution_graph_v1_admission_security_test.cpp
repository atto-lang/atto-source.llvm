#include "oreslang/runtime/ExecutionGraphV1.hpp"
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
using namespace oreslang::execution_v1;

static unsigned long cases = 0;
template <typename Fn>
static void reject(Fn&& action, const std::string& message) {
    try {
        action();
    } catch (const std::invalid_argument& ex) {
        if (std::string(ex.what()).find(message) != std::string::npos) {
            ++cases;
            return;
        }
        throw std::runtime_error(std::string("unexpected diagnostic: ") + ex.what());
    }
    throw std::runtime_error("malformed typed execution graph was admitted");
}
static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++cases;
}

int main() {
    try {
        using D = Domain;
        using K = Kind;
        using O = Op;
        Node constant{0, D::Cpu, O::ConstantI64, K::HostI64, {}, 42};
        reject([] { (void)Plan({}, 0, K::Unit); }, "size or output");
        reject([&] { (void)Plan({constant}, 1, K::HostI64); }, "size or output");
        reject([&] { auto n=constant; n.id = std::numeric_limits<std::size_t>::max(); (void)Plan({n}, 0, K::HostI64); }, "node ID");
        reject([&] { auto n=constant; n.inputs = {0}; (void)Plan({n}, 0, K::HostI64); }, "dependency");
        reject([&] { auto n=constant; n.domain = D::Gpu; (void)Plan({n}, 0, K::HostI64); }, "signature");
        reject([&] { auto n=constant; n.result = K::DeviceI64; (void)Plan({n}, 0, K::DeviceI64); }, "signature");
        reject([&] { auto n=constant; n.operation = static_cast<O>(900); (void)Plan({n}, 0, K::HostI64); }, "opcode");
        reject([&] { (void)Plan({constant}, 0, K::Unit); }, "output kind");
        reject([&] { Node n{1,D::Cpu,O::AddI64,K::HostI64,{0},0}; (void)Plan({constant,n},1,K::HostI64); }, "signature");
        reject([&] { Node n{1,D::Cpu,O::AddI64,K::HostI64,{0,0},0}; auto bad=constant; bad.result=K::Unit; (void)Plan({bad,n},1,K::HostI64); }, "signature");
        reject([&] { Node n{1,D::Cpu,O::UploadI64,K::DeviceI64,{0},0}; (void)Plan({constant,n},1,K::DeviceI64); }, "signature");
        reject([&] { Node n{1,D::Gpu,O::UploadI64,K::DeviceI64,{0},0}; auto bad=constant; bad.operation=O::Unit; bad.result=K::Unit; (void)Plan({bad,n},1,K::DeviceI64); }, "operand kind");
        reject([&] { std::vector<Node> too_many(4097, constant); (void)Plan(std::move(too_many),0,K::HostI64); }, "size or output");

        Builder cpu;
        auto a=cpu.cpu_i64(20), b=cpu.cpu_i64(22);
        auto plan=cpu.build(cpu.cpu_add(a,b));
        check(std::get<std::int64_t>(plan.execute_cpu()) == 42, "valid CPU graph rejected");
        std::function<void()> dispatched;
        auto future=plan.submit_cpu([&](std::function<void()> task) { dispatched=std::move(task); });
        check(!future.ready() && static_cast<bool>(dispatched), "nonblocking CPU scheduling failed");
        dispatched();
        check(std::get<std::int64_t>(future.get())==42, "CPU Future result corrupted");

        Builder gpu;
        auto device=gpu.upload_i64(gpu.cpu_i64(7));
        auto gpu_plan=gpu.build(gpu.download_i64(device));
        check(!gpu_plan.cpu_only(), "GPU plan marked CPU");
        bool enqueued=false;
        auto failed=gpu_plan.submit_cpu([&](std::function<void()>) { enqueued=true; });
        check(!enqueued, "GPU admitted CPU scheduling");
        bool denied=false;
        try { (void)failed.get(); }
        catch(const std::runtime_error& ex) {
            denied=std::string(ex.what()).find("no implicit CPU fallback")!=std::string::npos;
        }
        check(denied, "GPU fallback was not rejected");

        // Deterministic malformed-IR fuzz corpus; every candidate is invalid.
        std::uint32_t seed=0x91E10C23u;
        for (int i=0;i<20000;++i) {
            seed=seed*1664525u+1013904223u;
            auto n=constant;
            const int mutation=static_cast<int>(seed%6u);
            if (mutation==0) n.id=static_cast<std::size_t>(1+(seed%999u));
            if (mutation==1) n.inputs={0};
            if (mutation==2) n.domain=D::Gpu;
            if (mutation==3) n.result=K::Unit;
            if (mutation==4) n.operation=static_cast<O>(900+(seed%79u));
            if (mutation==5) n.operation=O::AddI64;
            reject([&] { (void)Plan({n},0,n.result); },
                   mutation==0 ? "node ID" :
                   mutation==1 ? "dependency" :
                   mutation==4 ? "opcode" : "signature");
        }
        std::cout << "typed graph IR security PASS: " << cases << " assertions\n";
        return 0;
    } catch(const std::exception& ex) {
        std::cerr << "typed graph security FAIL: " << ex.what() << '\n';
        return 1;
    }
}
