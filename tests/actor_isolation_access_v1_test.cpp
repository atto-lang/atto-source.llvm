#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include "oreslang/runtime/ActorIsolationAccessV1.hpp"
int main() {
    using atto::security::ActorIsolationAccessV1;
    ActorIsolationAccessV1 a(1, 2, 3, 64);
    auto grant = a.issue_allocation(8, 16);
    assert(grant.has_value());
    assert(a.checked_offset(*grant, 2, 16) == 8);
    assert(a.checked_offset(*grant, 3, 8) == 16);
    assert(!a.checked_offset(*grant, 1, 8));
    assert(!a.checked_offset(*grant, 3, 9));
    assert(!a.checked_offset(*grant, 4, 1));
    assert(!a.checked_offset(*grant, 0, 8));
    assert(!a.checked_offset(*grant, 2, 0));
    assert(!a.checked_offset(*grant, 2, UINT64_MAX));
    assert(!a.issue_allocation(16, 8));
    assert(!a.issue_allocation(1, 8));
    ActorIsolationAccessV1 other_actor(1, 4, 3, 64);
    ActorIsolationAccessV1 other_tenant(5, 2, 3, 64);
    ActorIsolationAccessV1 new_incarnation(1, 2, 4, 64);
    assert(!other_actor.checked_offset(*grant, 2, 8));
    assert(!other_tenant.checked_offset(*grant, 2, 8));
    assert(!new_incarnation.checked_offset(*grant, 2, 8));
    assert(a.release(*grant));
    assert(!a.checked_offset(*grant, 2, 8));
    auto reused = a.issue_allocation(8, 16);
    assert(reused && a.checked_offset(*reused, 2, 8) == 8);
    assert(!a.checked_offset(*grant, 2, 8));
    a.retire();
    assert(!a.issue_allocation(0, 8));
    assert(!a.checked_offset(*reused, 2, 8));
    auto sibling = other_actor.issue_allocation(0, 8);
    assert(sibling && other_actor.checked_offset(*sibling, 1, 8) == 0);
    const auto span = ActorIsolationAccessV1::max_compressed32_span;
    ActorIsolationAccessV1 max(1, 1, 1, span);
    auto tail = max.issue_allocation(span - 8, 8);
    assert(tail && max.checked_offset(*tail, UINT32_MAX, 8) == span - 8);
    assert(!max.checked_offset(*tail, UINT32_MAX, 9));
    assert(!max.issue_allocation(span, 1));
    bool denied = false;
    try { ActorIsolationAccessV1 invalid(1, 1, 1, span + 1); }
    catch (const std::invalid_argument&) { denied = true; }
    assert(denied);
}
