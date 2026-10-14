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

static Program valid_scalar_loop() {
    Program source = program("scalar_loop");
    auto& fn = source.functions.front();
    fn.scalar_while = std::make_unique<oreslang::ScalarWhileLoop>();
    fn.scalar_while->counter = "n";
    fn.scalar_while->initial = 0;
    fn.scalar_while->upper_bound = 3;
    oreslang::LocalBinding local;
    local.name = "n";
    local.owner_mutable = true;
    local.initializer = std::make_unique<Expression>();
    local.initializer->kind = Expression::Kind::Integer;
    local.initializer->integer = 0;
    fn.locals.push_back(std::move(local));
    fn.result->kind = Expression::Kind::Variable;
    fn.result->name = "n";
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

    // Special scalar-loop lowering used to return before the symbol guard.
    // Validate linker identity before selecting any backend fast path.
    {
        auto source = program("oreslang_assert_scalar_bypass_v1");
        source.functions.front().scalar_while =
            std::make_unique<oreslang::ScalarWhileLoop>();
        rejected(source, "reserved native runtime ABI symbol");
    }
    {
        auto source = program("boolean_scalar_bypass", true);
        source.functions.front().scalar_while =
            std::make_unique<oreslang::ScalarWhileLoop>();
        rejected(source, "unverified typed bool native result lowering");
    }
    {
        auto source = program("array_namespace_qualified");
        source.module = "Named";
        source.functions.front().name = "oreslang_int_array_try_new";
        rejected(source, "reserved native Array<int> ABI symbol");
    }

    // A fabricated effect row must not become implicitly pure when the
    // has_declared_effects flag is unset.
    {
        auto source = program("boolean_hidden_effect_row", true);
        source.functions.front().declared_effects.bits = 1;
        rejected(source, "unverified typed bool native result lowering");
    }
    // Boolean literals have no operands, call labels or bound names. A
    // hand-built AST with such fields is not a canonical source literal.
    {
        auto source = program("boolean_hidden_operand", true);
        source.functions.front().result->left = std::make_unique<Expression>();
        rejected(source, "unverified typed bool native result lowering");
    }
    {
        auto source = program("boolean_hidden_arguments", true);
        source.functions.front().result->arguments.push_back(
            std::make_unique<Expression>());
        rejected(source, "unverified typed bool native result lowering");
    }
    {
        auto source = program("boolean_hidden_labels", true);
        source.functions.front().result->argument_names.push_back("hidden");
        rejected(source, "unverified typed bool native result lowering");
    }
    {
        auto source = program("boolean_hidden_name", true);
        source.functions.front().result->name = "hidden";
        rejected(source, "unverified typed bool native result lowering");
    }

    // Positive compiler control: the direct emitter still exposes an i1,
    // without needing a source-level import or host-side bool stub.
    const auto native_ir = oreslang::emit_verified_llvm_ir(program("bool_control", true));
    if (native_ir.find("define i1 @bool_control()") == std::string::npos ||
        native_ir.find("ret i1 true") == std::string::npos)
        throw std::runtime_error("valid forged bool control did not emit native i1");

    // Positive control ensures the scalar fast path still admits the exact
    // bounded, single-owner shape while rejects come from direct codegen.
    const auto scalar_ir = oreslang::emit_verified_llvm_ir(valid_scalar_loop());
    if (scalar_ir.find("@oreslang_rt_scalar_frame_step(") == std::string::npos)
        throw std::runtime_error("valid scalar-loop frame was not lowered");
    {
        auto source = valid_scalar_loop();
        source.functions.push_back(std::move(program("other").functions.front()));
        rejected(source, "native scalar loop requires one standalone function");
    }
    {
        auto source = valid_scalar_loop();
        source.functions.front().result->integer = 7;
        rejected(source, "unverified experimental scalar-loop AST");
    }
    {
        auto source = valid_scalar_loop();
        source.functions.front().locals.front().field_mutable = true;
        rejected(source, "unverified experimental scalar-loop AST");
    }
    {
        auto source = valid_scalar_loop();
        source.functions.front().scalar_while->name = "ignored-payload";
        rejected(source, "unverified experimental scalar-loop AST");
    }
    {
        auto source = valid_scalar_loop();
        source.functions.front().scalar_while->counter.clear();
        source.functions.front().result->name.clear();
        source.functions.front().locals.front().name.clear();
        rejected(source, "unverified experimental scalar-loop AST");
    }
}
