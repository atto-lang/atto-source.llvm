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

    // Shape-check the separate scalar-loop fast path, which returns before
    // ordinary LLVM function lowering and must not discard forged operations.
    {
        Program loop;
        Function fn;
        fn.name = "bounded_counter";
        fn.scalar_while = std::make_unique<oreslang::ScalarWhileLoop>();
        fn.scalar_while->counter = "counter";
        fn.scalar_while->initial = 0;
        fn.scalar_while->upper_bound = 3;
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Variable;
        fn.result->name = "counter";
        oreslang::LocalBinding counter;
        counter.name = "counter";
        counter.owner_mutable = true;
        counter.initializer = std::make_unique<Expression>();
        counter.initializer->kind = Expression::Kind::Integer;
        counter.initializer->integer = 0;
        fn.locals.push_back(std::move(counter));
        loop.functions.push_back(std::move(fn));

        const auto ir = oreslang::emit_verified_llvm_ir(loop);
        if (ir.find("oreslang_rt_scalar_frame_step") == std::string::npos)
            throw std::runtime_error("valid scalar-loop AST was not lowered");
        loop.functions.front().actor_stops.push_back("hidden_effect");
        rejected(loop, "unverified experimental scalar-loop AST");
        loop.functions.front().actor_stops.clear();
        loop.functions.front().result->name = "wrong_counter";
        rejected(loop, "unverified experimental scalar-loop AST");
        loop.functions.front().result->name = "counter";
        loop.functions.front().locals.front().initializer->integer = 8;
        rejected(loop, "unverified experimental scalar-loop AST");
        loop.functions.front().locals.front().initializer->integer = 0;
        // With a second function, the ordinary LLVM path used to ignore the
        // pending scalar loop entirely. Mixed Programs fail before codegen.
        auto second = program("companion");
        loop.functions.push_back(std::move(second.functions.front()));
        rejected(loop, "experimental scalar loop requires exactly one standalone function");
    }

    // Forged Programs bypass parser symbol-table uniqueness. In particular,
    // LLVM auto-renames duplicate StructType and actor init/handler exports;
    // accepting either changes the native ABI without source authorization.
    {
        auto source = program("duplicate_class_ast", true);
        oreslang::ClassDecl first;
        first.name = "Ledger";
        source.classes.push_back(std::move(first));
        oreslang::ClassDecl second;
        second.name = "Ledger";
        source.classes.push_back(std::move(second));
        rejected(source, "duplicate or unnamed native class/actor declaration");
    }
    {
        auto source = program("duplicate_actor_ast", true);
        oreslang::ActorDecl first;
        first.name = "Worker";
        source.actors.push_back(std::move(first));
        oreslang::ActorDecl second;
        second.name = "Worker";
        source.actors.push_back(std::move(second));
        rejected(source, "duplicate or unnamed native class/actor declaration");
    }
    {
        auto source = program("class_actor_collision_ast", true);
        oreslang::ClassDecl cls;
        cls.name = "Worker";
        source.classes.push_back(std::move(cls));
        oreslang::ActorDecl actor;
        actor.name = "Worker";
        source.actors.push_back(std::move(actor));
        rejected(source, "duplicate or unnamed native class/actor declaration");
    }
    {
        auto source = program("unnamed_actor_ast", true);
        source.actors.emplace_back();
        rejected(source, "duplicate or unnamed native class/actor declaration");
    }

    // A separately lowered cooperative continuation is exclusive to its
    // Program. A crafted AST must not mix its generated ABI with functions,
    // classes, actors or a second continuation.
    {
        auto mixed = program("mixed_continuation", true);
        mixed.cooperative_loops.emplace_back();
        mixed.cooperative_loops.front().name = "counter";
        mixed.cooperative_loops.front().variable = "value";
        rejected(mixed, "unverified native cooperative-loop Program shape");
    }
    {
        Program duplicate;
        duplicate.cooperative_loops.resize(2);
        rejected(duplicate, "unverified native cooperative-loop Program shape");
    }
    {
        Program mixed;
        mixed.cooperative_loops.emplace_back();
        mixed.classes.emplace_back();
        rejected(mixed, "unverified native cooperative-loop Program shape");
    }
    {
        Program named;
        named.cooperative_loops.emplace_back();
        named.cooperative_loops.front().name = "counter";
        named.cooperative_loops.front().variable = "value";
        named.module = "Fake";
        rejected(named, "unverified native cooperative-loop Program shape");
    }
    {
        Program malformed;
        malformed.cooperative_loops.emplace_back();
        malformed.cooperative_loops.front().name = "counter.with.dot";
        malformed.cooperative_loops.front().variable = "value";
        rejected(malformed, "invalid native cooperative-loop identifier");
    }
    {
        auto source = program("oreslang_rt_injected_resume");
        rejected(source, "reserved native runtime ABI symbol");
    }
    {
        auto source = program("malformed$name");
        rejected(source, "invalid native function linker identifier");
    }
    {
        auto source = program("normal", true);
        source.module = "module.with.dot";
        rejected(source, "invalid native module linker identifier");
    }
    {
        auto source = program("class_bad_name", true);
        oreslang::ClassDecl cls;
        cls.name = "Bad.Class";
        source.classes.push_back(std::move(cls));
        rejected(source, "duplicate or unnamed native class/actor declaration");
    }
    {
        Program valid;
        valid.cooperative_loops.emplace_back();
        valid.cooperative_loops.front().name = "counter";
        valid.cooperative_loops.front().variable = "value";
        valid.cooperative_loops.front().initial = 0;
        const auto ir = oreslang::emit_verified_llvm_ir(valid);
        if (ir.find("oreslang_rt_counter_resume") == std::string::npos)
            throw std::runtime_error("valid standalone continuation IR missing");
    }

    // Field tables are compiler-owned ordered layouts, not dictionaries
    // that may silently select the first of multiple matching field names.
    {
        auto source = program("duplicate_class_field_ast", true);
        oreslang::ClassDecl cls;
        cls.name = "Ledger";
        oreslang::ClassField first;
        first.name = "balance";
        cls.fields.push_back(first);
        cls.fields.push_back(first);
        source.classes.push_back(std::move(cls));
        rejected(source, "duplicate or unnamed native class field");
    }
    {
        auto source = program("oversized_class_ast", true);
        oreslang::ClassDecl cls;
        cls.name = "Ledger";
        for (int i = 0; i < 17; ++i) {
            oreslang::ClassField field;
            field.name = "field_" + std::to_string(i);
            cls.fields.push_back(std::move(field));
        }
        source.classes.push_back(std::move(cls));
        rejected(source, "invalid native class field layout");
    }
    {
        auto source = program("empty_class_ast", true);
        oreslang::ClassDecl cls;
        cls.name = "Ledger";
        source.classes.push_back(std::move(cls));
        rejected(source, "invalid native class field layout");
    }
    {
        auto source = program("duplicate_actor_field_ast", true);
        oreslang::ActorDecl actor;
        actor.name = "Worker";
        oreslang::LocalBinding first;
        first.name = "count";
        actor.fields.push_back(std::move(first));
        oreslang::LocalBinding second;
        second.name = "count";
        actor.fields.push_back(std::move(second));
        source.actors.push_back(std::move(actor));
        rejected(source, "duplicate or unnamed native actor field");
    }
    {
        auto source = program("oversized_actor_ast", true);
        oreslang::ActorDecl actor;
        actor.name = "Worker";
        for (int i = 0; i < 17; ++i) {
            oreslang::LocalBinding field;
            field.name = "field_" + std::to_string(i);
            actor.fields.push_back(std::move(field));
        }
        source.actors.push_back(std::move(actor));
        rejected(source, "invalid native actor field layout");
    }
    {
        auto source = program("empty_actor_ast", true);
        oreslang::ActorDecl actor;
        actor.name = "Worker";
        source.actors.push_back(std::move(actor));
        rejected(source, "invalid native actor field layout");
    }

    // Reject invisible children and labels on any integer literal, including
    // literals nested in arithmetic rather than returned directly.
    {
        auto source = program("integer_hidden_operand");
        source.functions.front().result->left = std::make_unique<Expression>();
        rejected(source, "unverified native integer literal AST");
    }
    {
        auto source = program("integer_hidden_argument");
        source.functions.front().result->arguments.push_back(
            std::make_unique<Expression>());
        rejected(source, "unverified native integer literal AST");
    }
    {
        auto source = program("integer_hidden_name");
        source.functions.front().result->name = "hidden_call";
        rejected(source, "unverified native integer literal AST");
    }
    {
        auto source = program("integer_hidden_labels");
        source.functions.front().result->argument_names.push_back("unseen");
        rejected(source, "unverified native integer literal AST");
    }
    {
        auto source = program("nested_integer_hidden_call");
        auto root = std::make_unique<Expression>();
        root->kind = Expression::Kind::Add;
        root->left = std::make_unique<Expression>();
        root->left->kind = Expression::Kind::Integer;
        root->left->integer = 1;
        root->left->arguments.push_back(std::make_unique<Expression>());
        root->right = std::make_unique<Expression>();
        root->right->kind = Expression::Kind::Integer;
        root->right->integer = 2;
        source.functions.front().result = std::move(root);
        rejected(source, "unverified native integer literal AST");
    }

    // Positive canonical field and scalar controls must still compile.
    {
        auto source = program("canonical_class_control", true);
        oreslang::ClassDecl cls;
        cls.name = "Ledger";
        oreslang::ClassField field;
        field.name = "balance";
        cls.fields.push_back(std::move(field));
        source.classes.push_back(std::move(cls));
        (void)oreslang::emit_verified_llvm_ir(source);
    }
    const auto int_ir = oreslang::emit_verified_llvm_ir(program("int_control"));
    if (int_ir.find("ret i64 7") == std::string::npos)
        throw std::runtime_error("canonical integer literal was rejected");

    // Positive compiler control: the direct emitter still exposes an i1,
    // without needing a source-level import or host-side bool stub.
    const auto native_ir = oreslang::emit_verified_llvm_ir(program("bool_control", true));
    if (native_ir.find("define i1 @bool_control()") == std::string::npos ||
        native_ir.find("ret i1 true") == std::string::npos)
        throw std::runtime_error("valid forged bool control did not emit native i1");
}
