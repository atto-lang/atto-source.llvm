#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang {
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
struct RegionMoveLowering {
    llvm::Value* region = nullptr;
    llvm::FunctionCallee read;
    llvm::FunctionCallee drop;
    const std::unordered_map<std::string, llvm::Value*>* live_flags = nullptr;
};

llvm::Value* lower(const Expression& expr,
                   llvm::IRBuilder<>& builder,
                   llvm::Type* i64,
                   const std::unordered_map<std::string, llvm::Value*>& storage,
                   const std::unordered_map<std::string, llvm::Value*>& field_storage,
                   const std::unordered_map<std::string, llvm::Function*>& functions,
                   const RegionMoveLowering* moves = nullptr) {
    switch (expr.kind) {
        case Expression::Kind::Integer:
            return llvm::ConstantInt::getSigned(i64, expr.integer);
        case Expression::Kind::Variable: {
            llvm::Value* stored = builder.CreateLoad(i64, storage.at(expr.name),
                expr.name + ".read");
            if (moves && moves->live_flags &&
                    moves->live_flags->count(expr.name))
                return builder.CreateCall(moves->read, {moves->region, stored},
                    expr.name + ".owned.value");
            return stored;
        }
        case Expression::Kind::Field:
            return builder.CreateLoad(i64, field_storage.at(field_key(expr.left->name, expr.name)),
                                      expr.left->name + "." + expr.name + ".read");
        case Expression::Kind::Borrow:
        case Expression::Kind::BorrowMut: {
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
                llvm::Value* temporary = builder.CreateAlloca(i64, nullptr,
                    "borrow.owner.snapshot");
                builder.CreateStore(snapshot, temporary);
                return temporary;
            }
            return storage.at(expr.left->name);
        }
        case Expression::Kind::Negate:
            return builder.CreateNeg(lower(*expr.left, builder, i64, storage, field_storage, functions, moves), "neg");
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
                    return value;
                }
            }
            return lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
        }
        case Expression::Kind::Add: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            return builder.CreateAdd(left, right, "add");
        }
        case Expression::Kind::Subtract: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            return builder.CreateSub(left, right, "sub");
        }
        case Expression::Kind::Multiply: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            return builder.CreateMul(left, right, "mul");
        }
        case Expression::Kind::ModuleCall: {
            // This path is admitted only by the whole-program, pre-IR scan
            // above. i64 primitive equality is widened into no host callback.
            if (expr.arguments.size() != 2 ||
                    (expr.name != "equal" && expr.name != "not_equal"))
                throw std::runtime_error("unverified native Assert primitive");
            llvm::Value* lhs = lower(*expr.arguments[0], builder, i64,
                storage, field_storage, functions, moves);
            llvm::Value* rhs = lower(*expr.arguments[1], builder, i64,
                storage, field_storage, functions, moves);
            return expr.name == "equal"
                ? builder.CreateICmpEQ(lhs, rhs, "assert.equal.int")
                : builder.CreateICmpNE(lhs, rhs, "assert.not_equal.int");
        }
        case Expression::Kind::Call: {
            std::vector<llvm::Value*> arguments;
            for (const auto& arg : expr.arguments)
                arguments.push_back(lower(*arg, builder, i64, storage, field_storage, functions, moves));
            llvm::Function* callee = functions.at(expr.name);
            return builder.CreateCall(callee, arguments,
                callee->getReturnType()->isVoidTy() ? "" : "call");
        }
    }
    throw std::runtime_error("invalid expression kind while lowering LLVM IR");
}

} // namespace

std::string emit_verified_llvm_ir(const Program& source, bool wasm32_target) {
    // A deliberately narrow, executable primitive specialization of the
    // sealed Assert namespace. This is NOT Any, stdlib source-module linking,
    // assertion throws, trap, or general typed expression lowering.
    // Admission is global and completes before *any* LLVM IR is produced.
    // Never trust that every caller used parse_program(): future macro
    // expansion or AST transformation must not bypass the sealed import
    // boundary. Check even import-free programs before constructing LLVM IR.
        auto contains_module_call = [&](const Expression* root, const auto& visit) -> bool {
            if (!root) return false;
            if (root->kind == Expression::Kind::ModuleCall) return true;
            if (root->left && visit(root->left.get(), visit)) return true;
            if (root->right && visit(root->right.get(), visit)) return true;
            for (const auto& arg : root->arguments)
                if (visit(arg.get(), visit)) return true;
            return false;
        };
        // Conditional arms are nested CFG scopes. A shallow scan would
        // admit an i1-producing module call inside an unrelated int function.
        auto scan_statements = [&](const std::vector<Statement>& actions,
                                  const auto& scan, const auto& matches) -> bool {
            for (const auto& stmt : actions) {
                if (matches(stmt.expression.get()) ||
                        matches(stmt.condition_lhs.get()) ||
                        matches(stmt.condition_rhs.get()) ||
                        (stmt.branch_local &&
                         matches(stmt.branch_local->initializer.get())) ||
                        scan(stmt.then_statements, scan, matches) ||
                        scan(stmt.else_statements, scan, matches))
                    return true;
            }
            return false;
        };

    auto has_module_call = [&](const Function& fn) -> bool {
        if (contains_module_call(fn.result.get(), contains_module_call)) return true;
        for (const auto& local : fn.locals)
            if (contains_module_call(local.initializer.get(), contains_module_call))
                return true;
        if (scan_statements(fn.statements, scan_statements,
                [&](const Expression* e) {
                    return contains_module_call(e, contains_module_call);
                })) return true;
        for (const auto& guard : fn.early_returns)
            if (contains_module_call(guard.lhs.get(), contains_module_call) ||
                    contains_module_call(guard.rhs.get(), contains_module_call) ||
                    contains_module_call(guard.result.get(), contains_module_call))
                return true;
        for (const auto& send : fn.actor_sends)
            if (contains_module_call(send.payload.get(), contains_module_call))
                return true;
        for (const auto& write : fn.actor_field_writes)
            if (contains_module_call(write.expression.get(), contains_module_call))
                return true;
        return false;
    };
    if (source.imports.empty()) {
        for (const auto& fn : source.functions)
            if (fn.returns_bool || has_module_call(fn))
                throw std::runtime_error(
                    "native bool/Assert call requires a sealed std/assert import");
        for (const auto& actor : source.actors) {
            for (const auto& field : actor.fields)
                if (contains_module_call(field.initializer.get(), contains_module_call))
                    throw std::runtime_error("unbound native Assert call in actor field");
            if (has_module_call(actor.on_message))
                throw std::runtime_error("unbound native Assert call in actor handler");
        }
    }
    if (!source.imports.empty()) {
        if (wasm32_target || source.imports.size() != 1 ||
                source.imports.front().symbol != "Assert" ||
                source.imports.front().canonical_path != "std/assert" ||
                !source.actors.empty() || !source.classes.empty())
            throw std::runtime_error("native Assert scalar specialization requires a sealed host-only import");

        for (const auto& fn : source.functions) {
            if (fn.returns_bool) {
                if (fn.name == "main" || fn.returns_void || !fn.result ||
                        fn.result->kind != Expression::Kind::ModuleCall ||
                        !fn.result->left ||
                        fn.result->left->kind != Expression::Kind::Variable ||
                        fn.result->left->name != "Assert" ||
                        (fn.result->name != "equal" && fn.result->name != "not_equal") ||
                        fn.result->arguments.size() != 2 ||
                        !fn.statements.empty() || !fn.early_returns.empty() ||
                        !fn.locals.empty() || !fn.actor_spawns.empty() ||
                        !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
                        fn.has_declared_effects)
                    throw std::runtime_error(
                        "native Assert bool functions require direct equal/not_equal primitive return");
                for (const auto& param : fn.parameters)
                    if (!param.class_name.empty() || param.mode != Parameter::Mode::Value)
                        throw std::runtime_error("native Assert bool parameters must be by-value int");
                for (const auto& arg : fn.result->arguments)
                    if (!arg || (arg->kind != Expression::Kind::Integer &&
                            !(arg->kind == Expression::Kind::Variable &&
                              std::any_of(fn.parameters.begin(), fn.parameters.end(),
                                  [&](const Parameter& p) { return p.name == arg->name; }))))
                        throw std::runtime_error(
                            "native Assert scalar arguments must be int literals or by-value int parameters");
            } else {
                if (contains_module_call(fn.result.get(), contains_module_call))
                    throw std::runtime_error(
                        "native Assert member calls cannot enter an int/void expression");
                for (const auto& local : fn.locals)
                    if (contains_module_call(local.initializer.get(), contains_module_call))
                        throw std::runtime_error("native Assert member requires a typed bool return");
                if (scan_statements(fn.statements, scan_statements,
                        [&](const Expression* e) {
                            return contains_module_call(e, contains_module_call);
                        }))
                    throw std::runtime_error("native Assert member requires a typed bool return");
                for (const auto& guard : fn.early_returns)
                    if (contains_module_call(guard.lhs.get(), contains_module_call) ||
                            contains_module_call(guard.rhs.get(), contains_module_call) ||
                            contains_module_call(guard.result.get(), contains_module_call))
                        throw std::runtime_error("native Assert member requires a typed bool return");
            }
        }
        // The older int-only Call lowering would otherwise attempt to use an
        // i1-producing function in an i64 arithmetic/return context.
        for (const auto& fn : source.functions) {
            if (!fn.returns_bool) {
                auto calls_bool = [&](const Expression* root, const auto& visit) -> bool {
                    if (!root) return false;
                    if (root->kind == Expression::Kind::Call &&
                            std::any_of(source.functions.begin(), source.functions.end(),
                              [&](const Function& callee) {
                                  return callee.returns_bool && callee.name == root->name;
                              })) return true;
                    if (root->left && visit(root->left.get(), visit)) return true;
                    if (root->right && visit(root->right.get(), visit)) return true;
                    for (const auto& arg : root->arguments)
                        if (visit(arg.get(), visit)) return true;
                    return false;
                };
                if (calls_bool(fn.result.get(), calls_bool))
                    throw std::runtime_error("native bool-to-int coercion requires explicit lowering");
                for (const auto& local : fn.locals)
                    if (calls_bool(local.initializer.get(), calls_bool))
                        throw std::runtime_error("native bool-to-int coercion requires explicit lowering");
                if (scan_statements(fn.statements, scan_statements,
                        [&](const Expression* e) {
                            return calls_bool(e, calls_bool);
                        }))
                    throw std::runtime_error("native bool-to-int coercion requires explicit lowering");
                for (const auto& guard : fn.early_returns)
                    if (calls_bool(guard.lhs.get(), calls_bool) ||
                            calls_bool(guard.rhs.get(), calls_bool) ||
                            calls_bool(guard.result.get(), calls_bool))
                        throw std::runtime_error("native bool-to-int coercion requires explicit lowering");
            }
        }
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
        for (const auto& fn : source.functions) {
            if (!fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
                !fn.actor_stops.empty() || !fn.actor_field_writes.empty())
                throw std::runtime_error("wasm32 actor operations are not implemented");
            for (const auto& local : fn.locals)
                if (local.heap_owned)
                    throw std::runtime_error("wasm32 native heap-owned regions are not implemented");
        }
    }
    // Native C entrypoints return i32; Oreslang int remains an internal i64.
    // Qualified module.main retains its exported guest symbol and gains a wrapper.
    // Browser Wasm is deliberately excluded: it has a separate host entry ABI.
    constexpr const char* guest_main_symbol = "__oreslang_guest_main";
    const auto main_it = std::find_if(source.functions.begin(), source.functions.end(),
        [](const Function& fn) { return fn.name == "main"; });
    const bool native_main = !wasm32_target && main_it != source.functions.end();
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
    if (wasm32_target) {
        const auto profile = TargetAbi::parse("wasm32-unknown-unknown");
        if (!profile.experimental_wasm32_ir())
            throw std::runtime_error("wasm32 target profile is not enabled");
        module.setTargetTriple(std::string(profile.triple()));
        module.setDataLayout("e-m:e-p:32:32-i64:64-n32:64-S128");
        const auto& actual = module.getDataLayout();
        if (actual.getPointerSizeInBits(0) != profile.pointer_bits()
                || actual.isLittleEndian() != (profile.endian() == "little"))
            throw std::runtime_error("wasm32 target ABI/data layout mismatch");
    }
    llvm::Type* i64 = llvm::Type::getInt64Ty(context);
    llvm::Type* loan_ptr = i64->getPointerTo();
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
    const bool has_region_bindings = std::any_of(source.functions.begin(), source.functions.end(),
        [](const Function& fn) {
            return std::any_of(fn.locals.begin(), fn.locals.end(),
                [](const LocalBinding& binding) { return binding.heap_owned; });
        });
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

    // Only generated source actors resolve this native scheduler/heap ABI.
    // No actor imports appear in ordinary scalar/class compilation.
    llvm::FunctionCallee fieldInit, fieldInitMutable, fieldRead, fieldWrite;
    llvm::FunctionCallee sourceSpawn, sourceSend, sourceStop;
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
        sourceSpawn = module.getOrInsertFunction("oreslang_source_actor_spawn",
            llvm::FunctionType::get(native_ptr, {native_ptr, native_ptr, i64}, false));
        sourceSend = module.getOrInsertFunction("oreslang_source_actor_send",
            llvm::FunctionType::get(voidTy, {native_ptr, i64}, false));
        sourceStop = module.getOrInsertFunction("oreslang_source_actor_stop",
            llvm::FunctionType::get(voidTy, {native_ptr}, false));
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
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> field_indices;
    for (const auto& cls : source.classes) {
        std::vector<llvm::Type*> layout(cls.fields.size(), i64);
        class_types.emplace(cls.name, llvm::StructType::create(context, layout, cls.name));
        std::unordered_map<std::string, std::size_t> indices;
        for (std::size_t i = 0; i < cls.fields.size(); ++i)
            indices.emplace(cls.fields[i].name, i);
        field_indices.emplace(cls.name, std::move(indices));
    }

    // Exact by-value i64 / borrowed-i64-pointer argument types for all
    // same-module functions; no unsound conversion to i64 is permitted.
    for (const auto& fn : source.functions) {
        const std::string symbol = native_main && source.module.empty() && fn.name == "main"
            ? guest_main_symbol
            : (source.module.empty() ? fn.name : source.module + "." + fn.name);
        std::vector<llvm::Type*> argument_types;
        for (const auto& param : fn.parameters)
            argument_types.push_back(param.class_name.empty()
                ? (param.mode == Parameter::Mode::Value ? i64 : loan_ptr)
                : class_types.at(param.class_name)->getPointerTo());
        llvm::FunctionType* signature = llvm::FunctionType::get(
            fn.returns_void ? llvm::Type::getVoidTy(context)
                            : (fn.returns_bool ? llvm::Type::getInt1Ty(context) : i64),
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
                for (const auto& [field, field_index] : field_indices.at(param.class_name)) {
                    field_storage.emplace(field_key(param.name, field),
                        builder.CreateStructGEP(class_types.at(param.class_name), &arg,
                            static_cast<unsigned>(field_index), param.name + "." + field));
                }
            } else if (param.mode == Parameter::Mode::Value) {
                llvm::Value* slot = builder.CreateAlloca(i64, nullptr, param.name + ".slot");
                builder.CreateStore(&arg, slot);
                storage.emplace(param.name, slot);
            } else {
                storage.emplace(param.name, &arg);
            }
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
                llvm::ConstantInt::get(i64, actor->fields.size())}, spawn.name + ".actor.ref");
            actorRefs.emplace(spawn.name, ref);
        }
        // An explicitly "owned int" lives in a bounded native C region;
        // ordinary int remains stack-owned. Both allow only call-scoped loans.
        struct LocalSlot {
            std::string name;
            llvm::Value* address;
            bool heap_owned;
            std::uint64_t bytes;
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
        const RegionMoveLowering region_moves{region, regionRead, regionDrop, &owned_live_flags};
        for (const auto& binding : fn.locals) {
            if (binding.new_class || binding.move_class) {
                llvm::StructType* shape = class_types.at(binding.class_name);
                llvm::Value* slot = builder.CreateAlloca(shape, nullptr, binding.name + ".record");
                const auto cls = std::find_if(source.classes.begin(), source.classes.end(),
                    [&](const ClassDecl& item) { return item.name == binding.class_name; });
                const std::uint64_t bytes = static_cast<std::uint64_t>(cls->fields.size()) * 8;
                builder.CreateLifetimeStart(slot,
                    llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), bytes));
                for (std::size_t i = 0; i < cls->fields.size(); ++i) {
                    const ClassField& item = cls->fields[i];
                    llvm::Value* address = builder.CreateStructGEP(shape, slot,
                        static_cast<unsigned>(i), binding.name + "." + item.name);
                    // The admitted stack class contains only trivially movable i64 fields.
                    llvm::Value* initial = binding.new_class
                        ? static_cast<llvm::Value*>(llvm::ConstantInt::getSigned(i64, item.initial))
                        : builder.CreateLoad(i64,
                            field_storage.at(field_key(binding.move_source, item.name)),
                            binding.move_source + "." + item.name + ".take");
                    builder.CreateStore(initial, address);
                    field_storage.emplace(field_key(binding.name, item.name), address);
                }
                storage.emplace(binding.name, slot);
                lexical_slots.push_back({binding.name, slot, false, bytes});
                continue;
            }
            llvm::Value* initial = lower(*binding.initializer, builder, i64, storage, field_storage, declarations, &region_moves);
            llvm::Value* slot = nullptr;
            if (binding.heap_owned) {
                // Store only an opaque, generation-checked lifetime token.
                // No raw region payload pointer crosses into guest LLVM IR.
                llvm::Value* handle = builder.CreateCall(regionNew,
                    {region, initial}, binding.name + ".owned.handle");
                slot = builder.CreateAlloca(i64, nullptr, binding.name + ".owned.handle.slot");
                builder.CreateStore(handle, slot);
                llvm::Value* live = builder.CreateAlloca(builder.getInt1Ty(), nullptr,
                    binding.name + ".owned.live");
                builder.CreateStore(builder.getTrue(), live);
                owned_live_flags.emplace(binding.name, live);
            } else {
                slot = builder.CreateAlloca(i64, nullptr, binding.name + ".stack.owned");
                builder.CreateLifetimeStart(slot, llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), 8));
                builder.CreateStore(initial, slot);
            }
            storage.emplace(binding.name, slot);
            lexical_slots.push_back({binding.name, slot, binding.heap_owned, 8});
        }
        std::vector<const Statement*> deferred_effects;
        auto lower_effect = [&](const Statement& effect) {
            if (effect.kind == Statement::Kind::FieldWrite) {
                llvm::Value* updated = lower(*effect.expression, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                builder.CreateStore(updated, field_storage.at(
                    field_key(effect.receiver, effect.field)));
            } else if (effect.kind == Statement::Kind::Call) {
                lower(*effect.expression, builder, i64, storage, field_storage, declarations, &region_moves);
            } else {
                throw std::runtime_error("unsupported native conditional arm");
            }
        };
        // Branch-local trivial i64 owners are initialized only along their
        // reached predecessor and end their lifetime before the join. They
        // never enter the enclosing lexical_slots or deferred cleanup list.
        auto lower_arm = [&](const std::vector<Statement>& arm) {
            std::vector<std::string> scoped_names;
            for (const auto& action : arm) {
                if (action.kind != Statement::Kind::ScopedLocal) {
                    lower_effect(action);
                    continue;
                }
                const LocalBinding& local = *action.branch_local;
                llvm::Value* init = lower(*local.initializer, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                llvm::Value* slot = builder.CreateAlloca(i64, nullptr,
                    local.name + ".arm.stack");
                builder.CreateLifetimeStart(slot,
                    llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), 8));
                builder.CreateStore(init, slot);
                if (!storage.emplace(local.name, slot).second)
                    throw std::runtime_error("branch-local shadows existing LLVM storage");
                scoped_names.push_back(local.name);
            }
            for (auto it = scoped_names.rbegin(); it != scoped_names.rend(); ++it) {
                builder.CreateLifetimeEnd(storage.at(*it),
                    llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), 8));
                storage.erase(*it);
            }
        };
        for (const auto& stmt : fn.statements) {
            if (stmt.kind == Statement::Kind::Defer) {
                deferred_effects.push_back(&stmt);
                continue;
            }
            if (stmt.kind == Statement::Kind::Conditional) {
                llvm::Value* lhs = lower(*stmt.condition_lhs, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                llvm::Value* rhs = lower(*stmt.condition_rhs, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                llvm::Value* condition = builder.CreateICmpEQ(lhs, rhs, "if.else.eq");
                llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(
                    context, "if.then", function);
                llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create(
                    context, "if.else", function);
                llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
                    context, "if.merge", function);
                builder.CreateCondBr(condition, thenBlock, elseBlock);
                builder.SetInsertPoint(thenBlock);
                lower_arm(stmt.then_statements);
                builder.CreateBr(mergeBlock);
                builder.SetInsertPoint(elseBlock);
                lower_arm(stmt.else_statements);
                builder.CreateBr(mergeBlock);
                builder.SetInsertPoint(mergeBlock);
                continue;
            }
            lower_effect(stmt);
        }

        // Emit observable actor effects in source order. Reordering all sends
        // ahead of all stops incorrectly executes a stopped worker's messages.
        for (const auto& action : fn.actor_actions) {
            if (action.kind == ActorAction::Kind::Send) {
                const auto& send = fn.actor_sends.at(action.index);
                llvm::Value* payload = lower(*send.payload, builder, i64, storage, field_storage, declarations, &region_moves);
                builder.CreateCall(sourceSend, {actorRefs.at(send.reference), payload});
            } else {
                builder.CreateCall(sourceStop, {actorRefs.at(fn.actor_stops.at(action.index))});
            }
        }
        // One cleanup authority for every guarded-return path. A function
        // with only its final return and no registered defer has exactly one
        // exit by construction, so preserve its direct ret IR contract.
        // That fast path still runs the same drop and lifetime cleanup.
        auto emit_lexical_cleanup = [&]() {
            for (auto it = lexical_slots.rbegin(); it != lexical_slots.rend(); ++it) {
                if (it->heap_owned) {
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
                    // llvm.lifetime.end is not a source-level destructor.
                    builder.CreateLifetimeEnd(it->address,
                        llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), it->bytes));
                }
            }
            if (region != nullptr) builder.CreateCall(regionClose, {region});
        };
        if (fn.early_returns.empty() && deferred_effects.empty()) {
            // Maintain stable native IR (ret i64 42, for example) for
            // functions without a multi-exit cleanup obligation.
            llvm::Value* result = fn.result
                ? lower(*fn.result, builder, i64, storage, field_storage, declarations, &region_moves)
                : nullptr;
            emit_lexical_cleanup();
            if (fn.returns_void) builder.CreateRetVoid();
            else builder.CreateRet(result);
        } else {
            llvm::Value* return_snapshot = fn.returns_void
                ? nullptr : builder.CreateAlloca(fn.returns_bool ? builder.getInt1Ty() : i64,
                    nullptr, "return.snapshot.slot");
            llvm::BasicBlock* cleanup = llvm::BasicBlock::Create(
                context, "exit.cleanup", function);
            auto branch_to_cleanup = [&](llvm::Value* value) {
                if (!fn.returns_void) builder.CreateStore(value, return_snapshot);
                builder.CreateBr(cleanup);
            };
            for (const auto& guard : fn.early_returns) {
                llvm::Value* left = lower(*guard.lhs, builder, i64, storage, field_storage,
                    declarations, &region_moves);
                llvm::Value* right = lower(*guard.rhs, builder, i64, storage, field_storage,
                    declarations, &region_moves);
                llvm::Value* equal = builder.CreateICmpEQ(left, right, "return.guard.eq");
                llvm::BasicBlock* taken = llvm::BasicBlock::Create(context, "guard.return", function);
                llvm::BasicBlock* next = llvm::BasicBlock::Create(context, "guard.next", function);
                builder.CreateCondBr(equal, taken, next);
                builder.SetInsertPoint(taken);
                llvm::Value* selected = lower(*guard.result, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                branch_to_cleanup(selected);
                builder.SetInsertPoint(next);
            }
            llvm::Value* result = fn.result
                ? lower(*fn.result, builder, i64, storage, field_storage,
                    declarations, &region_moves) : nullptr;
            branch_to_cleanup(result);
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
            else builder.CreateRet(builder.CreateLoad(
                fn.returns_bool ? builder.getInt1Ty() : i64,
                return_snapshot, "return.snapshot"));
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
            b.CreateCall(llvm::Intrinsic::getDeclaration(&module, llvm::Intrinsic::trap));
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
    if (native_main) {
        // OS/process ABI boundary: never expose i64/void Oreslang main as C main.
        // Reject invalid statuses without Unix low-byte truncation (256 -> 0).
        llvm::Type* i32 = llvm::Type::getInt32Ty(context);
        auto* host_main = llvm::Function::Create(
            llvm::FunctionType::get(i32, false),
            llvm::Function::ExternalLinkage, "main", module);
        auto* entry = llvm::BasicBlock::Create(context, "entry", host_main);
        llvm::IRBuilder<> builder(entry);
        llvm::Function* guest = declarations.at("main");
        llvm::Value* result = builder.CreateCall(guest);
        if (main_it->returns_void) {
            builder.CreateRet(llvm::ConstantInt::get(i32, 0));
        } else {
            llvm::Value* valid = builder.CreateICmpULE(
                result, llvm::ConstantInt::get(i64, 255), "valid.exit.status");
            llvm::Value* narrowed = builder.CreateTrunc(result, i32);
            builder.CreateRet(builder.CreateSelect(valid, narrowed,
                llvm::ConstantInt::get(i32, 1), "checked.exit.status"));
        }
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
