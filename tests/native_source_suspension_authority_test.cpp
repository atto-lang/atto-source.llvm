#include "oreslang/frontend/Parser.hpp"

// Independent host-side authority regression. This proves the compiler's
// immutable-snapshot gate shapes, not that a guest await has run on LLVM.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <string>

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

    return 0;
}
