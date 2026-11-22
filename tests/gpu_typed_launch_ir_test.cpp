#include "oreslang/runtime/GpuTypedLaunchIr.hpp"

#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace oreslang::gpu;
using namespace oreslang::gpu::typed_ir;

static std::size_t assertions = 0;
static void check(bool yes) {
    ++assertions;
    if (!yes) throw std::runtime_error("GPU typed IR assertion failed");
}
template <class F> static void reject(const char* code, F f) {
    try { f(); }
    catch (const std::invalid_argument& e) {
        check(std::string(e.what()).find(code) != std::string::npos);
        return;
    }
    throw std::runtime_error(std::string("did not reject ") + code);
}
static BufferView buffer(std::string id, std::int64_t generation,
                         std::int64_t offset, std::int64_t length, Access mode) {
    return {"tenant-1", "gpu-1", std::move(id), generation, 1024,
            offset, length, mode};
}
static Request request() {
    return {
        {"saxpy", {Parameter::F32, Parameter::ReadF32, Parameter::ReadF32,
                    Parameter::WriteF32, Parameter::U64},
                   {Effect::DeviceRead, Effect::DeviceWrite}, true},
        {ScalarF32{0x40000000}, buffer("x",1,0,1024,Access::Read),
         buffer("y",1,0,1024,Access::Read),
         buffer("out",1,0,1024,Access::Write), ScalarU64{256}},
        {"tenant-1", "gpu-1", "queue-1", 4},
        Physical{{1,1,1},{256,1,1}}, 0,
        {{65535,65535,65535}, {1024,1024,64}, 1024, 49152}, 8192, 8192
    };
}
static bool independent_alias_oracle(const std::vector<Argument>& a) {
    for (std::size_t i=0; i<a.size(); ++i) {
        const auto& x=std::get<BufferView>(a[i]);
        for (std::size_t j=i+1; j<a.size(); ++j) {
            const auto& y=std::get<BufferView>(a[j]);
            if (x.allocation != y.allocation ||
                (!writes(x.access) && !writes(y.access))) continue;
            if (x.offset < y.offset + y.length &&
                y.offset < x.offset + x.length) return true;
        }
    }
    return false;
}
int main() {
    const auto base=request();
    auto ok=verify(base);
    check(ok.kernel_symbol=="saxpy");
    check(ok.physical.total_dispatched_lanes==256);
    check(ok.logical_work_items==256);
    check(ok.buffer_count==3);
    check(!ok.executable());

    auto invalid=base;
    invalid.kernel.returns_void=false;
    reject("GPU-LAUNCH-001", [&] { (void)verify(invalid); });
    invalid=base; invalid.arguments[1]=ScalarF32{0};
    reject("GPU-LAUNCH-002", [&] { (void)verify(invalid); });
    invalid=base; invalid.scope.queue_generation=0;
    reject("GPU-LAUNCH-004", [&] { (void)verify(invalid); });
    invalid=base; std::get<BufferView>(invalid.arguments[1]).tenant="foreign";
    reject("GPU-LAUNCH-004", [&] { (void)verify(invalid); });
    invalid=base; std::get<BufferView>(invalid.arguments[1]).generation=0;
    reject("GPU-LAUNCH-005", [&] { (void)verify(invalid); });
    invalid=base; std::get<BufferView>(invalid.arguments[1]).offset=2;
    reject("GPU-LAUNCH-005", [&] { (void)verify(invalid); });
    invalid=base; std::get<BufferView>(invalid.arguments[1]).length=1028;
    reject("GPU-LAUNCH-005", [&] { (void)verify(invalid); });
    invalid=base; std::get<BufferView>(invalid.arguments[1]).access=Access::Write;
    reject("GPU-LAUNCH-005", [&] { (void)verify(invalid); });
    invalid=base; std::get<BufferView>(invalid.arguments[3]).access=Access::Read;
    reject("GPU-LAUNCH-005", [&] { (void)verify(invalid); });
    invalid=base; std::get<BufferView>(invalid.arguments[3]).allocation="x";
    reject("GPU-LAUNCH-005", [&] { (void)verify(invalid); });
    invalid=base; std::get<BufferView>(invalid.arguments[3]).allocation="x";
    std::get<BufferView>(invalid.arguments[3]).generation=2;
    reject("GPU-LAUNCH-005", [&] { (void)verify(invalid); });
    invalid=base; invalid.kernel.effects.push_back(Effect::HostIo);
    reject("GPU-LAUNCH-006", [&] { (void)verify(invalid); });
    invalid=base; invalid.kernel.effects={Effect::DeviceWrite};
    reject("GPU-LAUNCH-006", [&] { (void)verify(invalid); });
    invalid=base; invalid.max_referenced_bytes=3071;
    reject("GPU-LAUNCH-005", [&] { (void)verify(invalid); });
    invalid=base; invalid.max_total_dispatched_lanes=255;
    reject("GPU-LAUNCH-003", [&] { (void)verify(invalid); });
    invalid=base; invalid.geometry=Physical{{0,1,1},{256,1,1}};
    reject("GPU-LAUNCH-003", [&] { (void)verify(invalid); });
    invalid=base; invalid.geometry=Physical{{1,1,1},{0,1,1}};
    reject("GPU-LAUNCH-003", [&] { (void)verify(invalid); });
    invalid=base; invalid.geometry=Logical{{0,1,1},{256,1,1}};
    auto empty=verify(invalid);
    check(empty.physical.empty_launch && empty.logical_work_items==0 && !empty.executable());
    invalid=base; invalid.geometry=Logical{{257,1,1},{256,1,1}};
    auto padded=verify(invalid);
    check(padded.logical_work_items==257);
    check(padded.physical.total_dispatched_lanes==512);
    invalid.max_total_dispatched_lanes=511;
    reject("GPU-LAUNCH-003", [&] { (void)verify(invalid); });

    // Independent quadratic overlap oracle vs sorted verifier. Use only
    // well-formed, in-quota scoped F32 views to isolate alias reasoning.
    std::mt19937_64 rng(0xA770C0DEULL);
    for (int trial=0;trial<2048;++trial) {
        Request r=base;
        r.kernel.parameters.clear();
        r.arguments.clear();
        int count=1+static_cast<int>(rng()%48);
        for (int j=0;j<count;++j) {
            std::string id="allocation-"+std::to_string(rng()%4);
            auto off=static_cast<std::int64_t>(4*(rng()%232));
            auto len=static_cast<std::int64_t>(4*(1+rng()%16));
            bool write=rng()%3==0;
            r.kernel.parameters.push_back(write?Parameter::WriteF32:Parameter::ReadF32);
            r.arguments.push_back(buffer(id,1,off,len,write?Access::Write:Access::Read));
        }
        r.max_referenced_bytes=1024*48;
        bool expected=independent_alias_oracle(r.arguments);
        bool rejected=false;
        try { (void)verify(r); }
        catch (const std::invalid_argument& e) {
            rejected=true;
            check(std::string(e.what()).find("GPU-LAUNCH-005") != std::string::npos);
        }
        check(expected==rejected);
    }
    std::cout << "PASS " << assertions << " host-only typed GPU IR assertions; "
              << "2048 randomized independent alias comparisons; device execution NOT_EXECUTED\n";
}
