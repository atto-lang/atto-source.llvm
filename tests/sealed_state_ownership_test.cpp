#include "oreslang/types/SealedStateOwnership.hpp"
#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace oreslang;

static std::unique_ptr<Expression> value(Expression::Kind kind, const char* name = "") {
    auto expression = std::make_unique<Expression>();
    expression->kind = kind;
    expression->name = name;
    return expression;
}

static ActorDecl scope(bool writing = true) {
    ActorDecl actor;
    actor.name = "Counter";
    actor.isolated = actor.has_state_schema = actor.sealed_state_synchronous_v1 = true;
    actor.sealed_state_write_v1 = writing;
    ActorStateField schema;
    schema.name = "count";
    actor.state_schema.push_back(schema);
    LocalBinding slot;
    slot.name = "count";
    slot.field_mutable = writing;
    slot.initializer = value(Expression::Kind::Integer);
    actor.fields.push_back(std::move(slot));
    actor.on_message.name = "on_message";
    Parameter input;
    input.name = "input";
    actor.on_message.parameters.push_back(input);
    actor.on_message.result = value(Expression::Kind::Variable, "count");
    if (writing) {
        ActorFieldWrite write;
        write.field = "count";
        write.expression = value(Expression::Kind::Variable, "input");
        actor.on_message.actor_field_writes.push_back(std::move(write));
    }
    return actor;
}

static unsigned rejected = 0;
static void mutation(const std::function<void(ActorDecl&)>& change) {
    ActorDecl actor = scope();
    validate_sealed_actor_ownership(actor);
    change(actor);
    try { validate_sealed_actor_ownership(actor); }
    catch (const std::runtime_error&) { ++rejected; return; }
    std::fputs("mutated sealed-state ownership was accepted\n", stderr);
    std::abort();
}

int main() {
    validate_sealed_actor_ownership(scope());
    validate_sealed_actor_ownership(scope(false));
    mutation([](ActorDecl& a) { a.has_state_schema = false; });
    mutation([](ActorDecl& a) { a.fields[0].initializer->integer = 99; });
    mutation([](ActorDecl& a) { a.sealed_state_write_v1 = false; a.fields[0].field_mutable = false; });
    mutation([](ActorDecl& a) { a.on_message.parameters[0].mode = Parameter::Mode::BorrowMut; });
    mutation([](ActorDecl& a) { a.on_message.result = value(Expression::Kind::BorrowMut); });
    mutation([](ActorDecl& a) { a.on_message.result = value(Expression::Kind::Call, "escape"); });
    mutation([](ActorDecl& a) { a.on_message.result->name = "foreign_actor_state"; });
    mutation([](ActorDecl& a) { a.fields[0].int_array = true; });
    mutation([](ActorDecl& a) { a.fields[0].constructor_arguments.push_back(value(Expression::Kind::Call, "escape")); });
    mutation([](ActorDecl& a) { a.on_message.async_source_i64 = true; });
    mutation([](ActorDecl& a) { a.on_message.ready_await_actor = "peer"; });
    mutation([](ActorDecl& a) { a.on_message.locals.emplace_back(); });
    mutation([](ActorDecl& a) { a.on_message.actor_field_writes[0].field = "foreign"; });
    mutation([](ActorDecl& a) {
        auto result = value(Expression::Kind::Add);
        result->left = value(Expression::Kind::Variable, "count");
        result->right = value(Expression::Kind::Integer);
        a.on_message.result = std::move(result);
    });
    mutation([](ActorDecl& a) {
        for (unsigned i = 0; i < 130; ++i) {
            auto expression = value(Expression::Kind::Negate);
            expression->left = std::move(a.on_message.actor_field_writes[0].expression);
            a.on_message.actor_field_writes[0].expression = std::move(expression);
        }
    });
    if (rejected != 15) std::abort();
    std::puts("SEALED_STATE_OWNERSHIP_PASS positives=2 rejected_mutations=15");
}
