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

// Targets the ref32 lowering must support. Layout strings are LLVM's own for
// each triple. The i32 edge sits at byte 8 on every target; only the trailing
// padding differs (i686 SysV aligns i64 to 4, so the struct is 12 bytes).
struct Ref32Target final {
    const char* name;
    const char* triple;
    const char* layout;
    unsigned pointer_bits;
    std::uint64_t node_bytes;
};
static constexpr Ref32Target kTargets[] = {
    {"native64", "", "e-p:64:64-i64:64-n8:16:32:64", 64, 16},
    {"i686", "i686-unknown-linux-gnu",
     "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i128:128-f64:32:64-f80:32-n8:16:32-S128",
     32, 12},
    {"armv7", "armv7-unknown-linux-gnueabihf",
     "e-m:e-p:32:32-Fi8-i64:64-v128:64:128-a:0:32-n32-S64", 32, 16},
    {"wasm32", "wasm32-unknown-unknown",
     "e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-i128:128-n32:64-S128-ni:1:10:20", 32, 16},
};

static void produce_checked_llvm(llvm::Module& module, std::uint64_t node_bytes) {
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
            layout->getSizeInBytes() == node_bytes,
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
        // --emit prints the native64 module; --emit=<name> prints that target.
        std::string emit_target;
        if (argc == 2) {
            const std::string arg = argv[1];
            if (arg == "--emit") emit_target = "native64";
            else if (arg.rfind("--emit=", 0) == 0) emit_target = arg.substr(7);
            else throw std::runtime_error("unknown managed-ref LLVM test argument");
        }
        require(argc <= 2, "unknown managed-ref LLVM test argument");
        std::string emitted;

        for (const auto& target : kTargets) {
            llvm::Module module(std::string("actor-compact32-ir-") + target.name, ctx);
            module.setDataLayout(target.layout);
            require(module.getDataLayout().getPointerSizeInBits(0) == target.pointer_bits,
                    "test target table pointer width disagrees with its DataLayout");
            produce_checked_llvm(module, target.node_bytes);
            std::string printed;
            llvm::raw_string_ostream out(printed);
            module.print(out, nullptr);
            out.flush();
            if (emit_target == target.name) emitted = printed;

            const auto includes = [&](const std::string& token) {
                require(printed.find(token) != std::string::npos, token.c_str());
            };
            const auto excludes = [&](const std::string& token, const char* why) {
                require(printed.find(token) == std::string::npos, why);
            };
            // Identical on every width: the stored edge is always i32 and the
            // validator ABI is always (ptr, ptr, i64 span, i32 stored).
            includes("type { i64, i32 }");
            includes("load i32");
            // The source field must be admitted by a conditional branch
            // BEFORE any attempt to dereference its i32 encoded bits.
            const auto guard_at = printed.find("br i1 %ref32.source.valid");
            const auto first_load_at = printed.find("%ref32.encoded = load i32");
            require(guard_at != std::string::npos &&
                    first_load_at != std::string::npos &&
                    guard_at < first_load_at,
                    "source ref32 field read precedes source arena bounds check");
            includes("ref32.source.bounds");
            includes("ref32.source.read");
            includes("ref32.source.delta");
            includes("ref32.source.fits");
            includes("ref32.arena.no_wrap");
            includes("ref32.source.valid");
            includes("ref32.proof");
            includes("ref32.bounds");
            includes("ref32.deref");
            includes("ref32.invalid");
            includes("ref32.byte_offset = shl i64");
            includes("ref32.end.representable");
            includes("call i1 @oreslang_test_original_proof_check");
            includes("@llvm.trap");
            includes("getelementptr i8");
            excludes("getelementptr inbounds i8",
                     "native ref offset was incorrectly marked inbounds");
            // Every base conversion must use exactly the target pointer width.
            const std::string base_conv = "%ref32.base.address = ptrtoint ptr ";
            const std::string want_width =
                target.pointer_bits == 32 ? " to i32" : " to i64";
            std::size_t conversions = 0;
            for (std::size_t at = printed.find(base_conv); at != std::string::npos;
                    at = printed.find(base_conv, at + 1)) {
                const auto eol = printed.find('\n', at);
                const auto line = printed.substr(at, eol - at);
                require(line.size() >= want_width.size() &&
                        line.compare(line.size() - want_width.size(),
                                     want_width.size(), want_width) == 0,
                        "actor base ptrtoint does not match the target pointer width");
                ++conversions;
            }
            require(conversions == 2, "expected one checked base conversion per accessor");
            // Same target pointer width must be used for the source field.
            const std::string source_conv = "%ref32.source.address = ptrtoint ptr ";
            std::size_t source_conversions = 0;
            for (std::size_t at = printed.find(source_conv);
                 at != std::string::npos;
                 at = printed.find(source_conv, at + 1)) {
                const auto eol = printed.find('\\n', at);
                const auto line = printed.substr(at, eol - at);
                require(line.size() >= want_width.size() &&
                        line.compare(line.size() - want_width.size(),
                                     want_width.size(), want_width) == 0,
                        "source ref32 address conversion has wrong pointer width");
                ++source_conversions;
            }
            require(source_conversions == 2,
                    "expected source address guard in both load and store");

            if (target.pointer_bits == 32) {
                // 32-bit: base is widened for the i64 checks, the access end
                // is capped by the 32-bit address space, and GEP receives the
                // truncated, already-bounded offset.
                includes("%ref32.base.wide = zext i32 %ref32.base.address to i64");
                includes("%ref32.end.representable = icmp ule i64 %ref32.access_end, 4294967295");
                includes("%ref32.byte_offset.native = trunc i64 %ref32.byte_offset to i32");
            } else {
                includes("%ref32.end.representable = icmp ule i64 %ref32.access_end, -1");
                excludes("ref32.base.wide",
                         "native64 target widened a pointer it should not");
                excludes("ref32.byte_offset.native",
                         "native64 target truncated the decoded offset");
            }
        }

        // Unsupported pointer models must fail closed: 16-bit pointers, and
        // fat/capability pointers whose GEP index width differs from the
        // pointer width (CHERI-style "p:128:128:128:64").
        for (const char* unsupported : {"e-p:16:16-i64:64-n8:16",
                                        "e-p:64:64:64:32-i64:64-n32:64"}) {
            llvm::Module probe_module("ref32-unsupported-pointer-model", ctx);
            probe_module.setDataLayout(unsupported);
            auto* fn = llvm::Function::Create(
                llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                    {llvm::Type::getInt8Ty(ctx)->getPointerTo(),
                     llvm::Type::getInt64Ty(ctx)}, false),
                llvm::Function::ExternalLinkage, "probe", probe_module);
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
            auto* p = fn->getArg(0);
            auto* span = fn->getArg(1);
            bool rejected = false;
            try {
                ActorRef32Access evidence{p, p, span, p,
                    probe_module.getOrInsertFunction("check",
                        llvm::FunctionType::get(llvm::Type::getInt1Ty(ctx),
                            {p->getType(), p->getType(), span->getType(),
                             llvm::Type::getInt32Ty(ctx)}, false))};
                (void)emit_checked_actor_ref32_load(probe_module, b, evidence,
                    llvm::Type::getInt64Ty(ctx), 8);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, unsupported);
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

        if (!emit_target.empty()) {
            require(!emitted.empty(), "unknown --emit target");
            std::cout << emitted;
            return 0;
        }
        std::cout << "PASS physical i32 actor fields, checked LLVM decode/load/store, "
                     "native64 + i686/armv7/wasm32 target verification and trap paths\n";
        return 0;
    } catch (const std::exception& err) {
        std::cerr << "FAIL actor managed-ref LLVM lowering: " << err.what() << "\n";
        return 1;
    }
}
