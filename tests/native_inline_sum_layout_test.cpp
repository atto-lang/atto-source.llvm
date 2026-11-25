#include "oreslang/types/NativeInlineSumLayout.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
using L = oreslang::types::NativeInlineSumLayout;
using K = L::Kind;
static unsigned checks = 0;
static void require(bool b) { ++checks; if (!b) std::abort(); }
struct Probe { bool* live; unsigned char* expected; unsigned calls = 0; std::uint32_t id = 0; };
static void drop(void* context, std::uint32_t id, unsigned char* p) noexcept {
    auto& probe = *static_cast<Probe*>(context);
    require(!*probe.live && p == probe.expected);
    ++probe.calls; probe.id = id;
}
static void rejected(const L::Node* nodes, std::size_t n) {
    try { (void)L::build(nodes, n); } catch (const std::invalid_argument&) { ++checks; return; }
    std::abort();
}
int main() {
    // Result<Option<int>, NumericError> and Option<Result<int, NumericError>>.
    const L::Node a[] = {{K::I64}, {K::Option, 0}, {K::NumericError}, {K::Result, 1, 2}};
    const L::Node b[] = {{K::I64}, {K::NumericError}, {K::Result, 0, 1}, {K::Option, 2}};
    auto x = L::build(a, 4), y = L::build(b, 4);
    require(x.root().bytes == 24 && x.root().alignment == 8 && x.root().payload_offset == 8);
    require(y.root().bytes == 24 && y.root().alignment == 8 && !y.root().needs_drop);
    // Result<Option<OwnedA>, OwnedB>: same union storage, distinct drop IDs.
    const L::Node owned[] = {{K::OwnedLeaf, 0, 0, 16, 8, 11}, {K::Option, 0},
                             {K::OwnedLeaf, 0, 0, 32, 16, 22}, {K::Result, 1, 2}};
    auto plan = L::build(owned, 4);
    require(plan.root().bytes == 48 && plan.root().alignment == 16 && plan.root().needs_drop);
    alignas(64) std::array<unsigned char, 64> storage{};
    for (unsigned outer = 0; outer < 3; ++outer) for (unsigned inner = 0; inner < 3; ++inner) {
        storage.fill(255); storage[0] = static_cast<unsigned char>(outer);
        storage[16] = static_cast<unsigned char>(inner);
        bool live = true; Probe p{&live, storage.data() + (outer == 1 ? 16 : 24)};
        auto r = plan.retire(storage.data(), storage.size(), live, drop, &p);
        if (outer == 2 || (outer == 0 && inner == 2)) {
            require(r == L::Retire::Invalid && live && p.calls == 0); continue;
        }
        require(r == L::Retire::Retired && !live);
        require(p.calls == (outer == 1 || inner == 1 ? 1u : 0u));
        if (p.calls) require(p.id == (outer == 1 ? 22u : 11u));
        const auto calls = p.calls;
        require(plan.retire(storage.data(), storage.size(), live, drop, &p) == L::Retire::Inactive);
        require(p.calls == calls);
    }
    bool live = true; storage[0] = 1;
    require(plan.retire(storage.data(), storage.size(), live, nullptr, nullptr) == L::Retire::Invalid && live);
    require(plan.retire(storage.data(), 47, live, drop, nullptr) == L::Retire::Invalid && live);
    require(plan.retire(storage.data() + 1, 63, live, drop, nullptr) == L::Retire::Invalid && live);
    const L::Node cycle[] = {{K::Option, 0}}; rejected(cycle, 1);
    const L::Node bad_align[] = {{K::OwnedLeaf, 0, 0, 12, 3, 1}}; rejected(bad_align, 1);
    const L::Node no_drop[] = {{K::OwnedLeaf, 0, 0, 8, 8, 0}}; rejected(no_drop, 1);
    const L::Node unknown[] = {{static_cast<K>(255)}}; rejected(unknown, 1);
    const L::Node big[] = {{K::OwnedLeaf, 0, 0, L::max_bytes, 1, 1}, {K::Option, 0}}; rejected(big, 2);
    std::array<L::Node, 33> deep{}; deep[0] = {K::Bool};
    for (std::size_t i = 1; i < deep.size(); ++i) deep[i] = {K::Option, i - 1};
    require(L::build(deep.data(), 32).root().depth == 32); rejected(deep.data(), 33);
    rejected(nullptr, 0);
    std::cout << "native inline sum HOST-ONLY checks=" << checks << '\n';
}
