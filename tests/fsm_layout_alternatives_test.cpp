// Host-only differential fixture: exercise actual DopLayout.hpp address planning.
// This is NOT compiled guest Oreslang FSM/ECS or an LLVM lowering test.
#include "oreslang/runtime/DopLayout.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
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
struct Handle { std::uint32_t owner, slot, generation; };
struct View { std::uint64_t epoch; };
class Store {
public:
    explicit Store(Layout layout)
        : layout_(layout), plan_(plan(layout)), bytes_(static_cast<std::size_t>(plan_.byte_length()), 0xA5) {
        for (std::uint32_t r = 0; r < kRows; ++r) set(r, {});
        generations_.fill(1);
    }
    Handle handle(std::uint32_t slot) const {
        check(slot < kRows, "invalid ECS entity slot");
        return {kOwner, slot, generations_[slot]};
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
    View begin_view() { check(loans_ == 0, "overlapping mutable view"); ++loans_; return {epoch_}; }
    Row read_view(View v, std::uint32_t slot) const {
        check(v.epoch == epoch_, "stale migration view");
        check(slot < kRows, "view row out of bounds");
        return get(slot);
    }
    void end_view(View v) {
        check(loans_ == 1 && v.epoch == epoch_, "invalid loan release");
        --loans_;
    }
    // One physical authority only: all fields copied before atomically replacing store bytes.
    void migrate(Layout target) {
        check(loans_ == 0, "layout migration while borrowed");
        if (layout_ == target) return;
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
    store.migrate(Layout::AoSoA);
    must_reject([&]{ (void)store.read_view(held, 2); }, "stale view after migration admitted");
    check(store.read(h) == Row{}, "migration changed logical data");
    store.apply(h, 0);
    store.apply(h, 1);
    store.migrate(Layout::SoA);
    check(store.read(h) == Row{1, 2, 1}, "SoA migration lost state or version");
    must_reject([&]{ (void)store.read({kOwner + 1, 2, h.generation}); }, "wrong owner admitted");
    must_reject([&]{ (void)store.read({kOwner, kRows, 1}); }, "out-of-bounds id admitted");
    store.retire(h);
    must_reject([&]{ (void)store.read(h); }, "retired handle resurrected (ABA)");
    check(store.read(store.handle(2)) == Row{}, "recycled entity leaked old state");
}
struct Access { std::uint32_t owner, reads, writes; bool proven_region; };
bool can_parallel(const Access& a, const Access& b) {
    if (!a.proven_region || !b.proven_region) return false;
    if (a.owner != b.owner) return true; // independent, authenticated regions only
    return ((a.writes & (b.reads | b.writes)) == 0) &&
           ((b.writes & (a.reads | a.writes)) == 0);
}
void check_ecs_admission() {
    check(!can_parallel({7, 1, 2, true}, {7, 2, 0, true}), "ECS read/write conflict admitted");
    check(!can_parallel({7, 0, 4, true}, {7, 0, 4, true}), "ECS write/write conflict admitted");
    check(can_parallel({7, 1, 2, true}, {7, 4, 0, true}), "ECS independent fields rejected");
    check(can_parallel({7, 1, 2, true}, {8, 2, 1, true}), "ECS distinct proven regions rejected");
    check(!can_parallel({7, 1, 2, true}, {8, 2, 1, false}), "ECS unproven domain admitted");
}
} // namespace
int main() {
    check_geometry();
    check_trace_refinement();
    check_lifecycle();
    check_ecs_admission();
    std::cout << "FSM_LAYOUT_HOST_PASS traces=1023 layouts=3 rows=5 depth=4"
                 " negative=guard,overflow,loans,stale-view,owner,generation,ecs\n";
}
