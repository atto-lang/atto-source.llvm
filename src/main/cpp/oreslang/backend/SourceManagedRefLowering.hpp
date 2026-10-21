#pragma once

#include "oreslang/backend/ManagedRefMirLowering.hpp"
#include "oreslang/types/SourceOwnershipChecker.hpp"

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "llvm/IR/Argument.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"

namespace oreslang::backend {

class SourceManagedRefRuntimeContext final {
    ActorCompactRuntimeContext compact_;
    llvm::Value* ref_state_ = nullptr;

    SourceManagedRefRuntimeContext(
            ActorCompactRuntimeContext compact, llvm::Value* ref_state)
        : compact_(compact), ref_state_(ref_state) {}

    friend class SourceManagedRefLowering;
public:
    SourceManagedRefRuntimeContext() = delete;
};

class SourceManagedRefLowering final {
    static void require_exact_source(
            std::string_view authorized, std::string_view actual) {
        if (authorized.size() != actual.size() || authorized != actual)
            throw std::invalid_argument(
                "managed Ref authorization belongs to different source text");
    }

    static void require_use_binding(
            const SourceManagedRefAuthorization& authorization,
            std::string_view source,
            const TargetAbi& target,
            std::string_view actor_name,
            std::string_view ref_field,
            std::size_t use_offset) {
        require_exact_source(authorization.source_, source);
        if (std::string_view(authorization.target_triple_) != target.triple() ||
                std::string_view(authorization.actor_name_) != actor_name ||
                std::string_view(authorization.ref_field_) != ref_field ||
                authorization.use_offset_ != use_offset)
            throw std::invalid_argument(
                "managed Ref authorization does not match source use site");
        if (target.pointer_bits() != 64)
            throw std::invalid_argument(
                "managed Ref source lowering requires native64 target");
    }

    static void require_init_binding(
            const SourceManagedRefInitAuthorization& authorization,
            std::string_view source,
            const TargetAbi& target,
            std::string_view actor_name,
            std::string_view ref_field,
            std::size_t declaration_offset) {
        require_exact_source(authorization.source_, source);
        if (std::string_view(authorization.target_triple_) != target.triple() ||
                std::string_view(authorization.actor_name_) != actor_name ||
                std::string_view(authorization.ref_field_) != ref_field ||
                authorization.declaration_offset_ != declaration_offset)
            throw std::invalid_argument(
                "managed Ref initialization does not match source declaration");
        if (target.pointer_bits() != 64)
            throw std::invalid_argument(
                "managed Ref source initialization requires native64 target");
    }

    static void require_storage_index(std::size_t index) {
        if (index >= 16)
            throw std::invalid_argument(
                "managed Ref state slot index exceeds source bound");
    }

    static void initialize_ref_bits(
            llvm::IRBuilder<>& ir,
            const SourceManagedRefRuntimeContext& runtime,
            std::size_t index,
            llvm::Value* bits) {
        require_storage_index(index);
        llvm::Module* module =
            validate_actor_compact_context(ir, runtime.compact_);
        auto* i64 = llvm::Type::getInt64Ty(ir.getContext());
        auto* i32 = llvm::Type::getInt32Ty(ir.getContext());
        if (!bits || bits->getType() != i32)
            throw std::invalid_argument(
                "managed Ref initialization requires i32 compact bits");
        auto* ptr = runtime.compact_.actor_arena->getType();
        const auto callee = actor_checked_runtime_decl(
            module, "ores_actor_managed_ref_state_init_i32",
            llvm::FunctionType::get(
                llvm::Type::getVoidTy(ir.getContext()),
                {ptr, i64, i64, i32}, false));
        ir.CreateCall(
            callee,
            {runtime.ref_state_,
             runtime.compact_.actor_domain,
             llvm::ConstantInt::get(
                 i64, static_cast<std::uint64_t>(index)),
             bits});
    }

    static llvm::Value* load_ref_bits(
            llvm::IRBuilder<>& ir,
            const SourceManagedRefRuntimeContext& runtime,
            std::size_t index) {
        require_storage_index(index);
        llvm::Module* module =
            validate_actor_compact_context(ir, runtime.compact_);
        auto* i64 = llvm::Type::getInt64Ty(ir.getContext());
        auto* i32 = llvm::Type::getInt32Ty(ir.getContext());
        auto* ptr = runtime.compact_.actor_arena->getType();
        const auto callee = actor_checked_runtime_decl(
            module, "ores_actor_managed_ref_state_load_i32",
            llvm::FunctionType::get(
                i32, {ptr, i64, i64}, false));
        return ir.CreateCall(
            callee,
            {runtime.ref_state_,
             runtime.compact_.actor_domain,
             llvm::ConstantInt::get(
                 i64, static_cast<std::uint64_t>(index))},
            "actor.managed.ref.bits");
    }

public:
    /*
     * Bind only compiler-generated managed-actor entrypoint arguments.
     * init and on_message both reserve argument positions:
     *   0 arena*, 1 opaque domain-bound ref-state*, 2 domain i64, 3 epoch i64.
     */
    static SourceManagedRefRuntimeContext bind_actor_context(
            llvm::IRBuilder<>& ir,
            llvm::Value* actor_arena,
            llvm::Value* ref_state,
            llvm::Value* actor_domain,
            llvm::Value* actor_epoch) {
        auto* arena_arg = llvm::dyn_cast_or_null<llvm::Argument>(actor_arena);
        auto* state_arg = llvm::dyn_cast_or_null<llvm::Argument>(ref_state);
        auto* domain_arg = llvm::dyn_cast_or_null<llvm::Argument>(actor_domain);
        auto* epoch_arg = llvm::dyn_cast_or_null<llvm::Argument>(actor_epoch);
        if (!arena_arg || !state_arg || !domain_arg || !epoch_arg ||
                arena_arg->getArgNo() != 0 || state_arg->getArgNo() != 1 ||
                domain_arg->getArgNo() != 2 || epoch_arg->getArgNo() != 3)
            throw std::invalid_argument(
                "managed Ref runtime context must use generated host argument positions");
        llvm::Function* fn = arena_arg->getParent();
        if (!fn || state_arg->getParent() != fn || domain_arg->getParent() != fn ||
                epoch_arg->getParent() != fn || !ir.GetInsertBlock() ||
                ir.GetInsertBlock()->getParent() != fn)
            throw std::invalid_argument(
                "managed Ref runtime context arguments belong to different function");
        auto* i64 = llvm::Type::getInt64Ty(ir.getContext());
        if (!actor_arena->getType()->isPointerTy() ||
                !ref_state->getType()->isPointerTy() ||
                actor_domain->getType() != i64 || actor_epoch->getType() != i64)
            throw std::invalid_argument(
                "managed Ref runtime context has invalid LLVM types");

        ActorCompactRuntimeContext compact{
            actor_arena, actor_domain, actor_epoch
        };
        (void)validate_actor_compact_context(ir, compact);
        return SourceManagedRefRuntimeContext(compact, ref_state);
    }

    static void lower_init(
            llvm::IRBuilder<>& ir,
            SourceManagedRefInitAuthorization authorization,
            std::string_view source,
            const TargetAbi& target,
            std::string_view actor_name,
            std::string_view ref_field,
            std::size_t declaration_offset,
            const SourceManagedRefRuntimeContext& runtime) {
        require_init_binding(
            authorization, source, target, actor_name, ref_field,
            declaration_offset);

        llvm::Module* module =
            validate_actor_compact_context(ir, runtime.compact_);
        auto* i64 = llvm::Type::getInt64Ty(ir.getContext());
        auto* i32 = llvm::Type::getInt32Ty(ir.getContext());
        auto* ptr = runtime.compact_.actor_arena->getType();
        const auto callee = actor_checked_runtime_decl(
            module, "ores_actor_arena_compact_new_i64",
            llvm::FunctionType::get(
                i32, {ptr, i64, i64, i64}, false));
        llvm::Value* bits = ir.CreateCall(
            callee,
            {runtime.compact_.actor_arena,
             runtime.compact_.actor_domain,
             runtime.compact_.actor_epoch,
             llvm::ConstantInt::getSigned(
                 i64, authorization.initial_value_)},
            "actor.managed.ref.initial");
        initialize_ref_bits(
            ir, runtime, authorization.storage_index_, bits);
    }

    static llvm::Value* lower_read(
            llvm::IRBuilder<>& ir,
            SourceManagedRefAuthorization authorization,
            std::string_view source,
            const TargetAbi& target,
            std::string_view actor_name,
            std::string_view ref_field,
            std::size_t use_offset,
            const SourceManagedRefRuntimeContext& runtime) {
        require_use_binding(
            authorization, source, target, actor_name, ref_field, use_offset);
        const auto verified =
            verify_actor_compressed32(authorization.evidence_);
        if (verified.access() != ManagedRefAccess::Read)
            throw std::invalid_argument(
                "managed Ref read requires read source authorization");
        llvm::Value* bits =
            load_ref_bits(ir, runtime, authorization.storage_index_);
        return lower_actor_compact_i64_read(
            ir, verified, runtime.compact_, bits, 0);
    }

    static llvm::Value* lower_write(
            llvm::IRBuilder<>& ir,
            SourceManagedRefAuthorization authorization,
            std::string_view source,
            const TargetAbi& target,
            std::string_view actor_name,
            std::string_view ref_field,
            std::size_t use_offset,
            const SourceManagedRefRuntimeContext& runtime,
            llvm::Value* new_value) {
        require_use_binding(
            authorization, source, target, actor_name, ref_field, use_offset);
        const auto verified =
            verify_actor_compressed32(authorization.evidence_);
        if (verified.access() != ManagedRefAccess::Write)
            throw std::invalid_argument(
                "managed Ref write requires write source authorization");
        llvm::Value* bits =
            load_ref_bits(ir, runtime, authorization.storage_index_);
        lower_actor_compact_i64_write(
            ir, verified, runtime.compact_, bits, 0, new_value);
        return new_value;
    }
};

} // namespace oreslang::backend
