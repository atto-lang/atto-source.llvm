#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oreslang::types {

// Native LLVM compiler registry. This describes source identity and safety
// obligations; it does not claim that every entry has native lowering.
enum class CoreTypeScope : std::uint8_t { Core, Alloc, Explicit };
enum class CoreTypeKind : std::uint8_t { Type, Constructor, Trait };
enum class CoreOwnership : std::uint8_t { Copy, PayloadDependent, BorrowedView, Owned, Trait };
enum class CoreAllocation : std::uint8_t { None, BorrowedView, AllocatorRequired };
enum class CoreDropPolicy : std::uint8_t {
    None, ActivePayloadOnce, AllInitializedMembers, OwnedStorage, NotAValue
};
enum class CoreSendPolicy : std::uint8_t {
    Always, PayloadDependent, NeverBorrowed, ProofRequired, NotApplicable
};
enum class CoreLoweringStatus : std::uint8_t {
    NativeScalar, NativeScalarSumSubset, ContractOnly, LegacyStringView
};

struct CoreTypeInfo {
    std::string_view name;
    std::string_view canonical;
    CoreTypeScope scope;
    CoreTypeKind kind;
    std::uint8_t min_arity;
    std::uint8_t max_arity;
    CoreOwnership ownership;
    CoreAllocation allocation;
    CoreDropPolicy drop;
    CoreSendPolicy send;
    CoreLoweringStatus lowering;

    constexpr bool accepts_arity(std::size_t arity) const noexcept {
        return arity >= min_arity && arity <= max_arity;
    }
};

inline constexpr CoreTypeInfo kCoreTypeRegistry[] = {
    {"int","core/primitive/i64",CoreTypeScope::Core,CoreTypeKind::Type,0,0,CoreOwnership::Copy,CoreAllocation::None,CoreDropPolicy::None,CoreSendPolicy::Always,CoreLoweringStatus::NativeScalar},
    {"Int","core/primitive/i64",CoreTypeScope::Core,CoreTypeKind::Type,0,0,CoreOwnership::Copy,CoreAllocation::None,CoreDropPolicy::None,CoreSendPolicy::Always,CoreLoweringStatus::ContractOnly},
    {"i64","core/primitive/i64",CoreTypeScope::Core,CoreTypeKind::Type,0,0,CoreOwnership::Copy,CoreAllocation::None,CoreDropPolicy::None,CoreSendPolicy::Always,CoreLoweringStatus::NativeScalar},
    {"bool","core/primitive/bool",CoreTypeScope::Core,CoreTypeKind::Type,0,0,CoreOwnership::Copy,CoreAllocation::None,CoreDropPolicy::None,CoreSendPolicy::Always,CoreLoweringStatus::NativeScalar},
    {"boolean","core/primitive/bool",CoreTypeScope::Core,CoreTypeKind::Type,0,0,CoreOwnership::Copy,CoreAllocation::None,CoreDropPolicy::None,CoreSendPolicy::Always,CoreLoweringStatus::NativeScalar},
    {"Bool","core/primitive/bool",CoreTypeScope::Core,CoreTypeKind::Type,0,0,CoreOwnership::Copy,CoreAllocation::None,CoreDropPolicy::None,CoreSendPolicy::Always,CoreLoweringStatus::ContractOnly},
    {"string","core/primitive/string-view",CoreTypeScope::Core,CoreTypeKind::Type,0,0,CoreOwnership::BorrowedView,CoreAllocation::BorrowedView,CoreDropPolicy::None,CoreSendPolicy::NeverBorrowed,CoreLoweringStatus::LegacyStringView},
    {"Option","core/option/Option",CoreTypeScope::Core,CoreTypeKind::Type,1,1,CoreOwnership::PayloadDependent,CoreAllocation::None,CoreDropPolicy::ActivePayloadOnce,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::NativeScalarSumSubset},
    {"Some","core/option/Some",CoreTypeScope::Core,CoreTypeKind::Constructor,1,1,CoreOwnership::PayloadDependent,CoreAllocation::None,CoreDropPolicy::ActivePayloadOnce,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::NativeScalarSumSubset},
    {"None","core/option/None",CoreTypeScope::Core,CoreTypeKind::Constructor,0,0,CoreOwnership::PayloadDependent,CoreAllocation::None,CoreDropPolicy::None,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::NativeScalarSumSubset},
    {"Result","core/result/Result",CoreTypeScope::Core,CoreTypeKind::Type,2,2,CoreOwnership::PayloadDependent,CoreAllocation::None,CoreDropPolicy::ActivePayloadOnce,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::NativeScalarSumSubset},
    {"Ok","core/result/Ok",CoreTypeScope::Core,CoreTypeKind::Constructor,1,1,CoreOwnership::PayloadDependent,CoreAllocation::None,CoreDropPolicy::ActivePayloadOnce,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::NativeScalarSumSubset},
    {"Err","core/result/Err",CoreTypeScope::Core,CoreTypeKind::Constructor,1,1,CoreOwnership::PayloadDependent,CoreAllocation::None,CoreDropPolicy::ActivePayloadOnce,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::NativeScalarSumSubset},
    {"Error","core/error/Error",CoreTypeScope::Core,CoreTypeKind::Type,0,0,CoreOwnership::Owned,CoreAllocation::None,CoreDropPolicy::OwnedStorage,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"Array","core/array/Array",CoreTypeScope::Core,CoreTypeKind::Type,1,2,CoreOwnership::PayloadDependent,CoreAllocation::None,CoreDropPolicy::AllInitializedMembers,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"FixedArray","core/array/FixedArray",CoreTypeScope::Core,CoreTypeKind::Type,2,2,CoreOwnership::PayloadDependent,CoreAllocation::None,CoreDropPolicy::AllInitializedMembers,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"Slice","core/slice/Slice",CoreTypeScope::Core,CoreTypeKind::Type,1,1,CoreOwnership::BorrowedView,CoreAllocation::BorrowedView,CoreDropPolicy::None,CoreSendPolicy::NeverBorrowed,CoreLoweringStatus::ContractOnly},
    {"Tuple","core/tuple/Tuple",CoreTypeScope::Core,CoreTypeKind::Type,0,32,CoreOwnership::PayloadDependent,CoreAllocation::None,CoreDropPolicy::AllInitializedMembers,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"Pair","core/tuple/Pair",CoreTypeScope::Core,CoreTypeKind::Type,2,2,CoreOwnership::PayloadDependent,CoreAllocation::None,CoreDropPolicy::AllInitializedMembers,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"Iterator","core/iter/Iterator",CoreTypeScope::Core,CoreTypeKind::Trait,1,1,CoreOwnership::Trait,CoreAllocation::None,CoreDropPolicy::NotAValue,CoreSendPolicy::NotApplicable,CoreLoweringStatus::ContractOnly},
    {"Range","core/range/Range",CoreTypeScope::Core,CoreTypeKind::Type,1,1,CoreOwnership::PayloadDependent,CoreAllocation::None,CoreDropPolicy::None,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"Eq","core/cmp/Eq",CoreTypeScope::Core,CoreTypeKind::Trait,0,0,CoreOwnership::Trait,CoreAllocation::None,CoreDropPolicy::NotAValue,CoreSendPolicy::NotApplicable,CoreLoweringStatus::ContractOnly},
    {"Ord","core/cmp/Ord",CoreTypeScope::Core,CoreTypeKind::Trait,0,0,CoreOwnership::Trait,CoreAllocation::None,CoreDropPolicy::NotAValue,CoreSendPolicy::NotApplicable,CoreLoweringStatus::ContractOnly},
    {"Ordering","core/cmp/Ordering",CoreTypeScope::Core,CoreTypeKind::Type,0,0,CoreOwnership::Copy,CoreAllocation::None,CoreDropPolicy::None,CoreSendPolicy::Always,CoreLoweringStatus::ContractOnly},
    {"From","core/convert/From",CoreTypeScope::Core,CoreTypeKind::Trait,1,1,CoreOwnership::Trait,CoreAllocation::None,CoreDropPolicy::NotAValue,CoreSendPolicy::NotApplicable,CoreLoweringStatus::ContractOnly},
    {"Into","core/convert/Into",CoreTypeScope::Core,CoreTypeKind::Trait,1,1,CoreOwnership::Trait,CoreAllocation::None,CoreDropPolicy::NotAValue,CoreSendPolicy::NotApplicable,CoreLoweringStatus::ContractOnly},
    {"TryFrom","core/convert/TryFrom",CoreTypeScope::Core,CoreTypeKind::Trait,1,1,CoreOwnership::Trait,CoreAllocation::None,CoreDropPolicy::NotAValue,CoreSendPolicy::NotApplicable,CoreLoweringStatus::ContractOnly},
    {"TryInto","core/convert/TryInto",CoreTypeScope::Core,CoreTypeKind::Trait,1,1,CoreOwnership::Trait,CoreAllocation::None,CoreDropPolicy::NotAValue,CoreSendPolicy::NotApplicable,CoreLoweringStatus::ContractOnly},
    {"Display","core/fmt/Display",CoreTypeScope::Core,CoreTypeKind::Trait,0,0,CoreOwnership::Trait,CoreAllocation::None,CoreDropPolicy::NotAValue,CoreSendPolicy::NotApplicable,CoreLoweringStatus::ContractOnly},
    {"Debug","core/fmt/Debug",CoreTypeScope::Core,CoreTypeKind::Trait,0,0,CoreOwnership::Trait,CoreAllocation::None,CoreDropPolicy::NotAValue,CoreSendPolicy::NotApplicable,CoreLoweringStatus::ContractOnly},
    {"Sendable","core/marker/Sendable",CoreTypeScope::Core,CoreTypeKind::Trait,0,0,CoreOwnership::Trait,CoreAllocation::None,CoreDropPolicy::NotAValue,CoreSendPolicy::NotApplicable,CoreLoweringStatus::ContractOnly},
    {"String","alloc/string/String",CoreTypeScope::Alloc,CoreTypeKind::Type,0,0,CoreOwnership::Owned,CoreAllocation::AllocatorRequired,CoreDropPolicy::OwnedStorage,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"List","alloc/list/List",CoreTypeScope::Alloc,CoreTypeKind::Type,1,1,CoreOwnership::Owned,CoreAllocation::AllocatorRequired,CoreDropPolicy::OwnedStorage,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"Vec","alloc/vec/Vec",CoreTypeScope::Alloc,CoreTypeKind::Type,1,1,CoreOwnership::Owned,CoreAllocation::AllocatorRequired,CoreDropPolicy::OwnedStorage,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"Map","alloc/map/Map",CoreTypeScope::Alloc,CoreTypeKind::Type,2,2,CoreOwnership::Owned,CoreAllocation::AllocatorRequired,CoreDropPolicy::OwnedStorage,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"Set","alloc/set/Set",CoreTypeScope::Alloc,CoreTypeKind::Type,1,1,CoreOwnership::Owned,CoreAllocation::AllocatorRequired,CoreDropPolicy::OwnedStorage,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"Box","alloc/box/Box",CoreTypeScope::Alloc,CoreTypeKind::Type,1,1,CoreOwnership::Owned,CoreAllocation::AllocatorRequired,CoreDropPolicy::OwnedStorage,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"HashMap","alloc/hash_map/HashMap",CoreTypeScope::Explicit,CoreTypeKind::Type,2,2,CoreOwnership::Owned,CoreAllocation::AllocatorRequired,CoreDropPolicy::OwnedStorage,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"HashSet","alloc/hash_set/HashSet",CoreTypeScope::Explicit,CoreTypeKind::Type,1,1,CoreOwnership::Owned,CoreAllocation::AllocatorRequired,CoreDropPolicy::OwnedStorage,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly},
    {"Entry","alloc/map/Entry",CoreTypeScope::Explicit,CoreTypeKind::Type,2,2,CoreOwnership::PayloadDependent,CoreAllocation::None,CoreDropPolicy::AllInitializedMembers,CoreSendPolicy::PayloadDependent,CoreLoweringStatus::ContractOnly}
};

inline const CoreTypeInfo* core_type_info(std::string_view name) noexcept {
    for (const auto& entry : kCoreTypeRegistry)
        if (entry.name == name) return &entry;
    return nullptr;
}

inline bool core_sum_carrier(std::string_view name, std::size_t arity) noexcept {
    const auto* entry = core_type_info(name);
    return entry && entry->scope == CoreTypeScope::Core
        && entry->kind == CoreTypeKind::Type
        && entry->ownership == CoreOwnership::PayloadDependent
        && entry->accepts_arity(arity)
        && (name == "Option" || name == "Result");
}

// Prelude names are auto-imported, not reserved words. Only the ADT
// constructors and the core carrier/collection/loan identities are protected;
// other catalogued names (Box, Error, Pair, Iterator, ...) may be declared by
// user code and shadow the prelude, as in Rust.
inline bool protected_core_identity(std::string_view name) noexcept {
    // The compatibility spelling is contextual; existing value names remain valid.
    if (name == "boolean") return false;
    const auto* entry = core_type_info(name);
    if (entry && entry->kind == CoreTypeKind::Constructor) return true;
    if (entry && entry->canonical.rfind("core/primitive/", 0) == 0) return true;
    return name == "Option" || name == "Result" || name == "Array" ||
        name == "FixedArray" || name == "List" || name == "FixedList" ||
        name == "Vector" || name == "Slice" || name == "Tuple" ||
        name == "Map" || name == "Set" || name == "ReadLoan" || name == "WriteLoan";
}

} // namespace oreslang::types
