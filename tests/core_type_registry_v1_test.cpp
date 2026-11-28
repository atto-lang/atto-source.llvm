#include "oreslang/types/CoreTypeRegistry.hpp"

#include <cassert>
#include <set>
#include <string_view>

int main() {
    using namespace oreslang::types;

    std::set<std::string_view> names;
    for (const auto& entry : kCoreTypeRegistry) {
        assert(!entry.name.empty());
        assert(!entry.canonical.empty());
        assert(entry.canonical.find("..") == std::string_view::npos);
        assert(names.insert(entry.name).second);
        assert(entry.min_arity <= entry.max_arity);
        if (entry.kind == CoreTypeKind::Trait) {
            assert(entry.ownership == CoreOwnership::Trait);
            assert(entry.drop == CoreDropPolicy::NotAValue);
        }
        if (entry.ownership == CoreOwnership::BorrowedView) {
            assert(entry.allocation == CoreAllocation::BorrowedView);
            assert(entry.drop == CoreDropPolicy::None);
            assert(entry.send == CoreSendPolicy::NeverBorrowed);
        }
        if (entry.ownership == CoreOwnership::Owned)
            assert(entry.drop == CoreDropPolicy::OwnedStorage);
    }

    const auto* option = core_type_info("Option");
    const auto* result = core_type_info("Result");
    assert(option && result);
    assert(option->accepts_arity(1) && !option->accepts_arity(2));
    assert(result->accepts_arity(2) && !result->accepts_arity(1));
    assert(option->ownership == CoreOwnership::PayloadDependent);
    assert(option->drop == CoreDropPolicy::ActivePayloadOnce);
    assert(result->drop == CoreDropPolicy::ActivePayloadOnce);
    assert(core_sum_carrier("Option", 1));
    assert(core_sum_carrier("Result", 2));
    assert(!core_sum_carrier("Option", 2));
    assert(!core_sum_carrier("Result", 1));
    assert(!core_sum_carrier("Object", 1));

    assert(core_type_info("None")->drop == CoreDropPolicy::None);
    for (const auto name : {"Array", "FixedArray", "Tuple", "Pair", "Entry"})
        assert(core_type_info(name)->drop == CoreDropPolicy::AllInitializedMembers);
    const auto* slice = core_type_info("Slice");
    assert(slice && slice->ownership == CoreOwnership::BorrowedView);
    const auto* list = core_type_info("List");
    assert(list && list->scope == CoreTypeScope::Alloc);
    assert(list->allocation == CoreAllocation::AllocatorRequired);
    assert(list->drop == CoreDropPolicy::OwnedStorage);
    assert(core_type_info("HashMap")->scope == CoreTypeScope::Explicit);
    assert(core_type_info("int")->canonical == core_type_info("i64")->canonical);
    assert(protected_core_identity("Option"));
    assert(protected_core_identity("int"));
    assert(!protected_core_identity("HashMap"));
    assert(core_type_info("Option")->lowering == CoreLoweringStatus::NativeScalarSumSubset);
    assert(core_type_info("Result")->lowering == CoreLoweringStatus::NativeScalarSumSubset);
    return 0;
}
