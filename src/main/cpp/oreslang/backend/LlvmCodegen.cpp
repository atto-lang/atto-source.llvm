#include "oreslang/backend/LlvmCodegen.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/SHA256.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang {
namespace {

// Source-compiled nominal identity follows Java TaggedMessageCodec byte-for-
// byte; schema hashes are SHA-256 over a SORTED closed domain, not declaration
// order, and variant identity includes its declared wire tag.
struct GuestNominalCase {
    std::string qualified;
    std::string owner;
    std::string schema;
    std::uint32_t tag;
    std::array<std::uint8_t, 32> shape_hash;
    std::vector<std::uint8_t> wire;
};
std::array<std::uint8_t, 32> sha(std::string_view text) {
    return llvm::SHA256::hash(llvm::ArrayRef<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}
std::string hex(const std::array<std::uint8_t, 32>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto b : bytes) {
        result.push_back(digits[b >> 4]);
        result.push_back(digits[b & 15]);
    }
    return result;
}
void mp_string(std::vector<std::uint8_t>& out, const std::string& value) {
    if (value.size() <= 31)
        out.push_back(static_cast<std::uint8_t>(0xa0 | value.size()));
    else if (value.size() <= 255) {
        out.push_back(0xd9);
        out.push_back(static_cast<std::uint8_t>(value.size()));
    } else throw std::runtime_error("enum name exceeds wire string budget");
    out.insert(out.end(), value.begin(), value.end());
}
void mp_tag(std::vector<std::uint8_t>& out, std::uint32_t tag) {
    if (tag <= 127) out.push_back(static_cast<std::uint8_t>(tag));
    else if (tag <= 255) {
        out.push_back(0xcc);
        out.push_back(static_cast<std::uint8_t>(tag));
    } else if (tag <= 65535) {
        out.push_back(0xcd);
        out.push_back(static_cast<std::uint8_t>(tag >> 8));
        out.push_back(static_cast<std::uint8_t>(tag));
    } else {
        out.push_back(0xce);
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(static_cast<std::uint8_t>(tag >> shift));
    }
}
std::vector<GuestNominalCase> tagged_cases(
    const Program& program, const Actor& actor) {
    if (actor.messages_type.empty()) return {};
    std::vector<std::string> names;
    for (const auto& p : program.tagged_patterns)
        if (p.name == actor.messages_type) names = p.alternatives;
    for (const auto& e : program.enums)
        if (e.name == actor.messages_type)
            for (const auto& c : e.cases)
                names.push_back(e.name + "." + c.name);
    if (names.empty()) return {};
    if (names.size() > 64)
        throw std::runtime_error("tagged actor exceeds native 64-case limit");
    std::vector<std::string> sorted = names;
    std::sort(sorted.begin(), sorted.end());
    std::string fingerprint = "oreslang.tagged.domain.v1/"
                            + actor.messages_type + "/";
    for (const auto& name : sorted)
        fingerprint += std::to_string(name.size()) + ":" + name + ";";
    const std::string domain_hash = hex(sha(fingerprint));
    const auto shape_hash = sha("oreslang.actor.shape.v2/T[S,I]");
    std::vector<GuestNominalCase> result;
    for (const auto& name : names) {
        const auto dot = name.find('.');
        const std::string owner = name.substr(0, dot);
        const std::string variant = name.substr(dot + 1);
        const auto decl = std::find_if(program.enums.begin(), program.enums.end(),
            [&](const EnumDeclaration& d) { return d.name == owner; });
        if (decl == program.enums.end())
            throw std::runtime_error("unresolved enum identity while emitting LLVM");
        const auto item = std::find_if(decl->cases.begin(), decl->cases.end(),
            [&](const EnumCase& c) { return c.name == variant; });
        if (item == decl->cases.end() || item->has_payload)
            throw std::runtime_error("LLVM typed source subset requires payloadless cases");
        const std::uint32_t tag = item->tag;
        const std::string schema = "oreslang.enum." + domain_hash + "."
                                  + hex(sha(name + "#" + std::to_string(tag)));
        std::vector<std::uint8_t> wire{0x94, 0x02};
        mp_string(wire, schema);
        wire.push_back(0xc4); wire.push_back(32);
        wire.insert(wire.end(), shape_hash.begin(), shape_hash.end());
        wire.push_back(0x92); // [nominal enum owner, declared tag]
        mp_string(wire, owner);
        mp_tag(wire, tag);
        result.push_back({name, owner, schema, tag, shape_hash, std::move(wire)});
    }
    return result;
}

llvm::Value* lower(const Expression& expr,
                   llvm::IRBuilder<>& builder,
                   llvm::Type* i64,
                   const std::unordered_map<std::string, llvm::Value*>& parameters,
                   const std::unordered_map<std::string, llvm::Value*>& locals,
                   const std::unordered_map<std::string, llvm::Function*>& functions) {
    switch (expr.kind) {
        case Expression::Kind::Integer:
            return llvm::ConstantInt::getSigned(i64, expr.integer);
        case Expression::Kind::Boolean:
            return llvm::ConstantInt::get(i64, expr.integer ? 1 : 0);
        case Expression::Kind::Variable:
            if (auto local = locals.find(expr.name); local != locals.end()) {
                return builder.CreateLoad(i64, local->second, expr.name + ".load");
            }
            return parameters.at(expr.name);
        case Expression::Kind::Negate:
            return builder.CreateNeg(lower(*expr.left, builder, i64, parameters, locals, functions),
                                     "neg");
        case Expression::Kind::Add:
            return builder.CreateAdd(lower(*expr.left, builder, i64, parameters, locals, functions),
                                     lower(*expr.right, builder, i64, parameters, locals, functions),
                                     "add");
        case Expression::Kind::Subtract:
            return builder.CreateSub(lower(*expr.left, builder, i64, parameters, locals, functions),
                                     lower(*expr.right, builder, i64, parameters, locals, functions),
                                     "sub");
        case Expression::Kind::Multiply:
            return builder.CreateMul(lower(*expr.left, builder, i64, parameters, locals, functions),
                                     lower(*expr.right, builder, i64, parameters, locals, functions),
                                     "mul");
        case Expression::Kind::Call: {
            std::vector<llvm::Value*> arguments;
            for (const auto& arg : expr.arguments) {
                arguments.push_back(lower(*arg, builder, i64, parameters, locals, functions));
            }
            return builder.CreateCall(functions.at(expr.name), arguments, "call");
        }
    }
    throw std::runtime_error("invalid expression kind while lowering LLVM IR");
}

} // namespace

std::string emit_verified_llvm_ir(const Program& source) {
    llvm::LLVMContext context;
    llvm::Module module("oreslang-source", context);
    llvm::IntegerType* i64 = llvm::Type::getInt64Ty(context);
    llvm::Type* ptr = llvm::PointerType::get(context, 0);
    llvm::FunctionCallee heapOpen = module.getOrInsertFunction(
            "oreslang_owned_i64_region_open",
            llvm::FunctionType::get(ptr, {i64}, false));
    llvm::FunctionCallee heapNew = module.getOrInsertFunction(
            "oreslang_owned_i64_region_new",
            llvm::FunctionType::get(ptr, {ptr, i64}, false));
    llvm::FunctionCallee heapDrop = module.getOrInsertFunction(
            "oreslang_owned_i64_region_drop",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr, ptr}, false));
    llvm::FunctionCallee heapClose = module.getOrInsertFunction(
            "oreslang_owned_i64_region_close",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr}, false));

    llvm::FunctionCallee fieldInit = module.getOrInsertFunction(
            "ores_actor_i64_heap_field_init",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                    {ptr, i64, i64, i64, i64}, false));
    llvm::FunctionCallee fieldRead = module.getOrInsertFunction(
            "ores_actor_i64_heap_field_read",
            llvm::FunctionType::get(i64, {ptr, i64, i64, i64}, false));

    llvm::FunctionCallee fieldInitMutable = module.getOrInsertFunction(
            "ores_actor_i64_heap_field_init_mutable",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                    {ptr, i64, i64, i64, i64}, false));
    llvm::FunctionCallee fieldWrite = module.getOrInsertFunction(
            "ores_actor_i64_heap_field_write",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                    {ptr, i64, i64, i64, i64}, false));

    std::unordered_map<std::string, llvm::Function*> declarations;
    std::unordered_map<std::string, llvm::Function*> actorInitializers;
    std::unordered_map<std::string, llvm::Function*> actorMessageHandlers;
    // Compiler-owned nominal metadata: source order fixes callback ordinal,
    // while the SORTED domain fingerprint fixes Java-compatible wire identity.
    std::unordered_map<std::string, std::vector<GuestNominalCase>> taggedCases;
    std::unordered_map<std::string, llvm::Constant*> taggedContracts;
    std::unordered_map<std::string, llvm::Constant*> taggedIdentities;
    std::unordered_map<std::string,
        std::unordered_map<std::string, llvm::Constant*>> taggedFrames;
    llvm::IRBuilder<> constants(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* contractTy = llvm::StructType::get(context, {ptr, ptr, ptr});
    auto* nominalTy = llvm::StructType::get(context, {ptr, i32});
    for (const auto& actor : source.actors) {
        auto cases = tagged_cases(source, actor);
        if (cases.empty()) continue;
        std::vector<llvm::Constant*> contracts, identities;
        const auto prefix = actor.name + ".tagged.";
        const auto shapePtr = constants.CreateGlobalStringPtr("T[S,I]",
            prefix + "shape", 0, &module);
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const auto& c = cases[i];
            const std::string symbol = prefix + std::to_string(i);
            const auto schema = constants.CreateGlobalStringPtr(c.schema,
                symbol + ".schema", 0, &module);
            const auto owner = constants.CreateGlobalStringPtr(c.owner,
                symbol + ".owner", 0, &module);
            auto* rawHash = llvm::ConstantDataArray::get(context,
                llvm::ArrayRef<std::uint8_t>(c.shape_hash.data(),
                                             c.shape_hash.size()));
            auto* hashGlobal = new llvm::GlobalVariable(module, rawHash->getType(),
                true, llvm::GlobalValue::PrivateLinkage, rawHash, symbol + ".sha");
            contracts.push_back(llvm::ConstantStruct::get(contractTy,
                {schema, shapePtr, hashGlobal}));
            identities.push_back(llvm::ConstantStruct::get(nominalTy,
                {owner, llvm::ConstantInt::get(i32, c.tag)}));
            const std::string wire(reinterpret_cast<const char*>(c.wire.data()),
                                   c.wire.size());
            taggedFrames[actor.name].emplace(c.qualified,
                constants.CreateGlobalStringPtr(wire, symbol + ".wire", 0, &module));
        }
        auto* contractArray = llvm::ConstantArray::get(
            llvm::ArrayType::get(contractTy, contracts.size()), contracts);
        auto* identityArray = llvm::ConstantArray::get(
            llvm::ArrayType::get(nominalTy, identities.size()), identities);
        taggedContracts[actor.name] = new llvm::GlobalVariable(module,
            contractArray->getType(), true, llvm::GlobalValue::PrivateLinkage,
            contractArray, prefix + "contracts");
        taggedIdentities[actor.name] = new llvm::GlobalVariable(module,
            identityArray->getType(), true, llvm::GlobalValue::PrivateLinkage,
            identityArray, prefix + "identities");
        taggedCases.emplace(actor.name, std::move(cases));
    }
    const std::string actorSymbolPrefix = "oreslang_actor_"
        + (source.module.empty() ? "" : source.module + "_");
    std::unordered_map<std::string, std::size_t> actorFieldCounts;
    for (const auto& actor : source.actors) {
        auto* ctorType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(context), {ptr, i64, i64}, false);
        actorInitializers.emplace(actor.name, llvm::Function::Create(
            ctorType, llvm::Function::ExternalLinkage,
            actorSymbolPrefix + actor.name + "_init", module));
        // The native i64 heap requires capacity >=1, even for a stateless
        // actor with an exclusively typed inbound message contract.
        actorFieldCounts.emplace(actor.name, std::max<std::size_t>(1, actor.fields.size()));
        const auto method = std::find_if(actor.methods.begin(), actor.methods.end(),
            [&](const Function& f) { return f.name ==
                (actor.messages_type.empty() ? "on_message" : "receive"); });
        if (method != actor.methods.end() && method->parameters.size() == 1) {
            auto* msgType = taggedCases.count(actor.name)
                ? llvm::FunctionType::get(i64, {ptr, i64, i64, i64, ptr, i64}, false)
                : llvm::FunctionType::get(i64, {ptr, i64, i64, i64}, false);
            actorMessageHandlers.emplace(actor.name, llvm::Function::Create(
                msgType, llvm::Function::ExternalLinkage,
                actorSymbolPrefix + actor.name + "_"
                + (actor.messages_type.empty() ? "on_message" : "receive"), module));
        }
    }
    const auto sourceSpawn = module.getOrInsertFunction(
        "oreslang_source_actor_spawn",
        llvm::FunctionType::get(ptr, {ptr, ptr, i64}, false));
    const auto sourceSpawnTagged = module.getOrInsertFunction(
        "oreslang_source_actor_spawn_tagged_variants",
        llvm::FunctionType::get(ptr, {ptr, ptr, i64, ptr, ptr, i64}, false));
    const auto sourceSendTaggedChecked = module.getOrInsertFunction(
        "oreslang_source_actor_send_variant_checked",
        llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                {ptr, ptr, i64}, false));
    const auto sourceStop = module.getOrInsertFunction(
        "oreslang_source_actor_stop",
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr}, false));
    const auto sourceSend = module.getOrInsertFunction(
        "oreslang_source_actor_send",
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr, i64}, false));
    // Host main blocks only at its outer ABI boundary. The guest continuation
    // itself executes on the root ActorGroup CONTROL lane after a real Future
    // completes, never inline on the initializing worker.
    const auto sourceAwaitReady = module.getOrInsertFunction(
        "oreslang_source_actor_await_ready",
        llvm::FunctionType::get(i64, {ptr, ptr}, false));

    // Declare all functions first, enabling forward calls and recursion.
    for (const auto& fn : source.functions) {
        const std::string symbol = source.module.empty()
            ? fn.name : source.module + "." + fn.name;
        const std::vector<llvm::Type*> argument_types(fn.parameters.size(), i64);
        llvm::FunctionType* signature = llvm::FunctionType::get(i64, argument_types, false);
        // ABI and public/private linker visibility are not stable yet.
        declarations.emplace(fn.name, llvm::Function::Create(
            signature, llvm::Function::ExternalLinkage, symbol, module));
    }
    for (const auto& fn : source.functions) {
        llvm::Function* function = declarations.at(fn.name);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", function);
        llvm::IRBuilder<> builder(entry);
        std::unordered_map<std::string, llvm::Value*> parameters;
        std::size_t index = 0;
        for (auto& arg : function->args()) {
            arg.setName(fn.parameters[index]);
            parameters.emplace(fn.parameters[index], &arg);
            ++index;
        }
        std::unordered_map<std::string, llvm::Value*> actorRefs;
        std::unordered_map<std::string, std::string> actorRefTypes;
        for (const auto& spawn : fn.actor_spawns) {
            auto found = actorMessageHandlers.find(spawn.actor_type);
            llvm::Value* handler = found == actorMessageHandlers.end()
                ? static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(ptr)))
                : static_cast<llvm::Value*>(found->second);
            llvm::Value* ref;
            if (taggedCases.count(spawn.actor_type)) {
                ref = builder.CreateCall(sourceSpawnTagged,
                    {actorInitializers.at(spawn.actor_type), handler,
                     llvm::ConstantInt::get(i64, actorFieldCounts.at(spawn.actor_type)),
                     taggedContracts.at(spawn.actor_type),
                     taggedIdentities.at(spawn.actor_type),
                     llvm::ConstantInt::get(i64, taggedCases.at(spawn.actor_type).size())},
                    spawn.name + ".native.tagged.actor.ref");
            } else {
                ref = builder.CreateCall(sourceSpawn,
                    {actorInitializers.at(spawn.actor_type), handler,
                     llvm::ConstantInt::get(i64, actorFieldCounts.at(spawn.actor_type))},
                    spawn.name + ".native.actor.ref");
            }
            actorRefs.emplace(spawn.name, ref);
            actorRefTypes.emplace(spawn.name, spawn.actor_type);
        }
        // Scalar locals are either stack owned ('int') or explicitly backed
        // by a bounded native lexical heap region ('owned int'). The latter
        // has one region per function activation. This is not yet an actor
        // heap that survives between mailbox turns.
        struct OwnedSlot { llvm::Value* address; bool heap; };
        std::unordered_map<std::string, llvm::Value*> locals;
        std::vector<OwnedSlot> slots;
        const auto heapCount = std::count_if(
                fn.locals.begin(), fn.locals.end(),
                [](const LocalBinding& item) { return item.heap_owned; });
        llvm::Value* region = nullptr;
        if (heapCount != 0) {
            region = builder.CreateCall(heapOpen, {
                    llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(heapCount))
            }, "actor.owned.lexical.region");
        }
        for (const auto& local : fn.locals) {
            llvm::Value* initial = lower(*local.initializer, builder, i64,
                                         parameters, locals, declarations);
            llvm::Value* address = nullptr;
            if (local.heap_owned) {
                address = builder.CreateCall(heapNew, {region, initial},
                                             local.name + ".native.owned");
            } else {
                address = builder.CreateAlloca(i64, nullptr, local.name + ".stack.owned");
                builder.CreateLifetimeStart(address, llvm::ConstantInt::get(i64, 8));
                builder.CreateStore(initial, address);
            }
            locals.emplace(local.name, address);
            slots.push_back({address, local.heap_owned});
        }
        auto emitSend = [&](llvm::IRBuilder<>& at, const ActorSend& send,
                            const std::unordered_map<std::string, llvm::Value*>& refs,
                            const std::unordered_map<std::string, llvm::Value*>& params,
                            const std::unordered_map<std::string, llvm::Value*>& slots) {
            const auto& actorType = actorRefTypes.at(send.reference);
            if (!send.tagged_variant.empty()) {
                const auto& cases = taggedCases.at(actorType);
                const auto it = std::find_if(cases.begin(), cases.end(),
                    [&](const GuestNominalCase& c) { return c.qualified == send.tagged_variant; });
                if (it == cases.end())
                    throw std::runtime_error("source tagged send outside closed domain");
                at.CreateCall(sourceSendTaggedChecked,
                    {refs.at(send.reference),
                     taggedFrames.at(actorType).at(send.tagged_variant),
                     llvm::ConstantInt::get(i64, it->wire.size())});
            } else {
                auto* payload = lower(*send.payload, at, i64, params, slots, declarations);
                at.CreateCall(sourceSend, {refs.at(send.reference), payload});
            }
        };
        llvm::Value* result = nullptr;
        if (!fn.ready_await_actor.empty()) {
            // First compiler-generated suspend point: the program's original
            // post-await guest work is compiled into a separate callback.
            // The trusted top-level entrypoint waits outside the native pool;
            // the callback is scheduled on the exclusive root actor lane.
            auto* continuationType = llvm::FunctionType::get(i64, {ptr}, false);
            auto* resume = llvm::Function::Create(continuationType,
                llvm::Function::InternalLinkage,
                function->getName().str() + ".ready_continuation", module);
            auto* resumed = llvm::BasicBlock::Create(context, "admitted", resume);
            llvm::IRBuilder<> resumeBuilder(resumed);
            std::unordered_map<std::string, llvm::Value*> resumedActors{
                {fn.ready_await_actor, &*resume->arg_begin()}};
            const std::unordered_map<std::string, llvm::Value*> noLocals;
            const std::unordered_map<std::string, llvm::Value*> noParams;
            for (const auto& send : fn.actor_sends)
                emitSend(resumeBuilder, send, resumedActors, noParams, noLocals);

            auto* resumedResult = lower(*fn.result, resumeBuilder, i64,
                                        noParams, noLocals, declarations);
            resumeBuilder.CreateRet(resumedResult);
            result = builder.CreateCall(sourceAwaitReady,
                {actorRefs.at(fn.ready_await_actor), resume}, "future.await.result");
        } else {
            result = lower(*fn.result, builder, i64, parameters,
                           locals, declarations);
            for (const auto& send : fn.actor_sends)
                emitSend(builder, send, actorRefs, parameters, locals);

        }
        for (const auto& name : fn.actor_stops) {
            builder.CreateCall(sourceStop, {actorRefs.at(name)});
        }
        for (auto iter = slots.rbegin(); iter != slots.rend(); ++iter) {
            if (iter->heap) {
                builder.CreateCall(heapDrop, {region, iter->address});
            } else {
                builder.CreateLifetimeEnd(iter->address, llvm::ConstantInt::get(i64, 8));
            }
        }
        if (region != nullptr) builder.CreateCall(heapClose, {region});
        builder.CreateRet(result);
    }

    // Native C entrypoint for a module-scoped main: retain the fully scoped
    // Bar.main symbol while providing the executable's conventional main ABI.
    if (!source.module.empty()) {
        auto entry = declarations.find("main");
        if (entry != declarations.end() && entry->second->arg_empty()) {
            auto* entryType = llvm::FunctionType::get(i64, {}, false);
            auto* nativeMain = llvm::Function::Create(
                entryType, llvm::Function::ExternalLinkage, "main", module);
            llvm::IRBuilder<> mainBuilder(
                llvm::BasicBlock::Create(context, "entry", nativeMain));
            mainBuilder.CreateRet(mainBuilder.CreateCall(entry->second, {}));
        }
    }

    // Actor field layout is fixed and private to an admitted native actor
    // heap. Runtime/FFI code is responsible for creating a heap with at least
    // actor.fields.size() slots and passing fresh scheduler turn tokens.
    for (const auto& actor : source.actors) {
        const std::string symbol = actorSymbolPrefix + actor.name;
        auto* initTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(context), {ptr, i64, i64}, false);
        auto* initialize = actorInitializers.at(actor.name);
        if (initialize->getFunctionType() != initTy)
            throw std::runtime_error("generated actor initializer ABI mismatch");
        auto* initBlock = llvm::BasicBlock::Create(context, "entry", initialize);
        llvm::IRBuilder<> initBuilder(initBlock);
        auto arg = initialize->arg_begin();
        llvm::Value* actorHeap = &*arg++;
        llvm::Value* domain = &*arg++;
        llvm::Value* epoch = &*arg++;
        const std::unordered_map<std::string, llvm::Value*> noValues;
        const std::unordered_map<std::string, llvm::Function*> noFunctions;
        for (std::size_t index = 0; index < actor.fields.size(); ++index) {
            auto* value = lower(*actor.fields[index].initializer, initBuilder, i64,
                                noValues, noValues, noFunctions);
            initBuilder.CreateCall(actor.fields[index].field_mutable ? fieldInitMutable : fieldInit,
                 {actorHeap, domain, epoch,
                  llvm::ConstantInt::get(i64, index), value});
        }
        initBuilder.CreateRetVoid();

        for (const auto& method : actor.methods) {
            std::vector<llvm::Type*> types{ptr, i64, i64};
            types.insert(types.end(), method.parameters.size(), i64);
            if (method.is_receiver && taggedCases.count(actor.name)) {
                // Callback receives compiler-local case ordinal, owned frame
                // bytes and length. The mailbox validates the nominal
                // contract before the callback runs.
                types.push_back(ptr);
                types.push_back(i64);
            }
            auto* signature = llvm::FunctionType::get(i64, types, false);
            auto existing = actorMessageHandlers.find(actor.name);
            auto* fn = (method.name == "on_message" || method.name == "receive")
                           && existing != actorMessageHandlers.end()
                    ? existing->second
                    : llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                                             symbol + "_" + method.name, module);
            if (fn->getFunctionType() != signature)
                throw std::runtime_error("generated actor message ABI mismatch");
            auto* block = llvm::BasicBlock::Create(context, "entry", fn);
            llvm::IRBuilder<> builder(block);
            auto args = fn->arg_begin();
            llvm::Value* heap = &*args++;
            llvm::Value* currentDomain = &*args++;
            llvm::Value* currentEpoch = &*args++;
            std::unordered_map<std::string, llvm::Value*> parameters;
            for (const auto& name : method.parameters) {
                parameters.emplace(name, &*args++);
            }
            std::unordered_map<std::string, llvm::Value*> locals;
            for (std::size_t index = 0; index < actor.fields.size(); ++index) {
                llvm::Value* value = builder.CreateCall(fieldRead,
                    {heap, currentDomain, currentEpoch,
                     llvm::ConstantInt::get(i64, index)}, actor.fields[index].name);
                auto* slot = builder.CreateAlloca(i64, nullptr,
                                                actor.fields[index].name + ".field");
                builder.CreateStore(value, slot);
                locals.emplace(actor.fields[index].name, slot);
            }
            struct LocalSlot {llvm::Value* slot; bool heap;};
            std::vector<LocalSlot> ownedSlots;
            const auto count = std::count_if(method.locals.begin(), method.locals.end(),
                    [](const LocalBinding& local){return local.heap_owned;});
            llvm::Value* region = nullptr;
            if (count) region = builder.CreateCall(heapOpen,
                   {llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(count))});
            for (const auto& local : method.locals) {
                auto* value = lower(*local.initializer, builder, i64,
                                    parameters, locals, declarations);
                llvm::Value* address = nullptr;
                if (local.heap_owned) {
                    address = builder.CreateCall(heapNew, {region, value});
                } else {
                    address = builder.CreateAlloca(i64, nullptr, local.name + ".local");
                    builder.CreateLifetimeStart(address, llvm::ConstantInt::get(i64, 8));
                    builder.CreateStore(value, address);
                }
                locals.emplace(local.name, address);
                ownedSlots.push_back({address, local.heap_owned});
            }
            if (!method.tagged_arms.empty()) {
                const auto& cases = taggedCases.at(actor.name);
                auto* invalid = llvm::BasicBlock::Create(context, "match.invalid", fn);
                auto* joined = llvm::BasicBlock::Create(context, "match.join", fn);
                auto* dispatch = builder.CreateSwitch(
                    parameters.at(method.match_subject), invalid,
                    static_cast<unsigned>(method.tagged_arms.size()));
                for (const auto& arm : method.tagged_arms) {
                    const auto selected = std::find_if(cases.begin(), cases.end(),
                        [&](const GuestNominalCase& c) {
                            return c.qualified == arm.qualified_case;
                        });
                    if (selected == cases.end())
                        throw std::runtime_error("compiler enum arm escaped closed domain");
                    const std::size_t ordinal = std::distance(cases.begin(), selected);
                    auto* armBlock = llvm::BasicBlock::Create(context,
                        "match.nominal." + std::to_string(ordinal), fn);
                    dispatch->addCase(llvm::ConstantInt::get(i64, ordinal), armBlock);
                    builder.SetInsertPoint(armBlock);
                    for (const auto& assignment : arm.assignments) {
                        const auto field = std::find_if(actor.fields.begin(), actor.fields.end(),
                            [&](const LocalBinding& f) {
                                return f.name == assignment.field;
                            });
                        if (field == actor.fields.end() || !field->field_mutable)
                            throw std::runtime_error("invalid tagged actor field mutation");
                        auto* updated = lower(*assignment.value, builder, i64,
                                              parameters, locals, declarations);
                        builder.CreateCall(fieldWrite,
                            {heap, currentDomain, currentEpoch,
                             llvm::ConstantInt::get(i64,
                                 std::distance(actor.fields.begin(), field)), updated});
                        builder.CreateStore(updated, locals.at(assignment.field));
                    }
                    builder.CreateBr(joined);
                }
                builder.SetInsertPoint(invalid);
                // Runtime's trusted nominal schema table makes this
                // unreachable in well-formed deliveries, but never invoke
                // an arbitrary arm on an out-of-range case ordinal.
                builder.CreateRet(llvm::ConstantInt::getSigned(i64, -1));
                builder.SetInsertPoint(joined);
            }
            if (!method.boolean_arms.empty()) {
                auto* subject = parameters.at(method.match_subject);
                auto* condition = builder.CreateICmpNE(
                    subject, llvm::ConstantInt::get(i64, 0), "message.match.bool");
                auto* whenTrue = llvm::BasicBlock::Create(context, "match.true", fn);
                auto* whenFalse = llvm::BasicBlock::Create(context, "match.false", fn);
                auto* joined = llvm::BasicBlock::Create(context, "match.join", fn);
                builder.CreateCondBr(condition, whenTrue, whenFalse);
                // The parser proves exactly two exhaustive, nonoverlapping
                // bool arms; no implicit fallthrough or untyped default.
                for (const auto& arm : method.boolean_arms) {
                    builder.SetInsertPoint(arm.match_value ? whenTrue : whenFalse);
                    for (const auto& assignment : arm.assignments) {
                        const auto field = std::find_if(actor.fields.begin(), actor.fields.end(),
                            [&](const LocalBinding& f) { return f.name == assignment.field; });
                        if (field == actor.fields.end() || !field->field_mutable)
                            throw std::runtime_error("invalid receive match field mutation");
                        auto* updated = lower(*assignment.value, builder, i64,
                                              parameters, locals, declarations);
                        builder.CreateCall(fieldWrite,
                            {heap, currentDomain, currentEpoch,
                             llvm::ConstantInt::get(i64,
                                std::distance(actor.fields.begin(), field)), updated});
                        builder.CreateStore(updated, locals.at(assignment.field));
                    }
                    builder.CreateBr(joined);
                }
                builder.SetInsertPoint(joined);
            }
            for (const auto& assignment : method.assignments) {
                const auto match = std::find_if(actor.fields.begin(), actor.fields.end(),
                    [&](const LocalBinding& f) { return f.name == assignment.field; });
                // Parser validation proves the field exists and is mutable.
                if (match == actor.fields.end() || !match->field_mutable)
                    throw std::runtime_error("invalid actor field mutation lowering");
                llvm::Value* updated = lower(*assignment.value, builder, i64,
                                             parameters, locals, declarations);
                builder.CreateCall(fieldWrite,
                    {heap, currentDomain, currentEpoch,
                     llvm::ConstantInt::get(i64, std::distance(actor.fields.begin(), match)), updated});
                builder.CreateStore(updated, locals.at(assignment.field));
            }
            auto* result = lower(*method.result, builder, i64,
                                 parameters, locals, declarations);
            for (auto iter = ownedSlots.rbegin(); iter != ownedSlots.rend(); ++iter) {
                if (iter->heap) builder.CreateCall(heapDrop, {region, iter->slot});
                else builder.CreateLifetimeEnd(iter->slot, llvm::ConstantInt::get(i64, 8));
            }
            if (region) builder.CreateCall(heapClose, {region});
            builder.CreateRet(result);
        }
    }

    std::string diagnostics;
    llvm::raw_string_ostream errors(diagnostics);
    if (llvm::verifyModule(module, &errors)) {
        errors.flush();
        throw std::runtime_error("invalid generated LLVM module: " + diagnostics);
    }
    // Strict no-tracing-GC admission. The current backend emits stack-local scalars and an audited
    // bounded native i64 region.  Any future lowering must explicitly prove that its new
    // native calls cannot invoke a collector or allocate managed objects.
    // Reject indirect/unknown external calls by default.
    for (const llvm::Function& function : module) {
        if (function.isDeclaration()) continue;
        if (function.hasGC()) {
            throw std::runtime_error("LLVM strict no-GC forbids GC strategy annotations");
        }
        for (const llvm::BasicBlock& block : function) {
            for (const llvm::Instruction& instruction : block) {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call) continue;
                const llvm::Function* target = call->getCalledFunction();
                if (target == nullptr) {
                    throw std::runtime_error("LLVM strict no-GC rejects indirect calls without ownership proof");
                }
                if (target->isDeclaration()) {
                    const std::string name = target->getName().str();
                    if (name.rfind("llvm.lifetime.start", 0) != 0
                            && name.rfind("llvm.lifetime.end", 0) != 0
                            && name != "oreslang_owned_i64_region_open"
                            && name != "oreslang_owned_i64_region_new"
                            && name != "oreslang_owned_i64_region_drop"
                            && name != "oreslang_owned_i64_region_close"
                            && name != "ores_actor_i64_heap_field_init"
                            && name != "ores_actor_i64_heap_field_read"
                            && name != "ores_actor_i64_heap_field_init_mutable"
                            && name != "ores_actor_i64_heap_field_write"
                            && name != "oreslang_source_actor_spawn_tagged_variants"
                            && name != "oreslang_source_actor_send_variant_checked"
                            && name != "oreslang_source_actor_spawn"
                            && name != "oreslang_source_actor_stop"
                            && name != "oreslang_source_actor_send"
                            && name != "oreslang_source_actor_await_ready") {
                        throw std::runtime_error("LLVM strict no-GC rejects unverified external call '" + name + "'");
                    }
                }
            }
        }
    }

    std::string result;
    llvm::raw_string_ostream output(result);
    module.print(output, nullptr);
    output.flush();
    return result;
}

} // namespace oreslang
