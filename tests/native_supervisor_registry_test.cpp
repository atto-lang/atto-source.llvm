#include "oreslang/runtime/NativeSupervisorRegistry.hpp"

#include <cstdint>
#include <set>
#include <stdexcept>
#include <vector>

using oreslang::runtime::NativeSupervisorRegistry;

static void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

static std::set<std::uint64_t> as_set(const std::vector<std::uint64_t>& ids) {
    return {ids.begin(), ids.end()};
}

int main() {
    NativeSupervisorRegistry tree(100, 8, 8);
    auto root = tree.root();
    auto daddy1 = tree.create_child(root);
    auto daddy2 = tree.create_child(root);
    auto nested = tree.create_child(daddy1);
    check(tree.parent_id(root) == 0, "root cannot have an ordinary parent");
    check(tree.parent_id(nested) == daddy1.id(), "child supervisor lost parent");
    check(as_set(tree.child_ids(root))
                  == std::set<std::uint64_t>{daddy1.id(), daddy2.id()},
          "root cannot enumerate supervised supervisors");
    check(as_set(tree.child_ids(daddy1))
                  == std::set<std::uint64_t>{nested.id()},
          "nested supervisor not registered");

    auto group1 = tree.create_group(daddy1, 2);
    auto group2 = tree.create_group(nested, 2);
    auto group3 = tree.create_group(daddy2, 2);
    check(group1->supervisor_id() == daddy1.id(),
          "group did not bind direct supervisor at construction");
    check(group2->supervisor_id() == nested.id(),
          "nested supervisor group identity missing");
    check(group1->runtime_id() == 100, "group created in a foreign runtime");
    check(as_set(tree.direct_group_ids(daddy1))
                  == std::set<std::uint64_t>{group1->group_id()},
          "direct group enumeration included descendants");
    check(as_set(tree.subtree_group_ids(daddy1))
                  == std::set<std::uint64_t>{
                      group1->group_id(), group2->group_id()},
          "descendant group missing");
    check(as_set(tree.subtree_group_ids(root))
                  == std::set<std::uint64_t>{
                      group1->group_id(), group2->group_id(), group3->group_id()},
          "root supervisor cannot enumerate subtree groups");

    group1->close();
    check(group1->supervisor_id() == daddy1.id(),
          "closed group supervisor identity must remain immutable");
    check(as_set(tree.subtree_group_ids(daddy1))
                  == std::set<std::uint64_t>{group2->group_id()},
          "closed group leaked into live subtree");
    group2.reset();
    check(tree.subtree_group_ids(daddy1).empty(),
          "destructed group leaked into registry snapshot");

    NativeSupervisorRegistry foreign(100, 8, 8);
    auto foreign_child = foreign.create_child(foreign.root());
    check(foreign_child.id() == daddy1.id(),
          "test setup requires coincident numeric supervisor ids");
    bool rejected = false;
    try { (void)tree.create_child(foreign_child); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "foreign parent supervisor accepted despite ID collision");
    rejected = false;
    try { (void)tree.create_group(foreign_child, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "foreign supervisor group accepted despite ID collision");

    NativeSupervisorRegistry bounded(200, 2, 1);
    auto child = bounded.create_child(bounded.root());
    bool too_many = false;
    try { (void)bounded.create_child(child); }
    catch (const std::length_error&) { too_many = true; }
    check(too_many, "supervision hierarchy capacity not enforced");
    auto one = bounded.create_group(child, 1);
    too_many = false;
    try { (void)bounded.create_group(child, 1); }
    catch (const std::length_error&) { too_many = true; }
    check(too_many, "bounded native group registry admitted excess groups");
    one->close();
    auto replacement = bounded.create_group(child, 1);
    check(replacement->supervisor_id() == child.id(),
          "closed-group prune failed to restore admission capacity");
}
