#include "oreslang/types/AutoTraitRules.hpp"
#include "oreslang/types/DopLogicalAccess.hpp"

#include <stdexcept>
#include <vector>

using oreslang::types::AutoTrait;
using oreslang::types::AutoType;
using oreslang::types::AutoTypeKind;
using namespace oreslang::types::dop_access;

#define REQUIRE(condition) do {     if (!(condition)) throw std::runtime_error("requirement failed: " #condition); } while (false)

namespace {
AutoType scalar() {
    AutoType value;
    value.kind = AutoTypeKind::Scalar;
    value.shareable_domain = true;
    return value;
}

AutoType wrap(AutoTypeKind kind, AutoType value, bool shared = false) {
    AutoType result;
    result.kind = kind;
    result.parameters.push_back(std::move(value));
    result.shareable_domain = shared;
    return result;
}

AutoType entry(AutoType key, AutoType value) {
    AutoType result;
    result.kind = AutoTypeKind::Entry;
    result.parameters.push_back(std::move(key));
    result.parameters.push_back(std::move(value));
    return result;
}

Place place(oreslang::dop::Layout layout,
            std::optional<std::string> archetype,
            std::optional<std::string> component,
            std::optional<std::string> field,
            std::optional<Range> rows,
            std::uint64_t epoch = 7) {
    return Place(layout, "world", std::move(archetype), std::move(component),
                 std::move(field), std::move(rows), epoch);
}
}

int main() {
    const auto box_caps = oreslang::types::infer_auto_traits(
        wrap(AutoTypeKind::Box, scalar()));
    REQUIRE(box_caps.send && box_caps.sync && !box_caps.share);

    const auto local_arc = oreslang::types::infer_auto_traits(
        wrap(AutoTypeKind::Arc, scalar(), false));
    REQUIRE(!local_arc.send && !local_arc.sync && !local_arc.share);

    const auto shared_arc = oreslang::types::infer_auto_traits(
        wrap(AutoTypeKind::Arc, scalar(), true));
    REQUIRE(shared_arc.send && shared_arc.sync && shared_arc.share);

    const auto boxed_for_arc = wrap(AutoTypeKind::Box, scalar());
    const auto arc_box = oreslang::types::infer_auto_traits(
        wrap(AutoTypeKind::Arc, boxed_for_arc, true));
    REQUIRE(arc_box.send && arc_box.sync && !arc_box.share);

    const auto rc = oreslang::types::infer_auto_traits(
        wrap(AutoTypeKind::Rc, scalar()));
    REQUIRE(!rc.send && !rc.sync && !rc.share);

    const auto plain_entry = oreslang::types::infer_auto_traits(
        entry(scalar(), scalar()));
    REQUIRE(plain_entry.send && plain_entry.sync && plain_entry.share);

    const auto boxed_entry = oreslang::types::infer_auto_traits(
        entry(scalar(), wrap(AutoTypeKind::Box, scalar())));
    REQUIRE(boxed_entry.send && boxed_entry.sync && !boxed_entry.share);

    const auto rc_entry = oreslang::types::infer_auto_traits(
        entry(scalar(), wrap(AutoTypeKind::Rc, scalar())));
    REQUIRE(!rc_entry.send && !rc_entry.sync && !rc_entry.share);

    bool entry_arity_rejected = false;
    try {
        AutoType malformed;
        malformed.kind = AutoTypeKind::Entry;
        malformed.parameters.push_back(scalar());
        (void)oreslang::types::infer_auto_traits(malformed);
    } catch (const std::invalid_argument&) {
        entry_arity_rejected = true;
    }
    REQUIRE(entry_arity_rejected);

    const auto refcell = oreslang::types::infer_auto_traits(
        wrap(AutoTypeKind::RefCell, scalar()));
    REQUIRE(refcell.send && !refcell.sync && !refcell.share);

    const auto cell_type = wrap(AutoTypeKind::RefCell, scalar());
    const auto mutex_cell = oreslang::types::infer_auto_traits(wrap(AutoTypeKind::Mutex, cell_type));
    const auto rwlock_cell = oreslang::types::infer_auto_traits(wrap(AutoTypeKind::RwLock, cell_type));
    REQUIRE(mutex_cell.send && mutex_cell.sync && !mutex_cell.share);
    REQUIRE(rwlock_cell.send && !rwlock_cell.sync && !rwlock_cell.share);

    AutoType dyn;
    dyn.kind = AutoTypeKind::Dyn;
    dyn.shareable_domain = true;
    dyn.dyn_bounds = {AutoTrait::Send, AutoTrait::Sync};
    const auto dyn_caps = oreslang::types::infer_auto_traits(dyn);
    REQUIRE(dyn_caps.send && dyn_caps.sync && !dyn_caps.share);
    dyn.dyn_bounds.insert(AutoTrait::Share);
    REQUIRE(oreslang::types::infer_auto_traits(dyn).share);

    AutoType denied = scalar();
    denied.negative.insert(AutoTrait::Send);
    REQUIRE(!oreslang::types::infer_auto_traits(denied).send);

    for (auto layout : {oreslang::dop::Layout::AoS,
                        oreslang::dop::Layout::SoA,
                        oreslang::dop::Layout::AoSoA}) {
        Loan x{place(layout, "moving", "Position", "x", std::nullopt), Access::Write};
        Loan y{place(layout, "moving", "Position", "y", std::nullopt), Access::Read};
        REQUIRE(!conflicts(x, y));
    }

    Loan whole{place(oreslang::dop::Layout::SoA, "moving", "Position",
                     std::nullopt, std::nullopt), Access::Read};
    Loan x_write{place(oreslang::dop::Layout::SoA, "moving", "Position", "x",
                       std::nullopt), Access::Write};
    REQUIRE(conflicts(whole, x_write));

    Loan first{place(oreslang::dop::Layout::AoSoA, "moving", "Position",
                     std::nullopt, Range(0, 16)), Access::Write};
    Loan second{place(oreslang::dop::Layout::AoSoA, "moving", "Position",
                      std::nullopt, Range(16, 32)), Access::Write};
    REQUIRE(!conflicts(first, second));
    REQUIRE(can_run_concurrently({first}, {second}));

    Loan overlap{place(oreslang::dop::Layout::AoSoA, "moving", "Position",
                       std::nullopt, Range(8, 24)), Access::Read};
    REQUIRE(conflicts(first, overlap));

    Loan velocity{place(oreslang::dop::Layout::SoA, "moving", "Velocity",
                        std::nullopt, std::nullopt), Access::Write};
    REQUIRE(!conflicts(x_write, velocity));

    const auto stale = place(oreslang::dop::Layout::AoSoA, "moving", "Position",
                             "x", Range(16, 32), 6);
    bool rejected = false;
    try { require_current_epoch(stale, 7); }
    catch (const std::invalid_argument&) { rejected = true; }
    REQUIRE(rejected);
    rejected = false;
    try { (void)overlaps(first.place, stale); }
    catch (const std::invalid_argument&) { rejected = true; }
    REQUIRE(rejected);

    rejected = false;
    try {
        (void)Place(oreslang::dop::Layout::SoA, "world", "moving", " ",
                    std::nullopt, std::nullopt, 7);
    } catch (const std::invalid_argument&) { rejected = true; }
    REQUIRE(rejected);

    rejected = false;
    try { require_internally_valid({whole, x_write}); }
    catch (const std::invalid_argument&) { rejected = true; }
    REQUIRE(rejected);

    return 0;
}
