#include "oreslang/backend/GpuG1SaxpyLaunchAdmission.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace oreslang::gpu::g1;
namespace {
std::uint64_t checks = 0;
void check(bool value) { ++checks; if (!value) std::abort(); }
template<class Action> void reject(Action action) {
    bool failed = false;
    try { action(); } catch(const std::invalid_argument&) { failed = true; }
    check(failed);
}
std::uint32_t seed = 0x49213b5;
std::uint32_t random32() {
    seed = 1664525u * seed + 1013904223u;
    return seed;
}
bool reference_admits(const std::array<DeviceView, 3>& views,
                     const QueueLocation& queue, const LaunchLimits& caps,
                     std::uint64_t n, std::uint64_t group_threads) {
    if (!queue.tenant || !queue.device || !queue.device_generation ||
        !queue.queue || !queue.queue_generation ||
        !caps.maximum_buffer_bytes || !caps.maximum_threads_per_group ||
        !caps.maximum_groups_x || !group_threads ||
        group_threads > caps.maximum_threads_per_group ||
        group_threads > UINT32_MAX) return false;
    std::uint64_t total_elements = 0;
    for (std::size_t i=0; i<views.size(); ++i) {
        const auto& v = views[i];
        if (!v.tenant || !v.device || !v.device_generation ||
            !v.allocation || !v.allocation_generation ||
            v.tenant != queue.tenant || v.device != queue.device ||
            v.device_generation != queue.device_generation ||
            v.loan != (i==2 ? Loan::ExclusiveWrite : Loan::ReadOnly) ||
            v.first > v.allocation_elements ||
            v.length > v.allocation_elements-v.first ||
            n > v.length ||
            v.allocation_elements > caps.maximum_buffer_bytes / 4) return false;
        bool first_appearance = true;
        for(std::size_t j=0;j<i;++j) {
            const auto& prev = views[j];
            if (prev.allocation != v.allocation) continue;
            first_appearance = false;
            if (prev.allocation_generation != v.allocation_generation ||
                prev.allocation_elements != v.allocation_elements) return false;
            if (i==2 && v.length && prev.length &&
                v.first < prev.first+prev.length &&
                prev.first < v.first+v.length) return false;
        }
        if (first_appearance) {
            if (v.allocation_elements > caps.maximum_buffer_bytes/4 - total_elements)
                return false;
            total_elements += v.allocation_elements;
        }
    }
    const std::uint64_t groups = n / group_threads + (n % group_threads != 0);
    return groups <= caps.maximum_groups_x && groups <= UINT32_MAX &&
           (groups==0 || group_threads <= UINT64_MAX / groups);
}
} // namespace
int main() {
    const QueueLocation q{1,2,3,4,5};
    const LaunchLimits normal{256,65535,128};
    const DeviceView x{1,2,3,10,1,10,0,10,Loan::ReadOnly};
    const DeviceView y{1,2,3,11,1,10,0,10,Loan::ReadOnly};
    const DeviceView out{1,2,3,12,1,10,0,10,Loan::ExclusiveWrite};
    check(admit_saxpy_launch(x,y,out,q,normal,10,16).physical_threads==16);

    // CUDA/NVPTX ABI: a 32-bit grid or block dimension may not wrap.
    constexpr std::uint64_t umax = UINT32_MAX;
    constexpr std::uint64_t huge = std::uint64_t{UINT32_MAX}+1;
    const DeviceView large_x{1,2,3,10,1,huge,0,huge,Loan::ReadOnly};
    const DeviceView large_y{1,2,3,11,1,huge,0,huge,Loan::ReadOnly};
    const DeviceView large_out{1,2,3,12,1,huge,0,huge,Loan::ExclusiveWrite};
    const LaunchLimits generous{huge,UINT64_MAX,3*huge*4};
    check(admit_saxpy_launch(large_x,large_y,large_out,q,generous,umax,1).groups_x==umax);
    reject([&]{(void)admit_saxpy_launch(large_x,large_y,large_out,q,generous,huge,1);});
    reject([&]{(void)admit_saxpy_launch(large_x,large_y,large_out,q,generous,1,huge);});
    check(admit_saxpy_launch(large_x,large_y,large_out,q,generous,huge,2).groups_x==huge/2);
    check(admit_saxpy_launch(large_x,large_y,large_out,q,generous,0,1).empty);
    reject([&]{(void)admit_saxpy_launch(large_x,large_y,large_out,q,
        {huge,UINT64_MAX,3*huge*4-1},0,1);});

    // Exact quota, one-allocation alias, disjoint prefix/suffix mutation.
    auto same_out=x; same_out.loan=Loan::ExclusiveWrite;
    same_out.first=5; same_out.length=5;
    auto prefix=x; prefix.length=5;
    check(admit_saxpy_launch(prefix,y,same_out,q,{256,65535,80},5,16).groups_x==1);
    reject([&]{(void)admit_saxpy_launch(prefix,y,same_out,q,{256,65535,79},5,16);});
    same_out.allocation_elements=9;
    reject([&]{(void)admit_saxpy_launch(prefix,y,same_out,q,{256,65535,80},5,16);});
    same_out=x; same_out.loan=Loan::ExclusiveWrite;
    reject([&]{(void)admit_saxpy_launch(x,y,same_out,q,normal,5,16);});
    check(admit_saxpy_launch(x,y,out,q,normal,0,16).empty);

    // Independent straightforward oracle against randomized identity, alias,
    // geometry and quota mutations, including zero logical launch shape.
    constexpr std::size_t cases=16384;
    for(std::size_t i=0;i<cases;++i) {
        std::array<DeviceView,3> views{x,y,out};
        for(std::size_t j=0;j<views.size();++j) {
            auto& v=views[j];
            v.allocation=1+random32()%4;
            v.allocation_generation=1+random32()%3;
            v.allocation_elements=random32()%33;
            v.first=random32()%35;
            v.length=random32()%35;
            v.loan=j==2 ? Loan::ExclusiveWrite : Loan::ReadOnly;
            // Some wrong domains and missing generations exercise denial.
            if (random32()%31 == 0) v.device=4;
            if (random32()%37 == 0) v.allocation_generation=0;
        }
        const std::uint64_t n = random32()%35;
        const std::uint64_t group = random32()%66;
        const LaunchLimits caps{1+random32()%66,1+random32()%8,1+random32()%401};
        const bool oracle=reference_admits(views,q,caps,n,group);
        bool actual=false;
        try {
            const auto result=admit_saxpy_launch(views[0],views[1],views[2],q,caps,n,group);
            actual=true;
            check(result.empty==(n==0));
            check(result.groups_x==n/group+(n%group!=0));
            check(result.physical_threads==result.groups_x*group);
        } catch(const std::invalid_argument&) {}
        if(actual!=oracle) {
            std::cerr<<"G1 admission mismatch case="<<i<<" expected="<<oracle<<" actual="<<actual<<'\n';
            return 1;
        }
        check(actual==oracle);
    }
    // Separate valid-construction domain guarantees thousands of actual
    // accepted plans; mutate the exact quota to force a paired rejection.
    constexpr std::size_t valid_pairs=4096;
    for(std::size_t i=0;i<valid_pairs;++i) {
        const std::uint64_t nx=1+random32()%61;
        const std::uint64_t ny=1+random32()%61;
        const std::uint64_t nz=1+random32()%61;
        DeviceView ax=x, ay=y, az=out;
        ax.allocation_elements=ax.length=nx;
        ay.allocation_elements=ay.length=ny;
        az.allocation_elements=az.length=nz;
        // Alternate unique allocations with read-only aliases that must
        // contribute a single resident allocation to quota accounting.
        const bool shared_reads=(i%2)==0;
        if(shared_reads) ay=ax;
        const std::uint64_t min_length=shared_reads ? (nx<nz?nx:nz)
            : (nx<ny ? (nx<nz?nx:nz) : (ny<nz?ny:nz));
        const std::uint64_t n=random32()%(min_length+1);
        const std::uint64_t group=1+random32()%64;
        const std::uint64_t bytes=4*(nx+(shared_reads?0:ny)+nz);
        LaunchLimits caps{64,8192,bytes};
        check(reference_admits({ax,ay,az},q,caps,n,group));
        const auto plan=admit_saxpy_launch(ax,ay,az,q,caps,n,group);
        check(plan.groups_x==n/group+(n%group!=0));
        check(plan.empty==(n==0));
        caps.maximum_buffer_bytes=bytes-1;
        check(!reference_admits({ax,ay,az},q,caps,n,group));
        reject([&]{(void)admit_saxpy_launch(ax,ay,az,q,caps,n,group);});
        if(shared_reads) {
            auto stale=ay;
            ++stale.allocation_generation;
            reject([&]{(void)admit_saxpy_launch(ax,stale,az,q,{64,8192,bytes},n,group);});
        }
    }
    std::cout<<"PASS "<<checks<<" host-only G1 ABI/property assertions; "<<cases<<" deterministic oracle probes\n";
}
