#include "oreslang/runtime/DopKernelIR.hpp"

#include <memory>
#include <optional>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    using namespace oreslang::dop;
    const Plan shape(Layout::SoA, 7, 3, 4, 16, 1);

    // The last backing allocation disappears without an explicit close().
    RegionRoot transient = RegionRoot::trusted_create(7, 3);
    {
        auto owner = std::make_unique<HostBuffer>(shape);
        transient = owner->region_root();
        check(transient.is_live_owned(), "new allocation root unexpectedly retired");
    }
    check(!transient.is_live_owned(), "orphaned allocation identity was not retired");

    // A read loan owns the backing State even after its wrapper is gone.
    RegionRoot read_root = RegionRoot::trusted_create(7, 3);
    std::optional<HostBuffer::ReadLoan> reading;
    {
        auto owner = std::make_unique<HostBuffer>(shape);
        read_root = owner->region_root();
        owner->write(1, 2, 37.0f);
        reading.emplace(owner->borrow_read());
    }
    check(read_root.is_live_owned(), "read loan prematurely retired its backing");
    check(reading->read(1, 2) == 37.0f, "loan lost backing storage");
    reading.reset();
    check(!read_root.is_live_owned(), "read loan final release failed to retire root");

    // Exclusive write loans also keep the last backing allocation alive.
    RegionRoot write_root = RegionRoot::trusted_create(7, 3);
    std::optional<HostBuffer::WriteLoan> writing;
    {
        auto owner = std::make_unique<HostBuffer>(shape);
        write_root = owner->region_root();
        writing.emplace(owner->borrow_write());
    }
    check(write_root.is_live_owned(), "write loan prematurely retired its backing");
    writing->write(0, 1, 17.0f);
    writing.reset();
    check(!write_root.is_live_owned(), "write loan final release failed to retire root");

    // Ownership transfer does not replace the backing identity.
    RegionRoot moved = RegionRoot::trusted_create(7, 3);
    {
        HostBuffer owner(shape);
        moved = owner.region_root();
        {
            auto successor = owner.take();
            check(successor.region_root().same_root(moved), "take minted a new root");
            check(moved.is_live_owned(), "owner move retired live storage");
        }
        check(!moved.is_live_owned(), "moved owner destructor did not retire storage");
    }

    // Explicit closure must remain idempotent and retire immediately.
    HostBuffer closed(shape);
    auto closed_root = closed.region_root();
    closed.close();
    check(!closed_root.is_live_owned(), "explicit close left storage root live");
    closed.close();
}
