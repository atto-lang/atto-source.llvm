#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/FunctionEffects.hpp"

#include <stdexcept>
#include <string>
#include <utility>

using namespace oreslang;

#define CHECK(cond) do { if (!(cond)) throw std::runtime_error("failed: " #cond); } while (false)

static std::unique_ptr<Expression> integer(std::int64_t value) {
    auto e = std::make_unique<Expression>();
    e->kind = Expression::Kind::Integer;
    e->integer = value;
    return e;
}

static std::unique_ptr<Expression> variable(std::string name) {
    auto e = std::make_unique<Expression>();
    e->kind = Expression::Kind::Variable;
    e->name = std::move(name);
    return e;
}

static std::unique_ptr<Expression> state_field(
        const std::string& binder, const std::string& field) {
    auto e = std::make_unique<Expression>();
    e->kind = Expression::Kind::Field;
    e->left = variable(binder);
    e->name = field;
    return e;
}

static Program valid_program() {
    Program p;
    ActorDecl actor;
    actor.name = "Counter";
    actor.sealed_state = true;

    LocalBinding field;
    field.name = "count";
    field.field_mutable = true;
    field.initializer = integer(41);
    actor.fields.push_back(std::move(field));

    Function method;
    method.name = "on_message";
    method.exported = true;
    Parameter parameter;
    parameter.name = "n";
    method.parameters.push_back(std::move(parameter));
    method.has_declared_effects = true;
    method.declared_effects.bits =
        static_cast<std::uint32_t>(FunctionEffect::StateRead);

    auto loan = std::make_unique<ActorStateLoan>();
    loan->mode = ActorStateLoan::Mode::Read;
    loan->binder = "s";
    loan->result = state_field("s", "count");
    method.actor_state_loan = std::move(loan);
    actor.on_message = std::move(method);

    p.actors.push_back(std::move(actor));
    return p;
}

template <typename Mutator>
static void rejects(Mutator&& mutate) {
    Program p = valid_program();
    mutate(p);
    bool rejected = false;
    try {
        (void)emit_verified_llvm_ir(p);
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

int main() {
    {
        Program p = valid_program();
        const std::string ir = emit_verified_llvm_ir(p);
        CHECK(ir.find("call i64 @ores_actor_i64_heap_field_read") != std::string::npos);
    }

    // Empty state { } is a legitimate actor schema: its zero physical guest
    // fields must not induce implicit state reads or writes.
    {
        Program p = valid_program();
        p.actors.front().fields.clear();
        p.actors.front().on_message.actor_state_loan->result = variable("n");
        const std::string ir = emit_verified_llvm_ir(p);
        CHECK(ir.find("call i64 @ores_actor_i64_heap_field_read") ==
              std::string::npos);
        CHECK(ir.find("call void @ores_actor_i64_heap_field_write") ==
              std::string::npos);
    }

    // Effect bits alone are not authority. The parser/source checker must
    // have produced an explicit declared effect row.
    rejects([](Program& p) {
        p.actors.front().on_message.has_declared_effects = false;
    });

    // A malformed compiler-built AST must fail before actor initialization
    // dereferences an absent field initializer.
    rejects([](Program& p) {
        p.actors.front().fields.front().initializer.reset();
    });

    // Sealed schema slots are writable only through the checked state-loan
    // path; a transformed AST may not silently downgrade slot metadata.
    rejects([](Program& p) {
        p.actors.front().fields.front().field_mutable = false;
    });

    // Ordinary actor-field mutation storage must never coexist with the
    // sealed state-loan representation.
    rejects([](Program& p) {
        ActorFieldWrite write;
        write.field = "count";
        write.expression = integer(1);
        p.actors.front().on_message.actor_field_writes.push_back(std::move(write));
    });

    rejects([](Program& p) {
        p.actors.front().on_message.actor_state_loan->mode =
            static_cast<ActorStateLoan::Mode>(255);
    });
    rejects([](Program& p) {
        auto& method = p.actors.front().on_message;
        method.actor_state_loan->mode =
            static_cast<ActorStateLoan::Mode>(255);
        method.declared_effects.bits =
            static_cast<std::uint32_t>(FunctionEffect::StateWrite);
    });
    rejects([](Program& p) {
        LocalBinding legacy;
        legacy.name = "other";
        legacy.initializer = integer(1);
        p.actors.front().on_message.locals.push_back(std::move(legacy));
    });
    rejects([](Program& p) {
        p.actors.front().on_message.actor_stops.push_back("other");
    });
    rejects([](Program& p) {
        Statement legacy;
        legacy.kind = Statement::Kind::Call;
        p.actors.front().on_message.statements.push_back(std::move(legacy));
    });

    // Reject malformed direct-AST inputs before LLVM field/parameter indexing.
    rejects([](Program& p) {
        p.actors.front().on_message.parameters.clear();
    });
    rejects([](Program& p) {
        p.actors.front().on_message.parameters.emplace_back();
    });
    rejects([](Program& p) {
        p.actors.front().on_message.parameters.front().name.clear();
    });
    rejects([](Program& p) {
        p.actors.front().on_message.name = "unchecked";
    });
    rejects([](Program& p) {
        p.actors.front().on_message.exported = false;
    });
    rejects([](Program& p) {
        p.actors.front().on_message.returns_void = true;
    });
    rejects([](Program& p) {
        p.actors.front().on_message.return_type = ScalarType::Bool;
    });
    rejects([](Program& p) {
        auto& field = p.actors.front().fields.front();
        field.name = "";
    });
    rejects([](Program& p) {
        auto& field = p.actors.front().fields.front();
        field.name = "state";
    });
    rejects([](Program& p) {
        auto& field = p.actors.front().fields.front();
        field.name = "n";
    });
    rejects([](Program& p) {
        LocalBinding dup;
        dup.name = "count";
        dup.field_mutable = true;
        dup.initializer = integer(1);
        p.actors.front().fields.push_back(std::move(dup));
    });
    rejects([](Program& p) {
        p.actors.front().on_message.declared_effects.bits |=
            FunctionEffects::UnresolvedCallBit;
    });
    rejects([](Program& p) {
        p.actors.front().on_message.actor_state_loan->binder.clear();
    });
    rejects([](Program& p) {
        p.actors.front().on_message.actor_state_loan->binder = "n";
    });
    rejects([](Program& p) {
        p.actors.front().on_message.actor_state_loan->result.reset();
    });
    rejects([](Program& p) {
        p.actors.front().on_message.actor_state_loan.reset();
    });

    return 0;
}
