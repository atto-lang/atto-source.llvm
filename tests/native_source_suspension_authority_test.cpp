#include "oreslang/frontend/Parser.hpp"

// Independent host-side authority regression. This proves the compiler's
// immutable-snapshot gate shapes, not that a guest await has run on LLVM.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>

using namespace oreslang;

int main() {
    Function function;
    function.name = "main";
    assert(!has_source_suspension(function));
    assert(!function.suspension_ownership_admitted);
    assert(function.suspension_authority_fingerprint == 0);

    // ActorRef readiness and physical get_done are separate source awaits.
    function.ready_await_actor = "worker";
    function.ready_await_offset = 24;
    assert(has_source_suspension(function));
    const auto ready = source_suspension_authority_fingerprint(function);
    assert(ready != 0);
    function.suspension_ownership_admitted = true;
    function.suspension_authority_fingerprint = ready;
    assert(function.suspension_authority_fingerprint ==
        source_suspension_authority_fingerprint(function));

    function.ready_await_offset++;
    assert(function.suspension_authority_fingerprint !=
        source_suspension_authority_fingerprint(function));
    function.ready_await_offset--;
    assert(function.suspension_authority_fingerprint ==
        source_suspension_authority_fingerprint(function));

    function.ready_await_actor.clear();
    function.done_await_actor = "worker";
    assert(has_source_suspension(function));
    assert(source_suspension_authority_fingerprint(function) != ready);
    function.done_await_actor.clear();
    assert(!has_source_suspension(function));

    // The Task/Future result binding, not merely producer existence, is
    // the actual suspension marker consumed by the ownership pass.
    SourceTaskI64Binding task;
    task.name = "scheduled";
    task.awaited = true;
    function.source_tasks.push_back(std::move(task));
    assert(!has_source_suspension(function));

    LocalBinding result;
    result.name = "value";
    result.task_await_name = "scheduled";
    result.offset = 71;
    function.locals.push_back(std::move(result));
    assert(has_source_suspension(function));
    const auto task_fingerprint =
        source_suspension_authority_fingerprint(function);
    function.locals.front().task_await_name = "forged";
    assert(source_suspension_authority_fingerprint(function) !=
        task_fingerprint);
    function.locals.front().task_await_name = "scheduled";

    // A borrowed parameter added after ownership validation must also
    // invalidate the checked suspension snapshot.
    Parameter borrowed;
    borrowed.name = "external";
    borrowed.mode = Parameter::Mode::Borrow;
    function.parameters.push_back(std::move(borrowed));
    assert(source_suspension_authority_fingerprint(function) !=
        task_fingerprint);
    const auto borrowed_fingerprint =
        source_suspension_authority_fingerprint(function);
    function.parameters.front().mode = Parameter::Mode::BorrowMut;
    assert(source_suspension_authority_fingerprint(function) !=
        borrowed_fingerprint);

    function.locals.front().task_await_name.clear();
    function.locals.front().future_await_name = "pipeline";
    assert(has_source_suspension(function));
    assert(source_suspension_authority_fingerprint(function) !=
        borrowed_fingerprint);


    // Regression: an earlier shallow fingerprint did not cover the checked
    // return/argument/initializer AST. Mutating those after approval must
    // revoke codegen authority without a fresh ownership pass.
    Function deep;
    deep.name = "main";
    deep.ready_await_actor = "worker";
    deep.ready_await_offset = 19;
    deep.result = std::make_unique<Expression>();
    deep.result->kind = Expression::Kind::Integer;
    deep.result->integer = 42;
    const auto before_return =
        source_suspension_authority_fingerprint(deep);
    deep.result->integer = 43;
    assert(source_suspension_authority_fingerprint(deep) != before_return);

    deep.result->kind = Expression::Kind::Call;
    deep.result->name = "compute";
    auto argument = std::make_unique<Expression>();
    argument->kind = Expression::Kind::Variable;
    argument->name = "value";
    deep.result->arguments.push_back(std::move(argument));
    const auto before_argument =
        source_suspension_authority_fingerprint(deep);
    deep.result->arguments.front()->name = "other";
    assert(source_suspension_authority_fingerprint(deep) != before_argument);

    LocalBinding checked_local;
    checked_local.name = "checked";
    checked_local.initializer = std::make_unique<Expression>();
    checked_local.initializer->kind = Expression::Kind::Integer;
    checked_local.initializer->integer = 11;
    deep.locals.push_back(std::move(checked_local));
    const auto before_initializer =
        source_suspension_authority_fingerprint(deep);
    deep.locals.front().initializer->integer = 99;
    assert(source_suspension_authority_fingerprint(deep) != before_initializer);

    Statement effect;
    effect.kind = Statement::Kind::ScalarWrite;
    effect.receiver = "checked";
    effect.expression = std::make_unique<Expression>();
    effect.expression->kind = Expression::Kind::Integer;
    effect.expression->integer = 1;
    deep.statements.push_back(std::move(effect));
    const auto before_statement =
        source_suspension_authority_fingerprint(deep);
    deep.statements.front().expression->integer = 2;
    assert(source_suspension_authority_fingerprint(deep) != before_statement);

    Function task_result;
    task_result.name = "main";
    SourceTaskI64Binding source_task;
    source_task.name = "task";
    source_task.awaited = true;
    source_task.result = std::make_unique<Expression>();
    source_task.result->kind = Expression::Kind::Integer;
    source_task.result->integer = 1;
    task_result.source_tasks.push_back(std::move(source_task));
    LocalBinding awaited;
    awaited.name = "output";
    awaited.task_await_name = "task";
    task_result.locals.push_back(std::move(awaited));
    assert(has_source_suspension(task_result));
    const auto before_task =
        source_suspension_authority_fingerprint(task_result);
    task_result.source_tasks.front().result->integer = 3;
    assert(source_suspension_authority_fingerprint(task_result) != before_task);

    return 0;
}
