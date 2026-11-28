#include "oreslang/runtime/NativeTelemetrySpan.h"
#include "oreslang/runtime/VersionedInstrumentationDispatch.hpp"

#include <llvm/ADT/StringExtras.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

extern "C" std::int64_t trace_once(std::int64_t);

namespace {
using namespace oreslang::runtime;
using Entry = std::int64_t (*)(std::int64_t);
constexpr char call_name[] = "call:trace_once";
constexpr char lexical_name[] = "lexical-checkout";

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::abort();
    }
}

struct Observations {
    std::array<std::uint64_t, 2> tokens{};
    unsigned begins = 0;
    unsigned ends = 0;
};
thread_local Observations observations;

struct InFlight {
    std::mutex mutex;
    std::condition_variable ready;
    bool entered = false;
    bool proceed = false;
};
thread_local InFlight* in_flight = nullptr;

// These ORC symbol bindings forward to the same runtime as the linked AOT
// function. The test observes actual runtime tokens and depth around each hook.
extern "C" std::uint64_t observe_begin(const char* name, std::uint64_t size) {
    auto& seen = observations;
    require(seen.begins < seen.tokens.size(), "unexpected duplicate span");
    const auto expected = seen.begins == 0 ? call_name : lexical_name;
    require(std::string_view(name, size) == expected, "unexpected span order or name");
    require(ores_native_telemetry_snapshot().active == seen.begins, "wrong inherited span depth");
    const auto token = ores_native_telemetry_span_begin(name, size);
    require(token != 0, "runtime did not record a span");
    require(seen.begins == 0 || token != seen.tokens[0], "parent and child reused an identity");
    seen.tokens[seen.begins++] = token;
    if (in_flight && seen.begins == 2) {
        std::unique_lock<std::mutex> lock(in_flight->mutex);
        in_flight->entered = true;
        in_flight->ready.notify_one();
        in_flight->ready.wait(lock, [] { return in_flight->proceed; });
    }
    return token;
}

extern "C" void observe_end(std::uint64_t token) {
    auto& seen = observations;
    require(seen.begins == 2 && seen.ends < 2, "unexpected span end");
    const auto depth = 2 - seen.ends;
    require(token == seen.tokens[depth - 1], "scope ended outside LIFO order");
    require(ores_native_telemetry_snapshot().active == depth, "wrong active depth before close");
    ores_native_telemetry_span_end(token);
    ++seen.ends;
    require(ores_native_telemetry_snapshot().active == depth - 1, "scope did not retire");
}

struct JitCode {
    std::unique_ptr<llvm::orc::LLJIT> engine;
    Entry entry;
};

std::int64_t invoke_aot(void*, std::int64_t input) noexcept {
    return trace_once(input);
}

std::int64_t invoke_jit(void* context, std::int64_t input) noexcept {
    return static_cast<JitCode*>(context)->entry(input);
}

// Test-only code generation over the exact IR from oreslang-llvmc. Generate
// a new call boundary around the checked guest, preserving its lexical scope.
std::shared_ptr<JitCode> compile_instrumented(llvm::MemoryBufferRef input) {
    llvm::ExitOnError exit_on_error;
    auto jit = exit_on_error(llvm::orc::LLJITBuilder()
        .setPlatformSetUp(llvm::orc::setUpInactivePlatform)
        .setLinkProcessSymbolsByDefault(false).create());
    auto context = std::make_unique<llvm::LLVMContext>();
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseIR(input, diagnostic, *context);
    if (!module)
        diagnostic.print("atto telemetry hybrid", llvm::errs());
    require(module != nullptr, "cannot parse Atto compiler IR");
    module->setDataLayout(jit->getDataLayout());
#if LLVM_VERSION_MAJOR >= 21
    module->setTargetTriple(jit->getTargetTriple());
#else
    module->setTargetTriple(jit->getTargetTriple().str());
#endif

    auto* i64 = llvm::Type::getInt64Ty(*context);
    auto* ptr = llvm::PointerType::getUnqual(*context);
    auto* guest_type = llvm::FunctionType::get(i64, {i64}, false);
    auto* begin_type = llvm::FunctionType::get(i64, {ptr, i64}, false);
    auto* end_type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {i64}, false);
    auto* guest = module->getFunction("trace_once");
    auto* begin = module->getFunction("ores_native_telemetry_span_begin");
    auto* end = module->getFunction("ores_native_telemetry_span_end");
    require(guest && !guest->isDeclaration() && guest->getFunctionType() == guest_type,
            "Atto guest ABI changed");
    require(begin && begin->isDeclaration() && begin->getFunctionType() == begin_type &&
                end && end->isDeclaration() && end->getFunctionType() == end_type,
            "Atto lexical probe ABI changed");

    auto* entry = llvm::Function::Create(guest_type, llvm::Function::ExternalLinkage,
                                         "trace_once_instrumented", *module);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "entry", entry));
    auto* name = builder.CreateGlobalString(call_name);
    auto* parent = builder.CreateCall(begin, {name, builder.getInt64(sizeof(call_name) - 1)});
    auto* result = builder.CreateCall(guest, {entry->getArg(0)});
    builder.CreateCall(end, {parent});
    builder.CreateRet(result);
    require(!llvm::verifyModule(*module, &llvm::errs()), "instrumented LLVM IR failed verification");

    llvm::orc::SymbolMap symbols;
    symbols[jit->mangleAndIntern(begin->getName())] = {
        llvm::orc::ExecutorAddr::fromPtr(&observe_begin), llvm::JITSymbolFlags::Exported};
    symbols[jit->mangleAndIntern(end->getName())] = {
        llvm::orc::ExecutorAddr::fromPtr(&observe_end), llvm::JITSymbolFlags::Exported};
    exit_on_error(jit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols))));
    exit_on_error(jit->addIRModule({std::move(module), std::move(context)}));
    auto address = exit_on_error(jit->lookup("trace_once_instrumented"));
    return std::make_shared<JitCode>(JitCode{std::move(jit), address.toPtr<Entry>()});
}

void check_counts(std::uint64_t count) {
    const auto stats = ores_native_telemetry_snapshot();
    require(stats.started == count && stats.ended == count &&
            stats.active == 0 && stats.dropped == 0, "unexpected runtime span counts");
}
} // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected the build's Atto compiler IR");
    auto input = llvm::MemoryBuffer::getFile(argv[1]);
    require(static_cast<bool>(input), "cannot read the build's Atto IR");
    const auto ir_digest = llvm::SHA256::hash(llvm::arrayRefFromStringRef((*input)->getBuffer()));
    const std::string ir_hash = llvm::toHex(ir_digest, true);
    const InstrumentationIdentity identity{
        "fixture", std::string(32, '1'), ir_hash, std::string(64, '2'),
        std::string(64, '3'), std::string(64, '4'), std::string(64, '5')};
    const auto baseline = std::make_shared<InstrumentedVersion>(
        InstrumentedVersion{identity, 1, &invoke_aot, {}, nullptr, true});
    VersionedInstrumentationDispatch slot(baseline, true, true, false, 1);

    // The linked guest returns normally before any LLVM JIT is initialized.
    ores_native_telemetry_set_enabled(0);
    require(slot.invoke(33) == 42, "AOT result with recording off");
    check_counts(0);
    ores_native_telemetry_set_enabled(1);
    require(slot.invoke(33) == 42, "AOT result with recording on");
    check_counts(1);

    require(!llvm::InitializeNativeTarget() && !llvm::InitializeNativeTargetAsmPrinter(),
            "LLVM native target is unavailable");
    auto code = compile_instrumented((*input)->getMemBufferRef());
    std::weak_ptr<JitCode> lease = code;
    auto variant = std::make_shared<InstrumentedVersion>(
        InstrumentedVersion{identity, 2, &invoke_jit, code, code.get(), false});
    code.reset();
    require(slot.publish(variant, InstrumentationMode::deep, false, true) ==
                PublishResult::unauthorized, "unauthorized version became active");
    require(slot.active_generation() == 1 && slot.publication_count() == 0,
            "denial changed the baseline");
    // The fixture supplies the trusted host attestations to the dispatch
    // component. Signature and tenant authentication have their own gates.
    require(slot.publish(variant, InstrumentationMode::deep, true, true) ==
                PublishResult::accepted, "could not publish new ORC code");
    variant.reset();
    require(slot.active_generation() == 2 && slot.invoke(33) == 42, "ORC guest result");
    check_counts(3);
    require(observations.begins == 2 && observations.ends == 2, "ORC added no parent span");

    InFlight gate;
    std::thread worker([&] {
        in_flight = &gate;
        require(slot.invoke(-10) == -1, "in-flight ORC guest result");
        check_counts(2);
        require(observations.begins == 2 && observations.ends == 2,
                "detach lost the active scopes");
    });
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.ready.wait(lock, [&] { return gate.entered; });
        ores_native_telemetry_set_enabled(0);
        slot.disable();
        require(slot.active_generation() == 1 && !lease.expired(),
                "detach did not preserve the in-flight native code lease");
        require(slot.invoke(33) == 42, "AOT result after ORC detach");
        check_counts(3);
        gate.proceed = true;
        gate.ready.notify_one();
    }
    worker.join();
    require(lease.expired(), "ORC code lease leaked after guest return");
    std::cout << "LLVM_AOT_ORC_SCOPED_TELEMETRY_PASS\n";
}
