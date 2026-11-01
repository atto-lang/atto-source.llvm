#include "state_region.hpp"
#include "state_region_wire_v1.hpp"
#include <cassert>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\\n", __FILE__, __LINE__, #x); std::abort(); } } while (false)
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <atomic>

static std::atomic<size_t> allocations{0};
void* operator new(std::size_t n) {
    ++allocations;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using ores::state::Region;
using ores::state::Mode;
using ores::state::Status;

int main() {
    using ores::state::wire::Requirement;
    using ores::state::wire::Provenance;
    using ores::state::wire::Kind;
    using ores::state::wire::Action;
    constexpr std::uint64_t max_id = ores::state::wire::kMaxJsonSafeId;
    const Provenance borrow{9, 3, 7, 1001, 41, 6, 501,
                            "inventory.stock", Kind::write, Action::borrow};
    const Provenance release = borrow.release_record();
    CHECK(borrow.valid() && borrow.authorizes_release(release));
    CHECK(!release.authorizes_release(borrow));
    CHECK(!borrow.authorizes_release(borrow));
    auto corrupted = release;
    // Exact field identity must match all canonical negative controls.
    corrupted.execution_id = 1002; CHECK(!borrow.authorizes_release(corrupted));
    corrupted = release; corrupted.runtime_domain = 8; CHECK(!borrow.authorizes_release(corrupted));
    corrupted = release; corrupted.execution_generation = 5; CHECK(!borrow.authorizes_release(corrupted));
    corrupted = release; corrupted.execution_slot = 42; CHECK(!borrow.authorizes_release(corrupted));
    corrupted = release; corrupted.region_id = 10; CHECK(!borrow.authorizes_release(corrupted));
    corrupted = release; corrupted.region_generation = 2; CHECK(!borrow.authorizes_release(corrupted));
    corrupted = release; corrupted.loan_id = 502; CHECK(!borrow.authorizes_release(corrupted));
    corrupted = release; corrupted.place = "inventory.reserved"; CHECK(!borrow.authorizes_release(corrupted));
    corrupted = release; corrupted.kind = Kind::read; CHECK(!borrow.authorizes_release(corrupted));

    const Provenance maximum{max_id, max_id, max_id, max_id, max_id, max_id, max_id,
                             "inventory.stock", Kind::read, Action::borrow};
    CHECK(maximum.authorizes_release(maximum.release_record()));
    corrupted = maximum; corrupted.region_id = max_id + 1; CHECK(!corrupted.valid());
    corrupted = maximum; corrupted.region_generation = max_id + 1; CHECK(!corrupted.valid());
    corrupted = maximum; corrupted.runtime_domain = max_id + 1; CHECK(!corrupted.valid());
    corrupted = maximum; corrupted.execution_id = max_id + 1; CHECK(!corrupted.valid());
    corrupted = maximum; corrupted.execution_slot = max_id + 1; CHECK(!corrupted.valid());
    corrupted = maximum; corrupted.execution_generation = max_id + 1; CHECK(!corrupted.valid());
    corrupted = maximum; corrupted.loan_id = max_id + 1; CHECK(!corrupted.valid());
    corrupted = maximum; corrupted.kind = static_cast<Kind>(255); CHECK(!corrupted.valid());
    corrupted = maximum; corrupted.action = static_cast<Action>(255); CHECK(!corrupted.valid());
    corrupted = maximum; corrupted.place = " "; CHECK(!corrupted.valid());

    Requirement requirement{"inventory", true, true, 501,
                            "inventory.stock", Kind::write, Action::borrow};
    CHECK(requirement.valid());
    auto requirement_release = requirement;
    requirement_release.action = Action::release;
    CHECK(requirement.authorizes_release(requirement_release));
    requirement_release.generation_fence = false;
    CHECK(!requirement.authorizes_release(requirement_release));
    requirement_release = requirement;
    requirement_release.loan_id = max_id + 1;
    CHECK(!requirement.authorizes_release(requirement_release));
    CHECK(!requirement_release.valid());
    Region region;
    const size_t before = allocations.load();
    // Arbitrary enum discriminants must not mint a read loan or release one.
    const auto forged_mode = static_cast<Mode>(255);
    CHECK(region.acquire(forged_mode) == Status::invalid_mode);
    CHECK(region.release(forged_mode) == Status::invalid_mode);
    CHECK(region.readers == 0 && !region.writer);
    // The 64-bit write generation must never wrap to a stale ABA value.
    Region exhausted;
    CHECK(exhausted.acquire(Mode::write) == Status::ok);
    exhausted.version = std::numeric_limits<uint64_t>::max();
    CHECK(exhausted.release(Mode::write) == Status::version_exhausted);
    CHECK(exhausted.writer);
    CHECK(exhausted.retire() == Status::busy);
    CHECK(exhausted.release(forged_mode) == Status::invalid_mode);
    // The last representable epoch is a valid completed write but cannot
    // authorize another writer; read-only access and retirement still work.
    Region boundary;
    boundary.version = std::numeric_limits<uint64_t>::max() - 1;
    CHECK(boundary.acquire(Mode::write) == Status::ok);
    CHECK(boundary.release(Mode::write) == Status::ok);
    CHECK(boundary.version == std::numeric_limits<uint64_t>::max());
    CHECK(boundary.acquire(Mode::write) == Status::version_exhausted);
    CHECK(!boundary.writer && boundary.readers == 0);
    CHECK(boundary.acquire(Mode::read) == Status::ok);
    CHECK(boundary.retire() == Status::busy);
    CHECK(boundary.release(Mode::read) == Status::ok);
    CHECK(boundary.retire() == Status::ok);

    CHECK(region.acquire(Mode::read) == Status::ok);
    CHECK(region.acquire(Mode::read) == Status::ok);
    CHECK(region.acquire(Mode::write) == Status::conflicting_access);
    CHECK(region.retire() == Status::busy);
    CHECK(region.release(Mode::read) == Status::ok);
    CHECK(region.release(Mode::read) == Status::ok);
    CHECK(region.acquire(Mode::write) == Status::ok);
    CHECK(region.pin() == Status::ok);
    CHECK(region.acquire(Mode::read) == Status::conflicting_access);
    CHECK(region.retire() == Status::busy);
    uint64_t value = 0;
    for (int i = 0; i < 100000; ++i) ++value;
    CHECK(value == 100000);
    CHECK(region.release(Mode::write) == Status::ok);
    CHECK(region.version == 1);
    CHECK(region.retire() == Status::busy);
    CHECK(region.unpin() == Status::ok);
    CHECK(region.retire() == Status::ok);
    CHECK(region.acquire(Mode::read) == Status::retired);
    CHECK(region.release(Mode::write) == Status::bad_release);
    CHECK(region.unpin() == Status::bad_release);
    CHECK(allocations.load() == before);
    std::puts("state_region: PASS (100000 mutations; 0 counted C++ heap allocations)");
}
