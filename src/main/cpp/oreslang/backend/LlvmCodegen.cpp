#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/backend/NativeFlatClasses.hpp"
#include "oreslang/types/NativeSumTypes.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/frontend/NativeScalarSpecialization.hpp"
#include "oreslang/runtime/TargetAbi.hpp"
#include "oreslang/runtime/OresIsolateAbi.h"
#include "oreslang/runtime/SourceIdentityV1.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "oreslang/backend/LlvmCompat.hpp"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#if LLVM_VERSION_MAJOR >= 21
#include "llvm/TargetParser/Triple.h"
#endif

namespace oreslang {

// Guest identifiers exclude $, so native selector suffixes cannot alias source names.
namespace {

std::string field_key(const std::string& receiver, const std::string& field) {
    return receiver + "." + field;
}

// Owned i64 parameters receive stack slots so the only permitted loans can
// pass their addresses through the call ABI. Borrowed inputs already hold
// caller-owned slot pointers: passing them to nested borrowed parameters is
// a checked, call-scoped reborrow with no new allocation. Neither address
// can escape as a result:
// native functions return only i64, and the ownership pass restricts rt borrow
// to nonescaping call arguments.
struct ActorRegionFieldLowering {
    llvm::Value* owner_offset_slot = nullptr;
    std::uint32_t byte_offset = 0;
};

struct RegionMoveLowering {
    llvm::Value* region = nullptr;
    llvm::FunctionCallee read;
    llvm::FunctionCallee drop;
    const std::unordered_map<std::string, llvm::Value*>* live_flags = nullptr;
    llvm::Value* array_region = nullptr;
    llvm::FunctionCallee array_add, array_get, array_set, array_length;
    const std::unordered_set<std::string>* array_names = nullptr;
    const std::unordered_set<std::string>* array_fields = nullptr;
    llvm::BasicBlock* resource_failure = nullptr;
    // Compiler-owned error tag survives every owner-drop and region close.
    // Values exactly match OresNativeIntArrayStatus; not a guest Result yet.
    llvm::Value* array_failure_status = nullptr;
    const std::unordered_map<std::string, llvm::Value*>* owned_failure_handles = nullptr;
    const std::unordered_map<std::string, const ClassDecl*>* record_classes = nullptr;
    const std::unordered_map<std::string, std::uint64_t>* record_schemas = nullptr;
    llvm::FunctionCallee record_new, record_get, record_check;
    const std::unordered_map<std::string, llvm::Value*>* class_array_regions = nullptr;
    const std::unordered_map<std::string, std::vector<std::size_t>>* class_array_parameters = nullptr;
    const std::unordered_map<std::string, bool>* owning_results = nullptr;
    llvm::Value* result_destination = nullptr;
    llvm::Value* result_region = nullptr;
    const NativeClassLayouts* layouts = nullptr;
    const Program* sum_program = nullptr;
    llvm::FunctionCallee sum_check;
    const std::unordered_map<std::string, ActorRegionFieldLowering>* actor_region_fields = nullptr;
    llvm::FunctionCallee actor_region_read_i64;
    // Compiler-frozen scalar direct targets, never a runtime selector table.
    const NativeI64DispatchImage* frozen_scalar_dispatch = nullptr;
    const std::unordered_map<std::string, llvm::Function*>* frozen_scalar_callees = nullptr;
    const std::unordered_map<std::string, std::string>* frozen_scalar_symbols = nullptr;
};

llvm::Value* receiver_array_region(const Expression& receiver, const RegionMoveLowering* moves) {
    if (receiver.kind == Expression::Kind::Field && receiver.left &&
            moves->class_array_regions)
        return moves->class_array_regions->at(native_place_key(*receiver.left));
    return moves->array_region;
}

// Only a lexical named array or a checked descendant array place is legal.
// The type/ownership pass must already have certified this receiver.
llvm::Value* array_handle_address(const Expression& receiver,
                                   const std::unordered_map<std::string, llvm::Value*>& storage,
                                   const std::unordered_map<std::string, llvm::Value*>& fields,
                                   const RegionMoveLowering* moves) {
    if (!moves) throw std::runtime_error("native Array<int> operation lacks region");
    if (receiver.kind == Expression::Kind::Variable && moves->array_names &&
            moves->array_names->count(receiver.name))
        return storage.at(receiver.name);
    if (receiver.kind == Expression::Kind::Field && receiver.left &&
            moves->array_fields) {
        const std::string key = native_place_key(receiver);
        if (moves->array_fields->count(key)) return fields.at(key);
    }
    throw std::runtime_error("native Array<int> receiver is not an owned place");
}

llvm::Value* checked_sum_tag(llvm::IRBuilder<>& builder, llvm::Type* i64, llvm::Value* handle,
                            std::uint64_t schema, std::size_t fields, llvm::FunctionCallee check, llvm::FunctionCallee get) {
    auto* function = builder.GetInsertBlock()->getParent();
    auto* schema_ok = llvm::BasicBlock::Create(builder.getContext(), "sum.schema.valid", function);
    auto* tag_ok = llvm::BasicBlock::Create(builder.getContext(), "sum.tag.valid", function);
    auto* invalid = llvm::BasicBlock::Create(builder.getContext(), "sum.invalid", function);
    auto* status = builder.CreateCall(check, {handle, llvm::ConstantInt::get(i64, schema), llvm::ConstantInt::get(i64, fields)});
    builder.CreateCondBr(builder.CreateICmpEQ(status, llvm::ConstantInt::get(i64, 0)), schema_ok, invalid);
    builder.SetInsertPoint(invalid);
    auto panic = function->getParent()->getOrInsertFunction("abort", llvm::FunctionType::get(builder.getVoidTy(), false));
    builder.CreateCall(panic); builder.CreateUnreachable();
    builder.SetInsertPoint(schema_ok);
    auto* tag = builder.CreateCall(get, {handle, llvm::ConstantInt::get(i64, schema), llvm::ConstantInt::get(i64, 0)});
    builder.CreateCondBr(builder.CreateICmpULE(tag, llvm::ConstantInt::get(i64, 1)), tag_ok, invalid);
    builder.SetInsertPoint(tag_ok); return tag;
}

llvm::AllocaInst* entry_alloca(llvm::IRBuilder<>& builder, llvm::Type* type, llvm::Value* count = nullptr, const llvm::Twine& name = "") {
    auto& entry = builder.GetInsertBlock()->getParent()->getEntryBlock();
    llvm::IRBuilder<> slots(&entry, entry.begin());
    return slots.CreateAlloca(type, count, name);
}

llvm::Value* checked_signed_arithmetic(llvm::IRBuilder<>& builder,
                                      llvm::Intrinsic::ID operation,
                                      llvm::Value* left, llvm::Value* right,
                                      const std::string& name) {
    // Source int follows the shared no-wrapping i64 contract. Use defined
    // overflow intrinsics, never poison-producing nsw arithmetic. A target
    // trap is the current failure ABI; typed actor errors remain separate.
    auto* module = builder.GetInsertBlock()->getModule();
    auto* parent = builder.GetInsertBlock()->getParent();
    auto* intrinsic = native_intrinsic(*module, operation, {left->getType()});
    auto* pair = builder.CreateCall(intrinsic, {left, right}, name + ".checked");
    auto* value = builder.CreateExtractValue(pair, {0}, name + ".value");
    auto* overflow = builder.CreateExtractValue(pair, {1}, name + ".overflow");
    auto* failed = llvm::BasicBlock::Create(builder.getContext(), name + ".trap", parent);
    auto* proceed = llvm::BasicBlock::Create(builder.getContext(), name + ".ok", parent);
    builder.CreateCondBr(overflow, failed, proceed);
    builder.SetInsertPoint(failed);
    builder.CreateCall(native_trap(*module));
    builder.CreateUnreachable();
    builder.SetInsertPoint(proceed);
    return value;
}

llvm::Value* lower(const Expression& expr,
                   llvm::IRBuilder<>& builder,
                   llvm::Type* i64,
                   const std::unordered_map<std::string, llvm::Value*>& storage,
                   const std::unordered_map<std::string, llvm::Value*>& field_storage,
                   const std::unordered_map<std::string, llvm::Function*>& functions,
                   const RegionMoveLowering* moves = nullptr) {
    switch (expr.kind) {
        case Expression::Kind::SumConstruct: {
            // Independent backend gate: a trap-origin Some may only wrap the
            // statically total integer subset. Never accidentally catch host,
            // ordinary guest, panic or cancellation errors by fabricating Some.
            if (expr.trap_origin && (expr.name != "Some" ||
                    expr.arguments.size() != 1 ||
                    !native_trap_pure_integer(*expr.arguments.front()) ||
                    expr.sum_type != "Option<int>"))
                throw std::runtime_error("unproved native trap AST reached LLVM codegen");
            const auto& type = native_sum(*moves->sum_program, expr.sum_type);
            const bool option = type.kind == NativeSumType::Kind::Option;
            const std::size_t count = option ? 2 : 3;
            const bool error = expr.name == "Err";
            const auto tag = option ? (expr.name == "Some" ? 1 : 0) : (error ? 1 : 0);
            auto* payload = expr.arguments.empty() ? llvm::ConstantInt::get(i64, 0)
                : lower(*expr.arguments[0], builder, i64, storage, field_storage, functions, moves);
            auto* values = entry_alloca(builder, i64, llvm::ConstantInt::get(i64, count), "sum.fields");
            for (std::size_t i = 0; i < count; ++i) builder.CreateStore(llvm::ConstantInt::get(i64, 0),
                builder.CreateGEP(i64, values, llvm::ConstantInt::get(i64, i)));
            builder.CreateStore(llvm::ConstantInt::get(i64, tag), values);
            if (!expr.arguments.empty()) builder.CreateStore(payload, builder.CreateGEP(i64, values, llvm::ConstantInt::get(i64, error ? 2 : 1)));
            return builder.CreateCall(moves->record_new, {llvm::ConstantInt::get(i64, moves->record_schemas->at(expr.sum_type)),
                llvm::ConstantInt::get(i64, count), values}, "sum.value");
        }
        case Expression::Kind::SumAccess: {
            const auto& type = native_sum(*moves->sum_program, expr.sum_type);
            auto* handle = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            const bool expect = expr.name == "expect";
            llvm::Value* fallback = expr.arguments.empty() || expect ? nullptr : lower(*expr.arguments[0], builder, i64, storage, field_storage, functions, moves);
            const auto schema = moves->record_schemas->at(expr.sum_type);
            auto* tag = checked_sum_tag(builder, i64, handle, schema, type.kind == NativeSumType::Kind::Option ? 2 : 3, moves->sum_check, moves->record_get);
            const bool option = type.kind == NativeSumType::Kind::Option;
            if (expr.name == "is_some" || expr.name == "is_none" || expr.name == "is_ok" || expr.name == "is_err") {
                const auto selected = expr.name == "is_some" || expr.name == "is_err" ? 1 : 0;
                return builder.CreateZExt(builder.CreateICmpEQ(tag, llvm::ConstantInt::get(i64, selected)), i64);
            }
            const auto selected = option ? 1 : 0;
            auto* function = builder.GetInsertBlock()->getParent();
            auto* present = llvm::BasicBlock::Create(builder.getContext(), "sum.payload.present", function);
            auto* absent = llvm::BasicBlock::Create(builder.getContext(), "sum.payload.absent", function);
            builder.CreateCondBr(builder.CreateICmpEQ(tag, llvm::ConstantInt::get(i64, selected)), present, absent);
            builder.SetInsertPoint(present);
            auto* value = builder.CreateCall(moves->record_get, {handle, llvm::ConstantInt::get(i64, schema), llvm::ConstantInt::get(i64, 1)});
            if (!fallback) {
                builder.SetInsertPoint(absent);
                auto flush = function->getParent()->getOrInsertFunction("ores_string_panic_flush",
                    llvm::FunctionType::get(builder.getVoidTy(), false));
                builder.CreateCall(flush);
                if (expect) {
                    // Literal message (enforced by the sum resolver) to stderr, then the
                    // same abort path as unwrap. Java appends ": <payload>"; this slice does not.
                    const std::string message = expr.arguments[0]->name + "\n";
                    auto* text = builder.CreateGlobalStringPtr(message, "expect.message");
                    auto write = function->getParent()->getOrInsertFunction("write", llvm::FunctionType::get(
                        i64, {builder.getInt32Ty(), builder.getPtrTy(), i64}, false));
                    builder.CreateCall(write, {builder.getInt32(2), text, llvm::ConstantInt::get(i64, message.size())});
                }
                auto panic = function->getParent()->getOrInsertFunction("abort", llvm::FunctionType::get(builder.getVoidTy(), false));
                builder.CreateCall(panic); builder.CreateUnreachable(); builder.SetInsertPoint(present); return value;
            }
            auto* merge = llvm::BasicBlock::Create(builder.getContext(), "sum.fallback.merge", function);
            builder.CreateBr(merge); builder.SetInsertPoint(absent); builder.CreateBr(merge);
            builder.SetInsertPoint(merge); auto* result = builder.CreatePHI(i64, 2);
            result->addIncoming(value, present); result->addIncoming(fallback, absent); return result;
        }
        case Expression::Kind::RecordConstruct: {
            if (!moves || !moves->record_classes || !moves->record_schemas)
                throw std::runtime_error("record construction lacks typed root region");
            const auto& cls = *moves->record_classes->at(expr.name);
            if (moves->layouts->at(expr.name).owns_arrays) throw std::runtime_error("owning constructor requires destination ownership");
            std::unordered_map<std::string, llvm::Value*> parameters;
            for (std::size_t i = 0; i < expr.arguments.size(); ++i) {
                const auto* original = constructor_field(moves->layouts->source(expr.name), i);
                if (original && !original->class_name.empty()) {
                    const auto& argument = *expr.arguments[i];
                    const auto& child = argument.kind == Expression::Kind::Take ? *argument.left : argument;
                    llvm::Value* record = nullptr;
                    if (child.kind != Expression::Kind::Variable) {
                        if (moves->layouts->at(original->class_name).owns_arrays) throw std::runtime_error("nested immutable record contains owned arrays");
                        record = lower(child, builder, i64, storage, field_storage, functions, moves);
                        builder.CreateCall(moves->record_check, {record, llvm::ConstantInt::get(i64, moves->record_schemas->at(original->class_name))});
                    }
                    std::size_t field_index = 0;
                    for (const auto& leaf : moves->layouts->at(original->class_name).leaves) {
                        auto* snapshot = entry_alloca(builder, i64, nullptr, "nested.record.argument");
                        llvm::Value* value = record ? static_cast<llvm::Value*>(builder.CreateCall(moves->record_get,
                            {record, llvm::ConstantInt::get(i64, moves->record_schemas->at(original->class_name)), llvm::ConstantInt::get(i64, field_index)}))
                            : builder.CreateLoad(i64, field_storage.at(field_key(child.name, leaf.path)));
                        ++field_index;
                        builder.CreateStore(value, snapshot);
                        parameters.emplace(field_key(original->name, leaf.path), snapshot);
                    }
                    continue;
                }
                auto* value = lower(*expr.arguments[i], builder, i64, storage, field_storage, functions, moves);
                auto* slot = entry_alloca(builder, i64, nullptr, "record.argument");
                builder.CreateStore(value, slot);
                parameters.emplace(cls.constructor_parameters.at(i), slot);
            }
            auto* values = entry_alloca(builder, i64, llvm::ConstantInt::get(i64, cls.fields.size()), "record.fields");
            const std::unordered_map<std::string, llvm::Value*> no_fields;
            for (std::size_t i = 0; i < cls.fields.size(); ++i) {
                auto* value = lower(*cls.fields[i].initializer_expression, builder, i64, parameters, no_fields, functions, moves);
                builder.CreateStore(value, builder.CreateGEP(i64, values, llvm::ConstantInt::get(i64, i)));
            }
            return builder.CreateCall(moves->record_new, {
                llvm::ConstantInt::get(i64, moves->record_schemas->at(cls.name)),
                llvm::ConstantInt::get(i64, cls.fields.size()), values}, "record.handle");
        }
        case Expression::Kind::String: {
            auto* module = builder.GetInsertBlock()->getModule();
            auto* bytes = builder.CreateGlobalStringPtr(expr.name, "string.literal");
            auto runtime = module->getOrInsertFunction("ores_string_literal",
                llvm::FunctionType::get(i64, {bytes->getType(), i64}, false));
            return builder.CreateCall(runtime, {bytes, llvm::ConstantInt::get(i64, expr.name.size())}, "string.handle");
        }
        case Expression::Kind::Console: {
            auto* value = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            auto* module = builder.GetInsertBlock()->getModule();
            if (expr.resolved_type == ScalarType::I64) {
                // Decimal text, matching Java's rendering of an int.
                auto format = module->getOrInsertFunction("ores_string_from_i64", llvm::FunctionType::get(i64, {i64}, false));
                value = builder.CreateCall(format, {value}, "console.int.text");
            } else if (expr.resolved_type == ScalarType::Bool) {
                auto format = module->getOrInsertFunction("ores_string_from_bool", llvm::FunctionType::get(i64, {i64}, false));
                value = builder.CreateCall(format, {value}, "console.bool.text");
            }
            auto runtime = builder.GetInsertBlock()->getModule()->getOrInsertFunction("ores_string_print",
                llvm::FunctionType::get(i64, {i64, i64}, false));
            return builder.CreateCall(runtime, {value, llvm::ConstantInt::get(i64, expr.name == "println")}, "console.status");
        }
        case Expression::Kind::Integer:
            return llvm::ConstantInt::getSigned(i64, expr.integer);
        case Expression::Kind::Boolean:
            return llvm::ConstantInt::get(i64, expr.boolean ? 1 : 0);
        case Expression::Kind::Variable: {
            if (!expr.resolved_class.empty()) {
                if (!moves || !moves->record_classes || !moves->record_schemas)
                    throw std::runtime_error("record value lacks root-region provenance");
                const auto& cls = *moves->record_classes->at(expr.resolved_class);
                if (moves->layouts->at(expr.resolved_class).owns_arrays) throw std::runtime_error("owning aggregate value requires destination transfer");
                auto* values = entry_alloca(builder, i64, llvm::ConstantInt::get(i64, cls.fields.size()), "return.record.fields");
                for (std::size_t i = 0; i < cls.fields.size(); ++i) {
                    auto* value = builder.CreateLoad(i64, field_storage.at(field_key(expr.name, cls.fields[i].name)));
                    builder.CreateStore(value, builder.CreateGEP(i64, values, llvm::ConstantInt::get(i64, i)));
                }
                return builder.CreateCall(moves->record_new, {
                    llvm::ConstantInt::get(i64, moves->record_schemas->at(cls.name)),
                    llvm::ConstantInt::get(i64, cls.fields.size()), values}, "return.record.handle");
            }
            llvm::Value* stored = builder.CreateLoad(i64, storage.at(expr.name),
                expr.name + ".read");
            if (moves && moves->live_flags &&
                    moves->live_flags->count(expr.name))
                return builder.CreateCall(moves->read, {moves->region, stored},
                    expr.name + ".owned.value");
            return stored;
        }
        case Expression::Kind::Field:
            if (moves && moves->actor_region_fields) {
                const auto found = moves->actor_region_fields->find(native_place_key(expr));
                if (found != moves->actor_region_fields->end()) {
                    llvm::Value* owner_offset = builder.CreateLoad(
                        llvm::Type::getInt32Ty(builder.getContext()),
                        found->second.owner_offset_slot, "actor.region.offset");
                    return builder.CreateCall(moves->actor_region_read_i64,
                        {owner_offset, llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(builder.getContext()),
                            found->second.byte_offset)},
                        expr.left->name + "." + expr.name + ".actor.region.read");
                }
            }
            if (expr.name == "size" && expr.left &&
                    expr.left->kind == Expression::Kind::Field) {
                llvm::Value* handle = builder.CreateLoad(i64,
                    array_handle_address(*expr.left, storage, field_storage, moves),
                    "class.array.size.handle");
                return builder.CreateCall(moves->array_length,
                    {receiver_array_region(*expr.left, moves), handle}, "class.array.size");
            }
            if (moves && moves->array_names && expr.left &&
                    expr.left->kind == Expression::Kind::Variable &&
                    moves->array_names->count(expr.left->name)) {
                if (expr.name != "size")
                    throw std::runtime_error("native Array<int> supports only size field");
                llvm::Value* handle = builder.CreateLoad(i64,
                    storage.at(expr.left->name), expr.left->name + ".array.handle");
                return builder.CreateCall(moves->array_length,
                    {moves->array_region, handle}, expr.left->name + ".size");
            }
            return builder.CreateLoad(i64, field_storage.at(native_place_key(expr)),
                                      expr.left->name + "." + expr.name + ".read");
        case Expression::Kind::ArrayCall: {
            if (expr.name == "contains_literal") {
                if (!expr.left || expr.arguments.size() != 1)
                    throw std::runtime_error("invalid checked String.contains_literal lowering");
                auto* text = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
                auto* needle = lower(*expr.arguments.front(), builder, i64, storage, field_storage, functions, moves);
                auto runtime = builder.GetInsertBlock()->getModule()->getOrInsertFunction(
                    "ores_string_contains_literal",
                    llvm::FunctionType::get(i64, {i64, i64}, false));
                return builder.CreateCall(runtime, {text, needle}, "string.contains.literal");
            }
            if (!expr.left)
                throw std::runtime_error("native Array<int> call has no owned receiver");
            llvm::Value* handle = builder.CreateLoad(i64,
                array_handle_address(*expr.left, storage, field_storage, moves),
                "owned.array.call.handle");
            llvm::Value* selected_region = receiver_array_region(*expr.left, moves);
            std::vector<llvm::Value*> args;
            for (const auto& arg : expr.arguments)
                args.push_back(lower(*arg, builder, i64, storage, field_storage, functions, moves));
            if (expr.name == "get" && args.size() == 1)
                return builder.CreateCall(moves->array_get,
                    {selected_region, handle, args[0]}, "array.get");
            if (expr.name == "get_copy" && args.size() == 1) {
                // Native scalar copy is checked before pointer arithmetic.
                // The C ABI validates region/handle authority even on None,
                // and writes the payload only for a present element.
                if (!moves || !moves->sum_program ||
                        expr.sum_type != "Option<int>" ||
                        native_sum(*moves->sum_program, expr.sum_type).value_type != ScalarType::I64)
                    throw std::runtime_error("unproven native Array.get_copy Option<int>");
                auto* payload_slot = entry_alloca(builder, i64,
                    llvm::ConstantInt::get(i64, 1), "array.copy.payload");
                builder.CreateStore(llvm::ConstantInt::get(i64, 0), payload_slot);
                auto* module = builder.GetInsertBlock()->getModule();
                auto checked_get = module->getOrInsertFunction(
                    "oreslang_int_array_try_get_copy",
                    llvm::FunctionType::get(builder.getInt32Ty(),
                        {selected_region->getType(), i64, i64, payload_slot->getType()}, false));
                llvm::Value* present = builder.CreateCall(checked_get,
                    {selected_region, handle, args[0], payload_slot}, "array.copy.present");
                auto* fields = entry_alloca(builder, i64,
                    llvm::ConstantInt::get(i64, 2), "array.copy.option.fields");
                builder.CreateStore(builder.CreateZExt(present, i64), fields);
                builder.CreateStore(builder.CreateLoad(i64, payload_slot),
                    builder.CreateGEP(i64, fields, llvm::ConstantInt::get(i64, 1)));
                return builder.CreateCall(moves->record_new,
                    {llvm::ConstantInt::get(i64, moves->record_schemas->at(expr.sum_type)),
                     llvm::ConstantInt::get(i64, 2), fields}, "array.get_copy.option");
            }
            if (expr.name == "add" && args.size() == 1) {
                llvm::Value* status = builder.CreateCall(moves->array_add,
                    {selected_region, handle, args[0]}, "array.add.status");
                llvm::BasicBlock* proceed = llvm::BasicBlock::Create(
                    builder.getContext(), "array.add.ok", builder.GetInsertBlock()->getParent());
                llvm::BasicBlock* failed = llvm::BasicBlock::Create(
                    builder.getContext(), "array.add.error", builder.GetInsertBlock()->getParent());
                builder.CreateCondBr(builder.CreateICmpEQ(status,
                    llvm::ConstantInt::get(status->getType(), 0), "array.add.success"),
                    proceed, failed);
                builder.SetInsertPoint(failed);
                builder.CreateStore(status, moves->array_failure_status);
                builder.CreateBr(moves->resource_failure);
                builder.SetInsertPoint(proceed);
                return llvm::ConstantInt::get(i64, 0);
            }
            if (expr.name == "set" && args.size() == 2) {
                builder.CreateCall(moves->array_set,
                    {selected_region, handle, args[0], args[1]});
                return llvm::ConstantInt::get(i64, 0); // compiler-only discarded ANF effect status
            }
            throw std::runtime_error("unsupported Array<int> native method");
        }
        case Expression::Kind::Borrow:
        case Expression::Kind::BorrowMut: {
            // The ownership pass admits checked scalar and class projections.
            // field_storage contains the actual in-place i64 address, not a
            // temporary copy: a mutable loan can write the field back.
            // Its conservatively whole-object lifetime ends with this call.
            if (expr.left && expr.left->kind == Expression::Kind::Field) {
                return expr.left->resolved_class.empty() ? field_storage.at(native_place_key(*expr.left))
                    : storage.at(native_place_key(*expr.left));
            }
            // Heap owner storage is a HANDLE, never a borrowable i64 address.
            // A shared call-scoped loan reads into a temporary scalar stack
            // slot that cannot escape this synchronous call. Mutable loans
            // require writeback and are conservatively rejected by checker.
            if (moves && moves->live_flags &&
                    moves->live_flags->count(expr.left->name)) {
                if (expr.kind == Expression::Kind::BorrowMut)
                    throw std::runtime_error("native owned-int mutable borrow needs handle-aware writeback");
                llvm::Value* handle = builder.CreateLoad(i64,
                    storage.at(expr.left->name), "borrow.owner.handle");
                llvm::Value* snapshot = builder.CreateCall(moves->read,
                    {moves->region, handle}, "borrow.owner.value");
                llvm::Value* temporary = entry_alloca(builder, i64, nullptr,
                    "borrow.owner.snapshot");
                builder.CreateStore(snapshot, temporary);
                return temporary;
            }
            return storage.at(expr.left->name);
        }
        case Expression::Kind::Negate: {
            auto* value = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            return checked_signed_arithmetic(builder, llvm::Intrinsic::ssub_with_overflow,
                llvm::ConstantInt::get(i64, 0), value, "neg");
        }
        case Expression::Kind::Copy:
        case Expression::Kind::Share:
            return lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
        case Expression::Kind::Take: {
            // Native owned-int transfer reads the scalar before destroying
            // the old heap allocation. It then disables the exit-time drop.
            // Field takes of trivially destructible i64 stay simple loads.
            if (moves && moves->live_flags && expr.left &&
                    expr.left->kind == Expression::Kind::Variable) {
                const auto found = moves->live_flags->find(expr.left->name);
                if (found != moves->live_flags->end()) {
                    llvm::Value* value = lower(*expr.left, builder, i64,
                        storage, field_storage, functions, moves);
                    llvm::Value* handle = builder.CreateLoad(i64,
                        storage.at(expr.left->name), "take.owned.handle");
                    builder.CreateCall(moves->drop, {moves->region, handle});
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0),
                        storage.at(expr.left->name));
                    builder.CreateStore(builder.getFalse(), found->second);
                    if (moves->owned_failure_handles) {
                        const auto shadow = moves->owned_failure_handles->find(expr.left->name);
                        if (shadow != moves->owned_failure_handles->end())
                            builder.CreateStore(llvm::ConstantInt::get(i64, 0), shadow->second);
                    }
                    return value;
                }
            }
            return lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
        }
        case Expression::Kind::Add: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            if (expr.resolved_type == ScalarType::String) {
                auto runtime = builder.GetInsertBlock()->getModule()->getOrInsertFunction("ores_string_concat",
                    llvm::FunctionType::get(i64, {i64, i64}, false));
                return builder.CreateCall(runtime, {left, right}, "string.concat");
            }
            return checked_signed_arithmetic(builder, llvm::Intrinsic::sadd_with_overflow,
                left, right, "add");
        }
        case Expression::Kind::Subtract: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            return checked_signed_arithmetic(builder, llvm::Intrinsic::ssub_with_overflow,
                left, right, "sub");
        }
        case Expression::Kind::Multiply: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            return checked_signed_arithmetic(builder, llvm::Intrinsic::smul_with_overflow,
                left, right, "mul");
        }
        case Expression::Kind::ShiftLeft:
        case Expression::Kind::ShiftRight:
        case Expression::Kind::ShiftUnsignedRight: {
            if (!expr.left || !expr.right)
                throw std::runtime_error("native shift AST missing operand");
            llvm::Value* left = lower(*expr.left, builder, i64, storage,
                                      field_storage, functions, moves);
            llvm::Value* distance = lower(*expr.right, builder, i64, storage,
                                          field_storage, functions, moves);
            // Java/Graal accepts shift distances only in 0..63. Unlike Java
            // bytecode, LLVM shifts with a distance >= bit width are poison,
            // and blindly masking to 63 would incorrectly admit 64, -1, etc.
            // The unsigned comparison rejects negatives and oversized i64
            // distances in one check BEFORE constructing any shift instruction.
            auto* module = builder.GetInsertBlock()->getModule();
            auto* parent = builder.GetInsertBlock()->getParent();
            auto* invalid = llvm::BasicBlock::Create(
                builder.getContext(), "shift.distance.invalid", parent);
            auto* valid = llvm::BasicBlock::Create(
                builder.getContext(), "shift.distance.valid", parent);
            builder.CreateCondBr(builder.CreateICmpULT(
                distance, llvm::ConstantInt::get(i64, 64)), valid, invalid);
            builder.SetInsertPoint(invalid);
            // Native target currently uses a trap for typed runtime errors.
            // This is fail-closed rather than Java's exception-class parity.
            builder.CreateCall(
                llvm::Intrinsic::getDeclaration(module, llvm::Intrinsic::trap));
            builder.CreateUnreachable();
            builder.SetInsertPoint(valid);
            if (expr.kind == Expression::Kind::ShiftLeft)
                return builder.CreateShl(left, distance, "shift.left");
            if (expr.kind == Expression::Kind::ShiftRight)
                return builder.CreateAShr(left, distance, "shift.signed.right");
            return builder.CreateLShr(left, distance, "shift.unsigned.right");
        }
        case Expression::Kind::Remainder:
        case Expression::Kind::Modulo: {
            if (!expr.left || !expr.right)
                throw std::runtime_error("native Integer.rem/mod requires two operands");
            const bool modulo = expr.kind == Expression::Kind::Modulo;
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            // A zero divisor traps, like checked overflow.
            auto* module = builder.GetInsertBlock()->getModule();
            auto* parent = builder.GetInsertBlock()->getParent();
            auto* failed = llvm::BasicBlock::Create(builder.getContext(), "rem.zero.trap", parent);
            auto* proceed = llvm::BasicBlock::Create(builder.getContext(), "rem.ok", parent);
            builder.CreateCondBr(builder.CreateICmpEQ(right, llvm::ConstantInt::get(i64, 0), "rem.zero"),
                failed, proceed);
            builder.SetInsertPoint(failed);
            builder.CreateCall(llvm::Intrinsic::getDeclaration(module, llvm::Intrinsic::trap));
            builder.CreateUnreachable();
            builder.SetInsertPoint(proceed);
            // Any value modulo -1 is 0, including INT64_MIN, but LLVM srem INT64_MIN, -1
            // is poison. srem by 1 is also 0, so divide by 1 in that case instead.
            llvm::Value* minus_one = builder.CreateICmpEQ(right,
                llvm::ConstantInt::getSigned(i64, -1), "rem.minus.one");
            llvm::Value* safe = builder.CreateSelect(minus_one, llvm::ConstantInt::get(i64, 1), right, "rem.divisor");
            llvm::Value* remainder = builder.CreateSRem(left, safe, "srem");
            if (!modulo) return remainder;
            // Euclidean modulus: shift a negative remainder by |divisor|. The true result
            // lies in [0, |divisor|), so wrapping add/sub is exact even for INT64_MIN.
            llvm::Value* negative = builder.CreateICmpSLT(remainder, llvm::ConstantInt::get(i64, 0), "mod.negative");
            llvm::Value* positive_divisor = builder.CreateICmpSGT(right, llvm::ConstantInt::get(i64, 0), "mod.divisor.positive");
            llvm::Value* adjusted = builder.CreateSelect(positive_divisor,
                builder.CreateAdd(remainder, right, "mod.add"),
                builder.CreateSub(remainder, right, "mod.sub"), "mod.adjusted");
            return builder.CreateSelect(negative, adjusted, remainder, "mod");
        }
        case Expression::Kind::BitXor: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            return builder.CreateXor(left, right, "bitxor");
        }
        case Expression::Kind::LogicalNot: {
            llvm::Value* value = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            return builder.CreateZExt(
                builder.CreateICmpEQ(value, llvm::ConstantInt::get(i64, 0), "bool.not.i1"),
                i64, "bool.not");
        }
        case Expression::Kind::Equal:
        case Expression::Kind::NotEqual:
        case Expression::Kind::Less:
        case Expression::Kind::LessEqual:
        case Expression::Kind::Greater:
        case Expression::Kind::GreaterEqual: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            if (expr.left->resolved_type == ScalarType::String) {
                if (expr.kind != Expression::Kind::Equal && expr.kind != Expression::Kind::NotEqual)
                    throw std::runtime_error("native String ordered comparison is not supported");
                auto runtime = builder.GetInsertBlock()->getModule()->getOrInsertFunction("ores_string_equal",
                    llvm::FunctionType::get(i64, {i64, i64}, false));
                llvm::Value* equal = builder.CreateCall(runtime, {left, right}, "string.equal");
                return expr.kind == Expression::Kind::Equal ? equal
                    : builder.CreateZExt(builder.CreateICmpEQ(equal, llvm::ConstantInt::get(i64, 0)), i64, "string.not.equal");
            }
            llvm::Value* compared = nullptr;
            switch (expr.kind) {
                case Expression::Kind::Equal:
                    compared = builder.CreateICmpEQ(left, right, "cmp.eq.i1"); break;
                case Expression::Kind::NotEqual:
                    compared = builder.CreateICmpNE(left, right, "cmp.ne.i1"); break;
                case Expression::Kind::Less:
                    compared = builder.CreateICmpSLT(left, right, "cmp.lt.i1"); break;
                case Expression::Kind::LessEqual:
                    compared = builder.CreateICmpSLE(left, right, "cmp.le.i1"); break;
                case Expression::Kind::Greater:
                    compared = builder.CreateICmpSGT(left, right, "cmp.gt.i1"); break;
                case Expression::Kind::GreaterEqual:
                    compared = builder.CreateICmpSGE(left, right, "cmp.ge.i1"); break;
                default: break;
            }
            return builder.CreateZExt(compared, i64, "cmp.bool");
        }
        case Expression::Kind::LogicalXor: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* lb = builder.CreateICmpNE(left, llvm::ConstantInt::get(i64, 0), "xor.left.i1");
            llvm::Value* rb = builder.CreateICmpNE(right, llvm::ConstantInt::get(i64, 0), "xor.right.i1");
            return builder.CreateZExt(builder.CreateXor(lb, rb, "xor.i1"), i64, "xor.bool");
        }
        case Expression::Kind::LogicalAnd:
        case Expression::Kind::LogicalOr: {
            llvm::Function* parent = builder.GetInsertBlock()->getParent();
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* leftBool = builder.CreateICmpNE(
                left, llvm::ConstantInt::get(i64, 0), "logic.left.i1");
            llvm::BasicBlock* leftExit = builder.GetInsertBlock();
            llvm::BasicBlock* rhs = llvm::BasicBlock::Create(
                builder.getContext(), "logic.rhs", parent);
            llvm::BasicBlock* merge = llvm::BasicBlock::Create(
                builder.getContext(), "logic.merge", parent);
            if (expr.kind == Expression::Kind::LogicalAnd)
                builder.CreateCondBr(leftBool, rhs, merge);
            else
                builder.CreateCondBr(leftBool, merge, rhs);

            builder.SetInsertPoint(rhs);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* rightBool = builder.CreateICmpNE(
                right, llvm::ConstantInt::get(i64, 0), "logic.right.i1");
            llvm::Value* rightCanonical = builder.CreateZExt(rightBool, i64, "logic.right.bool");
            llvm::BasicBlock* rhsExit = builder.GetInsertBlock();
            builder.CreateBr(merge);

            builder.SetInsertPoint(merge);
            auto* result = builder.CreatePHI(i64, 2, "logic.value");
            result->addIncoming(
                llvm::ConstantInt::get(i64,
                    expr.kind == Expression::Kind::LogicalOr ? 1 : 0),
                leftExit);
            result->addIncoming(rightCanonical, rhsExit);
            return result;
        }
        case Expression::Kind::Conditional: {
            // Type admission has proven a bool condition and compatible arm
            // result types; ownership admission has proven a valid affine join.
            // Evaluate the condition once, then only the selected value arm.
            llvm::Value* conditionValue = nullptr;
            if (expr.condition) {
                conditionValue = lower(*expr.condition, builder, i64,
                    storage, field_storage, functions, moves);
            } else {
                llvm::Value* lhs = lower(*expr.condition_lhs, builder, i64,
                    storage, field_storage, functions, moves);
                llvm::Value* rhs = lower(*expr.condition_rhs, builder, i64,
                    storage, field_storage, functions, moves);
                conditionValue = builder.CreateZExt(
                    builder.CreateICmpEQ(lhs, rhs, "ternary.eq.i1"),
                    i64, "ternary.eq");
            }
            llvm::Value* condition = builder.CreateICmpNE(
                conditionValue, llvm::ConstantInt::get(i64, 0), "ternary.cond.i1");
            llvm::Function* parent = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* whenTrue = llvm::BasicBlock::Create(
                builder.getContext(), "ternary.true", parent);
            llvm::BasicBlock* whenFalse = llvm::BasicBlock::Create(
                builder.getContext(), "ternary.false", parent);
            llvm::BasicBlock* merge = llvm::BasicBlock::Create(
                builder.getContext(), "ternary.merge", parent);
            builder.CreateCondBr(condition, whenTrue, whenFalse);

            builder.SetInsertPoint(whenTrue);
            llvm::Value* trueValue = lower(*expr.when_true, builder, i64,
                storage, field_storage, functions, moves);
            llvm::BasicBlock* trueExit = builder.GetInsertBlock();
            builder.CreateBr(merge);

            builder.SetInsertPoint(whenFalse);
            llvm::Value* falseValue = lower(*expr.when_false, builder, i64,
                storage, field_storage, functions, moves);
            llvm::BasicBlock* falseExit = builder.GetInsertBlock();
            builder.CreateBr(merge);

            builder.SetInsertPoint(merge);
            auto* value = builder.CreatePHI(i64, 2, "ternary.value");
            value->addIncoming(trueValue, trueExit);
            value->addIncoming(falseValue, falseExit);
            return value;
        }
        case Expression::Kind::Call: {
            const std::string call_selector = native_function_slot(
                expr.name, expr.arguments.size());
            llvm::Function* callee = functions.at(call_selector);
            if (moves && moves->frozen_scalar_dispatch) {
                const auto* witness = moves->frozen_scalar_dispatch->find_direct(call_selector);
                if (witness) {
                    if (!moves->frozen_scalar_callees || !moves->frozen_scalar_symbols)
                        throw std::runtime_error("native AOT static call lacks frozen target ledger");
                    const auto frozen_callee = moves->frozen_scalar_callees->find(call_selector);
                    const auto frozen_symbol = moves->frozen_scalar_symbols->find(call_selector);
                    if (frozen_callee == moves->frozen_scalar_callees->end() ||
                        frozen_symbol == moves->frozen_scalar_symbols->end() ||
                        frozen_callee->second != callee)
                        throw std::runtime_error("native AOT static call callee identity mismatch");
                    std::vector<std::string> formal_labels;
                    formal_labels.reserve(witness->parameter_abi.size());
                    if (callee->arg_size() < witness->parameter_abi.size())
                        throw std::runtime_error("native AOT static call formal ABI mismatch");
                    for (std::size_t i = 0; i < witness->parameter_abi.size(); ++i)
                        formal_labels.push_back(callee->getArg(i)->getName().str());
                    verify_native_i64_scalar_call_witness(
                        *witness, call_selector, frozen_symbol->second,
                        callee->getName().str(), expr.arguments.size(), formal_labels,
                        callee->getLinkage() == llvm::GlobalValue::InternalLinkage);
                }
            }
            const std::vector<std::size_t> no_hidden;
            const auto& hidden = moves && moves->class_array_parameters
                ? moves->class_array_parameters->at(native_function_slot(expr.name, expr.arguments.size())) : no_hidden;
            const bool owning = moves && moves->owning_results &&
                moves->owning_results->at(native_function_slot(expr.name, expr.arguments.size()));
            if (callee->arg_size() != expr.arguments.size() + hidden.size() + (owning ? 2 : 0)) {
                throw std::runtime_error("native named call argument count mismatch");
            }
            if (!expr.argument_names.empty()
                    && expr.argument_names.size() != expr.arguments.size()) {
                throw std::runtime_error("invalid native call label layout");
            }
            std::vector<llvm::Value*> ordered(expr.arguments.size(), nullptr);
            std::vector<const Expression*> ordered_source(expr.arguments.size(), nullptr);
            std::vector<bool> filled(expr.arguments.size(), false);
            std::size_t positional = 0;
            bool named_seen = false;
            for (std::size_t i = 0; i < expr.arguments.size(); ++i) {
                const std::string label = expr.argument_names.empty()
                        ? std::string{} : expr.argument_names[i];
                std::size_t target;
                if (!label.empty()) {
                    named_seen = true;
                    target = 0;
                    bool matched = false;
                    for (const auto& formal : callee->args()) {
                        if (formal.getName().str() == label) {
                            matched = true;
                            break;
                        }
                        ++target;
                    }
                    if (!matched) {
                        throw std::runtime_error("unknown native named argument '" + label + "'");
                    }
                } else {
                    if (named_seen) {
                        throw std::runtime_error("native positional argument after named argument");
                    }
                    target = positional++;
                }
                if (target >= ordered.size() || filled[target]) {
                    throw std::runtime_error("duplicate native named call binding");
                }
                filled[target] = true;
                ordered_source[target] = expr.arguments[i].get();
                // Emit instructions in source order, not in formal-slot order.
                ordered[target] = lower(*expr.arguments[i], builder, i64,
                                        storage, field_storage, functions, moves);
            }
            for (std::size_t parameter : hidden) {
                const auto& arg = *ordered_source.at(parameter);
                if (!arg.left || (arg.left->kind != Expression::Kind::Variable && arg.left->kind != Expression::Kind::Field) ||
                        (arg.kind != Expression::Kind::Borrow && arg.kind != Expression::Kind::BorrowMut))
                    throw std::runtime_error("collection class argument lacks named borrowed provenance");
                ordered.push_back(moves->class_array_regions->at(native_place_key(*arg.left)));
            }
            if (owning) {
                if (!moves->result_destination || !moves->result_region)
                    throw std::runtime_error("owning call result requires explicit destination ownership");
                ordered.push_back(moves->result_destination);
                ordered.push_back(moves->result_region);
            }
            return builder.CreateCall(callee, ordered,
                callee->getReturnType()->isVoidTy() ? "" : "call");
        }
    }
    throw std::runtime_error("invalid expression kind while lowering LLVM IR");
}

} // namespace

std::string emit_verified_llvm_ir(const Program& source, bool wasm32_target,
                                  bool cortex_m_target,
                                  bool bounded_allocator_provider,
                                  const std::string& isolate_entry,
                                  bool actor_region_v1,
                                  const std::vector<CheckedSourceRoutineV1>& checked_routines,
                                  const std::vector<CheckedLogSiteV1>& checked_sites) {
    // Every compiler retains its own frontend and source map. This optional
    // LLVM-owned admission binds reviewed static IDs to the native typed
    // source function selector; no source ID is synthesized at runtime.
    // The caller must already have checked the registry's source-unit/build
    // provenance. No guest log-site or dynamic instrumentation is implied.
    std::unordered_map<std::string, std::string> routine_source_ids;
    if (!checked_routines.empty()) {
        // ModuleLoader may flatten imports into an independently renamed
        // callable and clear the Program.imports list. Without per-unit
        // provenance, a manifest for root.atto must never certify imported
        // function bodies as if they originated in root.atto. Fail closed on
        // both direct imports and compiler-qualified/imported symbols.
        const bool has_cross_unit_callables = std::any_of(
            source.functions.begin(), source.functions.end(),
            [](const Function& fn) {
                return fn.name.find('.') != std::string::npos ||
                    fn.name.rfind("__ores_module_", 0) == 0;
            });
        if (checked_routines.size() != source.functions.size() ||
            !source.imports.empty() || has_cross_unit_callables ||
            !source.actors.empty() || !source.generators.empty() ||
            !source.classes.empty()) {
            throw std::runtime_error(
                "checked source IDs require exact single-unit scalar function coverage");
        }
        std::unordered_set<std::string> unique_routine_ids;
        for (const auto& id : checked_routines) {
            telemetry::SourceId parsed{};
            if (!telemetry::parse_source_id(id.routine_id,
                                            telemetry::SourceKind::Routine, parsed)) {
                throw std::runtime_error("malformed native checked routine source ID");
            }
            if (!unique_routine_ids.insert(id.routine_id).second)
                throw std::runtime_error("duplicate cross-function native routine source ID");
            if (!routine_source_ids.emplace(
                    native_function_slot(id.symbol, id.arity), id.routine_id).second) {
                throw std::runtime_error("duplicate checked native routine source selector");
            }
        }
        for (const auto& fn : source.functions) {
            if (!routine_source_ids.count(native_function_slot(fn.name, fn.parameters.size()))) {
                throw std::runtime_error("checked source routine registry missing a native callable");
            }
            // The v1 manifest spells signatures as name(int,...):int.
            // Name/arity matching alone is insufficient: bool, String,
            // borrows and void methods can have the same selector. Never
            // associate such a different typed ABI with a reviewed rid_.
            const bool exact_int_parameters = std::all_of(
                fn.parameters.begin(), fn.parameters.end(),
                [](const Parameter& param) {
                    return param.mode == Parameter::Mode::Value &&
                        param.scalar_type == ScalarType::I64 &&
                        param.class_name.empty() &&
                        param.generic_type.empty() &&
                        param.pending_type_alias.empty() &&
                        param.sum_type.empty();
                });
            if (fn.returns_void || fn.return_type != ScalarType::I64 ||
                !fn.return_class.empty() || !fn.return_sum.empty() ||
                !fn.pending_return_type_alias.empty() ||
                !fn.generic_parameters.empty() || !fn.method_owner.empty() ||
                !exact_int_parameters) {
                throw std::runtime_error(
                    "checked source routine manifest requires exact int-only callable signature");
            }
        }
    }
    // Every lexical log.info literal is an actual compiler-owned site.
    // The temporary selector+literal key is only an exact admission witness;
    // source-controlled site_id is the persistent identity. Multiple calls
    // to the same literal in one routine are ambiguous in this v1 slice.
    auto log_key = [&](const std::string& symbol, std::size_t arity,
                       const std::string& message) {
        return native_function_slot(symbol, arity) + '\x1f' + message;
    };
    std::unordered_set<std::string> emitted_log_keys;
    std::function<void(const Function&, const Statement&)> scan_log_site;
    scan_log_site = [&](const Function& fn, const Statement& stmt) {
        if (stmt.kind == Statement::Kind::TelemetryLogInfo) {
            if (!emitted_log_keys.insert(log_key(
                    fn.name, fn.parameters.size(), stmt.telemetry_operation)).second)
                throw std::runtime_error("ambiguous repeated native log.info literal requires explicit site locator");
        }
        for (const auto& child : stmt.then_statements) scan_log_site(fn, child);
        for (const auto& child : stmt.else_statements) scan_log_site(fn, child);
    };
    // A guest binding named "log" must never be silently treated as the
    // compiler-owned logging intrinsic. Reject it even if it appears in a
    // nested lexical scope or later in the function: v1 does not yet track
    // all control-flow and shadowing provenance in the static-site table.
    std::function<bool(const Statement&)> shadows_log;
    shadows_log = [&](const Statement& stmt) -> bool {
        if ((stmt.branch_local && stmt.branch_local->name == "log") ||
            (stmt.kind == Statement::Kind::UsingTelemetrySpan &&
             stmt.receiver == "log")) return true;
        return std::any_of(stmt.then_statements.begin(), stmt.then_statements.end(), shadows_log)
            || std::any_of(stmt.else_statements.begin(), stmt.else_statements.end(), shadows_log);
    };
    for (const auto& fn : source.functions) {
        const auto prior = emitted_log_keys.size();
        for (const auto& stmt : fn.statements) scan_log_site(fn, stmt);
        if (emitted_log_keys.size() != prior &&
            (std::any_of(fn.parameters.begin(), fn.parameters.end(),
                    [](const Parameter& p) { return p.name == "log"; }) ||
             std::any_of(fn.locals.begin(), fn.locals.end(),
                    [](const LocalBinding& local) { return local.name == "log"; }) ||
             std::any_of(fn.statements.begin(), fn.statements.end(), shadows_log)))
            throw std::runtime_error(
                "native compiler logging intrinsic cannot be shadowed by guest log binding");
    }
    if (emitted_log_keys.size() != checked_sites.size()) {
        throw std::runtime_error("native log.info requires exact source-controlled site ID coverage");
    }
    if (checked_sites.size() > 4096) {
        throw std::runtime_error("native static log site budget exceeded");
    }
    std::unordered_map<std::string, CheckedLogSiteV1> native_log_sites;
    std::unordered_set<std::string> seen_site_ids;
    for (const auto& site : checked_sites) {
        telemetry::SourceId rid{}, sid{};
        if (!telemetry::parse_source_id(
                site.routine_id, telemetry::SourceKind::Routine, rid) ||
            !telemetry::parse_source_id(
                site.site_id, telemetry::SourceKind::Site, sid) ||
            !seen_site_ids.insert(site.site_id).second) {
            throw std::runtime_error("invalid or duplicate source-controlled native log site identity");
        }
        const auto fn_key = native_function_slot(site.symbol, site.arity);
        auto parent = routine_source_ids.find(fn_key);
        if (parent == routine_source_ids.end() ||
            parent->second != site.routine_id) {
            throw std::runtime_error("source log site does not belong to checked source routine");
        }
        const auto key = log_key(site.symbol, site.arity, site.message);
        if (!emitted_log_keys.count(key) ||
            !native_log_sites.emplace(key, site).second) {
            throw std::runtime_error("stale, conflicting or misbound native static log site");
        }
    }
    if (!checked_sites.empty() &&
        (wasm32_target || cortex_m_target || !source.actors.empty() ||
         !source.generators.empty() || !source.classes.empty() ||
         std::any_of(source.functions.begin(), source.functions.end(),
             [](const Function& fn) {
                 return fn.async_source_i64 || !fn.source_tasks.empty() ||
                     !fn.future_pipelines.empty() ||
                     !fn.ready_await_actor.empty() ||
                     !fn.done_await_actor.empty() ||
                     !fn.actor_actions.empty() || !fn.actor_spawns.empty() ||
                     !fn.actor_field_writes.empty();
             }))) {
        throw std::runtime_error(
            "native log.info v1 only supports hosted non-suspending scalar AOT");
    }
    for (const auto& fn : source.functions) if (fn.name == "abort") throw std::runtime_error("reserved native panic runtime symbol collision");
    for (const auto& fn : source.functions)
        if (fn.name.rfind("ores_record_", 0) == 0)
            throw std::runtime_error("reserved native record runtime symbol collision");
    for (const auto& fn : source.functions)
        if (fn.name.rfind("ores_string_", 0) == 0)
            throw std::runtime_error("reserved native string runtime symbol collision");
    for (const auto& fn : source.functions)
        if (fn.name.rfind("ores_native_telemetry_", 0) == 0)
            throw std::runtime_error("reserved native telemetry ABI symbol collision");

    // First OresVM entry is deliberately a one-function, pure scalar state
    // transition. Runtime-owned state is injected, never a mutable LLVM global.
    if (!isolate_entry.empty()) {
        if (wasm32_target || cortex_m_target || source.functions.size() != 1 ||
            !source.imports.empty() || !source.actors.empty() ||
            !source.classes.empty() || !source.generators.empty() ||
            !source.cooperative_loops.empty() || !source.sum_types.empty())
            throw std::runtime_error("isolate entry requires one native scalar function");
        const auto& fn = source.functions.front();
        if (fn.name != isolate_entry || fn.name == "main" ||
            fn.name == "ores_guest_entry" || !fn.exported || fn.native_internal ||
            fn.async_source_i64 || fn.returns_void || fn.return_type != ScalarType::I64 ||
            !fn.return_class.empty() || !fn.return_sum.empty() ||
            fn.parameters.size() != 2 || !fn.locals.empty() ||
            !fn.statements.empty() || !fn.early_returns.empty() ||
            !fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
            !fn.actor_stops.empty() || !fn.actor_actions.empty() ||
            !fn.actor_field_writes.empty() || !fn.source_tasks.empty() ||
            !fn.future_pipelines.empty() || !fn.ready_await_actor.empty() ||
            !fn.done_await_actor.empty() || fn.scalar_while ||
            fn.caught_throw_value || !fn.method_owner.empty() ||
            !fn.result || !fn.generic_parameters.empty())
            throw std::runtime_error("isolate entry requires pure pub fnc (int,int): int");
        for (const auto& param : fn.parameters)
            if (param.mode != Parameter::Mode::Value ||
                param.scalar_type != ScalarType::I64 || !param.class_name.empty() ||
                !param.sum_type.empty() || !param.generic_type.empty())
                throw std::runtime_error("isolate entry requires i64 value arguments");
        const auto pure = [&](const auto& self, const Expression& expr) -> bool {
            using K = Expression::Kind;
            switch (expr.kind) {
                case K::Integer: return true;
                case K::Variable:
                    return expr.name == fn.parameters[0].name ||
                           expr.name == fn.parameters[1].name;
                case K::Negate: return expr.left && self(self, *expr.left);
                case K::Add: case K::Subtract: case K::Multiply:
                    return expr.left && expr.right && self(self, *expr.left) &&
                           self(self, *expr.right);
                default: return false;
            }
        };
        if (!pure(pure, *fn.result))
            throw std::runtime_error("isolate entry requires arithmetic-only result");
    }
    for (const auto& fn : source.functions)
        if (fn.name == "ores_guest_entry")
            throw std::runtime_error("reserved isolate ABI symbol collision");
    if (!source.imports.empty())
        throw std::runtime_error("unresolved native module imports cannot reach LLVM emission");
    for (const auto& actor : source.actors) {
        if (actor.max_actor_threads != MaxActorThreads::LOW) {
            throw std::runtime_error("parallel actor LLVM lowering requires compiler-proven per-turn actor state isolation");
        }
    }
    if (wasm32_target && cortex_m_target) throw std::runtime_error("conflicting LLVM target selectors");
    if (source.functions.size() == 1 && source.functions.front().scalar_while) {
        if (wasm32_target || !source.classes.empty() || !source.actors.empty())
            throw std::runtime_error("experimental scalar loop unsupported target/owners");
        return emit_scalar_while_frame_llvm_ir(*source.functions.front().scalar_while);
    }
    llvm::LLVMContext context;
    // A bounded, import-free Wasm guest may lower ONE synchronous actor.
    // Its state lives in private Wasm globals owned by exactly one Worker.
    // Native scheduler, ActorRef, suspension and supervisor APIs remain gated.
    if (wasm32_target) {
        if (source.actors.size() > 1)
            throw std::runtime_error("wasm32 actor guest supports one actor per instance");
        for (const auto& actor : source.actors) {
            if (actor.fields.empty() || actor.fields.size() > 16)
                throw std::runtime_error("wasm32 actor guest requires 1..16 scalar fields");
            if (actor.on_message.parameters.size() != 1 || !actor.on_message.result ||
                actor.on_message.returns_void || !actor.on_message.statements.empty() ||
                !actor.on_message.locals.empty() || !actor.on_message.early_returns.empty())
                throw std::runtime_error("wasm32 actor guest requires synchronous scalar handler");
        }
        for (const auto& cls : source.classes)
            for (const auto& field : cls.fields)
                if (field.int_array)
                    throw std::runtime_error("wasm32 class Array<int> owned fields are not implemented");
        for (const auto& fn : source.functions) {
            // A host-only caught-i64 CFG is not a certified wasm guest
            // exception ABI, even if an LLVM optimizer could emit its blocks.
            if (fn.caught_throw_value)
                throw std::runtime_error("wasm32 native caught-throw guest unsupported");
            if (!fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
                !fn.actor_stops.empty() || !fn.actor_field_writes.empty())
                throw std::runtime_error("wasm32 actor operations are not implemented");
            if (!fn.source_tasks.empty() || !fn.future_pipelines.empty())
                throw std::runtime_error(
                    "wasm32 source Task/Future scheduler runtime is not implemented");
            for (const auto& local : fn.locals) {
                // Preserve the native heap-owned diagnostic independently
                // of array-backed ownership rejection.
                if (local.heap_owned)
                    throw std::runtime_error("wasm32 native heap-owned regions are not implemented");
                if (local.int_array && !bounded_allocator_provider)
                    throw std::runtime_error(
                        "wasm32 Array<int> requires an explicit allocator provider");
            }
        }
    }
    // Native C entrypoints return i32; Oreslang int remains an internal i64.
    // Qualified module.main retains its exported guest symbol and gains a wrapper.
    // Browser Wasm is deliberately excluded: it has a separate host entry ABI.
    constexpr const char* guest_main_symbol = "__oreslang_guest_main";
    const auto main_it = std::find_if(source.functions.begin(), source.functions.end(),
        [](const Function& fn) { return fn.name == "main"; });
    const bool native_main = !wasm32_target && !cortex_m_target &&
        main_it != source.functions.end();
    bool string_runtime = false, console_runtime = false, record_runtime = !source.sum_types.empty(), iteration_runtime = false;
    const NativeClassLayouts layouts(source);
    std::unordered_map<std::string, ClassDecl> flat_classes;
    for (const auto& cls : source.classes) flat_classes.emplace(cls.name, native_flat_class(cls, layouts));
    auto class_owns_arrays = [&](const std::string& name) {
        return flat_classes.count(name) && layouts.at(name).owns_arrays;
    };
    for (const auto& type : source.sum_types)
        if (type.value_type == ScalarType::String || (type.kind == NativeSumType::Kind::Result && type.error_type == ScalarType::String)) string_runtime = true;
    const bool owning_result_runtime = std::any_of(source.functions.begin(), source.functions.end(),
        [&](const Function& f) { return class_owns_arrays(f.return_class); });
    std::function<void(const Expression*)> scan_string = [&](const Expression* e) {
        if (!e) return;
        if (e->kind == Expression::Kind::RecordConstruct && !class_owns_arrays(e->name)) record_runtime = true;
        if (e->kind == Expression::Kind::String || e->resolved_type == ScalarType::String) string_runtime = true;
        if (e->kind == Expression::Kind::Console) console_runtime = string_runtime = true;
        scan_string(e->left.get()); scan_string(e->right.get()); scan_string(e->condition.get());
        scan_string(e->condition_lhs.get()); scan_string(e->condition_rhs.get());
        scan_string(e->when_true.get()); scan_string(e->when_false.get());
        for (const auto& arg : e->arguments) scan_string(arg.get());
    };
    std::function<void(const Statement&)> scan_statement = [&](const Statement& s) {
        if (s.branch_local && s.branch_local->int_array) {
            if (wasm32_target)
                throw std::runtime_error("wasm32 scoped array owners require allocator-provider scope qualification");
            if (s.branch_local->array_element_type == ScalarType::String) string_runtime = true;
            if (!s.branch_local->array_record_class.empty()) record_runtime = true;
        }
        if (s.kind == Statement::Kind::ForEach || s.kind == Statement::Kind::ForIndex)
            iteration_runtime = true;
        scan_string(s.expression.get()); scan_string(s.condition.get());
        scan_string(s.loop_step.get());
        scan_string(s.condition_lhs.get()); scan_string(s.condition_rhs.get());
        if (s.branch_local) scan_string(s.branch_local->initializer.get());
        for (const auto& child : s.then_statements) scan_statement(child);
        for (const auto& child : s.else_statements) scan_statement(child);
    };
    auto scan_function = [&](const Function& fn) {
        if (!fn.return_class.empty() && !class_owns_arrays(fn.return_class)) record_runtime = true;
        if (fn.return_type == ScalarType::String) string_runtime = true;
        for (const auto& p : fn.parameters) if (p.scalar_type == ScalarType::String) string_runtime = true;
        for (const auto& local : fn.locals) {
            if (!local.array_record_class.empty() || (local.record_view && !class_owns_arrays(local.class_name))) record_runtime = true;
            if (local.int_array && local.array_element_type == ScalarType::String) string_runtime = true;
            scan_string(local.initializer.get());
            for (const auto& arg : local.constructor_arguments) scan_string(arg.get());
        }
        for (const auto& s : fn.statements) scan_statement(s);
        for (const auto& g : fn.early_returns) {
            scan_string(g.condition.get()); scan_string(g.lhs.get()); scan_string(g.rhs.get()); scan_string(g.result.get());
        }
        for (const auto& s : fn.caught_throw_defers) scan_statement(s);
        for (const auto& s : fn.actor_sends) scan_string(s.payload.get());
        for (const auto& w : fn.actor_field_writes) scan_string(w.expression.get());
        scan_string(fn.caught_throw_value.get());
        scan_string(fn.result.get());
    };
    for (const auto& fn : source.functions) scan_function(fn);
    for (const auto& cls : source.classes) for (const auto& field : cls.fields) {
        if (field.int_array && field.array_element_type == ScalarType::String) string_runtime = true;
        if (!field.array_record_class.empty()) record_runtime = true;
        if (field.scalar_type == ScalarType::String) string_runtime = true;
        scan_string(field.initializer_expression.get());
    }
    for (const auto& actor : source.actors) {
        for (const auto& field : actor.fields) scan_string(field.initializer.get());
        scan_function(actor.on_message);
    }
    if (string_runtime || record_runtime || owning_result_runtime || iteration_runtime) {
        if (!native_main || !source.actors.empty() || std::any_of(source.functions.begin(), source.functions.end(),
                [](const Function& f) {
                    return !f.ready_await_actor.empty() || f.caught_throw_value ||
                        f.scalar_while || !f.source_tasks.empty() ||
                        !f.future_pipelines.empty();
                }))
            throw std::runtime_error(string_runtime
                ? "native strings currently require a synchronous host main region without actors, suspension or exception frames"
                : "native records/owning results currently require a synchronous host main region without actors, suspension or exception frames");
    }
    std::uint32_t actor_region_slots = 0;
    std::uint32_t actor_region_stride = 8;
    if (actor_region_v1) {
        if (!native_main || !source.actors.empty())
            throw std::runtime_error("actor-region-v1 requires a synchronous native main without actors");
        for (const auto& function : source.functions) {
            if (!function.return_class.empty() ||
                std::any_of(function.parameters.begin(), function.parameters.end(),
                    [](const Parameter& parameter) { return !parameter.class_name.empty(); }))
                throw std::runtime_error(
                    "actor-region-v1 forbids class-valued function parameters and returns");
            if (&function != &*main_it &&
                std::any_of(function.locals.begin(), function.locals.end(),
                    [](const LocalBinding& local) { return !local.class_name.empty(); }))
                throw std::runtime_error(
                    "actor-region-v1 forbids class owners outside native main/0");
        }
        for (const auto& local : main_it->locals) {
            if (local.class_name.empty()) continue;
            if (!local.new_class || local.record_view || local.move_class ||
                !local.move_source.empty() || local.heap_owned || local.int_array ||
                !local.constructor_arguments.empty() || local.owner_mutable ||
                local.field_mutable)
                throw std::runtime_error(
                    "actor-region-v1 direct codegen requires fresh immutable class owners");
            ++actor_region_slots;
            const auto cls = std::find_if(source.classes.begin(), source.classes.end(),
                [&](const ClassDecl& candidate) { return candidate.name == local.class_name; });
            if (cls == source.classes.end())
                throw std::runtime_error("actor-region-v1 local has no resolved class layout");
            if (!cls->methods.empty() || cls->explicit_constructor ||
                !cls->constructor_parameters.empty() ||
                cls->fields.empty() || cls->fields.size() > 16)
                throw std::runtime_error(
                    "actor-region-v1 direct codegen requires 1..16 fields and no methods or explicit constructor");
            for (const auto& field : cls->fields) {
                const auto* initial = field.initializer_expression.get();
                if (!field.final_field || field.mutable_referent || field.int_array ||
                    !field.class_name.empty() || !field.sum_type.empty() ||
                    field.constructor_argument || field.scalar_type == ScalarType::String ||
                    !initial ||
                    (initial->kind != Expression::Kind::Integer &&
                     initial->kind != Expression::Kind::Boolean) ||
                    initial->left || initial->right || initial->condition ||
                    initial->condition_lhs || initial->condition_rhs ||
                    initial->when_true || initial->when_false ||
                    !initial->arguments.empty() || !initial->argument_names.empty() ||
                    initial->type_arguments_present || !initial->type_arguments.empty())
                    throw std::runtime_error(
                        "actor-region-v1 direct codegen requires immutable literal scalar defaults");
            }
            actor_region_stride = std::max(actor_region_stride,
                static_cast<std::uint32_t>(cls->fields.size() * sizeof(std::int64_t)));
        }
        if (!actor_region_slots || actor_region_slots > 4096u ||
            actor_region_stride > 1024u * 1024u)
            throw std::runtime_error("actor-region-v1 requires 1..4096 bounded class owners");
        std::function<void(const Statement&)> reject_branch_owner =
            [&](const Statement& statement) {
                if (statement.branch_local && !statement.branch_local->class_name.empty())
                    throw std::runtime_error(
                        "actor-region-v1 rejects branch-scoped class owners");
                for (const auto& child : statement.then_statements)
                    reject_branch_owner(child);
                for (const auto& child : statement.else_statements)
                    reject_branch_owner(child);
            };
        for (const auto& function : source.functions)
            for (const auto& statement : function.statements)
                reject_branch_owner(statement);
    }
    if (native_main) {
        if (!main_it->parameters.empty())
            throw std::runtime_error("native main() must have zero arguments");
        if (source.module.empty() && std::any_of(source.functions.begin(), source.functions.end(),
                [](const Function& fn) { return fn.name == "__oreslang_guest_main"; }))
            throw std::runtime_error("reserved native entrypoint symbol collision");
    }
    llvm::Module module("oreslang-source", context);
    // The wasm32 environment is an explicit code-generation target. This
    // emits verified, target-marked IR; object/linking remains in oreslang-wasm.
    // Never infer browser capabilities from the target triple alone.
    if (cortex_m_target) {
        // Experimental Cortex-M3 native ABI: scalar i64 functions, no OS wrapper.
        const auto profile = TargetAbi::parse("thumbv7m-none-eabi");
        if (!profile.experimental_cortexm_ir())
            throw std::runtime_error("Cortex-M IR emission is not admitted");
#if LLVM_VERSION_MAJOR >= 21
        module.setTargetTriple(llvm::Triple(std::string(profile.triple())));
#else
        module.setTargetTriple(std::string(profile.triple()));
#endif
        module.setDataLayout("e-m:e-p:32:32-Fi8-i64:64-v128:64:128-a:0:32-n32-S64");
        if (module.getDataLayout().getPointerSizeInBits(0) != 32 ||
            !module.getDataLayout().isLittleEndian())
            throw std::runtime_error("Cortex-M ABI/data layout mismatch");
    }
    if (wasm32_target) {
        const auto profile = TargetAbi::parse("wasm32-unknown-unknown");
        if (!profile.experimental_wasm32_ir())
            throw std::runtime_error("wasm32 target profile is not enabled");
#if LLVM_VERSION_MAJOR >= 21
        module.setTargetTriple(llvm::Triple(std::string(profile.triple())));
#else
        module.setTargetTriple(std::string(profile.triple()));
#endif
        module.setDataLayout("e-m:e-p:32:32-i64:64-n32:64-S128");
        const auto& actual = module.getDataLayout();
        if (actual.getPointerSizeInBits(0) != profile.pointer_bits()
                || actual.isLittleEndian() != (profile.endian() == "little"))
            throw std::runtime_error("wasm32 target ABI/data layout mismatch");
    }
    auto* i64 = llvm::Type::getInt64Ty(context);
    llvm::Type* i32 = llvm::Type::getInt32Ty(context);
    llvm::Type* loan_ptr = native_pointer_type(context, i64);
    // LLVM 18 removed Type::getInt8PtrTy. Use opaque address-space-zero
    // pointers on modern LLVM while preserving pre-18 typed-pointer support.
#if LLVM_VERSION_MAJOR >= 18
    llvm::Type* native_ptr = llvm::PointerType::get(context, 0);
#else
    llvm::Type* native_ptr = llvm::Type::getInt8PtrTy(context);
#endif
    // The test-only native owned-int experiment must never leak declarations
    // or allocations into ordinary int code. Resolve the ABI only when a
    // program actually contains a gated experimental heap-owned local.
    // AOT scoped probes remain dormant unless trusted native host control
    // explicitly enables them. Only hosted LLVM targets have the bounded,
    // C11 thread-local telemetry ABI; freestanding/Wasm must fail closed.
    std::function<bool(const Statement&)> has_native_scope =
        [&](const Statement& statement) -> bool {
            return statement.kind == Statement::Kind::UsingTelemetrySpan
                || std::any_of(statement.then_statements.begin(),
                               statement.then_statements.end(), has_native_scope)
                || std::any_of(statement.else_statements.begin(),
                               statement.else_statements.end(), has_native_scope);
        };
    const bool has_native_telemetry = std::any_of(
        source.functions.begin(), source.functions.end(),
        [&](const Function& fn) {
            return std::any_of(fn.statements.begin(), fn.statements.end(),
                               has_native_scope);
        });
    if (has_native_telemetry && (wasm32_target || cortex_m_target))
        throw std::runtime_error(
            "native telemetry span runtime currently supports only hosted LLVM AOT");
    llvm::FunctionCallee telemetryBegin, telemetryEnd;
    if (has_native_telemetry) {
        telemetryBegin = module.getOrInsertFunction(
            "ores_native_telemetry_span_begin",
            llvm::FunctionType::get(i64, {native_ptr, i64}, false));
        telemetryEnd = module.getOrInsertFunction(
            "ores_native_telemetry_span_end",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i64}, false));
    }
    llvm::FunctionCallee telemetryLog;
    if (!checked_sites.empty()) {
        telemetryLog = module.getOrInsertFunction(
            "ores_native_telemetry_log_info",
            llvm::FunctionType::get(i32, {i64, i64, i64, i64, native_ptr, i64}, false));
    }
    const bool has_source_tasks = std::any_of(
        source.functions.begin(), source.functions.end(),
        [](const Function& fn) {
            return !fn.source_tasks.empty() || !fn.future_pipelines.empty();
        });
    llvm::FunctionCallee taskStart, taskAwait, taskShutdown;
    if (has_source_tasks) {
        taskStart = module.getOrInsertFunction(
            "oreslang_source_task_start_i64",
            llvm::FunctionType::get(native_ptr, {native_ptr}, false));
        taskAwait = module.getOrInsertFunction(
            "oreslang_source_task_await_i64",
            llvm::FunctionType::get(i64, {native_ptr}, false));
        taskShutdown = module.getOrInsertFunction(
            "oreslang_source_task_runtime_shutdown",
            llvm::FunctionType::get(i32, {}, false));
    }

    const bool has_region_bindings = std::any_of(source.functions.begin(), source.functions.end(),
        [](const Function& fn) {
            return std::any_of(fn.locals.begin(), fn.locals.end(),
                [](const LocalBinding& binding) { return binding.heap_owned; });
        });
    llvm::FunctionCallee actorRegionGuestBegin, actorRegionGuestAlloc;
    llvm::FunctionCallee actorRegionGuestRead, actorRegionGuestWrite;
    llvm::FunctionCallee actorRegionGuestDrop, actorRegionGuestEnd;
    if (actor_region_v1) {
        auto* voidTy = llvm::Type::getVoidTy(context);
        auto* i32 = llvm::Type::getInt32Ty(context);
        actorRegionGuestBegin = module.getOrInsertFunction(
            "ores_actor_region_guest_v1_begin",
            llvm::FunctionType::get(voidTy, {i32, i32, i32}, false));
        actorRegionGuestAlloc = module.getOrInsertFunction(
            "ores_actor_region_guest_v1_alloc",
            llvm::FunctionType::get(i32, {i32}, false));
        actorRegionGuestRead = module.getOrInsertFunction(
            "ores_actor_region_guest_v1_read_i64",
            llvm::FunctionType::get(i64, {i32, i32}, false));
        actorRegionGuestWrite = module.getOrInsertFunction(
            "ores_actor_region_guest_v1_write_i64",
            llvm::FunctionType::get(voidTy, {i32, i32, i64}, false));
        actorRegionGuestDrop = module.getOrInsertFunction(
            "ores_actor_region_guest_v1_drop",
            llvm::FunctionType::get(voidTy, {i32}, false));
        actorRegionGuestEnd = module.getOrInsertFunction(
            "ores_actor_region_guest_v1_end",
            llvm::FunctionType::get(voidTy, {}, false));
    }

    llvm::FunctionCallee regionOpen, regionNew, regionRead, regionDrop, regionClose;
    if (has_region_bindings) {
        regionOpen = module.getOrInsertFunction("oreslang_owned_i64_region_open",
            llvm::FunctionType::get(native_ptr, {i64}, false));
        regionNew = module.getOrInsertFunction("oreslang_owned_i64_region_new",
            llvm::FunctionType::get(i64, {native_ptr, i64}, false));
        regionRead = module.getOrInsertFunction("oreslang_owned_i64_region_read",
            llvm::FunctionType::get(i64, {native_ptr, i64}, false));
        regionDrop = module.getOrInsertFunction("oreslang_owned_i64_region_drop",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {native_ptr, i64}, false));
        regionClose = module.getOrInsertFunction("oreslang_owned_i64_region_close",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {native_ptr}, false));
    }

    // Emit declarations only for functions that actually own Array<int>
    // locals. Opaque, unique handles are never exposed as guest values.
    std::function<bool(const Statement&)> contains_scoped_array = [&](const Statement& stmt) {
        return (stmt.kind == Statement::Kind::ScopedLocal && stmt.branch_local->int_array) ||
            std::any_of(stmt.then_statements.begin(), stmt.then_statements.end(), contains_scoped_array) ||
            std::any_of(stmt.else_statements.begin(), stmt.else_statements.end(), contains_scoped_array);
    };
    const bool has_array_bindings = std::any_of(source.classes.begin(), source.classes.end(), [&](const ClassDecl& cls) {
        return layouts.at(cls.name).owns_arrays;
    }) || std::any_of(source.functions.begin(), source.functions.end(),
        [&](const Function& fn) {
            return std::any_of(fn.statements.begin(), fn.statements.end(), contains_scoped_array) || std::any_of(fn.locals.begin(), fn.locals.end(),
                [&](const LocalBinding& b) {
                    if (b.int_array) return true;
                    if (!b.new_class) return false;
                    const auto cls = std::find_if(source.classes.begin(), source.classes.end(),
                        [&](const ClassDecl& candidate) { return candidate.name == b.class_name; });
                    return cls != source.classes.end() &&
                        std::any_of(cls->fields.begin(), cls->fields.end(),
                            [](const ClassField& fld) { return fld.int_array; });
                });
        });
    llvm::FunctionCallee arrayOpen, arrayNew, arrayAdd, arrayGet, arraySet,
                         arrayLength, arrayDrop, arrayClose, arrayAbort,
                         arrayPrepareFilledMany, arrayCommitMany, arrayTransferMany, arrayUnhandled;
    if (has_array_bindings) {
        auto* voidTy = llvm::Type::getVoidTy(context);
        auto* i32 = llvm::Type::getInt32Ty(context);
        arrayOpen = module.getOrInsertFunction("oreslang_int_array_try_region_open",
            llvm::FunctionType::get(i32, {native_pointer_type(context, native_ptr)}, false));
        arrayNew = module.getOrInsertFunction("oreslang_int_array_try_new",
            llvm::FunctionType::get(i32, {native_ptr, loan_ptr}, false));
        arrayTransferMany = module.getOrInsertFunction("oreslang_int_array_try_transfer_many",
            llvm::FunctionType::get(i32, {native_ptr, native_ptr, i64,
                native_pointer_type(context, loan_ptr), native_pointer_type(context, loan_ptr)}, false));
        arrayAdd = module.getOrInsertFunction("oreslang_int_array_try_add",
            llvm::FunctionType::get(i32, {native_ptr, i64, i64}, false));
        // Each opaque handle slot is i64. Arrays of handle-slot pointers
        // require an extra pointer layer in LLVM typed-pointer toolchains.
        arrayPrepareFilledMany = module.getOrInsertFunction(
            "oreslang_int_array_try_prepare_filled_many",
            llvm::FunctionType::get(i32,
                {native_ptr, i64, loan_ptr, loan_ptr, loan_ptr}, false));
        arrayCommitMany = module.getOrInsertFunction(
            "oreslang_int_array_commit_many",
            llvm::FunctionType::get(voidTy,
                {native_ptr, i64, native_pointer_type(context, loan_ptr),
                 native_pointer_type(context, loan_ptr)}, false));
        arrayAbort = module.getOrInsertFunction(
            bounded_allocator_provider
                ? "oreslang_freestanding_panic"
                : "abort",
            llvm::FunctionType::get(voidTy, {}, false));
        arrayUnhandled = module.getOrInsertFunction(
            "oreslang_int_array_unhandled_resource_failure",
            llvm::FunctionType::get(voidTy, {i32}, false));
        arrayGet = module.getOrInsertFunction("oreslang_int_array_get",
            llvm::FunctionType::get(i64, {native_ptr, i64, i64}, false));
        arraySet = module.getOrInsertFunction("oreslang_int_array_set",
            llvm::FunctionType::get(voidTy, {native_ptr, i64, i64, i64}, false));
        arrayLength = module.getOrInsertFunction("oreslang_int_array_length",
            llvm::FunctionType::get(i64, {native_ptr, i64}, false));
        arrayDrop = module.getOrInsertFunction("oreslang_int_array_drop",
            llvm::FunctionType::get(voidTy, {native_ptr, i64}, false));
        arrayClose = module.getOrInsertFunction("oreslang_int_array_region_close",
            llvm::FunctionType::get(voidTy, {native_ptr}, false));
    }

    // Only generated source actors resolve this native scheduler/heap ABI.
    // No actor imports appear in ordinary scalar/class compilation.
    llvm::FunctionCallee fieldInit, fieldInitMutable, fieldRead, fieldWrite;
    llvm::FunctionCallee sourceSpawn, sourceSend, sourceStop, sourceRequestStop,
        sourceAwaitReady, sourceAwaitDone;
    llvm::FunctionCallee actorScopedNew, actorScopedDrop;
    std::unordered_map<std::string, llvm::Function*> actorInits, actorHandlers;
    if (!source.actors.empty() && !wasm32_target) {
        auto* voidTy = llvm::Type::getVoidTy(context);
        fieldInit = module.getOrInsertFunction("ores_actor_i64_heap_field_init",
            llvm::FunctionType::get(voidTy, {native_ptr, i64, i64, i64, i64}, false));
        fieldInitMutable = module.getOrInsertFunction("ores_actor_i64_heap_field_init_mutable",
            llvm::FunctionType::get(voidTy, {native_ptr, i64, i64, i64, i64}, false));
        fieldRead = module.getOrInsertFunction("ores_actor_i64_heap_field_read",
            llvm::FunctionType::get(i64, {native_ptr, i64, i64, i64}, false));
        fieldWrite = module.getOrInsertFunction("ores_actor_i64_heap_field_write",
            llvm::FunctionType::get(voidTy, {native_ptr, i64, i64, i64, i64}, false));
        actorScopedNew = module.getOrInsertFunction("ores_actor_i64_heap_scoped_new",
            llvm::FunctionType::get(voidTy,
                {native_ptr, i64, i64, i64, native_ptr}, false));
        actorScopedDrop = module.getOrInsertFunction("ores_actor_i64_heap_scoped_drop",
            llvm::FunctionType::get(voidTy,
                {native_ptr, i64, i64, native_ptr}, false));
        sourceSpawn = module.getOrInsertFunction("oreslang_source_actor_spawn",
            llvm::FunctionType::get(native_ptr, {native_ptr, native_ptr, i64}, false));
        sourceSend = module.getOrInsertFunction("oreslang_source_actor_send",
            llvm::FunctionType::get(voidTy, {native_ptr, i64}, false));
        sourceStop = module.getOrInsertFunction("oreslang_source_actor_stop",
            llvm::FunctionType::get(voidTy, {native_ptr}, false));
        sourceRequestStop = module.getOrInsertFunction(
            "oreslang_source_actor_request_stop",
            llvm::FunctionType::get(voidTy, {native_ptr}, false));
        sourceAwaitReady = module.getOrInsertFunction(
            "oreslang_source_actor_await_ready",
            llvm::FunctionType::get(i64, {native_ptr, native_ptr, i64}, false));
        sourceAwaitDone = module.getOrInsertFunction(
            "oreslang_source_actor_await_done",
            llvm::FunctionType::get(i64, {native_ptr, native_ptr, i64}, false));
        for (const auto& actor : source.actors) {
            const std::string prefix = "oreslang_actor_" + actor.name + "_";
            actorInits.emplace(actor.name, llvm::Function::Create(
                llvm::FunctionType::get(voidTy, {native_ptr, i64, i64}, false),
                llvm::Function::ExternalLinkage, prefix + "init", module));
            actorHandlers.emplace(actor.name, llvm::Function::Create(
                llvm::FunctionType::get(i64, {native_ptr, i64, i64, i64}, false),
                llvm::Function::ExternalLinkage, prefix + "on_message", module));
        }
    }
    std::unordered_map<std::string, llvm::Function*> declarations;
    std::unordered_map<std::string, llvm::StructType*> class_types;
    std::unordered_map<std::string, const ClassDecl*> record_classes;
    std::unordered_map<std::string, std::uint64_t> record_schemas;
    llvm::FunctionCallee record_new, record_get, record_check, sum_check;
    if (record_runtime) {
        record_new = module.getOrInsertFunction("ores_record_new",
            llvm::FunctionType::get(i64, {i64, i64, native_ptr}, false));
        record_get = module.getOrInsertFunction("ores_record_get",
            llvm::FunctionType::get(i64, {i64, i64, i64}, false));
        sum_check = module.getOrInsertFunction("ores_record_check_shape",
            llvm::FunctionType::get(i64, {i64, i64, i64}, false));
        record_check = module.getOrInsertFunction("ores_record_check",
            llvm::FunctionType::get(i64, {i64, i64}, false));
    }
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> field_indices;
    std::unordered_map<std::string, bool> class_has_arrays;
    std::unordered_map<std::string, std::vector<std::size_t>> class_array_parameters;
    std::unordered_map<std::string, bool> owning_results;
    for (const auto& source_class : source.classes) {
        const auto& cls = flat_classes.at(source_class.name);
        record_classes.emplace(cls.name, &cls);
        record_schemas.emplace(cls.name, record_schemas.size() + 1);
        class_has_arrays.emplace(cls.name, layouts.at(cls.name).owns_arrays);
        std::vector<llvm::Type*> layout(cls.fields.size(), i64);
        class_types.emplace(cls.name, llvm::StructType::create(context, layout, cls.name));
        std::unordered_map<std::string, std::size_t> indices;
        for (std::size_t i = 0; i < cls.fields.size(); ++i)
            indices.emplace(cls.fields[i].name, i);
        field_indices.emplace(cls.name, std::move(indices));
    }

    for (const auto& type : source.sum_types) record_schemas.emplace(type.name, record_schemas.size() + 1);

    // Exact by-value i64 / borrowed-i64-pointer argument types for all
    // same-module functions; no unsound conversion to i64 is permitted.
    // Unique free functions retain their ABI symbol, overloaded families use arity slots.
    std::unordered_map<std::string, std::size_t> family_sizes;
    for (const auto& fn : source.functions) ++family_sizes[fn.name];
    // The existing typed native frontend admits concrete int generic
    // arguments only. Freeze a canonical, bounded one-per-declaration
    // specialization map before emitting any function bodies.
    const auto frozen_i64_dispatch = freeze_native_i64_dispatch_image(source);
    // The frozen image carries exact compiler-owned selectors, concrete type
    // tuples, bounded closure evidence and symbol suffixes. Do not replace
    // it with JIT profile data or a runtime name registry.
    std::unordered_set<std::string> emitted_function_symbols;
    std::unordered_map<std::string, llvm::Function*> frozen_scalar_callees;
    std::unordered_map<std::string, std::string> frozen_scalar_symbols;
    std::size_t emitted_concrete_specializations = 0;
    for (const auto& fn : source.functions) {
        const auto* specialization = frozen_i64_dispatch.find_direct(
            native_function_slot(fn.name, fn.parameters.size()));
        // Single compiler-owned symbol planner covers qualified module names,
        // overload slots, concrete generic ABI suffixes and reserved runtime
        // names. Do this before LLVM is allowed to silently uniquify a global.
        const std::string symbol = plan_native_aot_function_symbol(
            source, fn, frozen_i64_dispatch, family_sizes.at(fn.name),
            native_main, cortex_m_target);
        if (!emitted_function_symbols.insert(symbol).second)
            throw std::runtime_error("native AOT emitted function symbol collision");
        if (module.getNamedValue(symbol))
            throw std::runtime_error("native AOT direct dispatch linker symbol collision");
        std::vector<llvm::Type*> argument_types;
        for (const auto& param : fn.parameters)
            argument_types.push_back(param.class_name.empty()
                ? (param.mode == Parameter::Mode::Value ? i64 : loan_ptr)
                : native_pointer_type(context, class_types.at(param.class_name)));
        std::vector<std::size_t> hidden;
        for (std::size_t i = 0; i < fn.parameters.size(); ++i)
            if (!fn.parameters[i].class_name.empty() && class_has_arrays.at(fn.parameters[i].class_name)) {
                hidden.push_back(i);
                argument_types.push_back(native_ptr);
            }
        class_array_parameters.emplace(native_function_slot(fn.name, fn.parameters.size()), hidden);
        const bool owning = class_owns_arrays(fn.return_class);
        owning_results.emplace(native_function_slot(fn.name, fn.parameters.size()), owning);
        if (owning) { argument_types.push_back(native_ptr); argument_types.push_back(native_ptr); }
        llvm::FunctionType* signature = llvm::FunctionType::get(
            fn.returns_void ? llvm::Type::getVoidTy(context) : i64,
            argument_types, false);
        if (specialization != nullptr) {
            // Compare the LLVM physical ABI to the independently frozen
            // typed parameter modes before binding the direct-call symbol.
            // Hidden collection-region pointers and owned-return slots are
            // compiler-generated ABI parameters. Demand the *exact* count:
            // matching only explicit source arguments would silently allow
            // a dropped or forged trailing ownership parameter.
            std::size_t frozen_hidden_regions = 0;
            for (const auto& param : specialization->parameter_abi)
                if (!param.class_name.empty() && class_has_arrays.at(param.class_name))
                    ++frozen_hidden_regions;
            const std::size_t frozen_result_slots =
                class_owns_arrays(specialization->return_class) ? 2 : 0;
            if (hidden.size() != frozen_hidden_regions ||
                (owning ? 2u : 0u) != frozen_result_slots ||
                signature->getNumParams() !=
                    specialization->parameter_abi.size() +
                    frozen_hidden_regions + frozen_result_slots ||
                signature->getReturnType()->isVoidTy() != specialization->returns_void ||
                (!specialization->returns_void &&
                 signature->getReturnType() != i64))
                throw std::runtime_error("native AOT specialized LLVM function ABI mismatch");
            for (std::size_t i = 0; i < specialization->parameter_abi.size(); ++i) {
                const auto& param = specialization->parameter_abi[i];
                llvm::Type* expected = param.class_name.empty()
                    ? (param.mode == Parameter::Mode::Value ? i64 : loan_ptr)
                    : native_pointer_type(context, class_types.at(param.class_name));
                if (signature->getParamType(i) != expected)
                    throw std::runtime_error("native AOT specialized LLVM parameter ABI mismatch");
            }
        }
        llvm::Function* declaration = llvm::Function::Create(
            signature,
            (fn.native_internal || fn.async_source_i64 ||
             !fn.c_export_u32_symbol.empty() ||
             specialization != nullptr)
                ? llvm::Function::InternalLinkage
                : llvm::Function::ExternalLinkage,
            symbol, module);
        // LLVM may silently uniquify a symbol after a collision with a
        // runtime/import declaration. That would sever the frozen dispatch
        // target even when our source-function symbol set is collision-free.
        if (declaration->getName().str() != symbol)
            throw std::runtime_error("native AOT direct dispatch linker symbol collision");
        // Compiler-emitted metadata for the exact native callable. Native IR
        // metadata is NOT a runtime probe and may not survive object emission:
        // the authenticated artifact sidecar remains a separate future gate.
        if (auto id = routine_source_ids.find(
                native_function_slot(fn.name, fn.parameters.size()));
                id != routine_source_ids.end()) {
            declaration->setMetadata(
                "atto.source.routine_id.v1",
                llvm::MDNode::get(context, {llvm::MDString::get(context, id->second)}));
        }
        if (specialization != nullptr) {
            const auto selector = native_function_slot(fn.name, fn.parameters.size());
            if (!frozen_scalar_callees.emplace(selector, declaration).second ||
                !frozen_scalar_symbols.emplace(selector, symbol).second)
                throw std::runtime_error("duplicate native AOT frozen callee ledger identity");
            ++emitted_concrete_specializations;
        }
        // Forward references must already expose parameter labels.
        std::size_t label_index = 0;
        for (auto& formal : declaration->args()) {
            if (label_index < fn.parameters.size()) formal.setName(fn.parameters[label_index].name);
            else if (label_index < fn.parameters.size() + hidden.size())
                formal.setName("$region." + fn.parameters[hidden.at(label_index - fn.parameters.size())].name);
            else formal.setName(label_index == fn.parameters.size() + hidden.size() ? "$result" : "$result.region");
            ++label_index;
        }
        if (!declarations.emplace(native_function_slot(fn.name, fn.parameters.size()),
                                  declaration).second)
            throw std::runtime_error("native AOT duplicate declaration selector");
    }
    if (emitted_concrete_specializations != frozen_i64_dispatch.direct_targets.size())
        throw std::runtime_error("native AOT frozen direct dispatch image incomplete at emission");
    for (const auto& fn : source.functions) {
        llvm::Function* function = declarations.at(native_function_slot(fn.name, fn.parameters.size()));
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", function);
        llvm::IRBuilder<> builder(entry);
        std::unordered_map<std::string, llvm::Value*> storage;
        std::unordered_map<std::string, llvm::Value*> field_storage;
        std::unordered_map<std::string, llvm::Value*> class_array_regions;
        auto register_subobjects = [&](const std::string& owner, const std::string& type, llvm::Value* slot) {
            std::vector<std::string> names;
            for (const auto& child : layouts.at(type).subobjects) {
                const auto key = field_key(owner, child.first);
                auto* address = builder.CreateStructGEP(class_types.at(type), slot, static_cast<unsigned>(layouts.at(type).offsets.at(child.first)));
                auto* typed = builder.CreateBitCast(address, native_pointer_type(context, class_types.at(child.second)));
                if (!storage.emplace(key, typed).second) throw std::runtime_error("nested native storage collision");
                if (class_array_regions.count(owner)) class_array_regions.emplace(key, class_array_regions.at(owner));
                names.push_back(key);
            }
            return names;
        };

        const auto& hidden = class_array_parameters.at(native_function_slot(fn.name, fn.parameters.size()));
        const bool owning_return = owning_results.at(native_function_slot(fn.name, fn.parameters.size()));
        llvm::Value* result_destination = owning_return ? function->getArg(fn.parameters.size() + hidden.size()) : nullptr;
        llvm::Value* result_region = owning_return ? function->getArg(fn.parameters.size() + hidden.size() + 1) : nullptr;
        for (std::size_t i = 0; i < hidden.size(); ++i)
            class_array_regions.emplace(fn.parameters[hidden[i]].name, function->getArg(fn.parameters.size() + i));
        std::size_t index = 0;
        for (auto& arg : function->args()) {
            if (index == fn.parameters.size()) break;
            const Parameter& param = fn.parameters[index++];
            arg.setName(param.name);
            if (!param.class_name.empty()) {
                storage.emplace(param.name, &arg);
                for (const auto& [field, field_index] : field_indices.at(param.class_name)) {
                    field_storage.emplace(field_key(param.name, field),
                        builder.CreateStructGEP(class_types.at(param.class_name), &arg,
                            static_cast<unsigned>(field_index), param.name + "." + field));
                }
                register_subobjects(param.name, param.class_name, &arg);
            } else if (param.mode == Parameter::Mode::Value) {
                llvm::Value* slot = builder.CreateAlloca(i64, nullptr, param.name + ".slot");
                llvm::Value* incoming = &arg;
                if (param.scalar_type == ScalarType::Bool) {
                    // Public/native callers are not trusted to preserve the
                    // source-level canonical bool representation. Normalize
                    // every nonzero ABI value to exactly 1 at function entry.
                    incoming = builder.CreateZExt(
                        builder.CreateICmpNE(&arg, llvm::ConstantInt::get(i64, 0),
                            param.name + ".bool.abi.i1"),
                        i64, param.name + ".bool.abi");
                }
                builder.CreateStore(incoming, slot);
                storage.emplace(param.name, slot);
            } else {
                storage.emplace(param.name, &arg);
            }
        }

        for (const auto& parameter : fn.parameters) if (!parameter.sum_type.empty()) {
            auto* handle = builder.CreateLoad(i64, storage.at(parameter.name));
            checked_sum_tag(builder, i64, handle, record_schemas.at(parameter.sum_type), native_sum(source, parameter.sum_type).kind == NativeSumType::Kind::Option ? 2 : 3, sum_check, record_get);
        }

        // Every source actor ref is an opaque, unique capability owned by
        // trusted top-level main; it cannot enter scalar values or calls.
        std::unordered_map<std::string, llvm::Value*> actorRefs;
        for (const auto& spawn : fn.actor_spawns) {
            llvm::Value* ctor = builder.CreatePointerCast(actorInits.at(spawn.actor_type),
                native_ptr);
            llvm::Value* handler = builder.CreatePointerCast(actorHandlers.at(spawn.actor_type),
                native_ptr);
            const auto actor = std::find_if(source.actors.begin(), source.actors.end(),
                [&](const ActorDecl& a) { return a.name == spawn.actor_type; });
            llvm::Value* ref = builder.CreateCall(sourceSpawn, {ctor, handler,
                llvm::ConstantInt::get(i64,
                    actor->fields.size() + actor->on_message.locals.size())},
                spawn.name + ".actor.ref");
            actorRefs.emplace(spawn.name, ref);
        }
        // An explicitly "owned int" lives in a bounded native C region;
        // ordinary int remains stack-owned. Both allow only call-scoped loans.
        struct LocalSlot {
            std::string name;
            llvm::Value* address;
            bool heap_owned;
            std::uint64_t bytes;
            bool int_array = false;
            std::vector<std::string> owned_array_fields;
            bool actor_region_object = false;
        };
        std::vector<LocalSlot> lexical_slots;
        std::unordered_map<std::string, llvm::Value*> owned_live_flags;
        const auto heap_count = std::count_if(fn.locals.begin(), fn.locals.end(),
            [](const LocalBinding& b) { return b.heap_owned; });
        llvm::Value* region = nullptr;
        if (heap_count != 0)
            region = builder.CreateCall(regionOpen,
                {llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64),
                                        static_cast<std::uint64_t>(heap_count))}, "native.owned.region");
        std::unordered_set<std::string> array_names;
        std::unordered_set<std::string> array_field_keys;
        for (const auto& param : fn.parameters) if (!param.class_name.empty())
            for (const auto& field : record_classes.at(param.class_name)->fields)
                if (field.int_array) array_field_keys.insert(field_key(param.name, field.name));
        std::size_t array_owner_count = 0;
        std::function<void(const Statement&)> count_scoped_arrays = [&](const Statement& stmt) {
            if (stmt.kind == Statement::Kind::ScopedLocal && stmt.branch_local->int_array)
                ++array_owner_count;
            for (const auto& child : stmt.then_statements) count_scoped_arrays(child);
            for (const auto& child : stmt.else_statements) count_scoped_arrays(child);
        };
        for (const auto& stmt : fn.statements) count_scoped_arrays(stmt);
        for (const auto& local : fn.locals) {
            if (local.int_array) {
                array_names.insert(local.name);
                ++array_owner_count;
            } else if (local.new_class || (local.record_view && class_owns_arrays(local.class_name))) {
                const auto* cls = &flat_classes.at(local.class_name);
                for (const auto& fld : cls->fields)
                        if (fld.int_array) ++array_owner_count;
            }
        }
        // A compiler-owned four-array transaction stages four further
        // handles that must each have an initialized cleanup shadow before
        // the runtime can allocate. Whole-class moves add no new handles.
        for (const auto& statement : fn.statements)
            if (statement.kind == Statement::Kind::ArrayCommitFilled)
                array_owner_count += 4;
        if (array_owner_count > 64)
            throw std::runtime_error("native Array<int> per-function owned handle count exceeds 64");
        // Shadow owners are safe at every allocation failure: unlike class
        // field addresses, they are zeroed before the first checked call.
        llvm::Value* array_region = nullptr;
        llvm::Value* array_handle_shadows = nullptr;
        llvm::BasicBlock* array_failure = nullptr;
        llvm::Value* array_failure_status = nullptr;
        std::size_t next_array_shadow = 0;
        std::vector<llvm::Value*> heap_failure_slots;
        std::unordered_map<std::string, llvm::Value*> owned_failure_handles;
        // Maps each currently live source-level owner place to the token
        // shadow assigned at its original allocation. Moving an owner
        // changes the place, never the token or its failure cleanup authority.
        std::unordered_map<std::string, llvm::Value*> array_failure_places;
        if (array_owner_count || !hidden.empty()) {
            array_handle_shadows = builder.CreateAlloca(i64,
                llvm::ConstantInt::get(i64, array_owner_count), "array.failure.owners");
            for (std::size_t k = 0; k < array_owner_count; ++k) {
                llvm::Value* address = builder.CreateGEP(i64, array_handle_shadows,
                    llvm::ConstantInt::get(i64, k), "array.failure.slot.init");
                builder.CreateStore(llvm::ConstantInt::get(i64, 0), address);
            }
            for (const auto& local : fn.locals) {
                if (!local.heap_owned) continue;
                llvm::Value* shadow = builder.CreateAlloca(i64, nullptr,
                    local.name + ".owned.failure.slot");
                builder.CreateStore(llvm::ConstantInt::get(i64, 0), shadow);
                heap_failure_slots.push_back(shadow);
                owned_failure_handles.emplace(local.name, shadow);
            }
            array_failure = llvm::BasicBlock::Create(context, "array.resource.failure", function);
            array_failure_status = builder.CreateAlloca(
                llvm::Type::getInt32Ty(context), nullptr, "array.failure.status.slot");
            builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0),
                array_failure_status);
            if (array_owner_count) {
            llvm::Value* open_out = builder.CreateAlloca(native_ptr, nullptr, "array.region.out");
            llvm::Value* open_status = builder.CreateCall(arrayOpen, {open_out}, "array.region.status");
            llvm::BasicBlock* opened = llvm::BasicBlock::Create(
                context, "array.region.open.ok", function);
            llvm::BasicBlock* denied = llvm::BasicBlock::Create(
                context, "array.region.open.failure", function);
            builder.CreateCondBr(builder.CreateICmpEQ(open_status,
                llvm::ConstantInt::get(open_status->getType(), 0)), opened, denied);
            builder.SetInsertPoint(denied);
            // Preserve the actual typed resource status even when open fails
            // before a valid array-region pointer can be published.
            builder.CreateStore(open_status, array_failure_status);
            // A failed open published no native array, but a separate
            // experimental owned-int region may need deterministic cleanup.
            if (region != nullptr) builder.CreateCall(regionClose, {region});
            builder.CreateCall(arrayUnhandled, {open_status});
            builder.CreateUnreachable();
            builder.SetInsertPoint(opened);
            array_region = builder.CreateLoad(native_ptr, open_out, "native.int.array.region");
            }
        }
        std::unordered_map<std::string, ActorRegionFieldLowering> actor_region_fields;
        RegionMoveLowering region_moves{region, regionRead, regionDrop, &owned_live_flags,
            array_region, arrayAdd, arrayGet, arraySet, arrayLength, &array_names,
            &array_field_keys, array_failure, array_failure_status, &owned_failure_handles,
            &record_classes, &record_schemas, record_new, record_get, record_check,
            &class_array_regions, &class_array_parameters, &owning_results};
        if (actor_region_v1) {
            region_moves.actor_region_fields = &actor_region_fields;
            region_moves.actor_region_read_i64 = actorRegionGuestRead;
        }
        auto new_checked_array = [&](const std::string& label) -> llvm::Value* {
            if (!array_handle_shadows || next_array_shadow >= array_owner_count)
                throw std::runtime_error("missing native array failure owner slot");
            llvm::IRBuilder<> shadow_slots(llvm::cast<llvm::Instruction>(array_handle_shadows)->getNextNode());
            llvm::Value* address = shadow_slots.CreateGEP(i64, array_handle_shadows,
                llvm::ConstantInt::get(i64, next_array_shadow++),
                label + ".failure.owner");
            array_failure_places.emplace(label, address);
            llvm::Value* status = builder.CreateCall(arrayNew,
                {array_region, address}, label + ".array.create.status");
            llvm::BasicBlock* proceed = llvm::BasicBlock::Create(
                context, label + ".array.created", function);
            llvm::BasicBlock* denied_new = llvm::BasicBlock::Create(
                context, label + ".array.create.failure", function);
            builder.CreateCondBr(builder.CreateICmpEQ(status,
                llvm::ConstantInt::get(status->getType(), 0)),
                proceed, denied_new);
            builder.SetInsertPoint(denied_new);
            builder.CreateStore(status, array_failure_status);
            builder.CreateBr(array_failure);
            builder.SetInsertPoint(proceed);
            return builder.CreateLoad(i64, address, label + ".array.new");
        };
        region_moves.layouts = &layouts;
        region_moves.sum_program = &source;
        region_moves.sum_check = sum_check;
        region_moves.frozen_scalar_dispatch = &frozen_i64_dispatch;
        region_moves.frozen_scalar_callees = &frozen_scalar_callees;
        region_moves.frozen_scalar_symbols = &frozen_scalar_symbols;

        std::unordered_map<std::string, llvm::Value*> source_task_handles;
        for (const auto& task : fn.source_tasks) {
            if (!has_source_tasks || !task.result)
                throw std::runtime_error(
                    "source Task<int> lacks runtime/result lowering");
            auto* producer = llvm::Function::Create(
                llvm::FunctionType::get(i64, {}, false),
                llvm::Function::InternalLinkage,
                function->getName().str() + ".task." + task.name + ".producer",
                module);
            llvm::IRBuilder<> producer_builder(
                llvm::BasicBlock::Create(
                    context, "entry", producer));
            const std::unordered_map<std::string, llvm::Value*> no_storage;
            const std::unordered_map<std::string, llvm::Value*> no_fields;
            auto* produced = lower(
                *task.result, producer_builder, i64,
                no_storage, no_fields, declarations);
            producer_builder.CreateRet(produced);
            source_task_handles.emplace(
                task.name,
                builder.CreateCall(
                    taskStart,
                    {builder.CreatePointerCast(
                        producer, native_ptr,
                        task.name + ".task.producer.abi")},
                    task.name + ".task.handle"));
        }
        for (const auto& pipeline : fn.future_pipelines) {
            const std::uint64_t authority =
                future_pipeline_authority_fingerprint(pipeline);
            if (!has_source_tasks || pipeline.plans.empty() ||
                    pipeline.stages.empty() || !pipeline.ownership_admitted ||
                    pipeline.ownership_authority_fingerprint == 0 ||
                    pipeline.ownership_authority_fingerprint != authority)
                throw std::runtime_error(
                    "source Future<int> pipeline lacks exact post-ownership lowering authority");
            auto* producer = llvm::Function::Create(
                llvm::FunctionType::get(i64, {}, false),
                llvm::Function::InternalLinkage,
                function->getName().str() + ".future." + pipeline.name +
                    (pipeline.root_consumed ? ".consumed.producer"
                                            : ".borrowed.producer"),
                module);
            llvm::IRBuilder<> producer_builder(
                llvm::BasicBlock::Create(context, "entry", producer));
            llvm::Value* produced = producer_builder.CreateCall(
                declarations.at(native_function_slot(
                    pipeline.producer_function, 0)));
            for (const auto& stage_name : pipeline.stages) {
                produced = producer_builder.CreateCall(
                    declarations.at(native_function_slot(stage_name, 1)),
                    {produced},
                    pipeline.name + "." + stage_name + ".value");
            }
            producer_builder.CreateRet(produced);
            source_task_handles.emplace(
                pipeline.name,
                builder.CreateCall(
                    taskStart,
                    {builder.CreatePointerCast(
                        producer, native_ptr,
                        pipeline.name + ".future.producer.abi")},
                    pipeline.name + ".future.handle"));
        }

        for (const auto& binding : fn.locals) {
            if (binding.int_array) {
                llvm::Value* split_source = nullptr;
                llvm::Value* split_delimiter = nullptr;
                if (binding.initializer) {
                    const Expression& split = *binding.initializer;
                    if (split.kind != Expression::Kind::ArrayCall ||
                            split.name != "split_literal" || !split.left ||
                            split.arguments.size() != 1 ||
                            binding.array_element_type != ScalarType::String)
                        throw std::runtime_error("unchecked native owned String split initializer");
                    // Preserve lexical evaluation of String expressions
                    // before allocating the output array.
                    split_source = lower(*split.left, builder, i64,
                        storage, field_storage, declarations, &region_moves);
                    split_delimiter = lower(*split.arguments.front(), builder, i64,
                        storage, field_storage, declarations, &region_moves);
                }
                llvm::Value* handle = new_checked_array(binding.name);
                if (binding.initializer) {
                    if (!array_region || !array_failure || !array_failure_status)
                        throw std::runtime_error("native String split has no owned array region");
                    auto split_call = module.getOrInsertFunction(
                        "ores_string_split_literal",
                        llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                            {native_ptr, i64, i64, i64}, false));
                    auto* status = builder.CreateCall(split_call,
                        {array_region, handle, split_source, split_delimiter},
                        binding.name + ".split.status");
                    auto* ok = llvm::BasicBlock::Create(context,
                        binding.name + ".split.ok", function);
                    auto* denied = llvm::BasicBlock::Create(context,
                        binding.name + ".split.denied", function);
                    builder.CreateCondBr(builder.CreateICmpEQ(status,
                        llvm::ConstantInt::get(status->getType(), 0)), ok, denied);
                    builder.SetInsertPoint(denied);
                    builder.CreateStore(status, array_failure_status);
                    builder.CreateBr(array_failure);
                    builder.SetInsertPoint(ok);
                }
                llvm::Value* slot = builder.CreateAlloca(i64, nullptr,
                    binding.name + ".array.owner");
                builder.CreateStore(handle, slot);
                storage.emplace(binding.name, slot);
                lexical_slots.push_back({binding.name, slot, false, 8, true});
                continue;
            }
            if (binding.record_view && class_owns_arrays(binding.class_name)) {
                const auto& cls = *record_classes.at(binding.class_name);
                auto* shape = class_types.at(binding.class_name);
                auto* slot = builder.CreateAlloca(shape, nullptr, binding.name + ".owned.result");
                const auto bytes = cls.fields.size() * 8;
                native_lifetime_start(builder, slot, bytes);
                std::vector<std::string> owned_fields;
                for (std::size_t i = 0; i < cls.fields.size(); ++i) {
                    auto* address = builder.CreateStructGEP(shape, slot, static_cast<unsigned>(i));
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0), address);
                    const auto key = field_key(binding.name, cls.fields[i].name);
                    field_storage.emplace(key, address);
                    if (cls.fields[i].int_array) {
                        if (next_array_shadow >= array_owner_count) throw std::runtime_error("missing owning result cleanup slot");
                        auto* shadow = builder.CreateGEP(i64, array_handle_shadows,
                            llvm::ConstantInt::get(i64, next_array_shadow++));
                        array_failure_places.emplace(key, shadow);
                        array_field_keys.insert(key);
                        owned_fields.push_back(key);
                    }
                }
                RegionMoveLowering call_moves = region_moves;
                call_moves.result_destination = slot;
                call_moves.result_region = array_region;
                lower(*binding.initializer, builder, i64, storage, field_storage, declarations, &call_moves);
                for (const auto& key : owned_fields)
                    builder.CreateStore(builder.CreateLoad(i64, field_storage.at(key)), array_failure_places.at(key));
                storage.emplace(binding.name, slot);
                class_array_regions.emplace(binding.name, array_region);
                register_subobjects(binding.name, binding.class_name, slot);
                lexical_slots.push_back({binding.name, slot, false, bytes, false, std::move(owned_fields)});
                continue;
            }
            if (binding.record_view) {
                const auto& cls = *record_classes.at(binding.class_name);
                auto* shape = class_types.at(binding.class_name);
                auto* slot = builder.CreateAlloca(shape, nullptr, binding.name + ".record.view");
                const auto bytes = cls.fields.size() * 8;
                native_lifetime_start(builder, slot, bytes);
                auto* handle = lower(*binding.initializer, builder, i64, storage, field_storage, declarations, &region_moves);
                builder.CreateCall(record_check, {handle, llvm::ConstantInt::get(i64, record_schemas.at(cls.name))});
                for (std::size_t i = 0; i < cls.fields.size(); ++i) {
                    auto* value = builder.CreateCall(record_get, {handle,
                        llvm::ConstantInt::get(i64, record_schemas.at(cls.name)), llvm::ConstantInt::get(i64, i)});
                    auto* address = builder.CreateStructGEP(shape, slot, static_cast<unsigned>(i));
                    builder.CreateStore(value, address);
                    field_storage.emplace(field_key(binding.name, cls.fields[i].name), address);
                }
                storage.emplace(binding.name, slot);
                register_subobjects(binding.name, binding.class_name, slot);
                lexical_slots.push_back({binding.name, slot, false, bytes});
                continue;
            }
            if (binding.new_class || binding.move_class) {
                if (actor_region_v1) {
                    if (!binding.new_class || binding.move_class)
                        throw std::runtime_error("actor-region-v1 requires a fresh immutable class owner");
                    const auto& cls = flat_classes.at(binding.class_name);
                    const std::uint32_t object_bytes =
                        static_cast<std::uint32_t>(cls.fields.size() * sizeof(std::int64_t));
                    auto* i32 = llvm::Type::getInt32Ty(context);
                    llvm::Value* offset_slot = builder.CreateAlloca(
                        i32, nullptr, binding.name + ".actor.region.offset.slot");
                    llvm::Value* offset = builder.CreateCall(
                        actorRegionGuestAlloc,
                        {llvm::ConstantInt::get(i32, object_bytes)},
                        binding.name + ".actor.region.alloc");
                    builder.CreateStore(offset, offset_slot);
                    storage.emplace(binding.name, offset_slot);
                    for (std::size_t field_index = 0; field_index < cls.fields.size(); ++field_index) {
                        const auto& field = cls.fields[field_index];
                        if (!field.initializer_expression)
                            throw std::runtime_error("actor-region-v1 class field initializer missing");
                        const std::uint32_t byte_offset =
                            static_cast<std::uint32_t>(field_index * sizeof(std::int64_t));
                        const std::unordered_map<std::string, llvm::Value*> no_fields;
                        llvm::Value* initial = lower(*field.initializer_expression,
                            builder, i64, no_fields, no_fields, declarations, &region_moves);
                        builder.CreateCall(actorRegionGuestWrite,
                            {offset, llvm::ConstantInt::get(i32, byte_offset), initial});
                        actor_region_fields.emplace(field_key(binding.name, field.name),
                            ActorRegionFieldLowering{offset_slot, byte_offset});
                    }
                    const std::uint64_t object_size =
                        static_cast<std::uint64_t>(cls.fields.size() * sizeof(std::int64_t));
                    lexical_slots.push_back({binding.name, offset_slot, false,
                        object_size, false, {}, true});
                    continue;
                }
                if (class_has_arrays.at(binding.class_name))
                    class_array_regions.emplace(binding.name, binding.move_class
                        ? class_array_regions.at(binding.move_source) : array_region);
                llvm::StructType* shape = class_types.at(binding.class_name);
                llvm::Value* slot = builder.CreateAlloca(shape, nullptr, binding.name + ".record");
                const auto* cls = &flat_classes.at(binding.class_name);
                // Resolve constructor arguments while caller storage is live;
                // never expose private parameter slots as class fields.
                std::unordered_map<std::string, llvm::Value*> ctor_storage;
                std::unordered_map<std::string, std::string> ctor_sources;
                if (binding.new_class) {
                    if (binding.constructor_arguments.size() != cls->constructor_parameters.size())
                        throw std::runtime_error("native class constructor argument arity mismatch");
                    for (std::size_t ai = 0; ai < binding.constructor_arguments.size(); ++ai) {
                        const auto* source_field = constructor_field(layouts.source(binding.class_name), ai);
                        const auto& original_argument = *binding.constructor_arguments[ai];
                        const auto& source_argument = original_argument.kind == Expression::Kind::Take ? *original_argument.left : original_argument;
                        if (source_field && !source_field->class_name.empty()) {
                            if (source_argument.kind != Expression::Kind::Variable) throw std::runtime_error("nested constructor lacks named owner");
                            for (const auto& leaf : layouts.at(source_field->class_name).leaves) {
                                const auto destination = field_key(source_field->name, leaf.path);
                                const auto source_key = field_key(source_argument.name, leaf.path);
                                auto* value = builder.CreateLoad(i64, field_storage.at(source_key));
                                auto* snapshot = builder.CreateAlloca(i64, nullptr, "nested.constructor.snapshot");
                                builder.CreateStore(value, snapshot); ctor_storage.emplace(destination, snapshot);
                                ctor_sources.emplace(destination, source_key);
                            }
                            continue;
                        }
                        if (source_field && source_field->int_array) ctor_sources.emplace(source_field->name, source_argument.name);
                        llvm::Value* value = lower(*binding.constructor_arguments[ai],
                            builder, i64, storage, field_storage, declarations, &region_moves);
                        llvm::Value* arg_slot = builder.CreateAlloca(i64, nullptr,
                            binding.name + ".constructor." + cls->constructor_parameters[ai]);
                        builder.CreateStore(value, arg_slot);
                        ctor_storage.emplace(cls->constructor_parameters[ai], arg_slot);
                    }
                }
                const std::uint64_t bytes = static_cast<std::uint64_t>(cls->fields.size()) * 8;
                native_lifetime_start(builder, slot, bytes);
                // Prepare before publishing class field addresses. On any
                // failure the checked cleanup reads only initialized shadows,
                // never uninitialized class field storage.
                std::vector<std::pair<std::string, llvm::Value*>> staged_fields;
                std::vector<std::string> owned_fields;
                for (std::size_t i = 0; i < cls->fields.size(); ++i) {
                    const ClassField& item = cls->fields[i];
                    llvm::Value* address = builder.CreateStructGEP(shape, slot,
                        static_cast<unsigned>(i), binding.name + "." + item.name);
                    llvm::Value* initial = nullptr;
                    if (item.int_array) {
                        if (binding.new_class && item.constructor_argument) {
                            const auto source_key = ctor_sources.at(item.name);
                            initial = builder.CreateLoad(i64, ctor_storage.at(item.name));
                            auto* previous = storage.count(source_key) ? storage.at(source_key) : field_storage.at(source_key);
                            builder.CreateStore(llvm::ConstantInt::get(i64, 0), previous);
                            array_failure_places.emplace(field_key(binding.name, item.name), array_failure_places.at(source_key));
                        } else if (binding.new_class) {
                            initial = new_checked_array(binding.name + "." + item.name);
                        } else {
                            // Unique whole-class rt take: transfer the token
                            // into the new owner and disarm the original.
                            // No fallible allocation occurs during the move.
                            llvm::Value* previous = field_storage.at(
                                field_key(binding.move_source, item.name));
                            initial = builder.CreateLoad(i64, previous,
                                binding.move_source + "." + item.name + ".move.handle");
                            builder.CreateStore(initial, address);
                            builder.CreateStore(llvm::ConstantInt::get(i64, 0), previous);
                            array_failure_places.emplace(field_key(binding.name, item.name),
                                array_failure_places.at(field_key(binding.move_source, item.name)));
                        }
                        owned_fields.push_back(field_key(binding.name, item.name));
                    } else {
                        if (binding.new_class) {
                            if (!item.initializer_expression)
                                throw std::runtime_error("native class scalar initializer is missing");
                            const std::unordered_map<std::string, llvm::Value*> no_fields;
                            initial = lower(*item.initializer_expression, builder, i64,
                                ctor_storage, no_fields, declarations, &region_moves);
                        } else {
                            initial = builder.CreateLoad(i64,
                                field_storage.at(field_key(binding.move_source, item.name)),
                                binding.move_source + "." + item.name + ".take");
                        }
                    }
                    if (!item.int_array || binding.new_class)
                        builder.CreateStore(initial, address);
                    staged_fields.emplace_back(field_key(binding.name, item.name), address);
                }
                for (const auto& field : staged_fields)
                    field_storage.emplace(field.first, field.second);
                for (const auto& name : owned_fields)
                    array_field_keys.insert(name);
                storage.emplace(binding.name, slot);
                register_subobjects(binding.name, binding.class_name, slot);
                lexical_slots.push_back({binding.name, slot, false, bytes, false,
                                         std::move(owned_fields)});
                continue;
            }
            llvm::Value* initial = nullptr;
            if (!binding.task_await_name.empty() ||
                    !binding.future_await_name.empty()) {
                const std::string& awaited = !binding.task_await_name.empty()
                    ? binding.task_await_name
                    : binding.future_await_name;
                const auto task = source_task_handles.find(awaited);
                if (task == source_task_handles.end())
                    throw std::runtime_error(
                        "Task/Future<int> await lost runtime handle");
                initial = builder.CreateCall(
                    taskAwait, {task->second},
                    binding.name + (!binding.task_await_name.empty()
                        ? ".task.awaited" : ".future.awaited"));
            } else {
                if (!binding.initializer)
                    throw std::runtime_error(
                        "native scalar local lacks initializer");
                initial = lower(
                    *binding.initializer, builder, i64, storage,
                    field_storage, declarations, &region_moves);
            }
            llvm::Value* slot = nullptr;
            if (binding.heap_owned) {
                // Store only an opaque, generation-checked lifetime token.
                // No raw region payload pointer crosses into guest LLVM IR.
                llvm::Value* handle = builder.CreateCall(regionNew,
                    {region, initial}, binding.name + ".owned.handle");
                slot = builder.CreateAlloca(i64, nullptr, binding.name + ".owned.handle.slot");
                builder.CreateStore(handle, slot);
                if (array_owner_count)
                    builder.CreateStore(handle, owned_failure_handles.at(binding.name));
                llvm::Value* live = builder.CreateAlloca(builder.getInt1Ty(), nullptr,
                    binding.name + ".owned.live");
                builder.CreateStore(builder.getTrue(), live);
                owned_live_flags.emplace(binding.name, live);
            } else {
                slot = builder.CreateAlloca(i64, nullptr, binding.name + ".stack.owned");
                native_lifetime_start(builder, slot, 8);
                builder.CreateStore(initial, slot);
            }
            storage.emplace(binding.name, slot);
            lexical_slots.push_back({binding.name, slot, binding.heap_owned, 8});
        }
        std::vector<const Statement*> deferred_effects;
        auto lower_effect = [&](const Statement& effect) {
            if (effect.kind == Statement::Kind::ScalarWrite) {
                auto* value = lower(*effect.expression, builder, i64, storage, field_storage, declarations, &region_moves);
                builder.CreateStore(value, storage.at(effect.receiver));
                return;
            }
            if (effect.kind == Statement::Kind::ArrayCommitFilled) {
                // OwnershipChecker guarantees a live unique mutable class
                // with exactly four array fields; the class layout fixes
                // their ordering. All inputs are evaluated before staging.
                const auto binding = std::find_if(fn.locals.begin(), fn.locals.end(),
                    [&](const LocalBinding& b) { return b.name == effect.receiver; });
                if (binding == fn.locals.end() || binding->class_name.empty())
                    throw std::runtime_error("filled array commit lacks class owner");
                const auto cls = std::find_if(source.classes.begin(), source.classes.end(),
                    [&](const ClassDecl& item) { return item.name == binding->class_name; });
                if (cls == source.classes.end())
                    throw std::runtime_error("filled array commit lacks class layout");
                std::vector<std::string> owned_fields;
                for (const auto& item : cls->fields)
                    if (item.int_array)
                        owned_fields.push_back(field_key(effect.receiver, item.name));
                if (owned_fields.size() != 4 || effect.array_commit_values.size() != 4 ||
                        !array_handle_shadows || next_array_shadow + 4 > array_owner_count)
                    throw std::runtime_error("unverified filled array commit owners");

                const auto four = llvm::ConstantInt::get(i64, 4);
                llvm::Value* count = lower(*effect.array_commit_length, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                llvm::Value* lengths = builder.CreateAlloca(i64, four, "arrays.commit.lengths");
                llvm::Value* fill = builder.CreateAlloca(i64, four, "arrays.commit.values");
                llvm::Value* dests = builder.CreateAlloca(loan_ptr, four, "arrays.commit.dest.ptrs");
                llvm::Value* sources = builder.CreateAlloca(loan_ptr, four, "arrays.commit.src.ptrs");
                llvm::Value* staged = builder.CreateGEP(i64, array_handle_shadows,
                    llvm::ConstantInt::get(i64, next_array_shadow),
                    "arrays.commit.staged.owners");
                const std::size_t first_stage = next_array_shadow;
                next_array_shadow += 4;
                std::vector<llvm::Value*> previous_shadows;
                for (std::size_t i = 0; i < 4; ++i) {
                    const auto index = llvm::ConstantInt::get(i64, i);
                    llvm::Value* value = lower(*effect.array_commit_values[i],
                        builder, i64, storage, field_storage, declarations, &region_moves);
                    builder.CreateStore(count, builder.CreateGEP(i64, lengths, index));
                    builder.CreateStore(value, builder.CreateGEP(i64, fill, index));
                    builder.CreateStore(field_storage.at(owned_fields[i]),
                        builder.CreateGEP(loan_ptr, dests, index));
                    builder.CreateStore(builder.CreateGEP(i64, array_handle_shadows,
                            llvm::ConstantInt::get(i64, first_stage + i)),
                        builder.CreateGEP(loan_ptr, sources, index));
                    previous_shadows.push_back(array_failure_places.at(owned_fields[i]));
                }

                llvm::Value* status = builder.CreateCall(arrayPrepareFilledMany,
                    {array_region, four, lengths, fill, staged},
                    "arrays.commit.prepare.status");
                llvm::BasicBlock* accepted = llvm::BasicBlock::Create(
                    context, "arrays.commit.prepare.ok", function);
                llvm::BasicBlock* failed = llvm::BasicBlock::Create(
                    context, "arrays.commit.prepare.failure", function);
                builder.CreateCondBr(builder.CreateICmpEQ(status,
                    llvm::ConstantInt::get(status->getType(), 0)), accepted, failed);
                builder.SetInsertPoint(failed);
                builder.CreateStore(status, array_failure_status);
                // ABI contract: failed preparation never modifies staged[] or
                // published class handles. The shared epilogue drops only
                // the original live owners and then closes their region.
                builder.CreateBr(array_failure);
                builder.SetInsertPoint(accepted);
                builder.CreateCall(arrayCommitMany, {array_region, four, dests, sources});
                // IMPORTANT: commit_many disarms *sources[i] = 0. Since we
                // supplied hidden staging shadows as those source slots, the
                // runtime also zeroed our failure-cleanup mirrors. Restore
                // each new owner token from the published destination *after*
                // the infallible native commit and before any further guest
                // operations can fail. Otherwise a later resource failure
                // would leak four live arrays and abort on region_close.
                for (std::size_t i = 0; i < 4; ++i) {
                    llvm::Value* shadow = builder.CreateGEP(i64,
                        array_handle_shadows,
                        llvm::ConstantInt::get(i64, first_stage + i),
                        "arrays.commit.cleanup.owner");
                    llvm::Value* published = builder.CreateLoad(i64,
                        field_storage.at(owned_fields[i]),
                        "arrays.commit.published.owner");
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0),
                        previous_shadows[i]);
                    builder.CreateStore(published, shadow);
                    array_failure_places[owned_fields[i]] = shadow;
                }
            } else if (effect.kind == Statement::Kind::FieldWrite) {
                const std::string target = field_key(effect.receiver, effect.field);
                if (array_field_keys.count(target)) {
                    // OwnershipChecker admitted only straight-line direct
                    // rt take(local) into an exclusively owned mutable field.
                    // Both handles are already live in this private region.
                    const Expression& move = *effect.expression;
                    if (move.kind != Expression::Kind::Take || !move.left ||
                            move.left->kind != Expression::Kind::Variable)
                        throw std::runtime_error("unverified native array field move");
                    const std::string& source = move.left->name;
                    llvm::Value* original = field_storage.at(target);
                    llvm::Value* lexical = storage.at(source);
                    llvm::Value* incoming = builder.CreateLoad(i64, lexical,
                        source + ".transfer.owner");
                    llvm::Value* obsolete = builder.CreateLoad(i64, original,
                        target + ".replaced.owner");
                    // No allocation can occur between destruction and swap.
                    // The original allocation's failure shadow must also be
                    // disarmed before later checked array calls may fail.
                    builder.CreateCall(arrayDrop, {array_region, obsolete});
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0),
                        array_failure_places.at(target));
                    builder.CreateStore(incoming, original);
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0), lexical);
                    array_failure_places[target] = array_failure_places.at(source);
                } else {
                    llvm::Value* updated = lower(*effect.expression, builder, i64,
                        storage, field_storage, declarations, &region_moves);
                    builder.CreateStore(updated, field_storage.at(target));
                }
            } else if (effect.kind == Statement::Kind::TelemetryLogInfo) {
                // The compiler IR call is a real AOT guest callsite. Runtime
                // copies only source-controlled IDs and the bounded literal
                // into its carrier-local ring, never a guest object/handle.
                const auto key = log_key(fn.name, fn.parameters.size(),
                                         effect.telemetry_operation);
                auto found = native_log_sites.find(key);
                if (found == native_log_sites.end() || !telemetryLog.getCallee())
                    throw std::runtime_error("unverified native guest telemetry log site");
                telemetry::SourceId rid{}, sid{};
                if (!telemetry::parse_source_id(
                        found->second.routine_id, telemetry::SourceKind::Routine, rid) ||
                    !telemetry::parse_source_id(
                        found->second.site_id, telemetry::SourceKind::Site, sid))
                    throw std::runtime_error("corrupt checked native log source descriptor");
                auto lane = [](const telemetry::SourceId& id, std::size_t begin) {
                    std::uint64_t result = 0;
                    for (std::size_t k = begin; k < begin + 8; ++k)
                        result = (result << 8) | id.bytes[k];
                    return result;
                };
                auto* literal = builder.CreateGlobalStringPtr(
                    effect.telemetry_operation, "atto.telemetry.log.literal");
                auto* siteCall = builder.CreateCall(telemetryLog, {
                    llvm::ConstantInt::get(i64, lane(rid, 0)),
                    llvm::ConstantInt::get(i64, lane(rid, 8)),
                    llvm::ConstantInt::get(i64, lane(sid, 0)),
                    llvm::ConstantInt::get(i64, lane(sid, 8)),
                    literal,
                    llvm::ConstantInt::get(i64, effect.telemetry_operation.size())
                }, "atto.telemetry.log.receipt");
                siteCall->setMetadata("atto.source.site_id.v1",
                    llvm::MDNode::get(context, {
                        llvm::MDString::get(context, found->second.site_id)}));
                siteCall->setMetadata("atto.source.routine_id.v1",
                    llvm::MDNode::get(context, {
                        llvm::MDString::get(context, found->second.routine_id)}));
            } else if (effect.kind == Statement::Kind::Call) {
                lower(*effect.expression, builder, i64, storage, field_storage, declarations, &region_moves);
            } else {
                throw std::runtime_error("unsupported native conditional arm");
            }
        };
        // Branch-local trivial i64 owners are initialized only along their
        // reached predecessor and end their lifetime before the join. They
        // never enter the enclosing lexical_slots or deferred cleanup list.
        bool structured_returns = false;
        std::function<void(const Statement&)> find_returns = [&](const Statement& stmt) {
            if (stmt.kind == Statement::Kind::Return) structured_returns = true;
            for (const auto& child : stmt.then_statements) find_returns(child);
            for (const auto& child : stmt.else_statements) find_returns(child);
        };
        for (const auto& stmt : fn.statements) find_returns(stmt);
        llvm::BasicBlock* structured_cleanup = structured_returns
            ? llvm::BasicBlock::Create(context, "exit.cleanup", function) : nullptr;
        llvm::Value* structured_snapshot = structured_returns && !fn.returns_void
            ? builder.CreateAlloca(i64, nullptr, "return.snapshot.slot") : nullptr;
        struct ScopedSlot { std::string name; llvm::Value* address; std::size_t bytes; std::vector<std::string> fields; std::vector<std::string> subobjects; llvm::Value* array_shadow = nullptr; };
        auto scoped_value = [&](const LocalBinding& local, llvm::Value* init) {
            const bool record = !local.class_name.empty();
            llvm::Type* shape = record ? static_cast<llvm::Type*>(class_types.at(local.class_name)) : i64;
            const std::size_t bytes = record ? record_classes.at(local.class_name)->fields.size() * 8 : 8;
            // Allocate once per native invocation, never on every back-edge.
            llvm::IRBuilder<> entry_slots(&function->getEntryBlock(), function->getEntryBlock().begin());
            auto* slot = entry_slots.CreateAlloca(shape, nullptr, local.name + ".arm.stack");
            if (local.int_array) entry_slots.CreateStore(llvm::ConstantInt::get(i64, 0), slot);
            // Array handle slots remain addressable for the whole invocation:
            // a common return epilogue may inspect an unreached declaration.
            // The resource itself is still created/dropped at lexical edges.
            if (!local.int_array) native_lifetime_start(builder, slot, bytes);
            ScopedSlot result{local.name, slot, bytes, {}, {}};
            if (local.int_array) {
                llvm::Value* split_source = nullptr;
                llvm::Value* split_delimiter = nullptr;
                if (local.initializer) {
                    const auto& split = *local.initializer;
                    if (split.kind != Expression::Kind::ArrayCall ||
                            split.name != "split_literal" || !split.left ||
                            split.arguments.size() != 1 ||
                            local.array_element_type != ScalarType::String)
                        throw std::runtime_error("unchecked scoped String.split_literal owner");
                    split_source = lower(*split.left, builder, i64,
                        storage, field_storage, declarations, &region_moves);
                    split_delimiter = lower(*split.arguments.front(), builder, i64,
                        storage, field_storage, declarations, &region_moves);
                }
                init = new_checked_array(local.name);
                result.array_shadow = array_failure_places.at(local.name);
                array_names.insert(local.name);
                if (local.initializer) {
                    if (!array_region || !array_failure || !array_failure_status)
                        throw std::runtime_error("scoped String.split_literal lacks array region");
                    auto split_call = module.getOrInsertFunction("ores_string_split_literal",
                        llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                            {native_ptr, i64, i64, i64}, false));
                    auto* status = builder.CreateCall(split_call,
                        {array_region, init, split_source, split_delimiter},
                        local.name + ".scoped.split.status");
                    auto* ok = llvm::BasicBlock::Create(
                        context, local.name + ".scoped.split.ok", function);
                    auto* denied = llvm::BasicBlock::Create(
                        context, local.name + ".scoped.split.denied", function);
                    builder.CreateCondBr(builder.CreateICmpEQ(status,
                        llvm::ConstantInt::get(status->getType(), 0)), ok, denied);
                    builder.SetInsertPoint(denied);
                    builder.CreateStore(status, array_failure_status);
                    builder.CreateBr(array_failure);
                    builder.SetInsertPoint(ok);
                }
            }
            if (record) {
                const auto& cls = *record_classes.at(local.class_name);
                auto* schema = llvm::ConstantInt::get(i64, record_schemas.at(cls.name));
                builder.CreateCall(record_check, {init, schema});
                for (std::size_t i = 0; i < cls.fields.size(); ++i) {
                    auto* value = builder.CreateCall(record_get, {init, schema, llvm::ConstantInt::get(i64, i)});
                    auto* address = builder.CreateStructGEP(class_types.at(local.class_name), slot, static_cast<unsigned>(i));
                    builder.CreateStore(value, address);
                    const auto key = field_key(local.name, cls.fields[i].name);
                    if (!field_storage.emplace(key, address).second) throw std::runtime_error("scoped record field collision");
                    result.fields.push_back(key);
                }
            } else builder.CreateStore(init, slot);
            if (!storage.emplace(local.name, slot).second) throw std::runtime_error("scoped value shadows existing storage");
            if (record) result.subobjects = register_subobjects(local.name, local.class_name, slot);
            return result;
        };
        auto finish_scope = [&](const ScopedSlot& slot) {
            if (slot.array_shadow) {
                // The cleanup shadow outlives lexical storage and is also
                // consumed by the resource-failure epilogue. Retire both
                // slots before the next loop iteration can allocate again.
                auto* handle = builder.CreateLoad(i64, slot.address);
                auto* drop = llvm::BasicBlock::Create(context, "scope.array.drop", function);
                auto* next = llvm::BasicBlock::Create(context, "scope.array.drop.next", function);
                builder.CreateCondBr(builder.CreateICmpNE(handle, llvm::ConstantInt::get(i64, 0)), drop, next);
                builder.SetInsertPoint(drop);
                builder.CreateCall(arrayDrop, {array_region, handle});
                builder.CreateStore(llvm::ConstantInt::get(i64, 0), slot.address);
                builder.CreateStore(llvm::ConstantInt::get(i64, 0), slot.array_shadow);
                builder.CreateBr(next);
                builder.SetInsertPoint(next);
            }
            if (!slot.array_shadow) native_lifetime_end(builder, slot.address, slot.bytes);
        };
        auto forget_array_scope = [&](const ScopedSlot& slot) {
            if (slot.array_shadow) {
                array_names.erase(slot.name);
                array_failure_places.erase(slot.name);
            }
        };
        auto end_scope = [&](const ScopedSlot& slot) {
            finish_scope(slot);
            forget_array_scope(slot);
            for (const auto& key : slot.subobjects) { storage.erase(key); class_array_regions.erase(key); }
            for (const auto& key : slot.fields) field_storage.erase(key);
            storage.erase(slot.name);
        };
        std::vector<ScopedSlot> active_scopes;
        // Compiler-owned opaque span tokens: not visible to guest expressions.
        // This stack represents only the non-suspending region being lowered.
        std::vector<llvm::Value*> active_telemetry_scopes;
        std::vector<ScopedSlot> ordered_root_scopes;
        auto forget_scope = [&](const ScopedSlot& slot) {
            forget_array_scope(slot);
            for (const auto& key : slot.subobjects) { storage.erase(key); class_array_regions.erase(key); }
            for (const auto& key : slot.fields) field_storage.erase(key);
            storage.erase(slot.name);
        };
        std::function<void(const std::vector<Statement>&)> lower_arm;
        std::function<void(const Statement&)> lower_control;
        lower_arm = [&](const std::vector<Statement>& arm) {
            const auto scope_start = active_scopes.size();
            for (const auto& action : arm) {
                if (builder.GetInsertBlock()->getTerminator()) break;
                if (action.kind == Statement::Kind::ForEach || action.kind == Statement::Kind::ForIndex || action.kind == Statement::Kind::Conditional || action.kind == Statement::Kind::Return || action.kind == Statement::Kind::UsingTelemetrySpan) {
                    lower_control(action); continue;
                }
                if (action.kind != Statement::Kind::ScopedLocal) {
                    lower_effect(action);
                    continue;
                }
                const LocalBinding& local = *action.branch_local;
                llvm::Value* init = (local.initializer && !local.int_array)
                    ? lower(*local.initializer, builder, i64,
                        storage, field_storage, declarations, &region_moves) : nullptr;
                active_scopes.push_back(scoped_value(local, init));
            }
            while (active_scopes.size() > scope_start) {
                const auto scope = active_scopes.back(); active_scopes.pop_back();
                if (!builder.GetInsertBlock()->getTerminator()) end_scope(scope);
                else forget_scope(scope);
            }
        };
        lower_control = [&](const Statement& stmt) {
            if (stmt.kind == Statement::Kind::UsingTelemetrySpan) {
                if (!has_native_telemetry || !telemetryBegin.getCallee()
                        || !telemetryEnd.getCallee())
                    throw std::runtime_error("LLVM native telemetry ABI missing");
                llvm::Value* name = builder.CreateGlobalStringPtr(
                    stmt.telemetry_operation, "atto.telemetry.operation");
                llvm::Value* token = builder.CreateCall(
                    telemetryBegin,
                    {name, llvm::ConstantInt::get(i64, stmt.telemetry_operation.size())},
                    "atto.telemetry.scope.token");
                active_telemetry_scopes.push_back(token);
                lower_arm(stmt.then_statements);
                active_telemetry_scopes.pop_back();
                if (!builder.GetInsertBlock()->getTerminator())
                    builder.CreateCall(telemetryEnd, {token});
                return;
            }
            if (stmt.kind == Statement::Kind::Return) {
                auto* result = stmt.expression ? lower(*stmt.expression, builder, i64, storage, field_storage, declarations, &region_moves) : nullptr;
                if (!fn.returns_void) builder.CreateStore(result, structured_snapshot);
                // End only the live lexical span stack, LIFO, before dropping
                // temporary owners and taking the structured cleanup edge.
                for (auto it = active_telemetry_scopes.rbegin();
                     it != active_telemetry_scopes.rend(); ++it)
                    builder.CreateCall(telemetryEnd, {*it});
                for (auto it = active_scopes.rbegin(); it != active_scopes.rend(); ++it)
                    finish_scope(*it);
                builder.CreateBr(structured_cleanup); return;
            }
            if (stmt.kind == Statement::Kind::ForIndex) {
                auto* initial = lower(*stmt.branch_local->initializer, builder, i64, storage, field_storage, declarations, &region_moves);
                const auto counter = scoped_value(*stmt.branch_local, initial); active_scopes.push_back(counter);
                auto* header = llvm::BasicBlock::Create(context, "for.index.header", function);
                auto* body = llvm::BasicBlock::Create(context, "for.index.body", function);
                auto* exit = llvm::BasicBlock::Create(context, "for.index.exit", function);
                builder.CreateBr(header); builder.SetInsertPoint(header);
                auto* condition = lower(*stmt.condition, builder, i64, storage, field_storage, declarations, &region_moves);
                builder.CreateCondBr(builder.CreateICmpNE(condition, llvm::ConstantInt::get(i64, 0)), body, exit);
                builder.SetInsertPoint(body); lower_arm(stmt.then_statements);
                if (!builder.GetInsertBlock()->getTerminator()) {
                    auto* next = lower(*stmt.loop_step, builder, i64, storage, field_storage, declarations, &region_moves);
                    builder.CreateStore(next, counter.address); builder.CreateBr(header);
                }
                active_scopes.pop_back(); builder.SetInsertPoint(exit); end_scope(counter); return;
            }
            if (stmt.kind == Statement::Kind::ForEach) {
                auto* selected = receiver_array_region(*stmt.expression, &region_moves);
                auto* handle = builder.CreateLoad(i64, array_handle_address(*stmt.expression, storage, field_storage, &region_moves));
                auto* length = builder.CreateCall(arrayLength, {selected, handle});
                auto* entrance = builder.GetInsertBlock();
                auto* header = llvm::BasicBlock::Create(context, "foreach.header", function);
                auto* body = llvm::BasicBlock::Create(context, "foreach.body", function);
                auto* exit = llvm::BasicBlock::Create(context, "foreach.exit", function);
                builder.CreateBr(header); builder.SetInsertPoint(header);
                auto* index = builder.CreatePHI(i64, 2, "foreach.index");
                index->addIncoming(llvm::ConstantInt::get(i64, 0), entrance);
                builder.CreateCondBr(builder.CreateICmpULT(index, length), body, exit);
                builder.SetInsertPoint(body);
                auto* element = builder.CreateCall(arrayGet, {selected, handle, index});
                auto scope = scoped_value(*stmt.branch_local, element); active_scopes.push_back(scope);
                lower_arm(stmt.then_statements); active_scopes.pop_back();
                if (!builder.GetInsertBlock()->getTerminator()) {
                    end_scope(scope);
                    auto* next = builder.CreateAdd(index, llvm::ConstantInt::get(i64, 1));
                    auto* tail = builder.GetInsertBlock(); builder.CreateBr(header); index->addIncoming(next, tail);
                } else forget_scope(scope);
                builder.SetInsertPoint(exit); return;
            }
            llvm::Value* condition = nullptr;
            if (stmt.condition) condition = lower(*stmt.condition, builder, i64, storage, field_storage, declarations, &region_moves);
            else {
                auto* a = lower(*stmt.condition_lhs, builder, i64, storage, field_storage, declarations, &region_moves);
                auto* b = lower(*stmt.condition_rhs, builder, i64, storage, field_storage, declarations, &region_moves);
                condition = builder.CreateZExt(builder.CreateICmpEQ(a, b), i64);
            }
            auto* yes = llvm::BasicBlock::Create(context, "if.then", function);
            auto* no = llvm::BasicBlock::Create(context, "if.else", function);
            auto* join = llvm::BasicBlock::Create(context, "if.merge", function);
            builder.CreateCondBr(builder.CreateICmpNE(condition, llvm::ConstantInt::get(i64, 0)), yes, no);
            builder.SetInsertPoint(yes); lower_arm(stmt.then_statements); const bool yes_live = !builder.GetInsertBlock()->getTerminator(); if (yes_live) builder.CreateBr(join);
            builder.SetInsertPoint(no); lower_arm(stmt.else_statements); const bool no_live = !builder.GetInsertBlock()->getTerminator(); if (no_live) builder.CreateBr(join);
            builder.SetInsertPoint(join); if (!yes_live && !no_live) builder.CreateUnreachable();
        };
        if (fn.caught_throw_value) {
            // Real native CFG edges. Evaluate payload once, run scoped LIFO
            // cleanups, and transfer the typed scalar into the catch binding.
            llvm::Value* thrown = lower(*fn.caught_throw_value, builder, i64,
                storage, field_storage, declarations, &region_moves);
            llvm::Value* caught = builder.CreateAlloca(i64, nullptr, "catch.i64.slot");
            builder.CreateStore(thrown, caught);
            llvm::BasicBlock* unwind = llvm::BasicBlock::Create(
                context, "throw.cleanup", function);
            llvm::BasicBlock* handler = llvm::BasicBlock::Create(
                context, "throw.catch", function);
            builder.CreateBr(unwind);
            builder.SetInsertPoint(unwind);
            for (auto it = fn.caught_throw_defers.rbegin();
                 it != fn.caught_throw_defers.rend(); ++it) {
                llvm::Value* updated = lower(*it->expression, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                builder.CreateStore(updated, field_storage.at(
                    field_key(it->receiver, it->field)));
            }
            builder.CreateBr(handler);
            builder.SetInsertPoint(handler);
            if (!storage.emplace(fn.catch_binding, caught).second)
                throw std::runtime_error("duplicate native caught throw binding");
        }
        for (const auto& stmt : fn.statements) {
            if (stmt.kind == Statement::Kind::Defer) {
                deferred_effects.push_back(&stmt);
                continue;
            }
            if (builder.GetInsertBlock()->getTerminator()) break;
            if (stmt.kind == Statement::Kind::ScopedLocal) {
                const auto& local = *stmt.branch_local;
                auto* value = (local.initializer && !local.int_array)
                    ? lower(*local.initializer, builder, i64, storage, field_storage, declarations, &region_moves)
                    : nullptr;
                ordered_root_scopes.push_back(scoped_value(local, value));
                continue;
            }
            if (stmt.kind == Statement::Kind::ForEach || stmt.kind == Statement::Kind::ForIndex ||
                    stmt.kind == Statement::Kind::Conditional || stmt.kind == Statement::Kind::UsingTelemetrySpan) {
                lower_control(stmt); continue;
            }
            lower_effect(stmt);
        }

        /*
         * A lifecycle await splits trusted-main execution at one verified point.
         * get_ready resumes before stop; get_done requests stop first and resumes
         * only after the scheduler's physical terminal acknowledgement. Neither
         * path blocks an actor carrier.
         */
        llvm::Value* awaited_result = nullptr;
        if (!fn.ready_await_actor.empty()) {
            auto* continuation_type =
                llvm::FunctionType::get(i64, {native_ptr}, false);
            auto* resume = llvm::Function::Create(
                continuation_type,
                llvm::Function::InternalLinkage,
                function->getName().str() + ".ready_continuation",
                module);
            auto* admitted =
                llvm::BasicBlock::Create(context, "admitted", resume);
            llvm::IRBuilder<> resumed(admitted);
            auto* resumed_actor = &*resume->arg_begin();
            resumed_actor->setName(fn.ready_await_actor);

            const std::unordered_map<std::string, llvm::Value*> no_storage;
            const std::unordered_map<std::string, llvm::Value*> no_fields;
            for (const auto& action : fn.actor_actions) {
                if (action.kind != ActorAction::Kind::Send) continue;
                const auto& send = fn.actor_sends.at(action.index);
                llvm::Value* payload = lower(
                    *send.payload,
                    resumed,
                    i64,
                    no_storage,
                    no_fields,
                    declarations,
                    nullptr);
                if (send.reference != fn.ready_await_actor)
                    throw std::runtime_error(
                        "await continuation references unexpected ActorRef");
                resumed.CreateCall(sourceSend, {resumed_actor, payload});
            }
            llvm::Value* continuation_result = lower(
                *fn.result,
                resumed,
                i64,
                no_storage,
                no_fields,
                declarations,
                nullptr);
            resumed.CreateRet(continuation_result);

            llvm::Value* continuation_ptr =
                builder.CreatePointerCast(resume, native_ptr);
            awaited_result = builder.CreateCall(
                sourceAwaitReady,
                {
                    actorRefs.at(fn.ready_await_actor),
                    continuation_ptr,
                    llvm::ConstantInt::get(
                        llvm::cast<llvm::IntegerType>(i64),
                        static_cast<std::uint64_t>(fn.ready_await_offset))
                },
                "future.await.result");

            for (const auto& action : fn.actor_actions) {
                if (action.kind == ActorAction::Kind::Stop)
                    builder.CreateCall(
                        sourceStop,
                        {actorRefs.at(fn.actor_stops.at(action.index))});
            }
        } else if (!fn.done_await_actor.empty()) {
            /*
             * get_done is a terminal lifecycle await. All observable actor
             * effects occur before the boundary; stop itself is only a
             * nonblocking scheduler request. The trusted runtime waits for
             * physical quiescence and resumes this scalar continuation.
             */
            for (const auto& action : fn.actor_actions) {
                if (action.kind == ActorAction::Kind::Send) {
                    const auto& send = fn.actor_sends.at(action.index);
                    llvm::Value* payload = lower(
                        *send.payload,
                        builder,
                        i64,
                        storage,
                        field_storage,
                        declarations,
                        &region_moves);
                    builder.CreateCall(
                        sourceSend,
                        {actorRefs.at(send.reference), payload});
                } else {
                    const auto& stopped = fn.actor_stops.at(action.index);
                    if (stopped != fn.done_await_actor)
                        throw std::runtime_error(
                            "get_done continuation references unexpected ActorRef");
                    builder.CreateCall(
                        sourceRequestStop,
                        {actorRefs.at(stopped)});
                }
            }

            auto* continuation_type =
                llvm::FunctionType::get(i64, {native_ptr}, false);
            auto* resume = llvm::Function::Create(
                continuation_type,
                llvm::Function::InternalLinkage,
                function->getName().str() + ".done_continuation",
                module);
            auto* admitted =
                llvm::BasicBlock::Create(context, "admitted", resume);
            llvm::IRBuilder<> resumed(admitted);
            auto* resumed_actor = &*resume->arg_begin();
            resumed_actor->setName(fn.done_await_actor);

            const std::unordered_map<std::string, llvm::Value*> no_storage;
            const std::unordered_map<std::string, llvm::Value*> no_fields;
            llvm::Value* continuation_result = lower(
                *fn.result,
                resumed,
                i64,
                no_storage,
                no_fields,
                declarations,
                nullptr);
            resumed.CreateRet(continuation_result);

            llvm::Value* continuation_ptr =
                builder.CreatePointerCast(resume, native_ptr);
            awaited_result = builder.CreateCall(
                sourceAwaitDone,
                {
                    actorRefs.at(fn.done_await_actor),
                    continuation_ptr,
                    llvm::ConstantInt::get(
                        llvm::cast<llvm::IntegerType>(i64),
                        static_cast<std::uint64_t>(fn.done_await_offset))
                },
                "future.await.done.result");
        } else {
            // Emit observable actor effects in source order. Reordering all
            // sends ahead of all stops executes stopped workers' messages.
            for (const auto& action : fn.actor_actions) {
                if (action.kind == ActorAction::Kind::Send) {
                    const auto& send = fn.actor_sends.at(action.index);
                    llvm::Value* payload = lower(
                        *send.payload,
                        builder,
                        i64,
                        storage,
                        field_storage,
                        declarations,
                        &region_moves);
                    builder.CreateCall(
                        sourceSend,
                        {actorRefs.at(send.reference), payload});
                } else {
                    builder.CreateCall(
                        sourceStop,
                        {actorRefs.at(fn.actor_stops.at(action.index))});
                }
            }
        }
        // One cleanup authority for every guarded-return path. A function
        // with only its final return and no registered defer has exactly one
        // exit by construction, so preserve its direct ret IR contract.
        // That fast path still runs the same drop and lifetime cleanup.
        std::function<llvm::Value*(const Expression&)> lower_return;
        lower_return = [&](const Expression& expr) -> llvm::Value* {
            if (!owning_return) return lower(expr, builder, i64, storage, field_storage, declarations, &region_moves);
            if (expr.kind == Expression::Kind::Call) {
                RegionMoveLowering forwarding = region_moves;
                forwarding.result_destination = result_destination;
                forwarding.result_region = result_region;
                return lower(expr, builder, i64, storage, field_storage, declarations, &forwarding);
            }
            if (expr.kind == Expression::Kind::Conditional) {
                llvm::Value* condition = expr.condition
                    ? lower(*expr.condition, builder, i64, storage, field_storage, declarations, &region_moves)
                    : builder.CreateZExt(builder.CreateICmpEQ(
                        lower(*expr.condition_lhs, builder, i64, storage, field_storage, declarations, &region_moves),
                        lower(*expr.condition_rhs, builder, i64, storage, field_storage, declarations, &region_moves)), i64);
                auto* yes = llvm::BasicBlock::Create(context, "owning.return.yes", function);
                auto* no = llvm::BasicBlock::Create(context, "owning.return.no", function);
                auto* join = llvm::BasicBlock::Create(context, "owning.return.join", function);
                builder.CreateCondBr(builder.CreateICmpNE(condition, llvm::ConstantInt::get(i64, 0)), yes, no);
                builder.SetInsertPoint(yes); auto* a = lower_return(*expr.when_true); auto* a_exit = builder.GetInsertBlock(); builder.CreateBr(join);
                builder.SetInsertPoint(no); auto* b = lower_return(*expr.when_false); auto* b_exit = builder.GetInsertBlock(); builder.CreateBr(join);
                builder.SetInsertPoint(join); auto* value = builder.CreatePHI(i64, 2);
                value->addIncoming(a, a_exit); value->addIncoming(b, b_exit); return value;
            }
            if (!array_region || !array_failure)
                throw std::runtime_error("owning return lacks local cleanup authority");
            const auto& cls = *record_classes.at(fn.return_class);
            auto* shape = class_types.at(fn.return_class);
            std::unordered_map<std::string, llvm::Value*> parameters;
            std::unordered_map<std::string, std::string> constructor_sources;
            if (expr.kind == Expression::Kind::RecordConstruct) {
                for (std::size_t i = 0; i < expr.arguments.size(); ++i) {
                    const auto* original = constructor_field(layouts.source(fn.return_class), i);
                    const auto& argument = *expr.arguments[i];
                    const auto& child = argument.kind == Expression::Kind::Take ? *argument.left : argument;
                    if (original && !original->class_name.empty()) {
                        for (const auto& leaf : layouts.at(original->class_name).leaves) {
                            const auto key = field_key(original->name, leaf.path);
                            const auto source = field_key(child.name, leaf.path);
                            auto* slot = builder.CreateAlloca(i64, nullptr, "nested.return.argument");
                            builder.CreateStore(builder.CreateLoad(i64, field_storage.at(source)), slot);
                            parameters.emplace(key, slot); constructor_sources.emplace(key, source);
                        }
                        continue;
                    }
                    if (original && original->int_array) constructor_sources.emplace(original->name, child.name);
                    auto* value = lower(*expr.arguments[i], builder, i64, storage, field_storage, declarations, &region_moves);
                    auto* address = builder.CreateAlloca(i64, nullptr, "owning.return.argument");
                    builder.CreateStore(value, address); parameters.emplace(cls.constructor_parameters.at(i), address);
                }
            } else if (expr.kind != Expression::Kind::Variable) {
                throw std::runtime_error("unsupported owning aggregate return expression");
            }
            const std::size_t count = std::count_if(cls.fields.begin(), cls.fields.end(), [](const ClassField& f) { return f.int_array; });
            auto* destinations = builder.CreateAlloca(loan_ptr, llvm::ConstantInt::get(i64, count));
            auto* sources = builder.CreateAlloca(loan_ptr, llvm::ConstantInt::get(i64, count));
            std::vector<llvm::Value*> shadows;
            std::size_t position = 0;
            const std::unordered_map<std::string, llvm::Value*> no_fields;
            for (std::size_t i = 0; i < cls.fields.size(); ++i) {
                const auto& field = cls.fields[i];
                auto* destination = builder.CreateStructGEP(shape, result_destination, static_cast<unsigned>(i));
                if (field.int_array) {
                    std::string source_key;
                    llvm::Value* source_place = nullptr;
                    if (expr.kind == Expression::Kind::Variable) {
                        source_key = field_key(expr.name, field.name);
                        source_place = field_storage.at(source_key);
                    } else {
                        source_key = constructor_sources.at(field.name);
                        source_place = storage.count(source_key) ? storage.at(source_key) : field_storage.at(source_key);
                    }
                    auto* index = llvm::ConstantInt::get(i64, position++);
                    builder.CreateStore(destination, builder.CreateGEP(loan_ptr, destinations, index));
                    builder.CreateStore(source_place, builder.CreateGEP(loan_ptr, sources, index));
                    shadows.push_back(array_failure_places.at(source_key));
                } else {
                    auto* value = expr.kind == Expression::Kind::Variable
                        ? builder.CreateLoad(i64, field_storage.at(field_key(expr.name, field.name)))
                        : lower(*field.initializer_expression, builder, i64, parameters, no_fields, declarations, &region_moves);
                    builder.CreateStore(value, destination);
                }
            }
            auto* status = builder.CreateCall(arrayTransferMany, {array_region, result_region,
                llvm::ConstantInt::get(i64, count), destinations, sources});
            auto* ok = llvm::BasicBlock::Create(context, "owning.return.transferred", function);
            auto* denied = llvm::BasicBlock::Create(context, "owning.return.denied", function);
            builder.CreateCondBr(builder.CreateICmpEQ(status, llvm::ConstantInt::get(status->getType(), 0)), ok, denied);
            builder.SetInsertPoint(denied); builder.CreateStore(status, array_failure_status); builder.CreateBr(array_failure);
            builder.SetInsertPoint(ok);
            for (auto* shadow : shadows) builder.CreateStore(llvm::ConstantInt::get(i64, 0), shadow);
            return llvm::ConstantInt::get(i64, 0); // owning ABI publishes through caller storage
        };
        auto emit_lexical_cleanup = [&]() {
            // Root lexical locals can be referenced by deferred cleanup.
            // Their lifetimes end after defers, unlike branch temporaries.
            for (auto it = ordered_root_scopes.rbegin(); it != ordered_root_scopes.rend(); ++it)
                finish_scope(*it);
            for (auto it = lexical_slots.rbegin(); it != lexical_slots.rend(); ++it) {
                if (it->actor_region_object) {
                    auto* i32 = llvm::Type::getInt32Ty(context);
                    llvm::Value* offset = builder.CreateLoad(i32, it->address,
                        it->name + ".actor.region.drop.offset");
                    builder.CreateCall(actorRegionGuestDrop, {offset});
                    builder.CreateStore(llvm::ConstantInt::get(i32, 0), it->address);
                } else if (it->int_array) {
                    llvm::Value* handle = builder.CreateLoad(i64,
                        it->address, it->name + ".array.drop.handle");
                    // A lexical rt take disarms its previous owner slot.
                    // Reject duplicate drops of moved handles by skipping 0.
                    llvm::BasicBlock* do_drop = llvm::BasicBlock::Create(
                        context, it->name + ".array.drop.live", function);
                    llvm::BasicBlock* after_drop = llvm::BasicBlock::Create(
                        context, it->name + ".array.drop.next", function);
                    builder.CreateCondBr(builder.CreateICmpNE(handle,
                        llvm::ConstantInt::get(i64, 0)), do_drop, after_drop);
                    builder.SetInsertPoint(do_drop);
                    builder.CreateCall(arrayDrop, {array_region, handle});
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0), it->address);
                    builder.CreateBr(after_drop);
                    builder.SetInsertPoint(after_drop);
                } else if (it->heap_owned) {
                    llvm::Value* flag = owned_live_flags.at(it->name);
                    llvm::Value* live = builder.CreateLoad(builder.getInt1Ty(), flag,
                        it->name + ".drop.if.live");
                    llvm::BasicBlock* do_drop = llvm::BasicBlock::Create(
                        context, it->name + ".drop.yes", function);
                    llvm::BasicBlock* after_drop = llvm::BasicBlock::Create(
                        context, it->name + ".drop.next", function);
                    builder.CreateCondBr(live, do_drop, after_drop);
                    builder.SetInsertPoint(do_drop);
                    llvm::Value* handle = builder.CreateLoad(i64,
                        it->address, it->name + ".drop.handle");
                    builder.CreateCall(regionDrop, {region, handle});
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0), it->address);
                    builder.CreateStore(builder.getFalse(), flag);
                    builder.CreateBr(after_drop);
                    builder.SetInsertPoint(after_drop);
                } else {
                    // A class owns each array handle independently. Drop
                    // fields in reverse declaration order before ending the
                    // containing record lifetime (no aliases may escape).
                    for (auto fld = it->owned_array_fields.rbegin();
                         fld != it->owned_array_fields.rend(); ++fld) {
                        llvm::Value* address = field_storage.at(*fld);
                        llvm::Value* handle = builder.CreateLoad(i64, address,
                            *fld + ".drop.handle");
                        // Whole-class moves clear the source field to zero.
                        // Skip disarmed places, otherwise the trusted runtime
                        // correctly treats drop(0) as a fatal invalid handle.
                        llvm::BasicBlock* do_drop = llvm::BasicBlock::Create(
                            context, *fld + ".drop.live", function);
                        llvm::BasicBlock* after_drop = llvm::BasicBlock::Create(
                            context, *fld + ".drop.next", function);
                        builder.CreateCondBr(builder.CreateICmpNE(handle,
                            llvm::ConstantInt::get(i64, 0)), do_drop, after_drop);
                        builder.SetInsertPoint(do_drop);
                        builder.CreateCall(arrayDrop, {array_region, handle});
                        builder.CreateStore(llvm::ConstantInt::get(i64, 0), address);
                        builder.CreateBr(after_drop);
                        builder.SetInsertPoint(after_drop);
                    }
                    // llvm.lifetime.end is not a source-level destructor.
                    native_lifetime_end(builder, it->address, it->bytes);
                }
            }
            if (array_region != nullptr) builder.CreateCall(arrayClose, {array_region});
            if (region != nullptr) builder.CreateCall(regionClose, {region});
        };
        if (fn.early_returns.empty() && deferred_effects.empty() && !structured_returns) {
            // Maintain stable native IR (ret i64 42, for example) for
            // functions without a multi-exit cleanup obligation.
            llvm::Value* result = awaited_result
                ? awaited_result
                : (fn.result
                    ? lower_return(*fn.result)
                    : nullptr);
            emit_lexical_cleanup();
            if (fn.returns_void) builder.CreateRetVoid();
            else builder.CreateRet(result);
        } else {
            llvm::Value* return_snapshot = fn.returns_void
                ? nullptr : (structured_returns ? structured_snapshot : builder.CreateAlloca(i64, nullptr, "return.snapshot.slot"));
            llvm::BasicBlock* cleanup = structured_returns ? structured_cleanup : llvm::BasicBlock::Create(
                context, "exit.cleanup", function);
            auto branch_to_cleanup = [&](llvm::Value* value) {
                if (!fn.returns_void) builder.CreateStore(value, return_snapshot);
                builder.CreateBr(cleanup);
            };
            if (!builder.GetInsertBlock()->getTerminator()) {
                for (const auto& guard : fn.early_returns) {
                    llvm::Value* conditionValue = nullptr;
                    if (guard.condition) {
                        conditionValue = lower(*guard.condition, builder, i64, storage, field_storage,
                            declarations, &region_moves);
                    } else {
                        llvm::Value* left = lower(*guard.lhs, builder, i64, storage, field_storage,
                            declarations, &region_moves);
                        llvm::Value* right = lower(*guard.rhs, builder, i64, storage, field_storage,
                            declarations, &region_moves);
                        conditionValue = builder.CreateZExt(
                            builder.CreateICmpEQ(left, right, "return.guard.eq.i1"),
                            i64, "return.guard.eq");
                    }
                    llvm::Value* takenCondition = builder.CreateICmpNE(
                        conditionValue, llvm::ConstantInt::get(i64, 0), "return.guard.cond.i1");
                    llvm::BasicBlock* taken = llvm::BasicBlock::Create(context, "guard.return", function);
                    llvm::BasicBlock* next = llvm::BasicBlock::Create(context, "guard.next", function);
                    builder.CreateCondBr(takenCondition, taken, next);
                    builder.SetInsertPoint(taken);
                    llvm::Value* selected = lower_return(*guard.result);
                    branch_to_cleanup(selected);
                    builder.SetInsertPoint(next);
                }
                llvm::Value* result = awaited_result
                    ? awaited_result
                    : (fn.result
                        ? lower_return(*fn.result)
                        : nullptr);
                branch_to_cleanup(result);
            }
            builder.SetInsertPoint(cleanup);
            // Capture the return before deferred calls can mutate class fields.
            for (auto it = deferred_effects.rbegin(); it != deferred_effects.rend(); ++it) {
                const Statement& effect = **it;
                if (effect.receiver.empty()) {
                    lower(*effect.expression, builder, i64, storage, field_storage,
                        declarations, &region_moves);
                } else {
                    llvm::Value* updated = lower(*effect.expression, builder, i64,
                        storage, field_storage, declarations, &region_moves);
                    builder.CreateStore(updated, field_storage.at(
                        field_key(effect.receiver, effect.field)));
                }
            }
            emit_lexical_cleanup();
            if (fn.returns_void) builder.CreateRetVoid();
            else builder.CreateRet(builder.CreateLoad(i64, return_snapshot, "return.snapshot"));
        }
        if (array_failure) {
            builder.SetInsertPoint(array_failure);
            // The shadow owner slots contain 0 until the corresponding
            // checked new call succeeds; drop in reverse construction order.
            for (std::size_t k = array_owner_count; k != 0; --k) {
                llvm::Value* address = builder.CreateGEP(i64, array_handle_shadows,
                    llvm::ConstantInt::get(i64, k - 1), "array.failure.drop.slot");
                llvm::Value* handle = builder.CreateLoad(i64, address, "array.failure.handle");
                llvm::BasicBlock* drop = llvm::BasicBlock::Create(
                    context, "array.failure.drop", function);
                llvm::BasicBlock* next = llvm::BasicBlock::Create(
                    context, "array.failure.next", function);
                builder.CreateCondBr(builder.CreateICmpNE(handle,
                    llvm::ConstantInt::get(i64, 0)), drop, next);
                builder.SetInsertPoint(drop);
                builder.CreateCall(arrayDrop, {array_region, handle});
                builder.CreateStore(llvm::ConstantInt::get(i64, 0), address);
                builder.CreateBr(next);
                builder.SetInsertPoint(next);
            }
            if (array_region != nullptr) builder.CreateCall(arrayClose, {array_region});
            if (region != nullptr) {
                for (auto it = heap_failure_slots.rbegin();
                     it != heap_failure_slots.rend(); ++it) {
                    llvm::Value* handle = builder.CreateLoad(i64, *it,
                        "owned.failure.handle");
                    llvm::BasicBlock* drop = llvm::BasicBlock::Create(
                        context, "owned.failure.drop", function);
                    llvm::BasicBlock* next = llvm::BasicBlock::Create(
                        context, "owned.failure.next", function);
                    builder.CreateCondBr(builder.CreateICmpNE(handle,
                        llvm::ConstantInt::get(i64, 0)), drop, next);
                    builder.SetInsertPoint(drop);
                    builder.CreateCall(regionDrop, {region, handle});
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0), *it);
                    builder.CreateBr(next);
                    builder.SetInsertPoint(next);
                }
                builder.CreateCall(regionClose, {region});
            }
            // Preserve the exact checked status through cleanup, refusing
            // unknown or success status tags before guest-level error lowering.
            // Both expected resource errors still terminate here: a typed
            // throw/Result continuation MUST NOT resume with dropped owners.
            llvm::Value* typed_status = builder.CreateLoad(
                llvm::Type::getInt32Ty(context), array_failure_status,
                "array.failure.typed.status");
            llvm::BasicBlock* quota_error = llvm::BasicBlock::Create(
                context, "array.failure.limit", function);
            llvm::BasicBlock* memory_error = llvm::BasicBlock::Create(
                context, "array.failure.no-memory", function);
            llvm::BasicBlock* invalid_status = llvm::BasicBlock::Create(
                context, "array.failure.invalid-status", function);
            auto* dispatch = builder.CreateSwitch(typed_status, invalid_status, 2);
            dispatch->addCase(llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(context), 1), quota_error);
            dispatch->addCase(llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(context), 2), memory_error);
            for (llvm::BasicBlock* failure : {quota_error, memory_error}) {
                builder.SetInsertPoint(failure);
                // No return to a frame whose owners were already destroyed.
                builder.CreateCall(arrayUnhandled, {typed_status});
                builder.CreateUnreachable();
            }
            builder.SetInsertPoint(invalid_status);
            builder.CreateCall(arrayAbort, {});
            builder.CreateUnreachable();
        }
    }


    // Native sync generator v1: caller-owned pinned frame [state,index].
    // No guest/native stack survives a pull and no tracing-GC object is
    // introduced. ABI status is 1=YIELD, 0=DONE, -1=invalid activation.
    if (!source.generators.empty() && (wasm32_target || cortex_m_target))
        throw std::runtime_error(
            "native generator v1 is not yet admitted for wasm32/Cortex-M targets");
    for (const auto& generator : source.generators) {
        const auto linkage = generator.exported
            ? llvm::Function::ExternalLinkage
            : llvm::Function::InternalLinkage;
        const std::string prefix = "oreslang_rt_gen_" + generator.name + "_";
        auto* init = llvm::Function::Create(
            llvm::FunctionType::get(i32, {loan_ptr}, false),
            linkage, prefix + "init", module);
        auto* next = llvm::Function::Create(
            llvm::FunctionType::get(i32, {loan_ptr, loan_ptr}, false),
            linkage, prefix + "next", module);
        auto* close = llvm::Function::Create(
            llvm::FunctionType::get(i32, {loan_ptr}, false),
            linkage, prefix + "close", module);
        auto* zero = llvm::ConstantInt::get(i64, 0);
        auto* active_state = llvm::ConstantInt::get(i64, 1);
        auto* done_state = llvm::ConstantInt::get(i64, 2);
        const auto return_i32 = [&](llvm::IRBuilder<>& b, int value) {
            b.CreateRet(llvm::ConstantInt::getSigned(i32, value));
        };

        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", init));
            auto* frame = init->getArg(0);
            auto* nonnull = llvm::BasicBlock::Create(context, "nonnull", init);
            auto* invalid = llvm::BasicBlock::Create(context, "invalid", init);
            b.CreateCondBr(
                b.CreateICmpNE(frame, llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(loan_ptr))),
                nonnull, invalid);
            b.SetInsertPoint(invalid);
            return_i32(b, -1);
            b.SetInsertPoint(nonnull);
            auto* state = b.CreateLoad(i64, frame, "state");
            auto* fresh = llvm::BasicBlock::Create(context, "fresh", init);
            auto* active = llvm::BasicBlock::Create(context, "active", init);
            auto* done = llvm::BasicBlock::Create(context, "done", init);
            auto* invalid_state = llvm::BasicBlock::Create(context, "invalid.state", init);
            auto* state_switch = b.CreateSwitch(state, invalid_state, 3);
            state_switch->addCase(zero, fresh);
            state_switch->addCase(active_state, active);
            state_switch->addCase(done_state, done);
            b.SetInsertPoint(fresh);
            b.CreateStore(active_state, frame);
            b.CreateStore(zero, b.CreateInBoundsGEP(i64, frame,
                llvm::ConstantInt::get(i64, 1)));
            return_i32(b, 1);
            b.SetInsertPoint(active);
            auto* active_index_addr = b.CreateInBoundsGEP(
                i64, frame, llvm::ConstantInt::get(i64, 1));
            auto* active_index = b.CreateLoad(i64, active_index_addr, "active.index");
            auto* active_valid = llvm::BasicBlock::Create(context, "active.valid", init);
            auto* active_invalid = llvm::BasicBlock::Create(context, "active.invalid", init);
            b.CreateCondBr(
                b.CreateICmpULE(active_index,
                    llvm::ConstantInt::get(i64, generator.yields.size())),
                active_valid, active_invalid);
            b.SetInsertPoint(active_valid);
            return_i32(b, 0);
            b.SetInsertPoint(active_invalid);
            return_i32(b, -1);
            b.SetInsertPoint(done);
            auto* done_index_addr = b.CreateInBoundsGEP(
                i64, frame, llvm::ConstantInt::get(i64, 1));
            auto* done_index = b.CreateLoad(i64, done_index_addr, "done.index");
            auto* done_valid = llvm::BasicBlock::Create(context, "done.valid", init);
            auto* done_invalid = llvm::BasicBlock::Create(context, "done.invalid", init);
            b.CreateCondBr(b.CreateICmpEQ(done_index, zero), done_valid, done_invalid);
            b.SetInsertPoint(done_valid);
            return_i32(b, 0);
            b.SetInsertPoint(done_invalid);
            return_i32(b, -1);
            b.SetInsertPoint(invalid_state);
            return_i32(b, -1);
        }

        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", next));
            auto args = next->arg_begin();
            auto* frame = &*args++;
            auto* out = &*args;
            auto* nonnull = llvm::BasicBlock::Create(context, "nonnull", next);
            auto* null_invalid = llvm::BasicBlock::Create(context, "null.invalid", next);
            auto* null_pointer = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(loan_ptr));
            b.CreateCondBr(
                b.CreateAnd(b.CreateICmpNE(frame, null_pointer),
                            b.CreateICmpNE(out, null_pointer)),
                nonnull, null_invalid);
            b.SetInsertPoint(null_invalid);
            return_i32(b, -1);
            b.SetInsertPoint(nonnull);
            // The next() ABI writes a value through 'out' and advances the
            // two-word frame. Disallow output aliasing either frame slot:
            // otherwise out=frame could forge state and out=frame+1 could
            // overwrite the just-updated index (violating single delivery).
            auto* frame_index_addr = b.CreateInBoundsGEP(
                i64, frame, llvm::ConstantInt::get(i64, 1));
            auto* output_disjoint = b.CreateAnd(
                b.CreateICmpNE(out, frame),
                b.CreateICmpNE(out, frame_index_addr));
            auto* alias_invalid = llvm::BasicBlock::Create(
                context, "frame.output.alias", next);
            auto* alias_valid = llvm::BasicBlock::Create(
                context, "frame.output.disjoint", next);
            b.CreateCondBr(output_disjoint, alias_valid, alias_invalid);
            b.SetInsertPoint(alias_invalid);
            return_i32(b, -1);
            b.SetInsertPoint(alias_valid);
            auto* state = b.CreateLoad(i64, frame, "state");
            auto* active = llvm::BasicBlock::Create(context, "active", next);
            auto* done = llvm::BasicBlock::Create(context, "done", next);
            auto* invalid = llvm::BasicBlock::Create(context, "invalid", next);
            auto* state_switch = b.CreateSwitch(state, invalid, 2);
            state_switch->addCase(active_state, active);
            state_switch->addCase(done_state, done);
            b.SetInsertPoint(done);
            auto* done_index_addr = b.CreateInBoundsGEP(
                i64, frame, llvm::ConstantInt::get(i64, 1));
            auto* done_index = b.CreateLoad(i64, done_index_addr, "done.index");
            auto* done_valid = llvm::BasicBlock::Create(context, "done.valid", next);
            auto* done_invalid = llvm::BasicBlock::Create(context, "done.invalid", next);
            b.CreateCondBr(b.CreateICmpEQ(done_index, zero), done_valid, done_invalid);
            b.SetInsertPoint(done_valid);
            return_i32(b, 0);
            b.SetInsertPoint(done_invalid);
            return_i32(b, -1);
            b.SetInsertPoint(invalid);
            return_i32(b, -1);
            b.SetInsertPoint(active);
            auto* index_addr = b.CreateInBoundsGEP(
                i64, frame, llvm::ConstantInt::get(i64, 1));
            auto* index = b.CreateLoad(i64, index_addr, "yield.index");
            auto* exhausted = llvm::BasicBlock::Create(context, "exhausted", next);
            auto* yield_dispatch = llvm::BasicBlock::Create(context, "yield.dispatch", next);
            auto* invalid_index = llvm::BasicBlock::Create(context, "invalid.index.range", next);
            auto* yield_count = llvm::ConstantInt::get(i64, generator.yields.size());
            auto* in_range = b.CreateICmpULT(index, yield_count);
            auto* at_end = b.CreateICmpEQ(index, yield_count);
            auto* non_yield = llvm::BasicBlock::Create(context, "non.yield", next);
            b.CreateCondBr(in_range, yield_dispatch, non_yield);
            b.SetInsertPoint(non_yield);
            b.CreateCondBr(at_end, exhausted, invalid_index);
            b.SetInsertPoint(invalid_index);
            return_i32(b, -1);
            b.SetInsertPoint(exhausted);
            b.CreateStore(done_state, frame);
            b.CreateStore(zero, index_addr);
            return_i32(b, 0);
            b.SetInsertPoint(yield_dispatch);
            auto* bad_index = llvm::BasicBlock::Create(context, "bad.index", next);
            auto* index_switch = b.CreateSwitch(index, bad_index,
                static_cast<unsigned>(generator.yields.size()));
            for (std::size_t i = 0; i < generator.yields.size(); ++i) {
                auto* emit = llvm::BasicBlock::Create(
                    context, "yield." + std::to_string(i), next);
                index_switch->addCase(llvm::ConstantInt::get(i64, i), emit);
                b.SetInsertPoint(emit);
                b.CreateStore(
                    llvm::ConstantInt::getSigned(i64, generator.yields[i]), out);
                b.CreateStore(llvm::ConstantInt::get(i64, i + 1), index_addr);
                return_i32(b, 1);
            }
            b.SetInsertPoint(bad_index);
            return_i32(b, -1);
        }

        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", close));
            auto* frame = close->getArg(0);
            auto* nonnull = llvm::BasicBlock::Create(context, "nonnull", close);
            auto* null_invalid = llvm::BasicBlock::Create(context, "null.invalid", close);
            b.CreateCondBr(
                b.CreateICmpNE(frame, llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(loan_ptr))),
                nonnull, null_invalid);
            b.SetInsertPoint(null_invalid);
            return_i32(b, -1);
            b.SetInsertPoint(nonnull);
            auto* state = b.CreateLoad(i64, frame, "state");
            auto* active = llvm::BasicBlock::Create(context, "active", close);
            auto* done = llvm::BasicBlock::Create(context, "done", close);
            auto* invalid = llvm::BasicBlock::Create(context, "invalid", close);
            auto* state_switch = b.CreateSwitch(state, invalid, 2);
            state_switch->addCase(active_state, active);
            state_switch->addCase(done_state, done);
            b.SetInsertPoint(active);
            b.CreateStore(done_state, frame);
            b.CreateStore(zero, b.CreateInBoundsGEP(i64, frame,
                llvm::ConstantInt::get(i64, 1)));
            return_i32(b, 1);
            b.SetInsertPoint(done);
            auto* done_index_addr = b.CreateInBoundsGEP(
                i64, frame, llvm::ConstantInt::get(i64, 1));
            auto* done_index = b.CreateLoad(i64, done_index_addr, "done.index");
            auto* done_valid = llvm::BasicBlock::Create(context, "done.valid", close);
            auto* done_invalid = llvm::BasicBlock::Create(context, "done.invalid", close);
            b.CreateCondBr(b.CreateICmpEQ(done_index, zero), done_valid, done_invalid);
            b.SetInsertPoint(done_valid);
            return_i32(b, 0);
            b.SetInsertPoint(done_invalid);
            return_i32(b, -1);
            b.SetInsertPoint(invalid);
            return_i32(b, -1);
        }
    }

    // First compiler-generated heap continuation: exactly one frame-owned
    // scalar updated at a committed loop backedge. There is no actor or
    // scheduler admission here. The host owns the frame and must dispatch
    // on a single carrier with a strictly bounded fuel allowance.
    for (const auto& loop : source.cooperative_loops) {
        if (wasm32_target)
            throw std::runtime_error("wasm32 resumable source loop is unsupported");
        auto* i32 = llvm::Type::getInt32Ty(context);
        const std::string prefix = "oreslang_rt_" + loop.name + "_";
        auto* frame_type = loan_ptr;
        auto* initialize = llvm::Function::Create(
            llvm::FunctionType::get(i32, {frame_type}, false),
            llvm::Function::ExternalLinkage, prefix + "init", module);
        auto* resume = llvm::Function::Create(
            llvm::FunctionType::get(i32, {frame_type, i64}, false),
            llvm::Function::ExternalLinkage, prefix + "resume", module);
        auto* cancel = llvm::Function::Create(
            llvm::FunctionType::get(i32, {frame_type}, false),
            llvm::Function::ExternalLinkage, prefix + "cancel", module);
        auto* zero = llvm::ConstantInt::get(i64, 0);
        auto* one = llvm::ConstantInt::get(i64, 1);
        auto* two = llvm::ConstantInt::get(i64, 2);
        auto* max_fuel = llvm::ConstantInt::get(i64, 512);
        auto* init_value = llvm::ConstantInt::getSigned(i64, loop.initial);

        // INIT: only an uninitialized (zero-tagged) frame can be published.
        // Reinitializing a cancelled or active frame would resurrect work.
        {
            auto* entry = llvm::BasicBlock::Create(context, "entry", initialize);
            auto* publish = llvm::BasicBlock::Create(context, "publish", initialize);
            auto* invalid = llvm::BasicBlock::Create(context, "invalid", initialize);
            llvm::IRBuilder<> b(entry);
            auto* frame = initialize->getArg(0);
            auto* tag = b.CreateLoad(i64, frame, "state");
            auto* fresh = b.CreateICmpEQ(tag, zero);
            b.CreateCondBr(fresh, publish, invalid);
            b.SetInsertPoint(publish);
            auto* counter = b.CreateInBoundsGEP(i64, frame, one);
            b.CreateStore(init_value, counter);
            b.CreateStore(one, frame);
            b.CreateRet(llvm::ConstantInt::get(i32, 1));
            b.SetInsertPoint(invalid);
            b.CreateRet(llvm::ConstantInt::get(i32, 0));
        }
        // RESUME: cap the number of guest iterations independently of
        // untrusted host fuel; report YIELD only after a committed update.
        {
            auto* entry = llvm::BasicBlock::Create(context, "entry", resume);
            auto* body = llvm::BasicBlock::Create(context, "body", resume);
            auto* iteration = llvm::BasicBlock::Create(context, "iteration", resume);
            auto* handoff = llvm::BasicBlock::Create(context, "handoff", resume);
            auto* invalid = llvm::BasicBlock::Create(context, "invalid", resume);
            llvm::IRBuilder<> b(entry);
            auto* frame = resume->getArg(0);
            auto* fuel = resume->getArg(1);
            auto* state = b.CreateLoad(i64, frame, "state");
            auto* active = b.CreateICmpEQ(state, one);
            auto* valid_fuel = b.CreateICmpSGT(fuel, zero);
            b.CreateCondBr(b.CreateAnd(active, valid_fuel), body, invalid);
            b.SetInsertPoint(body);
            auto* bounded = b.CreateSelect(b.CreateICmpUGT(fuel, max_fuel),
                                            max_fuel, fuel, "bounded.fuel");
            b.CreateBr(iteration);
            b.SetInsertPoint(iteration);
            auto* remaining = b.CreatePHI(i64, 2, "remaining");
            remaining->addIncoming(bounded, body);
            auto* counter = b.CreateInBoundsGEP(i64, frame, one);
            auto* current = b.CreateLoad(i64, counter, "committed.counter");
            // Match the existing native i64 arithmetic: LLVM add without
            // 'nsw' or 'nuw' wraps modulo 2^64. Do not silently introduce
            // saturation into the resumable form of the same expression.
            // A guest-visible update is committed before the next yield.
            auto* next = b.CreateAdd(current, one, "next.counter");
            b.CreateStore(next, counter);
            auto* left = b.CreateSub(remaining, one);
            auto* expired = b.CreateICmpEQ(left, zero);
            b.CreateCondBr(expired, handoff, iteration);
            remaining->addIncoming(left, iteration);
            b.SetInsertPoint(handoff);
            b.CreateRet(llvm::ConstantInt::get(i32, 1)); // yielded
            b.SetInsertPoint(invalid);
            b.CreateRet(llvm::ConstantInt::getSigned(i32, -1));
        }
        // Cancellation is a trusted owner-turn action. Never interrupt
        // an in-flight native stack or revive a previously cancelled frame.
        {
            auto* entry = llvm::BasicBlock::Create(context, "entry", cancel);
            auto* retired = llvm::BasicBlock::Create(context, "retired", cancel);
            auto* invalid = llvm::BasicBlock::Create(context, "invalid", cancel);
            llvm::IRBuilder<> b(entry);
            auto* frame = cancel->getArg(0);
            b.CreateCondBr(b.CreateICmpEQ(b.CreateLoad(i64, frame), one),
                           retired, invalid);
            b.SetInsertPoint(retired);
            b.CreateStore(two, frame);
            b.CreateStore(zero, b.CreateInBoundsGEP(i64, frame, one));
            b.CreateRet(llvm::ConstantInt::get(i32, 1));
            b.SetInsertPoint(invalid);
            b.CreateRet(llvm::ConstantInt::get(i32, 0));
        }
    }

    // Native source actors execute only on OresVM scheduler turns. The
    // portable guest below uses an independent, no-import single-actor ABI.
    if (!wasm32_target) for (const auto& actor : source.actors) {
        auto* ctor = actorInits.at(actor.name);
        llvm::BasicBlock* initial = llvm::BasicBlock::Create(context, "entry", ctor);
        llvm::IRBuilder<> init(initial);
        auto a = ctor->arg_begin();
        llvm::Value* heap = &*a++;
        llvm::Value* domain = &*a++;
        llvm::Value* epoch = &*a++;
        const std::unordered_map<std::string, llvm::Value*> noStorage;
        const std::unordered_map<std::string, llvm::Function*> noFunctions;
        for (std::size_t i=0; i<actor.fields.size(); ++i) {
            const auto& field=actor.fields[i];
            llvm::Value* initialValue = lower(*field.initializer, init, i64,
                noStorage, noStorage, noFunctions);
            init.CreateCall(field.field_mutable ? fieldInitMutable : fieldInit,
                {heap, domain, epoch, llvm::ConstantInt::get(i64, i), initialValue});
        }
        init.CreateRetVoid();

        auto* handler = actorHandlers.at(actor.name);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", handler);
        llvm::IRBuilder<> builder(entry);
        auto args = handler->arg_begin();
        llvm::Value* actorHeap = &*args++;
        llvm::Value* currentDomain = &*args++;
        llvm::Value* currentEpoch = &*args++;
        llvm::Value* incoming = &*args++;
        std::unordered_map<std::string, llvm::Value*> values, emptyFields;
        const std::string param = actor.on_message.parameters[0].name;
        llvm::Value* pslot = builder.CreateAlloca(i64, nullptr, param + ".slot");
        builder.CreateStore(incoming, pslot);
        values.emplace(param, pslot);
        for (std::size_t i=0; i<actor.fields.size(); ++i) {
            const auto& field = actor.fields[i];
            llvm::Value* value = builder.CreateCall(fieldRead, {
                actorHeap, currentDomain, currentEpoch, llvm::ConstantInt::get(i64,i)},
                field.name + ".read");
            llvm::Value* slot = builder.CreateAlloca(i64, nullptr, field.name + ".slot");
            builder.CreateStore(value, slot);
            values.emplace(field.name, slot);
        }
        // The compiler owns these three-word stack tokens. Source locals
        // are immutable, synchronous and cannot be moved or borrowed here.
        std::vector<llvm::Value*> actor_owned_tokens;
        for (const auto& local : actor.on_message.locals) {
            if (!local.heap_owned || !local.initializer)
                throw std::runtime_error("unexpected actor handler owned-local kind");
            llvm::Value* initial = lower(*local.initializer, builder, i64,
                values, emptyFields, declarations);
            llvm::Value* token = builder.CreateAlloca(
                llvm::ArrayType::get(i64, 3), nullptr,
                local.name + ".actor.owned.token");
            builder.CreateCall(actorScopedNew,
                {actorHeap, currentDomain, currentEpoch, initial, token});
            // Pure reads of an immutable scalar agree with the native payload.
            llvm::Value* slot = builder.CreateAlloca(i64, nullptr,
                local.name + ".actor.owned.cached");
            builder.CreateStore(initial, slot);
            values.emplace(local.name, slot);
            actor_owned_tokens.push_back(token);
        }
        for (const auto& write : actor.on_message.actor_field_writes) {
            auto field = std::find_if(actor.fields.begin(), actor.fields.end(),
                [&](const LocalBinding& f) { return f.name == write.field; });
            if (field == actor.fields.end() || !field->field_mutable)
                throw std::runtime_error("invalid actor field write lowering");
            llvm::Value* value = lower(*write.expression, builder, i64, values, emptyFields, declarations);
            const std::size_t index = std::distance(actor.fields.begin(), field);
            builder.CreateCall(fieldWrite, {actorHeap, currentDomain, currentEpoch,
                llvm::ConstantInt::get(i64, index), value});
            builder.CreateStore(value, values.at(write.field));
        }
        llvm::Value* result = lower(*actor.on_message.result, builder, i64, values,
            emptyFields, declarations);
        // The native actor scheduler admits no next message until the current
        // synchronous handler has dropped all temporary owners in LIFO order.
        for (auto it = actor_owned_tokens.rbegin();
             it != actor_owned_tokens.rend(); ++it)
            builder.CreateCall(actorScopedDrop,
                {actorHeap, currentDomain, currentEpoch, *it});
        builder.CreateRet(result);
    }

    // Compiler-generated Wasm guest actor v0. One module instance belongs
    // exclusively to one dedicated Worker; no native OresVM symbols/imports.
    // INIT -> ACTIVE -> STOPPED is monotonic; any illegal call traps. This
    // is NOT source ActorRef/spawn/send/suspend/supervisor lowering.
    if (wasm32_target && !source.actors.empty()) {
        const auto& actor = source.actors.front();
        auto* voidTy = llvm::Type::getVoidTy(context);
        auto* zero = llvm::ConstantInt::get(i64, 0);
        auto* one = llvm::ConstantInt::get(i64, 1);
        auto* two = llvm::ConstantInt::get(i64, 2);
        auto* lifecycle = new llvm::GlobalVariable(module, i64, false,
            llvm::GlobalValue::InternalLinkage, zero, "oreslang.wasm.actor.lifecycle");
        std::vector<llvm::GlobalVariable*> globals;
        for (const auto& field : actor.fields) {
            globals.push_back(new llvm::GlobalVariable(module, i64, false,
                llvm::GlobalValue::InternalLinkage, zero,
                "oreslang.wasm.actor.field." + field.name));
        }
        // Every export validates the guest state itself, even if a buggy
        // host invokes it after teardown or initializes twice.
        auto guard = [&](llvm::IRBuilder<>& b, llvm::Function* fn,
                         llvm::Value* expected) {
            auto* pass = llvm::BasicBlock::Create(context, "admitted", fn);
            auto* denied = llvm::BasicBlock::Create(context, "trap", fn);
            auto* state = b.CreateLoad(i64, lifecycle, "lifecycle");
            b.CreateCondBr(b.CreateICmpEQ(state, expected), pass, denied);
            b.SetInsertPoint(denied);
            // 'unreachable' alone is optimizer UB, not a reliable runtime
            // guard. Emit a real llvm.trap, which lowers to Wasm's trap
            // instruction even at -Oz; never permit duplicate INIT/TEARDOWN.
            b.CreateCall(native_trap(module));
            b.CreateUnreachable();
            b.SetInsertPoint(pass);
        };
        auto* initialize = llvm::Function::Create(
            llvm::FunctionType::get(voidTy, {}, false),
            llvm::Function::ExternalLinkage, "oreslang_wasm_actor_init", module);
        llvm::IRBuilder<> init(llvm::BasicBlock::Create(context, "entry", initialize));
        guard(init, initialize, zero);
        const std::unordered_map<std::string, llvm::Value*> noStorage;
        const std::unordered_map<std::string, llvm::Function*> noFunctions;
        for (std::size_t i = 0; i < actor.fields.size(); ++i) {
            auto* initial = lower(*actor.fields[i].initializer, init, i64,
                noStorage, noStorage, noFunctions);
            init.CreateStore(initial, globals[i]);
        }
        init.CreateStore(one, lifecycle);
        init.CreateRetVoid();

        auto* handler = llvm::Function::Create(
            llvm::FunctionType::get(i64, {i64}, false),
            llvm::Function::ExternalLinkage, "oreslang_wasm_actor_on_message", module);
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", handler));
        guard(b, handler, one);
        std::unordered_map<std::string, llvm::Value*> values, emptyFields;
        auto* paramSlot = b.CreateAlloca(i64, nullptr, "message.slot");
        b.CreateStore(&*handler->arg_begin(), paramSlot);
        values.emplace(actor.on_message.parameters[0].name, paramSlot);
        for (std::size_t i = 0; i < actor.fields.size(); ++i) {
            auto* slot = b.CreateAlloca(i64, nullptr, actor.fields[i].name + ".slot");
            b.CreateStore(b.CreateLoad(i64, globals[i]), slot);
            values.emplace(actor.fields[i].name, slot);
        }
        for (const auto& write : actor.on_message.actor_field_writes) {
            auto field = std::find_if(actor.fields.begin(), actor.fields.end(),
                [&](const LocalBinding& f) { return f.name == write.field; });
            if (field == actor.fields.end() || !field->field_mutable)
                throw std::runtime_error("invalid wasm32 actor field write");
            auto* value = lower(*write.expression, b, i64, values, emptyFields, noFunctions);
            b.CreateStore(value, values.at(write.field));
        }
        auto* value = lower(*actor.on_message.result, b, i64, values,
            emptyFields, noFunctions);
        // Commit all private state only after evaluating the handler; no
        // host callback or suspension may interleave during this transition.
        for (std::size_t i = 0; i < actor.fields.size(); ++i)
            b.CreateStore(b.CreateLoad(i64, values.at(actor.fields[i].name)), globals[i]);
        b.CreateRet(value);

        auto* teardown = llvm::Function::Create(
            llvm::FunctionType::get(voidTy, {}, false),
            llvm::Function::ExternalLinkage, "oreslang_wasm_actor_teardown", module);
        llvm::IRBuilder<> finish(llvm::BasicBlock::Create(context, "entry", teardown));
        guard(finish, teardown, one);
        for (auto* field : globals) finish.CreateStore(zero, field);
        finish.CreateStore(two, lifecycle);
        finish.CreateRetVoid();
    }
    // P0 browser FFI foundation: an annotated, public, constant-returning
    // guest Oreslang function is the ONLY source of the version value. Its
    // i64 guest ABI stays unchanged. Generate a distinct, exactly i32 C
    // entrypoint so hosts never cast a 64-bit function to a 32-bit ABI.
    // There is intentionally no opaque-handle/tab/renderer ABI here.
    for (const auto& fn : source.functions) {
        if (fn.c_export_u32_symbol.empty()) continue;
        if (wasm32_target || cortex_m_target || !source.module.empty() ||
            source.functions.size() != 1 || !source.classes.empty() ||
            !source.generators.empty() || !source.cooperative_loops.empty() ||
            !source.type_only_declarations.empty() || !source.contracts.empty() ||
            !source.imports.empty() || !source.sum_types.empty() ||
            fn.c_export_u32_symbol != "ores_browser_abi_version" ||
            fn.name == fn.c_export_u32_symbol || fn.name == "main" ||
            fn.name.find('.') != std::string::npos ||
            !fn.exported || fn.native_internal || fn.async_source_i64 ||
            !fn.method_owner.empty() || !fn.parameters.empty() ||
            !fn.generic_parameters.empty() || fn.returns_void ||
            fn.return_type != ScalarType::I64 || !fn.return_class.empty() ||
            !fn.return_sum.empty() || !fn.pending_return_type_alias.empty() ||
            fn.has_declared_effects || !fn.result ||
            fn.result->kind != Expression::Kind::Integer ||
            fn.result->integer != 1 || !fn.locals.empty() ||
            !fn.statements.empty() || !fn.early_returns.empty() ||
            !fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
            !fn.actor_stops.empty() || !fn.actor_actions.empty() ||
            !fn.actor_field_writes.empty() ||
            !fn.ready_await_actor.empty() || !fn.done_await_actor.empty() ||
            !fn.ready_future_name.empty() || !fn.ready_future_actor.empty() ||
            !source.actors.empty() ||
            !fn.source_tasks.empty() || !fn.future_pipelines.empty() ||
            fn.scalar_while || fn.caught_throw_value ||
            !fn.caught_throw_defers.empty())
            throw std::runtime_error(
                "C_EXPORT_U32_REJECT: forged or unsupported browser host ABI declaration");
        // A companion plain guest i64 function must never impersonate one
        // of the browser's unimplemented C ABI entrypoints. A linker can
        // otherwise export an incorrect same-name symbol even while the
        // version trampoline is properly typed.
        for (const auto& other : source.functions) {
            if (other.name == "ores_browser_open" ||
                other.name == "ores_browser_dispatch" ||
                other.name == "ores_browser_snapshot" ||
                other.name == "ores_browser_close")
                throw std::runtime_error(
                    "C_EXPORT_U32_REJECT: unimplemented browser C symbol shadowed by guest");
        }
        // An overloaded guest family is subject to arity-based mangling.
        // This initial C ABI admits a single stable guest definition only;
        // never let adding an unrelated overload silently rename or redirect
        // the implementation behind the exported host symbol.
        if (family_sizes.at(fn.name) != 1)
            throw std::runtime_error(
                "C_EXPORT_U32_REJECT: overloaded guest export family forbidden");
        if (module.getNamedValue(fn.c_export_u32_symbol))
            throw std::runtime_error(
                "C_EXPORT_U32_REJECT: C ABI symbol collision");
        llvm::Function* guest =
            declarations.at(native_function_slot(fn.name, 0));
        if (guest->getReturnType() != i64 || guest->arg_size() != 0 ||
            guest->getLinkage() != llvm::GlobalValue::InternalLinkage)
            throw std::runtime_error(
                "C_EXPORT_U32_REJECT: guest ABI does not match checked i64() contract");
        auto* host = llvm::Function::Create(
            llvm::FunctionType::get(i32, false),
            llvm::Function::ExternalLinkage,
            fn.c_export_u32_symbol, module);
        auto* entry = llvm::BasicBlock::Create(context, "entry", host);
        auto* accepted = llvm::BasicBlock::Create(context, "u32.valid", host);
        auto* rejected = llvm::BasicBlock::Create(context, "u32.trap", host);
        llvm::IRBuilder<> b(entry);
        llvm::Value* value = b.CreateCall(guest);
        // Treat negative i64 as unsigned > UINT32_MAX; reject rather than
        // silently truncating a forged guest result.
        llvm::Value* in_range = b.CreateICmpULE(
            value, llvm::ConstantInt::get(i64, UINT32_MAX));
        b.CreateCondBr(in_range, accepted, rejected);
        b.SetInsertPoint(rejected);
        b.CreateCall(llvm::Intrinsic::getDeclaration(
            &module, llvm::Intrinsic::trap));
        b.CreateUnreachable();
        b.SetInsertPoint(accepted);
        b.CreateRet(b.CreateTrunc(value, i32));
    }
    if (native_main) {
        // OS/process ABI boundary: never expose i64/void Oreslang main as C main.
        // Reject invalid statuses without Unix low-byte truncation (256 -> 0).
        auto* host_main = llvm::Function::Create(
            llvm::FunctionType::get(i32, false),
            llvm::Function::ExternalLinkage, "main", module);
        auto* entry = llvm::BasicBlock::Create(context, "entry", host_main);
        llvm::IRBuilder<> builder(entry);
        llvm::Function* guest = declarations.at(native_function_slot("main", 0));
        if (actor_region_v1) {
            auto* i32 = llvm::Type::getInt32Ty(context);
            builder.CreateCall(actorRegionGuestBegin, {
                llvm::ConstantInt::get(i32, actor_region_slots),
                llvm::ConstantInt::get(i32, actor_region_stride),
                llvm::ConstantInt::get(i32, 64)});
        }
        llvm::FunctionCallee string_end;
        llvm::FunctionCallee record_end;
        if (string_runtime) {
            auto begin = module.getOrInsertFunction("ores_string_region_begin",
                llvm::FunctionType::get(i64, {i64}, false));
            string_end = module.getOrInsertFunction("ores_string_region_end",
                llvm::FunctionType::get(i64, {}, false));
            auto* status = builder.CreateCall(begin, {llvm::ConstantInt::get(i64, console_runtime)});
            auto* ready = llvm::BasicBlock::Create(context, "string.region.ready", host_main);
            auto* failed = llvm::BasicBlock::Create(context, "string.region.error", host_main);
            builder.CreateCondBr(builder.CreateICmpEQ(status, llvm::ConstantInt::get(i64, 0)), ready, failed);
            builder.SetInsertPoint(failed); builder.CreateRet(llvm::ConstantInt::get(i32, 1));
            builder.SetInsertPoint(ready);
        }
        if (record_runtime) {
            auto begin = module.getOrInsertFunction("ores_record_region_begin", llvm::FunctionType::get(i64, {}, false));
            record_end = module.getOrInsertFunction("ores_record_region_end", llvm::FunctionType::get(i64, {}, false));
            auto* status = builder.CreateCall(begin);
            auto* ready = llvm::BasicBlock::Create(context, "record.region.ready", host_main);
            auto* failed = llvm::BasicBlock::Create(context, "record.region.error", host_main);
            builder.CreateCondBr(builder.CreateICmpEQ(status, llvm::ConstantInt::get(i64, 0)), ready, failed);
            builder.SetInsertPoint(failed);
            if (string_runtime) builder.CreateCall(string_end);
            builder.CreateRet(llvm::ConstantInt::get(i32, 1));
            builder.SetInsertPoint(ready);
        }
        llvm::Value* result = builder.CreateCall(guest);
        if (actor_region_v1)
            builder.CreateCall(actorRegionGuestEnd);
        llvm::Value* tasks_ok = has_source_tasks
            ? builder.CreateICmpEQ(
                builder.CreateCall(taskShutdown),
                llvm::ConstantInt::get(i32, 1),
                "source.tasks.shutdown.ok")
            : llvm::ConstantInt::getTrue(context);
        llvm::Value* records_ok = record_runtime
            ? builder.CreateICmpEQ(builder.CreateCall(record_end), llvm::ConstantInt::get(i64, 0))
            : llvm::ConstantInt::getTrue(context);
        llvm::Value* output_ok = string_runtime
            ? builder.CreateICmpEQ(builder.CreateCall(string_end), llvm::ConstantInt::get(i64, 0))
            : llvm::ConstantInt::getTrue(context);
        output_ok = builder.CreateAnd(output_ok, records_ok);
        output_ok = builder.CreateAnd(output_ok, tasks_ok);
        if (main_it->returns_void) {
            builder.CreateRet(builder.CreateSelect(output_ok, llvm::ConstantInt::get(i32, 0), llvm::ConstantInt::get(i32, 1)));
        } else {
            llvm::Value* valid = builder.CreateICmpULE(
                result, llvm::ConstantInt::get(i64, 255), "valid.exit.status");
            valid = builder.CreateAnd(valid, output_ok);
            llvm::Value* narrowed = builder.CreateTrunc(result, i32);
            builder.CreateRet(builder.CreateSelect(valid, narrowed,
                llvm::ConstantInt::get(i32, 1), "checked.exit.status"));
        }
    }


    if (!isolate_entry.empty()) {
        // Isolate guest entry ABI v2: status is distinct from signed i64 output.
        // Tenant mutable state is accessed only through context callbacks.
        // No writable process-global guest state is emitted by this wrapper.
        auto* ctx_type = llvm::StructType::get(context,
            {i32, i32, i64, native_ptr, native_ptr, native_ptr});
        auto* wrapper = llvm::Function::Create(
            llvm::FunctionType::get(i32, {native_ptr, i64, native_ptr}, false),
            llvm::Function::ExternalLinkage, "ores_guest_entry", module);
        auto* entry = llvm::BasicBlock::Create(context, "entry", wrapper);
        auto* validate = llvm::BasicBlock::Create(context, "validate", wrapper);
        auto* read = llvm::BasicBlock::Create(context, "read", wrapper);
        auto* execute = llvm::BasicBlock::Create(context, "execute", wrapper);
        auto* success = llvm::BasicBlock::Create(context, "success", wrapper);
        auto* bad_context = llvm::BasicBlock::Create(context, "bad.context", wrapper);
        auto* bad_load = llvm::BasicBlock::Create(context, "bad.load", wrapper);
        auto* bad_store = llvm::BasicBlock::Create(context, "bad.store", wrapper);
        auto* bad_arithmetic = llvm::BasicBlock::Create(context, "bad.arithmetic", wrapper);
        llvm::IRBuilder<> b(entry);
        auto* ctx = wrapper->getArg(0);
        auto* input = wrapper->getArg(1);
        auto* out = wrapper->getArg(2);
        auto* pointers_valid = b.CreateAnd(b.CreateIsNotNull(ctx),
                                           b.CreateIsNotNull(out));
        b.CreateCondBr(pointers_valid, validate, bad_context);
        b.SetInsertPoint(validate);
        auto* typed_ctx = b.CreateBitCast(ctx, ctx_type->getPointerTo());
        auto* version = b.CreateLoad(i32, b.CreateStructGEP(ctx_type, typed_ctx, 0));
        auto* slots = b.CreateLoad(i32, b.CreateStructGEP(ctx_type, typed_ctx, 1));
        auto* data = b.CreateLoad(native_ptr, b.CreateStructGEP(ctx_type, typed_ctx, 3));
        auto* load_fn = b.CreateLoad(native_ptr, b.CreateStructGEP(ctx_type, typed_ctx, 4));
        auto* store_fn = b.CreateLoad(native_ptr, b.CreateStructGEP(ctx_type, typed_ctx, 5));
        auto* compatible = b.CreateAnd(
            b.CreateAnd(b.CreateICmpEQ(version, llvm::ConstantInt::get(i32, ORES_ISOLATE_ABI_VERSION)),
                        b.CreateICmpEQ(slots, llvm::ConstantInt::get(i32, ORES_ISOLATE_I64_SLOTS))),
            b.CreateAnd(b.CreateIsNotNull(data),
                b.CreateAnd(b.CreateIsNotNull(load_fn), b.CreateIsNotNull(store_fn))));
        b.CreateCondBr(compatible, read, bad_context);
        b.SetInsertPoint(read);
        auto* old_slot = b.CreateAlloca(i64, nullptr, "tenant.old");
        auto* load_type = llvm::FunctionType::get(i32, {native_ptr, i32, native_ptr}, false);
        // LLVM 14-17 typed pointers require an actual function pointer type;
        // LLVM 18+ opaque pointers naturally collapse this to the same ptr.
        auto* load_callee = b.CreateBitCast(load_fn, load_type->getPointerTo());
        auto* load_ok = b.CreateCall(load_type, load_callee,
            {data, llvm::ConstantInt::get(i32, 0), b.CreateBitCast(old_slot, native_ptr)});
        b.CreateCondBr(b.CreateICmpEQ(load_ok, llvm::ConstantInt::get(i32, 1)),
                       execute, bad_load);
        b.SetInsertPoint(execute);
        auto* old = b.CreateLoad(i64, old_slot);
        // In isolate-entry mode, lower the already-validated pure Oreslang AST
        // directly into the status-producing guest wrapper. Calling the
        // ordinary i64 function would use llvm.trap on signed overflow and
        // terminate the entire in-process OresVM, not just this tenant.
        // Each arithmetic operation branches to a recoverable ABI failure
        // before any state store. The ordinary function is internal-only.
        const auto& source_step = source.functions.front();
        const auto emit_checked_pure = [&](const auto& self,
                                           const Expression& expr) -> llvm::Value* {
            using K = Expression::Kind;
            switch (expr.kind) {
                case K::Integer:
                    return llvm::ConstantInt::getSigned(i64, expr.integer);
                case K::Variable:
                    if (expr.name == source_step.parameters[0].name) return old;
                    if (expr.name == source_step.parameters[1].name) return input;
                    throw std::logic_error("invalid isolate scalar parameter");
                case K::Negate:
                case K::Add:
                case K::Subtract:
                case K::Multiply: {
                    const bool negative = expr.kind == K::Negate;
                    auto* lhs = negative
                        ? static_cast<llvm::Value*>(llvm::ConstantInt::get(i64, 0))
                        : self(self, *expr.left);
                    auto* rhs = negative
                        ? self(self, *expr.left)
                        : self(self, *expr.right);
                    const auto op = (negative || expr.kind == K::Subtract)
                        ? llvm::Intrinsic::ssub_with_overflow
                        : (expr.kind == K::Multiply)
                            ? llvm::Intrinsic::smul_with_overflow
                            : llvm::Intrinsic::sadd_with_overflow;
                    auto* intrinsic = llvm::Intrinsic::getDeclaration(
                        &module, op, {i64});
                    auto* pair = b.CreateCall(intrinsic, {lhs, rhs},
                                               "isolate.arithmetic.checked");
                    auto* value = b.CreateExtractValue(pair, {0}, "isolate.arithmetic.value");
                    auto* overflow = b.CreateExtractValue(pair, {1}, "isolate.arithmetic.overflow");
                    auto* proceed = llvm::BasicBlock::Create(
                        context, "isolate.arithmetic.ok", wrapper);
                    b.CreateCondBr(overflow, bad_arithmetic, proceed);
                    b.SetInsertPoint(proceed);
                    return value;
                }
                default:
                    throw std::logic_error("unvalidated isolate expression");
            }
        };
        auto* next = emit_checked_pure(emit_checked_pure, *source_step.result);
        auto* store_type = llvm::FunctionType::get(i32, {native_ptr, i32, i64}, false);
        auto* store_callee = b.CreateBitCast(store_fn, store_type->getPointerTo());
        auto* saved = b.CreateCall(store_type, store_callee,
            {data, llvm::ConstantInt::get(i32, 0), next});
        b.CreateCondBr(b.CreateICmpEQ(saved, llvm::ConstantInt::get(i32, 1)),
                       success, bad_store);
        b.SetInsertPoint(success);
        b.CreateStore(next, out);
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
        b.SetInsertPoint(bad_context);
        b.CreateRet(llvm::ConstantInt::get(i32, 1));
        b.SetInsertPoint(bad_load);
        b.CreateRet(llvm::ConstantInt::get(i32, 2));
        b.SetInsertPoint(bad_store);
        b.CreateRet(llvm::ConstantInt::get(i32, ORES_GUEST_STORE_DENIED));
        b.SetInsertPoint(bad_arithmetic);
        b.CreateRet(llvm::ConstantInt::get(i32, ORES_GUEST_OVERFLOW));
    }
    std::string diagnostics;
    llvm::raw_string_ostream errors(diagnostics);
    if (llvm::verifyModule(module, &errors)) {
        errors.flush();
        throw std::runtime_error("invalid generated LLVM module: " + diagnostics);
    }
    std::string result;
    llvm::raw_string_ostream output(result);
    module.print(output, nullptr);
    output.flush();
    return result;
}

} // namespace oreslang

