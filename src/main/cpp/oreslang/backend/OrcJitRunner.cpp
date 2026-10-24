#include "oreslang/backend/OrcJitRunner.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Casting.h>

namespace oreslang {

int execute_host_llvm_ir_with_orc(const std::string& verified_ir) {
    // This is a developer/trusted-host execution mode, NOT an untrusted actor
    // sandbox. Never load foreign IR or dynamically resolve arbitrary symbols.
    if (llvm::InitializeNativeTarget() || llvm::InitializeNativeTargetAsmPrinter())
        throw std::runtime_error("ORC JIT: host target initialization failed");

    auto context = std::make_unique<llvm::LLVMContext>();
    llvm::SMDiagnostic parse_error;
    auto module = llvm::parseIR(
        llvm::MemoryBufferRef(llvm::StringRef(verified_ir), "oreslang-verified-ir"),
        parse_error, *context);
    if (!module)
        throw std::runtime_error("ORC JIT: could not parse compiler-generated LLVM IR");
    if (llvm::verifyModule(*module))
        throw std::runtime_error("ORC JIT: LLVM module verification failed");

    // LLVM global ctors/dtors can run outside the explicit main() call.
    // Inline assembly, aliases and ifuncs likewise bypass the first closed-IR
    // execution slice. The Oreslang source compiler is not supposed to emit
    // any of them here: reject future lowering changes until separately proven.
    if (!module->getModuleInlineAsm().empty())
        throw std::runtime_error("ORC JIT: module inline assembly is not admitted");
    if (module->alias_begin() != module->alias_end() ||
        module->ifunc_begin() != module->ifunc_end())
        throw std::runtime_error("ORC JIT: aliases and indirect functions are not admitted");
    if (module->getNamedGlobal("llvm.global_ctors") ||
        module->getNamedGlobal("llvm.global_dtors"))
        throw std::runtime_error("ORC JIT: global constructors/destructors are not admitted");

    // Module-level asm is not the only inline assembly form: call/callbr
    // instructions can invoke llvm::InlineAsm directly or through a pointer
    // cast. Reject both before any ORC materialization or execution.
    for (const auto& function : *module)
        for (const auto& block : function)
            for (const auto& instruction : block)
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    if (llvm::isa<llvm::InlineAsm>(
                            call->getCalledOperand()->stripPointerCasts()))
                        throw std::runtime_error(
                            "ORC JIT: function inline assembly is not admitted");

    // The native compiler's wrapper defines the guest process-exit contract.
    // Calling the guest i64 entry directly would bypass its 0..255 validation.
    const auto* entry = module->getFunction("main");
    if (!entry || entry->isDeclaration() || entry->isVarArg() ||
        entry->arg_size() != 0 || !entry->getReturnType()->isIntegerTy(32) ||
        entry->getCallingConv() != llvm::CallingConv::C)
        throw std::runtime_error("ORC JIT: requires native i32 main() wrapper");

    // Initial JIT slice accepts closed IR modules only. Runtime-host and FFI
    // dependencies need an explicit, capability-aware symbol registry first.
    for (const auto& function : *module)
        if (function.isDeclaration() && !function.isIntrinsic())
            throw std::runtime_error("ORC JIT: unresolved external function is not admitted");
    for (const auto& global : module->globals())
        if (global.isDeclaration())
            throw std::runtime_error("ORC JIT: unresolved external global is not admitted");

    auto jit_result = llvm::orc::LLJITBuilder().create();
    if (!jit_result)
        throw std::runtime_error("ORC JIT: " + llvm::toString(jit_result.takeError()));
    auto jit = std::move(*jit_result);

    // A source module without a target is native-host IR, not proof that an
    // explicitly foreign target can be executed safely. LLJIT has historically
    // differed in whether it rejects a non-host triple on module addition.
#if LLVM_VERSION_MAJOR >= 21
    // LLVM 21+ stores a Triple in the module (LLVM 14-20 store a string).
    const std::string module_triple = module->getTargetTriple().str();
#else
    const std::string module_triple = module->getTargetTriple();
#endif
    const std::string host_triple = jit->getTargetTriple().str();
    if (!module_triple.empty() && module_triple != host_triple)
        throw std::runtime_error("ORC JIT: incompatible host target triple");
    if (!module->getDataLayout().isDefault() &&
            module->getDataLayout() != jit->getDataLayout())
        throw std::runtime_error("ORC JIT: incompatible host data layout");
#if LLVM_VERSION_MAJOR >= 21
    module->setTargetTriple(jit->getTargetTriple());
#else
    module->setTargetTriple(host_triple);
#endif
    module->setDataLayout(jit->getDataLayout());

    if (auto error = jit->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(module), std::move(context))))
        throw std::runtime_error("ORC JIT: " + llvm::toString(std::move(error)));

    auto symbol = jit->lookup("main");
    if (!symbol)
        throw std::runtime_error("ORC JIT: " + llvm::toString(symbol.takeError()));
// LLVM 15+ LLJIT::lookup returns ExecutorAddr; LLVM 14 returns
// JITEvaluatedSymbol. The pointer conversion API differs accordingly.
#if LLVM_VERSION_MAJOR >= 15
    auto* entrypoint = symbol->toPtr<int()>();
#else
    auto* entrypoint = reinterpret_cast<int (*)()>(
        static_cast<std::uintptr_t>(symbol->getAddress()));
#endif
    if (!entrypoint)
        throw std::runtime_error("ORC JIT: null entrypoint");
    return entrypoint();
}

} // namespace oreslang
