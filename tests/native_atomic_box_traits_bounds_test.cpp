#include "oreslang/types/AutoTraitRules.hpp"

#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace oreslang::types;

#define REQUIRE(expr) do { if (!(expr)) throw std::runtime_error("failed: " #expr); } while (false)

static AutoType scalar() {
    AutoType t;
    t.kind = AutoTypeKind::Scalar;
    return t;
}
static AutoType wrap(AutoTypeKind kind, AutoType child, bool shared = false) {
    AutoType t;
    t.kind = kind;
    t.parameters.push_back(std::move(child));
    t.shareable_domain = shared;
    return t;
}
static void malformed_metadata_must_fail_closed() {
    AutoType hidden_scalar = scalar();
    hidden_scalar.parameters.push_back(wrap(AutoTypeKind::Rc, scalar()));
    auto capabilities = infer_auto_traits(hidden_scalar);
    REQUIRE(!capabilities.send && !capabilities.sync && !capabilities.share);
    hidden_scalar = scalar();
    hidden_scalar.dyn_bounds.insert(AutoTrait::Send);
    REQUIRE(!infer_auto_traits(hidden_scalar).send);

    AutoType hidden_dyn;
    hidden_dyn.kind = AutoTypeKind::Dyn;
    hidden_dyn.shareable_domain = true;
    hidden_dyn.dyn_bounds = {AutoTrait::Send, AutoTrait::Sync, AutoTrait::Share};
    hidden_dyn.parameters.push_back(wrap(AutoTypeKind::Rc, scalar()));
    capabilities = infer_auto_traits(hidden_dyn);
    REQUIRE(!capabilities.send && !capabilities.sync && !capabilities.share);

    AutoType unverified;
    unverified.kind = AutoTypeKind::Nominal;
    REQUIRE(!infer_auto_traits(unverified).send);
    for (auto kind : {AutoTypeKind::Box, AutoTypeKind::Arc, AutoTypeKind::Weak,
                      AutoTypeKind::Mutex, AutoTypeKind::RwLock, AutoTypeKind::Rc,
                      AutoTypeKind::RefCell}) {
        AutoType no_argument;
        no_argument.kind = kind;
        capabilities = infer_auto_traits(no_argument);
        REQUIRE(!capabilities.send && !capabilities.sync && !capabilities.share);
        AutoType wrong_arity = wrap(kind, scalar(), true);
        wrong_arity.parameters.push_back(scalar());
        capabilities = infer_auto_traits(wrong_arity);
        REQUIRE(!capabilities.send && !capabilities.sync && !capabilities.share);
    }
}
static void depth_and_budget_must_fail_closed() {
    AutoType deep = scalar();
    for (std::size_t i = 0; i < 160; ++i) {
        deep = wrap(AutoTypeKind::Box, std::move(deep));
    }
    auto capabilities = infer_auto_traits(deep);
    REQUIRE(!capabilities.send && !capabilities.sync && !capabilities.share);

    AutoType medium = scalar();
    for (std::size_t i = 0; i < 24; ++i) {
        medium = wrap(AutoTypeKind::Box, std::move(medium));
    }
    capabilities = infer_auto_traits(medium);
    REQUIRE(capabilities.send && capabilities.sync && !capabilities.share);

    AutoType wide;
    wide.kind = AutoTypeKind::Nominal;
    wide.complete_nominal_layout = true;
    wide.shareable_domain = true;
    wide.parameters = std::vector<AutoType>(5000, scalar());
    capabilities = infer_auto_traits(wide);
    REQUIRE(!capabilities.send && !capabilities.sync && !capabilities.share);
    REQUIRE(!all_have(wide.parameters, AutoTrait::Send));
    REQUIRE(all_have(std::vector<AutoType>(8, scalar()), AutoTrait::Sync));
}
static void valid_composition_remains_consistent() {
    const auto inner = scalar();
    const auto box = infer_auto_traits(wrap(AutoTypeKind::Box, inner));
    REQUIRE(box.send && box.sync && !box.share);

    auto rc = wrap(AutoTypeKind::Rc, inner);
    const auto rc_caps = infer_auto_traits(rc);
    REQUIRE(!rc_caps.send && !rc_caps.sync && !rc_caps.share);

    auto local_arc = wrap(AutoTypeKind::Arc, scalar());
    const auto local_caps = infer_auto_traits(local_arc);
    REQUIRE(!local_caps.send && !local_caps.sync && !local_caps.share);
    auto shared_arc = wrap(AutoTypeKind::Arc, scalar(), true);
    const auto shared_caps = infer_auto_traits(shared_arc);
    REQUIRE(shared_caps.send && shared_caps.sync && shared_caps.share);
    auto arc_of_rc = wrap(AutoTypeKind::Arc, std::move(rc), true);
    const auto denied = infer_auto_traits(arc_of_rc);
    REQUIRE(!denied.send && !denied.sync && !denied.share);

    auto refcell = wrap(AutoTypeKind::RefCell, scalar());
    auto mutex = infer_auto_traits(wrap(AutoTypeKind::Mutex, refcell));
    auto rwlock = infer_auto_traits(wrap(AutoTypeKind::RwLock, refcell));
    REQUIRE(mutex.send && mutex.sync && !mutex.share);
    REQUIRE(rwlock.send && !rwlock.sync && !rwlock.share);
}
int main() {
    malformed_metadata_must_fail_closed();
    depth_and_budget_must_fail_closed();
    valid_composition_remains_consistent();
    std::puts("ATTO_NATIVE_TRAIT_BUDGET_PASS");
}
