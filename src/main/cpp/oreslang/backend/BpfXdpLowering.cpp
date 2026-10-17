#include "oreslang/backend/BpfXdpLowering.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#if LLVM_VERSION_MAJOR >= 21
#include "llvm/TargetParser/Triple.h"
#endif

namespace oreslang {

std::string emit_verified_bpf_xdp_ir(const Program& source) {
    // No fallback to the ordinary i64 guest/actor/runtime lowering. A single
    // constant-action XDP entrypoint is the complete initial admission set.
    if (!source.module.empty() || !source.classes.empty() ||
        !source.actors.empty() || !source.cooperative_loops.empty() ||
        source.functions.size() != 1)
        throw std::runtime_error("eBPF XDP requires one top-level exported ores_xdp function");

    const auto& fn = source.functions.front();
    if (fn.name != "ores_xdp" || !fn.exported || !fn.parameters.empty() ||
        !fn.generic_parameters.empty() || fn.returns_void ||
        !fn.result || fn.result->kind != Expression::Kind::Integer ||
        !fn.locals.empty() || !fn.statements.empty() || !fn.early_returns.empty() ||
        !fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
        !fn.actor_stops.empty() || !fn.actor_actions.empty() ||
        !fn.actor_field_writes.empty() || fn.scalar_while ||
        fn.caught_throw_value || !fn.caught_throw_defers.empty() ||
        fn.has_declared_effects)
        throw std::runtime_error("eBPF XDP admits only pub fnc ores_xdp(): int { return 0..3; }");

    // Stage 0 admits only the basic XDP actions supported without redirect state:
    // XDP_ABORTED=0, DROP=1, PASS=2, TX=3. XDP_REDIRECT=4 requires
    // bpf_redirect()/bpf_redirect_map() to establish redirect metadata and is
    // intentionally withheld until typed helper/map capabilities exist.
    const std::int64_t action = fn.result->integer;
    if (action < 0 || action > 3)
        throw std::runtime_error("eBPF XDP return action must be an integer literal 0..3");

    const auto profile = TargetAbi::parse("bpfel");
    if (!profile.experimental_bpf_ir() || profile.pointer_bits() != 64 ||
        profile.endian() != "little")
        throw std::runtime_error("eBPF XDP ABI profile mismatch");

    llvm::LLVMContext context;
    llvm::Module module("oreslang-bpf-xdp", context);
#if LLVM_VERSION_MAJOR >= 21
    module.setTargetTriple(llvm::Triple("bpfel"));
#else
    module.setTargetTriple("bpfel");
#endif
    module.setDataLayout("e-m:e-p:64:64-i64:64-i128:128-n32:64-S128");
    if (module.getDataLayout().getPointerSizeInBits(0) != 64 ||
        !module.getDataLayout().isLittleEndian())
        throw std::runtime_error("eBPF XDP LLVM data layout mismatch");

    auto* i32 = llvm::Type::getInt32Ty(context);
#if LLVM_VERSION_MAJOR >= 18
    auto* context_ptr = llvm::PointerType::get(context, 0);
#else
    auto* context_ptr = llvm::Type::getInt8PtrTy(context);
#endif
    auto* signature = llvm::FunctionType::get(i32, {context_ptr}, false);
    auto* xdp = llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                                       "ores_xdp", module);
    // Kernel BPF has no language-level exception/unwind path. Make that
    // explicit so even -O0 object emission cannot grow .eh_frame relocations.
    xdp->addFnAttr(llvm::Attribute::NoUnwind);
    xdp->setSection("xdp");
    xdp->getArg(0)->setName("ctx"); // opaque, never dereferenced by this slice
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", xdp));
    builder.CreateRet(llvm::ConstantInt::get(i32, static_cast<std::uint64_t>(action)));

    // Stage 0 deliberately marks the object non-GPL. This is a kernel capability
    // restriction, not a legal license declaration: later helper/kfunc support
    // must add an explicit source/deployment licensing contract before GPL-only
    // capabilities can ever be admitted.
    auto* license_value = llvm::ConstantDataArray::getString(context, "Proprietary", true);
    auto* license = new llvm::GlobalVariable(
        module, license_value->getType(), true,
        llvm::GlobalValue::ExternalLinkage, license_value, "_license");
    license->setSection("license");

    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    if (llvm::verifyModule(module, &diagnostics))
        throw std::runtime_error("eBPF LLVM IR verifier: " + diagnostics.str());
    std::string emitted;
    llvm::raw_string_ostream out(emitted);
    module.print(out, nullptr);
    out.flush();
    return emitted;
}

} // namespace oreslang
