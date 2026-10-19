#include "oreslang/backend/EbpfXdpCodegen.hpp"
#include "oreslang/types/EbpfXdpAdmission.hpp"

#include <stdexcept>
#include <string>

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#if LLVM_VERSION_MAJOR >= 21
#include "llvm/TargetParser/Triple.h"
#endif

namespace oreslang {
namespace {

llvm::Value* lower_scalar(const Expression& expr,
                          llvm::IRBuilder<>& ir,
                          llvm::Value* packet_bytes) {
    using Kind = Expression::Kind;
    auto* i64 = llvm::Type::getInt64Ty(ir.getContext());
    switch (expr.kind) {
        case Kind::Integer:
            return llvm::ConstantInt::getSigned(i64, expr.integer);
        case Kind::Variable:
            // EbpfXdpAdmission already proved the exact name and no pointer
            // ownership escape. Recheck on the emission boundary.
            if (expr.name != "packet_bytes")
                throw std::runtime_error("eBPF XDP unadmitted variable in emitter");
            return packet_bytes;
        case Kind::Negate:
            return ir.CreateNeg(lower_scalar(*expr.left, ir, packet_bytes), "xdp.neg");
        case Kind::Add:
            return ir.CreateAdd(lower_scalar(*expr.left, ir, packet_bytes),
                                lower_scalar(*expr.right, ir, packet_bytes), "xdp.add");
        case Kind::Subtract:
            return ir.CreateSub(lower_scalar(*expr.left, ir, packet_bytes),
                                lower_scalar(*expr.right, ir, packet_bytes), "xdp.sub");
        case Kind::Multiply:
            return ir.CreateMul(lower_scalar(*expr.left, ir, packet_bytes),
                                lower_scalar(*expr.right, ir, packet_bytes), "xdp.mul");
        default:
            throw std::runtime_error("eBPF XDP unadmitted expression in emitter");
    }
}

} // namespace

std::string emit_experimental_ebpf_xdp_ir(const Program& source) {
    // This function performs admission independently of caller/CLI checks.
    EbpfXdpAdmission::require(source);
    llvm::LLVMContext context;
    llvm::Module module("oreslang-experimental-xdp", context);
#if LLVM_VERSION_MAJOR >= 21
    module.setTargetTriple(llvm::Triple("bpfel"));
#else
    module.setTargetTriple("bpfel");
#endif
    // LLVM BPFEL 64-bit register/pointer ABI. Do not inherit host DataLayout.
    module.setDataLayout("e-m:e-p:64:64-i64:64-i128:128-n32:64-S128");
    if (module.getDataLayout().getPointerSizeInBits(0) != 64 ||
        !module.getDataLayout().isLittleEndian())
        throw std::runtime_error("eBPF XDP target ABI/data layout mismatch");

    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    const Function& src = source.functions.front();

    // The lexical guest policy has no host, heap, map, helper or packet
    // pointers. It may only inspect the checked packet byte count.
    auto* policy = llvm::Function::Create(
        llvm::FunctionType::get(i64, {i64}, false),
        llvm::GlobalValue::InternalLinkage, "oreslang_xdp_policy", module);
    policy->addFnAttr(llvm::Attribute::AlwaysInline);
    auto* length = policy->getArg(0);
    length->setName("packet_bytes");
    auto* next = llvm::BasicBlock::Create(context, "entry", policy);
    llvm::IRBuilder<> ir(next);

    for (const auto& guard : src.early_returns) {
        llvm::Value* left = lower_scalar(*guard.lhs, ir, length);
        llvm::Value* right = lower_scalar(*guard.rhs, ir, length);
        auto* taken = llvm::BasicBlock::Create(context, "guard.take", policy);
        auto* skip = llvm::BasicBlock::Create(context, "guard.skip", policy);
        ir.CreateCondBr(ir.CreateICmpEQ(left, right), taken, skip);
        ir.SetInsertPoint(taken);
        ir.CreateRet(lower_scalar(*guard.result, ir, length));
        ir.SetInsertPoint(skip);
    }
    ir.CreateRet(lower_scalar(*src.result, ir, length));

    // This is a restricted XDP context ABI: data at +0 and data_end at +4
    // are u32 offsets. Only length metadata is read: packet memory is never
    // dereferenced and its pointers cannot escape this wrapper.
    llvm::Type* ctx_ptr = llvm::PointerType::getUnqual(i32);
    auto* xdp = llvm::Function::Create(
        llvm::FunctionType::get(i32, {ctx_ptr}, false),
        llvm::GlobalValue::ExternalLinkage, "oreslang_xdp", module);
    xdp->setSection("xdp");
    xdp->getArg(0)->setName("ctx");
    auto* entry = llvm::BasicBlock::Create(context, "entry", xdp);
    auto* good = llvm::BasicBlock::Create(context, "ctx.valid", xdp);
    auto* bad = llvm::BasicBlock::Create(context, "ctx.invalid", xdp);
    llvm::IRBuilder<> b(entry);
    auto* data_ptr = b.CreateInBoundsGEP(i32, xdp->getArg(0), b.getInt64(0));
    auto* end_ptr = b.CreateInBoundsGEP(i32, xdp->getArg(0), b.getInt64(1));
    auto* data = b.CreateLoad(i32, data_ptr, "ctx.data");
    auto* end = b.CreateLoad(i32, end_ptr, "ctx.data_end");
    // No unchecked underflow: malformed or untrusted contexts abort.
    b.CreateCondBr(b.CreateICmpUGE(end, data), good, bad);
    b.SetInsertPoint(bad);
    b.CreateRet(b.getInt32(0)); // XDP_ABORTED
    b.SetInsertPoint(good);
    auto* bytes = b.CreateZExt(b.CreateSub(end, data, "packet.bytes.u32"), i64);
    auto* decision = b.CreateCall(policy, {bytes}, "policy.action.i64");
    // XDP_ABORTED=0, DROP=1, PASS=2, TX=3, REDIRECT=4. No
    // verifier-visible negative/out-of-range return values.
    auto* valid_action = b.CreateICmpULE(decision, b.getInt64(4));
    auto* action = b.CreateSelect(valid_action, b.CreateTrunc(decision, i32),
                                  b.getInt32(0), "xdp.checked.action");
    b.CreateRet(action);

    // Test-only MIT license metadata. This is NOT a source-license
    // attestation; a production emitter must require explicit source
    // licensing and release provenance before permitting deployment.
    auto* license_data = llvm::ConstantDataArray::getString(context, "MIT", true);
    auto* license = new llvm::GlobalVariable(
        module, license_data->getType(), true,
        llvm::GlobalValue::ExternalLinkage, license_data, "_license");
    license->setSection("license");

    std::string errors;
    llvm::raw_string_ostream diagnostic(errors);
    if (llvm::verifyModule(module, &diagnostic)) {
        diagnostic.flush();
        throw std::runtime_error("invalid experimental eBPF XDP LLVM IR: " + errors);
    }
    std::string out;
    llvm::raw_string_ostream output(out);
    module.print(output, nullptr);
    output.flush();
    return out;
}

} // namespace oreslang
