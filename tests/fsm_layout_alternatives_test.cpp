// Host-only differential fixture: exercise actual DopLayout.hpp address planning.
// This is NOT compiled guest Oreslang FSM/ECS or an LLVM lowering test.
#include "oreslang/runtime/DopLayout.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using oreslang::dop::Layout;
using oreslang::dop::Plan;
constexpr std::uint32_t kRows = 5, kFields = 3, kOwner = 0xBEEF;
constexpr std::array<Layout, 3> kLayouts{{Layout::AoS, Layout::SoA, Layout::AoSoA}};
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct Row {
    std::uint32_t state = 0, version = 0, ticks = 0;
};
bool operator==(const Row& a, const Row& b) {
    return a.state == b.state && a.version == b.version && a.ticks == b.ticks;
}
struct Step { bool accepted; Row next; std::uint32_t effect; };
// Pure total FSM: (Idle=0, Running=1, Done=2); events Start/Tick/Finish/Reset=0..3.
// Rejected guards and missing edges are observationally no-ops, not success-stutters.
Step reduce(Row old, std::uint32_t event) {
    Row next = old;
    std::uint32_t effect = 0;
    if (old.version == std::numeric_limits<std::uint32_t>::max())
        return {false, old, 0};
    if (old.state == 0 && event == 0) { next.state = 1; next.ticks = 0; effect = 1; }
    else if (old.state == 1 && event == 1 && old.ticks < 3) { ++next.ticks; effect = 2; }
    else if (old.state == 1 && event == 2 && old.ticks >= 2) { next.state = 2; effect = 3; }
    else if (old.state == 2 && event == 3) { next.state = 0; next.ticks = 0; effect = 4; }
    else return {false, old, 0};
    ++next.version;
    return {true, next, effect};
}
Plan plan(Layout l) { return Plan(l, kRows, kFields, 4, 16, l == Layout::AoSoA ? 4 : 1); }
std::uint32_t raw_read(const Plan& p, const std::vector<std::uint8_t>& b,
                       std::uint32_t r, std::uint32_t f) {
    auto off = p.byte_offset(r, f);
    check(off + 4 <= b.size(), "read overrun");
    std::uint32_t out = 0;
    std::memcpy(&out, b.data() + static_cast<std::size_t>(off), sizeof(out));
    return out;
}
void raw_write(const Plan& p, std::vector<std::uint8_t>& b,
               std::uint32_t r, std::uint32_t f, std::uint32_t value) {
    auto off = p.byte_offset(r, f);
    check(off + 4 <= b.size(), "write overrun");
    std::memcpy(b.data() + static_cast<std::size_t>(off), &value, sizeof(value));
}
// These shared ownership tickets are HOST-REFERENCE provenance, not a guest ABI.
 // Production must use compiler-issued, nonforgeable region/loan capabilities.
struct Handle {
    std::uint32_t owner, slot, generation;
    std::shared_ptr<const std::uint8_t> store_token;
};
struct View {
    std::uint64_t epoch;
    std::shared_ptr<const std::uint8_t> loan_token;
};
class Access {
public:
    Access() = default; // inadmissible / unproved request
private:
    friend class Store;
    friend bool can_parallel(const Access&, const Access&);
    Access(std::shared_ptr<const std::uint8_t> region, std::uint32_t reads, std::uint32_t writes)
        : region_(std::move(region)), reads_(reads), writes_(writes) {}
    std::shared_ptr<const std::uint8_t> region_;
    std::uint32_t reads_ = 0, writes_ = 0;
};
class Store {
public:
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;
    explicit Store(Layout layout)
        : layout_(layout), plan_(plan(layout)), bytes_(static_cast<std::size_t>(plan_.byte_length()), 0xA5) {
        for (std::uint32_t r = 0; r < kRows; ++r) set(r, {});
        generations_.fill(1);
    }
    Handle handle(std::uint32_t slot) const {
        check(slot < kRows, "invalid ECS entity slot");
        return {kOwner, slot, generations_[slot], store_token_};
    }
    Row read(Handle h) const {
        verify_handle(h);
        return get(h.slot);
    }
    Step apply(Handle h, std::uint32_t event) {
        verify_handle(h);
        check(loans_ == 0, "FSM mutation while borrowed");
        Step s = reduce(get(h.slot), event);
        if (s.accepted) set(h.slot, s.next);
        return s;
    }
    void retire(Handle h) {
        verify_handle(h);
        check(loans_ == 0, "retire while borrowed");
        check(generations_[h.slot] != std::numeric_limits<std::uint32_t>::max(),
              "generation exhausted");
        ++generations_[h.slot];
        set(h.slot, {}); // deterministic slot reuse; production ECS may swap-delete.
    }
    View begin_view() {
        check(loans_ == 0, "overlapping mutable view");
        auto ticket = std::make_shared<const std::uint8_t>(0);
        active_loan_ = ticket;
        loans_ = 1; // publish only after successful ticket allocation
        return {epoch_, std::move(ticket)};
    }
    Row read_view(const View& v, std::uint32_t slot) const {
        check(loans_ == 1 && active_loan_ && v.loan_token == active_loan_ &&
              v.epoch == epoch_, "foreign, released or stale view");
        check(slot < kRows, "view row out of bounds");
        return get(slot);
    }
    void end_view(const View& v) {
        check(loans_ == 1 && active_loan_ && v.loan_token == active_loan_ &&
              v.epoch == epoch_, "foreign, duplicate or stale loan release");
        loans_ = 0;
        active_loan_.reset();
    }
    Access issue_access(std::uint32_t reads, std::uint32_t writes) const {
        return Access(store_token_, reads, writes);
    }
    // One physical authority only: all fields copied before atomically replacing store bytes.
    void migrate(Layout target) {
        check(loans_ == 0, "layout migration while borrowed");
        if (layout_ == target) return;
        check(epoch_ != std::numeric_limits<std::uint64_t>::max(),
              "migration epoch exhausted");
        Plan new_plan = plan(target);
        std::vector<std::uint8_t> new_bytes(static_cast<std::size_t>(new_plan.byte_length()), 0xA5);
        for (std::uint32_t r = 0; r < kRows; ++r) {
            const Row old = get(r);
            raw_write(new_plan, new_bytes, r, 0, old.state);
            raw_write(new_plan, new_bytes, r, 1, old.version);
            raw_write(new_plan, new_bytes, r, 2, old.ticks);
        }
        // Layout Plan assignment is nonthrowing (only primitive members).
        plan_ = new_plan;
        bytes_.swap(new_bytes);
        layout_ = target;
        ++epoch_;
    }
private:
    void verify_handle(Handle h) const {
        check(h.store_token && h.store_token == store_token_, "foreign ECS store");
        check(h.owner == kOwner, "wrong ECS ownership domain");
        check(h.slot < kRows, "out-of-bounds ECS slot");
        check(generations_[h.slot] == h.generation, "stale ECS handle / ABA");
    }
    Row get(std::uint32_t r) const {
        return {raw_read(plan_, bytes_, r, 0), raw_read(plan_, bytes_, r, 1),
                raw_read(plan_, bytes_, r, 2)};
    }
    void set(std::uint32_t r, Row row) {
        raw_write(plan_, bytes_, r, 0, row.state);
        raw_write(plan_, bytes_, r, 1, row.version);
        raw_write(plan_, bytes_, r, 2, row.ticks);
    }
    const std::shared_ptr<const std::uint8_t> store_token_ =
        std::make_shared<const std::uint8_t>(0);
    std::shared_ptr<const std::uint8_t> active_loan_;
    Layout layout_;
    Plan plan_;
    std::vector<std::uint8_t> bytes_;
    std::array<std::uint32_t, kRows> generations_{};
    std::uint64_t epoch_ = 1;
    std::uint32_t loans_ = 0;
};
template<class F> void must_reject(F f, const char* message) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
void check_geometry() {
    for (Layout l : kLayouts) {
        const Plan p = plan(l);
        std::vector<std::uint64_t> offsets;
        for (std::uint32_t row = 0; row < kRows; ++row)
            for (std::uint32_t field = 0; field < kFields; ++field) {
                auto offset = p.byte_offset(row, field);
                check(offset + 4 <= p.byte_length(), "FSM field extends beyond region");
                for (auto prev : offsets) check(prev != offset, "FSM field collision");
                offsets.push_back(offset);
            }
        must_reject([&]{ (void)p.byte_offset(kRows, 0); }, "AoSoA tail lane address admitted");
        must_reject([&]{ (void)p.byte_offset(0, kFields); }, "invalid field admitted");
    }
    check(plan(Layout::AoSoA).byte_length() == 96, "AoSoA padded tail geometry");
}
void check_rejection_atomicity() {
    const Row guarded{1, 12, 1};
    const Step a = reduce(guarded, 2); // Finish requires at least two ticks.
    check(!a.accepted && a.next == guarded && a.effect == 0,
          "failed FSM guard emitted output or changed state");
    const Row capped{1, 33, 3};
    const Step b = reduce(capped, 1); // Cannot tick past the fixed bound.
    check(!b.accepted && b.next == capped && b.effect == 0,
          "FSM counter cap violated");
    const Row overflow{0, std::numeric_limits<std::uint32_t>::max(), 0};
    const Step c = reduce(overflow, 0);
    check(!c.accepted && c.next == overflow && c.effect == 0,
          "FSM version overflow admitted");
    const Row unknown{99, 2, 0};
    const Step d = reduce(unknown, 0);
    check(!d.accepted && d.next == unknown && d.effect == 0,
          "unknown FSM state accepted");
    Store store(Layout::SoA);
    const Handle h = store.handle(0);
    const Row before = store.read(h);
    const Step e = store.apply(h, 2);
    check(!e.accepted && e.effect == 0 && store.read(h) == before,
          "rejected physical-store transition mutated data");
}
void check_trace_refinement() {
    std::uint32_t traces = 0;
    std::uint32_t permutations = 1;
    for (unsigned depth = 0; depth <= 4; ++depth) {
        for (std::uint32_t path = 0; path < permutations; ++path) {
            for (Layout l : kLayouts) {
                Store store(l);
                std::array<Row, kRows> oracle{};
                auto code = path;
                for (unsigned step = 0; step < depth; ++step) {
                    const std::uint32_t event = code % 4;
                    code /= 4;
                    // Ordered ECS query: stable slot order, never physical buffer order.
                    for (std::uint32_t id = 0; id < kRows; ++id) {
                        const Step expected = reduce(oracle[id], event);
                        const Step actual = store.apply(store.handle(id), event);
                        check(actual.accepted == expected.accepted &&
                              actual.effect == expected.effect && actual.next == expected.next,
                              "FSM outcome mismatch across physical layouts");
                        oracle[id] = expected.next;
                        check(store.read(store.handle(id)) == oracle[id],
                              "FSM logical row mismatch");
                    }
                }
                ++traces;
            }
        }
        permutations *= 4;
    }
    check(traces == 1023, "finite trace count changed"); // 3 * (1+4+16+64+256)
}
void check_lifecycle() {
    Store store(Layout::AoS);
    Handle h = store.handle(2);
    const View held = store.begin_view();
    must_reject([&]{ store.apply(h, 0); }, "mutating during a read loan admitted");
    must_reject([&]{ store.migrate(Layout::AoSoA); }, "relayout during a read loan admitted");
    store.end_view(held);
    must_reject([&]{ (void)store.read_view(held, 2); },
                "released view still usable in same epoch");
    must_reject([&]{ store.end_view(held); }, "duplicate view release admitted");
    const View fresh = store.begin_view();
    must_reject([&]{ (void)store.read_view(held, 2); }, "old loan reused by new loan");
    check(store.read_view(fresh, 2) == Row{}, "fresh loan unusable");
    store.end_view(fresh);
    Store independent(Layout::AoS);
    must_reject([&]{ (void)independent.read(h); }, "cross-store handle accepted");
    const View other_view = independent.begin_view();
    must_reject([&]{ (void)store.read_view(other_view, 2); }, "cross-store view accepted");
    independent.end_view(other_view);
    store.migrate(Layout::AoSoA);
    must_reject([&]{ (void)store.read_view(held, 2); }, "stale view after migration admitted");
    check(store.read(h) == Row{}, "migration changed logical data");
    store.apply(h, 0);
    store.apply(h, 1);
    store.migrate(Layout::SoA);
    check(store.read(h) == Row{1, 2, 1}, "SoA migration lost state or version");
    must_reject([&]{ (void)store.read({kOwner + 1, 2, h.generation, h.store_token}); }, "wrong owner admitted");
    must_reject([&]{ (void)store.read({kOwner, kRows, 1, h.store_token}); }, "out-of-bounds id admitted");
    store.retire(h);
    must_reject([&]{ (void)store.read(h); }, "retired handle resurrected (ABA)");
    check(store.read(store.handle(2)) == Row{}, "recycled entity leaked old state");
}
bool can_parallel(const Access& a, const Access& b) {
    if (!a.region_ || !b.region_) return false;
    // Region tokens are minted by stores; no caller-asserted proven_region bool.
    if (a.region_ != b.region_) return true;
    return ((a.writes_ & (b.reads_ | b.writes_)) == 0) &&
           ((b.writes_ & (a.reads_ | a.writes_)) == 0);
}
void check_ecs_admission() {
    Store a(Layout::AoS), b(Layout::AoS);
    check(!can_parallel(a.issue_access(1, 2), a.issue_access(2, 0)),
          "ECS read/write conflict admitted");
    check(!can_parallel(a.issue_access(0, 4), a.issue_access(0, 4)),
          "ECS write/write conflict admitted");
    check(can_parallel(a.issue_access(1, 2), a.issue_access(4, 0)),
          "ECS independent fields rejected");
    check(can_parallel(a.issue_access(1, 2), b.issue_access(2, 1)),
          "ECS distinct minted regions rejected");
    check(!can_parallel(a.issue_access(1, 2), Access{}),
          "ECS unproved region admitted");
}
} // namespace
int main() {
    check_geometry();
    check_rejection_atomicity();
    check_trace_refinement();
    check_lifecycle();
    check_ecs_admission();
    std::cout << "FSM_LAYOUT_HOST_PASS traces=1023 layouts=3 rows=5 depth=4"
                 " negative=guard,overflow,loans,stale-view,owner,generation,ecs\n";
}
