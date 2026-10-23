#include "oreslang/backend/TargetAdmission.hpp"

#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>

using namespace oreslang;

namespace {

std::unique_ptr<Expression> integer(std::int64_t value) {
    auto expr = std::make_unique<Expression>();
    expr->kind = Expression::Kind::Integer;
    expr->integer = value;
    return expr;
}

Function fn(const std::string& name) {
    Function out;
    out.name = name;
    out.result = integer(0);
    return out;
}

bool rejects(const Program& program, NativeTargetKind target) {
    try {
        validate_target_admission(program, target);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

} // namespace

int main() {
    {
        bool rejected = false;
        try {
            (void)native_target_kind(true, true);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected);
    }

    {
        Program program;
        Function main = fn("main");
        Parameter arg;
        arg.name = "arg";
        main.parameters.push_back(std::move(arg));
        program.functions.push_back(std::move(main));
        assert(rejects(program, NativeTargetKind::Host));
    }

    {
        Program program;
        program.functions.push_back(fn("main"));
        program.functions.push_back(fn("__oreslang_guest_main"));
        assert(rejects(program, NativeTargetKind::Host));
    }

    {
        Program program;
        ActorDecl first;
        first.name = "A";
        ActorDecl second;
        second.name = "B";
        program.actors.push_back(std::move(first));
        program.actors.push_back(std::move(second));
        assert(rejects(program, NativeTargetKind::Wasm32));
    }

    {
        Program program;
        ActorDecl actor;
        actor.name = "Worker";
        program.actors.push_back(std::move(actor));
        assert(rejects(program, NativeTargetKind::Wasm32));
    }

    {
        Program program;
        ActorDecl actor;
        actor.name = "Worker";
        LocalBinding field;
        field.name = "total";
        field.field_mutable = true;
        actor.fields.push_back(std::move(field));
        Parameter message;
        message.name = "message";
        actor.on_message.parameters.push_back(std::move(message));
        actor.on_message.result = integer(0);
        program.actors.push_back(std::move(actor));
        assert(rejects(program, NativeTargetKind::Host));
        assert(rejects(program, NativeTargetKind::Wasm32));
    }

    {
        Program program;
        ActorDecl actor;
        actor.name = "Worker";

        LocalBinding field;
        field.name = "total";
        field.field_mutable = true;
        actor.fields.push_back(std::move(field));

        Parameter message;
        message.name = "message";
        actor.on_message.parameters.push_back(std::move(message));
        actor.on_message.result = integer(0);
        program.actors.push_back(std::move(actor));

        validate_target_admission(program, NativeTargetKind::Wasm32);
    }

    {
        Program program;
        Function caught = fn("work");
        caught.caught_throw_value = integer(1);
        program.functions.push_back(std::move(caught));
        assert(rejects(program, NativeTargetKind::Wasm32));
    }

    {
        Program program;
        CooperativeScalarLoop loop;
        loop.name = "spin";
        program.cooperative_loops.push_back(std::move(loop));
        assert(rejects(program, NativeTargetKind::Wasm32));
    }

    {
        Program program;
        ActorDecl actor;
        actor.name = "Worker";

        LocalBinding field;
        field.name = "total";
        field.field_mutable = false;
        actor.fields.push_back(std::move(field));

        Parameter message;
        message.name = "message";
        actor.on_message.parameters.push_back(std::move(message));
        actor.on_message.result = integer(0);

        ActorFieldWrite write;
        write.field = "total";
        write.expression = integer(1);
        actor.on_message.actor_field_writes.push_back(std::move(write));

        program.actors.push_back(std::move(actor));
        assert(rejects(program, NativeTargetKind::Host));
        assert(rejects(program, NativeTargetKind::Wasm32));
    }

    {
        Program host;
        validate_target_admission(host, NativeTargetKind::Host);

        Program cortex;
        validate_target_admission(cortex, NativeTargetKind::CortexM);
    }

    {
        Program scalar;
        Function loop = fn("main");
        loop.scalar_while = std::make_unique<ScalarWhileLoop>();
        scalar.functions.push_back(std::move(loop));
        validate_target_admission(scalar, NativeTargetKind::Host);
        assert(rejects(scalar, NativeTargetKind::Wasm32));
        assert(rejects(scalar, NativeTargetKind::CortexM));
    }

    return 0;
}
