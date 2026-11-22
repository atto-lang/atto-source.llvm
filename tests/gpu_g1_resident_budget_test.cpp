#include "oreslang/backend/GpuG1SaxpyLaunchAdmission.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace oreslang::gpu::g1;
static std::size_t checks = 0;
static void check(bool b) { ++checks; if (!b) std::abort(); }
template<typename F> static void rejects(F fn) {
    bool thrown = false;
    try { fn(); } catch (const std::invalid_argument&) { thrown = true; }
    check(thrown);
}

int main() {
    const QueueLocation q{1, 2, 3, 4, 5};
    const LaunchLimits l{256, 65535, 120};
    const DeviceView x{1, 2, 3, 10, 1, 10, 0, 10, Loan::ReadOnly};
    const DeviceView y{1, 2, 3, 11, 1, 10, 0, 10, Loan::ReadOnly};
    const DeviceView z{1, 2, 3, 12, 1, 10, 0, 10, Loan::ExclusiveWrite};
    check(admit_saxpy_launch(x,y,z,q,l,10,16).groups_x == 1);
    auto low = l; low.maximum_buffer_bytes = 119;
    rejects([&]{(void)admit_saxpy_launch(x,y,z,q,low,10,16);});
    low.maximum_buffer_bytes = 120;
    check(admit_saxpy_launch(x,y,z,q,low,10,16).groups_x == 1);
    auto aliasedRead = x;
    check(admit_saxpy_launch(x,aliasedRead,z,q,{256,65535,80},10,16).groups_x == 1);
    rejects([&]{(void)admit_saxpy_launch(x,aliasedRead,z,q,{256,65535,79},10,16);});
    aliasedRead.allocation_elements = 11;
    rejects([&]{(void)admit_saxpy_launch(x,aliasedRead,z,q,l,10,16);});
    auto disjointWrite = x;disjointWrite.loan=Loan::ExclusiveWrite;
    disjointWrite.first=10;disjointWrite.length=10;
    auto prefix=x;prefix.allocation_elements=20;prefix.length=10;
    disjointWrite.allocation_elements=20;
    check(admit_saxpy_launch(prefix,y,disjointWrite,q,l,10,16).groups_x==1);
    rejects([&]{(void)admit_saxpy_launch(prefix,y,disjointWrite,q,{256,65535,119},10,16);});
    disjointWrite.allocation_elements=21;
    rejects([&]{(void)admit_saxpy_launch(prefix,y,disjointWrite,q,l,10,16);});
    auto stale=x;stale.allocation_generation=2;
    rejects([&]{(void)admit_saxpy_launch(x,stale,z,q,l,10,16);});
    // 1,024 deterministic cross-product quota/alias probes.
    std::uint32_t seed=0x553311;
    for(std::size_t i=0;i<1024;i++) {
      seed=1664525U*seed+1013904223U;
      const std::uint64_t nx=1+(seed%25);
      seed=1664525U*seed+1013904223U;
      const std::uint64_t ny=1+(seed%25);
      seed=1664525U*seed+1013904223U;
      const std::uint64_t nz=1+(seed%25);
      DeviceView ax=x,ay=y,az=z;
      ax.allocation_elements=ax.length=nx;
      ay.allocation_elements=ay.length=ny;
      az.allocation_elements=az.length=nz;
      const std::uint64_t bytes=4*(nx+ny+nz);
      auto caps=l;caps.maximum_buffer_bytes=bytes;
      check(admit_saxpy_launch(ax,ay,az,q,caps,1,16).groups_x==1);
      caps.maximum_buffer_bytes=bytes-1;
      rejects([&]{(void)admit_saxpy_launch(ax,ay,az,q,caps,1,16);});
    }
    std::cout << "PASS " << checks << " GPU G1 resident budget and generation assertions (HOST_ONLY)\n";
}
