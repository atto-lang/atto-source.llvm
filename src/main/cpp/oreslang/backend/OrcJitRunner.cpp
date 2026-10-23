#include "oreslang/backend/OrcJitRunner.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>

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

    // The native compiler's wrapper defines the guest process-exit contract.
    // Calling the guest i64 entry directly would bypass its 0..255 validation.
    const auto* entry = module->getFunction("main");
    if (!entry || entry->isDeclaration() || entry->isVarArg() ||
        entry->arg_size() != 0 || !entry->getReturnType()->isIntegerTy(32))
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
