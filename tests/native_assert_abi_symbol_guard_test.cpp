#include "oreslang/backend/LlvmCodegen.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using oreslang::Expression;
using oreslang::Function;
using oreslang::Program;

static Program program(std::string name, bool boolean = false) {
    Program source;
    Function fn;
    fn.name = std::move(name);
    fn.returns_bool = boolean;
    fn.result = std::make_unique<Expression>();
    fn.result->kind = boolean ? Expression::Kind::Boolean :
                                 Expression::Kind::Integer;
    fn.result->integer = boolean ? 1 : 7;
    source.functions.push_back(std::move(fn));
    return source;
}

static void rejected(const Program& source, const std::string& message) {
    try {
        (void)oreslang::emit_verified_llvm_ir(source);
    } catch (const std::runtime_error& ex) {
        if (std::string(ex.what()).find(message) != std::string::npos)
            return;
        throw;
    }
    throw std::runtime_error("unverified source AST emitted native LLVM IR");
}

static Program scalar_program() {
    // Reconstruct the exact parser-admitted one-local source loop directly
    // without invoking parse_program or the ownership checker.
    Program source = program("count");
    Function& fn = source.functions.front();
    fn.scalar_while = std::make_unique<oreslang::ScalarWhileLoop>();
    fn.scalar_while->counter = "counter";
    fn.scalar_while->initial = 0;
    fn.scalar_while->upper_bound = 4;

    oreslang::LocalBinding local;
    local.name = "counter";
    local.owner_mutable = true;
    local.initializer = std::make_unique<Expression>();
    local.initializer->kind = Expression::Kind::Integer;
    local.initializer->integer = 0;
    fn.locals.push_back(std::move(local));

    fn.result->kind = Expression::Kind::Variable;
    fn.result->name = "counter";
    fn.result->integer = 0;
    return source;
}

int main() {
    // Program is public AST input to the LLVM emitter. Even hand-built
    // Programs cannot spoof native Assert ABI symbols around the parser.
    rejected(program("oreslang_assert_equal_scalar_v0"),
             "reserved native runtime ABI symbol");
    rejected(program("oreslang_assert_trap_bool_v0", true),
             "reserved native runtime ABI symbol");
    rejected(program("oreslang_assert_future_v1"),
             "reserved native runtime ABI symbol");

    // No AST mutation may turn an impure Boolean function into a constant
    // i1 and thereby erase resource cleanup/effects from the guest program.
    {
        auto source = program("boolean_effect", true);
        source.functions.front().has_declared_effects = true;
        rejected(source, "unverified typed bool native result lowering");
    }
    {
        auto source = program("boolean_cleanup", true);
        source.functions.front().caught_throw_defers.emplace_back();
        rejected(source, "unverified typed bool native result lowering");
    }
    {
        auto source = program("boolean_mail", true);
        source.functions.front().actor_sends.emplace_back();
        rejected(source, "unverified typed bool native result lowering");
    }
    {
        auto source = program("boolean_noncanonical", true);
        source.functions.front().result->integer = 2;
        rejected(source, "unverified typed bool native result lowering");
    }

    // Parser and emitter must agree on the *whole* native linker namespace.
    // Construct these Programs directly: no parser validation was called.
    rejected(program("abort"), "reserved native runtime ABI symbol");
    rejected(program("__oreslang_guest_main"),
             "reserved native runtime ABI symbol");
    rejected(program("oreslang_owned_i64_region_open"),
             "reserved native runtime ABI symbol");
    rejected(program("ores_actor_i64_heap_field_read"),
             "reserved native runtime ABI symbol");
    rejected(program("oreslang_source_actor_spawn"),
             "reserved native runtime ABI symbol");
    rejected(program("oreslang_actor_Worker_init"),
             "reserved native runtime ABI symbol");
    rejected(program("oreslang_wasm_actor_init"),
             "reserved native runtime ABI symbol");
    rejected(program("oreslang_int_array_try_region_open"),
             "reserved native Array<int> ABI symbol");

    // The Boolean slice is not a path to malformed void/i1 signatures or
    // nonconforming process-entry wrappers.
    {
        auto source = program("bool_void_conflict", true);
        source.functions.front().returns_void = true;
        rejected(source, "native bool/void return flags conflict");
    }
    rejected(program("main", true),
             "native bool main violates process exit ABI");

    // Duplicate source overload slots otherwise result in LLVM-renamed
    // exports and two bodies targeting one declaration.
    {
        auto source = program("duplicate_slot");
        Function other;
        other.name = "duplicate_slot";
        other.result = std::make_unique<Expression>();
        other.result->kind = Expression::Kind::Integer;
        other.result->integer = 8;
        source.functions.push_back(std::move(other));
        rejected(source, "duplicate native function slot");
    }
    {
        auto source = program("main");
        Function other;
        other.name = "main";
        oreslang::Parameter input;
        input.name = "x";
        other.parameters.push_back(std::move(input));
        other.result = std::make_unique<Expression>();
        other.result->kind = Expression::Kind::Integer;
        other.result->integer = 8;
        source.functions.push_back(std::move(other));
        rejected(source, "duplicate native entrypoint");
    }

    // Positive compiler control: the direct emitter still exposes an i1,
    // without needing a source-level import or host-side bool stub.
    const auto native_ir = oreslang::emit_verified_llvm_ir(program("bool_control", true));
    if (native_ir.find("define i1 @bool_control()") == std::string::npos ||
        native_ir.find("ret i1 true") == std::string::npos)
        throw std::runtime_error("valid forged bool control did not emit native i1");

    // The native scalar-loop shortcut also accepts public Program inputs.
    // It must never discard work from unvalidated function metadata.
    const auto scalar_ir = oreslang::emit_verified_llvm_ir(scalar_program());
    if (scalar_ir.find("@oreslang_rt_scalar_frame_step(") == std::string::npos)
        throw std::runtime_error("valid scalar-loop AST did not emit frame ABI");
    {
        auto source = scalar_program();
        source.functions.front().actor_sends.emplace_back();
        rejected(source, "unverified native scalar-loop AST fast path");
    }
    {
        auto source = scalar_program();
        source.functions.front().caught_throw_defers.emplace_back();
        rejected(source, "unverified native scalar-loop AST fast path");
    }
    {
        auto source = scalar_program();
        // A forged AST can carry actual effect bits with the declaration
        // boolean left false. Never rely on that boolean alone.
        source.functions.front().declared_effects.bits = 1;
        rejected(source, "unverified native scalar-loop AST fast path");
    }
    {
        auto source = scalar_program();
        source.functions.front().locals.front().initializer->integer = 9;
        rejected(source, "unverified native scalar-loop local owner");
    }
    {
        auto source = scalar_program();
        source.functions.front().result->name = "other";
        rejected(source, "unverified native scalar-loop AST fast path");
    }
    {
        auto source = scalar_program();
        source.functions.front().result->integer = 42;
        rejected(source, "unverified native scalar-loop AST fast path");
    }
    {
        auto source = scalar_program();
        source.module = "HiddenModule";
        rejected(source, "unverified native scalar-loop AST fast path");
    }
    {
        auto source = scalar_program();
        source.cooperative_loops.emplace_back();
        rejected(source, "unverified native scalar-loop AST fast path");
    }
    {
        auto source = scalar_program();
        source.functions.push_back(std::move(program("other").functions.front()));
        rejected(source, "native scalar loop requires one standalone function");
    }
}
