#include "oreslang/backend/ManagedRefLowering.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

using oreslang::backend::ActorRef32Access;
using oreslang::backend::emit_checked_actor_ref32_load;
using oreslang::backend::emit_checked_actor_ref32_store;

static void require(bool yes, const char* why) {
    if (!yes) throw std::runtime_error(why);
}

static void produce_checked_llvm(llvm::Module& module) {
    auto& ctx = module.getContext();
    auto* i1 = llvm::Type::getInt1Ty(ctx);
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* ptr = llvm::Type::getInt8Ty(ctx)->getPointerTo();
    // Object payload is a native 64-bit scalar plus a physical i32 edge.
    // C++ test ABI mirrors this exact, non-packed native representation.
    auto* object_ty = llvm::StructType::create(ctx, {i64, i32}, "ActorNode");
    const llvm::StructLayout* layout = module.getDataLayout().getStructLayout(object_ty);
    require(layout->getElementOffset(1) == 8 &&
            layout->getSizeInBytes() == 16,
            "compact reference field physical layout changed");

    auto* auth_ty = llvm::FunctionType::get(
        i1, {ptr, ptr, i64, i32}, false);
    auto* auth = llvm::Function::Create(auth_ty,
        llvm::Function::ExternalLinkage, "oreslang_test_original_proof_check", module);

    // Both entrypoints accept a source OBJECT pointer (never a direct
    // unchecked slot address), a trusted actor base/span, and a trusted
    // original-generation proof. The field GEP is typed i32.
    auto* read_ty = llvm::FunctionType::get(i64,
        {ptr, ptr, i64, ptr}, false);
    auto* read_fn = llvm::Function::Create(read_ty,
        llvm::Function::ExternalLinkage, "oreslang_test_read_compact", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", read_fn));
        auto arg = read_fn->arg_begin();
        auto* object = &*arg++;
        auto* base = &*arg++;
        auto* span = &*arg++;
        auto* original = &*arg++;
        auto* typed = b.CreatePointerCast(object, object_ty->getPointerTo());
        auto* field = b.CreateStructGEP(object_ty, typed, 1, "actor.link.i32");
        ActorRef32Access access{field, base, span, original, auth};
        b.CreateRet(emit_checked_actor_ref32_load(module, b, access, i64, 8));
    }

    auto* write_ty = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
        {ptr, ptr, i64, ptr, i64}, false);
    auto* write_fn = llvm::Function::Create(write_ty,
        llvm::Function::ExternalLinkage, "oreslang_test_write_compact", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", write_fn));
        auto arg = write_fn->arg_begin();
        auto* object = &*arg++;
        auto* base = &*arg++;
        auto* span = &*arg++;
        auto* original = &*arg++;
        auto* incoming = &*arg++;
        auto* typed = b.CreatePointerCast(object, object_ty->getPointerTo());
        auto* field = b.CreateStructGEP(object_ty, typed, 1, "actor.link.i32");
        ActorRef32Access access{field, base, span, original, auth};
        emit_checked_actor_ref32_store(module, b, access, incoming, 8);
        b.CreateRetVoid();
    }

    std::string errors;
    llvm::raw_string_ostream diagnostic(errors);
    if (llvm::verifyModule(module, &diagnostic))
        throw std::runtime_error("invalid native compact-ref LLVM IR: " +
                                  diagnostic.str());
}

int main(int argc, char** argv) {
    try {
        llvm::LLVMContext ctx;
        llvm::Module module("actor-compact32-ir", ctx);
        module.setDataLayout("e-p:64:64-i64:64-n8:16:32:64");
        produce_checked_llvm(module);
        std::string printed;
        llvm::raw_string_ostream out(printed);
        module.print(out, nullptr);
        out.flush();

        const auto includes = [&](const std::string& token) {
            require(printed.find(token) != std::string::npos, token.c_str());
        };
        includes("type { i64, i32 }");
        includes("load i32");
        includes("ref32.proof");
        includes("ref32.bounds");
        includes("ref32.deref");
        includes("ref32.invalid");
        includes("ref32.byte_offset = shl i64");
        includes("call i1 @oreslang_test_original_proof_check");
        includes("@llvm.trap");
        includes("getelementptr i8");
        require(printed.find("getelementptr inbounds i8") == std::string::npos,
                "native ref offset was incorrectly marked inbounds");

        // The lowering must fail closed when caller evidence is absent or
        // the target ABI is not proven to use 64-bit native pointers.
        {
            llvm::Module wasm("not-native64", ctx);
            wasm.setDataLayout("e-p:32:32-i64:64-n32:64");
            auto* fn = llvm::Function::Create(
                llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                    {llvm::Type::getInt8Ty(ctx)->getPointerTo(),
                     llvm::Type::getInt64Ty(ctx)}, false),
                llvm::Function::ExternalLinkage, "probe", wasm);
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
            auto* p = fn->getArg(0);
            auto* span = fn->getArg(1);
            bool rejected = false;
            try {
                ActorRef32Access evidence{p, p, span, p,
                    wasm.getOrInsertFunction("check",
                        llvm::FunctionType::get(llvm::Type::getInt1Ty(ctx),
                            {p->getType(), p->getType(), span->getType(),
                             llvm::Type::getInt32Ty(ctx)}, false))};
                (void)emit_checked_actor_ref32_load(wasm, b, evidence,
                    llvm::Type::getInt64Ty(ctx), 8);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "wasm32 incorrectly emitted native64 actor pointer arithmetic");
        }

        // A caller cannot forge an undersized access, omit the original
        // provenance proof, or emit compact pointers into a layout-less module.
        {
            llvm::Module negative("ref32-invalid-admission", ctx);
            negative.setDataLayout("e-p:64:64-i64:64-n8:16:32:64");
            auto* pty = llvm::Type::getInt8Ty(ctx)->getPointerTo();
            auto* span_ty = llvm::Type::getInt64Ty(ctx);
            auto* fail_fn = llvm::Function::Create(
                llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                    {pty, span_ty}, false),
                llvm::Function::ExternalLinkage, "probe_invalid", negative);
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fail_fn));
            auto* p = fail_fn->getArg(0);
            auto* span = fail_fn->getArg(1);
            auto checker = negative.getOrInsertFunction("original_proof",
                llvm::FunctionType::get(llvm::Type::getInt1Ty(ctx),
                    {pty, pty, span_ty, llvm::Type::getInt32Ty(ctx)}, false));
            ActorRef32Access valid{p, p, span, p, checker};
            auto must_deny = [&](const ActorRef32Access& access,
                                 std::uint64_t access_bytes,
                                 const char* why) {
                bool denied = false;
                try {
                    (void)emit_checked_actor_ref32_load(negative, b, access,
                        llvm::Type::getInt64Ty(ctx), access_bytes);
                } catch (const std::invalid_argument&) {
                    denied = true;
                }
                require(denied, why);
            };
            must_deny(valid, 1, "underreported i64 load width admitted");
            auto unproven = valid;
            unproven.original_proof = nullptr;
            must_deny(unproven, 8, "missing original generation proof admitted");
            llvm::Module unspecified("ref32-no-target-abi", ctx);
            auto* other_fn = llvm::Function::Create(
                llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                    {pty, span_ty}, false),
                llvm::Function::ExternalLinkage, "probe_unspecified", unspecified);
            llvm::IRBuilder<> other(llvm::BasicBlock::Create(ctx, "entry", other_fn));
            auto* other_p = other_fn->getArg(0);
            ActorRef32Access no_layout{
                other_p, other_p, other_fn->getArg(1), other_p,
                unspecified.getOrInsertFunction("original_proof",
                    llvm::FunctionType::get(llvm::Type::getInt1Ty(ctx),
                        {pty, pty, span_ty, llvm::Type::getInt32Ty(ctx)}, false))};
            bool denied = false;
            try {
                (void)emit_checked_actor_ref32_load(unspecified, other, no_layout,
                    llvm::Type::getInt64Ty(ctx), 8);
            } catch (const std::invalid_argument&) {
                denied = true;
            }
            require(denied, "missing target DataLayout admitted ref32 decode");
        }

        if (argc == 2 && std::string(argv[1]) == "--emit") {
            std::cout << printed;
            return 0;
        }
        require(argc == 1, "unknown managed-ref LLVM test argument");
        std::cout << "PASS physical i32 actor fields, checked LLVM decode/load/store, "
                     "native64 target verification and trap paths\n";
        return 0;
    } catch (const std::exception& err) {
        std::cerr << "FAIL actor managed-ref LLVM lowering: " << err.what() << "\n";
        return 1;
    }
}
