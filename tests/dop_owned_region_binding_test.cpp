#include "oreslang/runtime/DopKernelIR.hpp"
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
template <typename F> void rejects(F action) {
    try { action(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("unowned or retired region passed physical admission");
}
void check(bool ok) {
    if (!ok) throw std::runtime_error("owned region binding invariant failed");
}
}

int main() {
    using namespace oreslang::dop;
    Plan shape(Layout::SoA, 7, 3, 4, 16, 1);
    HostBuffer owner(shape), independent_owner(shape);
    auto physical = owner.region_root();
    auto physical_copy = owner.region_root();
    auto other_physical = independent_owner.region_root();
    auto synthetic = RegionRoot::trusted_create(7, 3);
    check(physical.is_live_owned());
    check(physical.same_root(physical_copy));
    check(!physical.same_root(other_physical));
    check(!physical.same_root(synthetic));
    check(!synthetic.is_live_owned());
    RegionTask valid("bound", {RegionAccess(physical, 0, 0, 7, RegionMode::Write)});
    RegionTask forged("unbound", {RegionAccess(synthetic, 0, 0, 7, RegionMode::Read)});
    require_live_owned(valid);
    rejects([&] { require_live_owned(forged); });
    check(independent(valid, RegionTask("other-owner", {
        RegionAccess(other_physical, 0, 0, 7, RegionMode::Write)})));
    {
        auto loan = owner.borrow_read();
        rejects([&] { owner.close(); });
        require_live_owned(valid); // one live guarded allocation
    }
    // An ownership transfer moves the wrapper, not the allocation identity.
    HostBuffer successor = owner.take();
    check(successor.region_root().same_root(physical));
    rejects([&] { (void)owner.region_root(); });
    require_live_owned(valid);
    successor.close();
    check(!physical.is_live_owned());
    check(!physical_copy.is_live_owned());
    rejects([&] { require_live_owned(valid); });
    rejects([&] { (void)successor.region_root(); });
    check(other_physical.is_live_owned());
    independent_owner.close();
    check(!other_physical.is_live_owned());
}
