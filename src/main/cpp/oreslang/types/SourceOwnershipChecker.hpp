#pragma once

#include "oreslang/backend/ManagedRefMir.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace oreslang {

namespace backend {
class SourceManagedRefLowering;
}

/*
 * Source-bound actor-private Ref<int> initialization plan. It carries no raw
 * pointer or managed-reference evidence: the lowering friend must match it to
 * the exact source declaration and target before invoking the trusted arena
 * allocation/publication ABI.
 */
class SourceManagedRefInitAuthorization final {
    std::string source_;
    std::string target_triple_;
    std::string actor_name_;
    std::string ref_field_;
    std::size_t declaration_offset_ = 0;
    std::int64_t initial_value_ = 0;

    SourceManagedRefInitAuthorization(
            std::string source,
            std::string target_triple,
            std::string actor_name,
            std::string ref_field,
            std::size_t declaration_offset,
            std::int64_t initial_value)
        : source_(std::move(source)),
          target_triple_(std::move(target_triple)),
          actor_name_(std::move(actor_name)),
          ref_field_(std::move(ref_field)),
          declaration_offset_(declaration_offset),
          initial_value_(initial_value) {}

    friend class SourceOwnershipChecker;
    friend class backend::SourceManagedRefLowering;

public:
    SourceManagedRefInitAuthorization() = delete;
    SourceManagedRefInitAuthorization(const SourceManagedRefInitAuthorization&) = delete;
    SourceManagedRefInitAuthorization& operator=(const SourceManagedRefInitAuthorization&) = delete;
    SourceManagedRefInitAuthorization(SourceManagedRefInitAuthorization&&) noexcept = default;
    SourceManagedRefInitAuthorization& operator=(SourceManagedRefInitAuthorization&&) noexcept = default;

    std::string_view target_triple() const noexcept { return target_triple_; }
    std::string_view actor_name() const noexcept { return actor_name_; }
    std::string_view ref_field() const noexcept { return ref_field_; }
    std::size_t declaration_offset() const noexcept { return declaration_offset_; }
    std::int64_t initial_value() const noexcept { return initial_value_; }
};

/*
 * Site-bound production authorization. Ordinary compiler callers can inspect
 * the derived claims and source identity, but cannot extract/replay the raw
 * ActorRefOwnershipEvidence. The future source LLVM lowering is the only
 * backend friend allowed to consume that inner capability, after matching the
 * authorization to the exact HIR use site.
 */
class SourceManagedRefAuthorization final {
    backend::ActorRefOwnershipEvidence evidence_;
    // Exact source text is retained privately so the future lowering friend
    // can match authorization to the same compilation unit without relying on
    // a collision-prone ad-hoc hash.
    std::string source_;
    std::string target_triple_;
    std::string actor_name_;
    std::string ref_field_;
    std::size_t use_offset_ = 0;

    SourceManagedRefAuthorization(
            backend::ActorRefOwnershipEvidence evidence,
            std::string source,
            std::string target_triple,
            std::string actor_name,
            std::string ref_field,
            std::size_t use_offset)
        : evidence_(std::move(evidence)),
          source_(std::move(source)),
          target_triple_(std::move(target_triple)),
          actor_name_(std::move(actor_name)),
          ref_field_(std::move(ref_field)),
          use_offset_(use_offset) {}

    friend class SourceOwnershipChecker;
    friend class backend::SourceManagedRefLowering;

public:
    SourceManagedRefAuthorization() = delete;
    SourceManagedRefAuthorization(const SourceManagedRefAuthorization&) = delete;
    SourceManagedRefAuthorization& operator=(const SourceManagedRefAuthorization&) = delete;
    SourceManagedRefAuthorization(SourceManagedRefAuthorization&&) noexcept = default;
    SourceManagedRefAuthorization& operator=(SourceManagedRefAuthorization&&) noexcept = default;

    backend::ActorManagedRefMir claims() const noexcept {
        return evidence_.claims();
    }
    std::string_view target_triple() const noexcept { return target_triple_; }
    std::string_view actor_name() const noexcept { return actor_name_; }
    std::string_view ref_field() const noexcept { return ref_field_; }
    std::size_t use_offset() const noexcept { return use_offset_; }
};

/*
 * Sole production source authority for ActorRefOwnershipEvidence.
 *
 * This API intentionally accepts source text, not caller-authored Program/HIR,
 * ownership graphs, MIR claims or booleans. It reparses and revalidates the
 * source, derives the managed-ref use and loan mode from that program, builds
 * the ownership CFG internally, and only then mints backend evidence.
 *
 * The first vertical slice supports actor-private Ref<int> declared in the
 * single sealed state schema and one terminal ref_read/ref_write operation.
 */
class SourceOwnershipChecker final {
public:
    static SourceManagedRefInitAuthorization authorize_actor_managed_ref_init(
        std::string_view source,
        std::string_view actor_name,
        std::string_view ref_field,
        const TargetAbi& target);

    static SourceManagedRefAuthorization authorize_actor_managed_ref(
        std::string_view source,
        std::string_view actor_name,
        std::string_view ref_field,
        const TargetAbi& target);
};

} // namespace oreslang
