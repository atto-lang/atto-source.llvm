#include "oreslang/backend/LlvmCodegen.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang {
namespace {

// Owned i64 parameters receive stack slots so the only permitted loans can
// pass their addresses through the call ABI. Borrowed inputs already hold
// caller-owned slot pointers: passing them to nested borrowed parameters is
// a checked, call-scoped reborrow with no new allocation. Neither address
// can escape as a result:
// native functions return only i64, and the ownership pass restricts rt borrow
// to nonescaping call arguments.
llvm::Value* lower(const Expression& expr,
                   llvm::IRBuilder<>& builder,
                   llvm::Type* i64,
                   const std::unordered_map<std::string, llvm::Value*>& storage,
                   const std::unordered_map<std::string, llvm::Value*>& field_storage,
                   const std::unordered_map<std::string, llvm::Function*>& functions) {
    switch (expr.kind) {
        case Expression::Kind::Integer:
            return llvm::ConstantInt::getSigned(i64, expr.integer);
        case Expression::Kind::Variable:
            return builder.CreateLoad(i64, storage.at(expr.name), expr.name + ".read");
        case Expression::Kind::Field:
            return builder.CreateLoad(i64, field_storage.at(expr.left->name),
                                      expr.left->name + "." + expr.name + ".read");
        case Expression::Kind::Borrow:
        case Expression::Kind::BorrowMut:
            // Only addressable named parameters are admitted by OwnershipChecker.
            return storage.at(expr.left->name);
        case Expression::Kind::Negate:
            return builder.CreateNeg(lower(*expr.left, builder, i64, storage, field_storage, functions), "neg");
        case Expression::Kind::Copy:
        case Expression::Kind::Take:
        case Expression::Kind::Share:
            // These remain primitive value operations, never graph references.
            return lower(*expr.left, builder, i64, storage, field_storage, functions);
        case Expression::Kind::Add: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions);
            return builder.CreateAdd(left, right, "add");
        }
        case Expression::Kind::Subtract: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions);
            return builder.CreateSub(left, right, "sub");
        }
        case Expression::Kind::Multiply: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions);
            return builder.CreateMul(left, right, "mul");
        }
        case Expression::Kind::Call: {
            std::vector<llvm::Value*> arguments;
            for (const auto& arg : expr.arguments)
                arguments.push_back(lower(*arg, builder, i64, storage, field_storage, functions));
            llvm::Function* callee = functions.at(expr.name);
            return builder.CreateCall(callee, arguments,
                callee->getReturnType()->isVoidTy() ? "" : "call");
        }
    }
    throw std::runtime_error("invalid expression kind while lowering LLVM IR");
}

} // namespace

std::string emit_verified_llvm_ir(const Program& source) {
    llvm::LLVMContext context;
    llvm::Module module("oreslang-source", context);
    llvm::Type* i64 = llvm::Type::getInt64Ty(context);
    llvm::Type* loan_ptr = i64->getPointerTo();
    llvm::Type* native_ptr = llvm::Type::getInt8PtrTy(context);
    // The test-only native owned-int experiment must never leak declarations
    // or allocations into ordinary int code. Resolve the ABI only when a
    // program actually contains a gated experimental heap-owned local.
    const bool has_region_bindings = std::any_of(source.functions.begin(), source.functions.end(),
        [](const Function& fn) {
            return std::any_of(fn.locals.begin(), fn.locals.end(),
                [](const LocalBinding& binding) { return binding.heap_owned; });
        });
    llvm::FunctionCallee regionOpen, regionNew, regionDrop, regionClose;
    if (has_region_bindings) {
        regionOpen = module.getOrInsertFunction("oreslang_owned_i64_region_open",
            llvm::FunctionType::get(native_ptr, {i64}, false));
        regionNew = module.getOrInsertFunction("oreslang_owned_i64_region_new",
            llvm::FunctionType::get(native_ptr, {native_ptr, i64}, false));
        regionDrop = module.getOrInsertFunction("oreslang_owned_i64_region_drop",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {native_ptr, native_ptr}, false));
        regionClose = module.getOrInsertFunction("oreslang_owned_i64_region_close",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {native_ptr}, false));
    }
    std::unordered_map<std::string, llvm::Function*> declarations;
    std::unordered_map<std::string, llvm::StructType*> class_types;
    std::unordered_map<std::string, std::int64_t> class_defaults;
    for (const auto& cls : source.classes) {
        class_types.emplace(cls.name, llvm::StructType::create(context, {i64}, cls.name));
        class_defaults.emplace(cls.name, cls.initial);
    }

    // Exact by-value i64 / borrowed-i64-pointer argument types for all
    // same-module functions; no unsound conversion to i64 is permitted.
    for (const auto& fn : source.functions) {
        const std::string symbol = source.module.empty()
            ? fn.name : source.module + "." + fn.name;
        std::vector<llvm::Type*> argument_types;
        for (const auto& param : fn.parameters)
            argument_types.push_back(param.class_name.empty()
                ? (param.mode == Parameter::Mode::Value ? i64 : loan_ptr)
                : class_types.at(param.class_name)->getPointerTo());
        llvm::FunctionType* signature = llvm::FunctionType::get(
            fn.returns_void ? llvm::Type::getVoidTy(context) : i64,
            argument_types, false);
        declarations.emplace(fn.name, llvm::Function::Create(
            signature, llvm::Function::ExternalLinkage, symbol, module));
    }
    for (const auto& fn : source.functions) {
        llvm::Function* function = declarations.at(fn.name);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", function);
        llvm::IRBuilder<> builder(entry);
        std::unordered_map<std::string, llvm::Value*> storage;
        std::unordered_map<std::string, llvm::Value*> field_storage;
        std::size_t index = 0;
        for (auto& arg : function->args()) {
            const Parameter& param = fn.parameters[index++];
            arg.setName(param.name);
            if (!param.class_name.empty()) {
                storage.emplace(param.name, &arg);
                field_storage.emplace(param.name,
                    builder.CreateStructGEP(class_types.at(param.class_name), &arg, 0,
                                            param.name + ".field"));
            } else if (param.mode == Parameter::Mode::Value) {
                llvm::Value* slot = builder.CreateAlloca(i64, nullptr, param.name + ".slot");
                builder.CreateStore(&arg, slot);
                storage.emplace(param.name, slot);
            } else {
                storage.emplace(param.name, &arg);
            }
        }
        // An explicitly "owned int" lives in a bounded native C region;
        // ordinary int remains stack-owned. Both allow only call-scoped loans.
        struct LocalSlot { llvm::Value* address; bool heap_owned; };
        std::vector<LocalSlot> lexical_slots;
        const auto heap_count = std::count_if(fn.locals.begin(), fn.locals.end(),
            [](const LocalBinding& b) { return b.heap_owned; });
        llvm::Value* region = nullptr;
        if (heap_count != 0)
            region = builder.CreateCall(regionOpen,
                {llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64),
                                        static_cast<std::uint64_t>(heap_count))}, "native.owned.region");
        for (const auto& binding : fn.locals) {
            if (binding.new_class) {
                llvm::StructType* shape = class_types.at(binding.class_name);
                llvm::Value* slot = builder.CreateAlloca(shape, nullptr, binding.name + ".record");
                builder.CreateLifetimeStart(slot,
                    llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), 8));
                llvm::Value* field = builder.CreateStructGEP(shape, slot, 0, binding.name + ".field");
                builder.CreateStore(llvm::ConstantInt::getSigned(i64,
                    class_defaults.at(binding.class_name)), field);
                storage.emplace(binding.name, slot);
                field_storage.emplace(binding.name, field);
                lexical_slots.push_back({slot, false});
                continue;
            }
            llvm::Value* initial = lower(*binding.initializer, builder, i64, storage, field_storage, declarations);
            llvm::Value* slot = nullptr;
            if (binding.heap_owned) {
                slot = builder.CreateCall(regionNew, {region, initial}, binding.name + ".heap.owned");
            } else {
                slot = builder.CreateAlloca(i64, nullptr, binding.name + ".stack.owned");
                builder.CreateLifetimeStart(slot, llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), 8));
                builder.CreateStore(initial, slot);
            }
            storage.emplace(binding.name, slot);
            lexical_slots.push_back({slot, binding.heap_owned});
        }
        for (const auto& stmt : fn.statements) {
            if (stmt.kind == Statement::Kind::FieldWrite) {
                llvm::Value* updated = lower(*stmt.expression, builder, i64, storage, field_storage, declarations);
                builder.CreateStore(updated, field_storage.at(stmt.receiver));
            } else {
                lower(*stmt.expression, builder, i64, storage, field_storage, declarations);
            }
        }
        llvm::Value* result = fn.result
            ? lower(*fn.result, builder, i64, storage, field_storage, declarations) : nullptr;
        for (auto it = lexical_slots.rbegin(); it != lexical_slots.rend(); ++it) {
            if (it->heap_owned) builder.CreateCall(regionDrop, {region, it->address});
            else builder.CreateLifetimeEnd(it->address,
                llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), 8));
        }
        if (region != nullptr) builder.CreateCall(regionClose, {region});
        if (fn.returns_void) builder.CreateRetVoid();
        else builder.CreateRet(result);
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
