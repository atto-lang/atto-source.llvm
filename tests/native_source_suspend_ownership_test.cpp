#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/OwnershipChecker.hpp"
#include "oreslang/types/TaskSourceAuthority.hpp"

#include <cstdlib>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace {
void require(bool condition) {
    if (!condition) std::abort();
}

oreslang::Program source_task() {
    // This is a real parsed source Task (not a manually invented CFG).
    return oreslang::parse_program(R"(
pub fnc main(): int {
    let Task<int> w = task { return 41; };
    let int v = await w;
    return v + 1;
}
)");
}

void must_reject_borrowed_parameter(
        oreslang::Parameter::Mode mode) {
    auto program = source_task();
    require(program.functions.size() == 1);
    oreslang::Parameter parameter;
    parameter.name = "injected_parent_loan";
    parameter.scalar_type = oreslang::ScalarType::I64;
    parameter.mode = mode;
    // Simulate a stale/rewritten compiler AST introduced after the parser
    // admitted the initial capture-free Task. The semantic pass, not only
    // the parsing grammar, must still reject ambient caller-owned loans.
    program.functions.front().parameters.push_back(std::move(parameter));
    bool rejected = false;
    try {
        oreslang::validate_i64_ownership(program);
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find(
            "borrowed parameter crosses native suspension") !=
            std::string_view::npos;
    }
    require(rejected);
    require(!program.functions.front().source_tasks.front().ownership_admitted);
}
// Once the compiler has admitted the Task, a later pass must not silently
// alter the enclosing source function's capability envelope and keep the
// earlier Task fingerprint valid at native IR emission.
void must_reject_mutated_task_envelope(
        const std::function<void(oreslang::Function&)>& mutate) {
    auto program = source_task();
    oreslang::validate_i64_ownership(program);
    auto& function = program.functions.front();
    oreslang::types::require_source_task_i64_function(function);
    mutate(function);
    bool rejected = false;
    try {
        oreslang::types::require_source_task_i64_function(function);
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find(
            "changed after ownership check") != std::string_view::npos;
    }
    require(rejected);
}

} // namespace

int main() {
    auto program = source_task();
    require(program.functions.size() == 1);
    require(program.functions.front().source_tasks.size() == 1);
    oreslang::validate_i64_ownership(program);
    require(program.functions.front().source_tasks.front().ownership_admitted);
    must_reject_borrowed_parameter(
        oreslang::Parameter::Mode::Borrow);
    must_reject_borrowed_parameter(
        oreslang::Parameter::Mode::BorrowMut);

    must_reject_mutated_task_envelope([](auto& fn) {
        fn.name = "renamed_main";
    });
    must_reject_mutated_task_envelope([](auto& fn) {
        fn.async_source_i64 = true;
    });
    must_reject_mutated_task_envelope([](auto& fn) {
        fn.method_owner = "ForgedActor";
    });
    must_reject_mutated_task_envelope([](auto& fn) {
        fn.ready_await_actor = "forged_actor";
    });
    must_reject_mutated_task_envelope([](auto& fn) {
        fn.locals.front().task_await_name = "other_task";
    });
    must_reject_mutated_task_envelope([](auto& fn) {
        fn.locals.front().future_await_name = "other_future";
    });
    must_reject_mutated_task_envelope([](auto& fn) {
        oreslang::LocalBinding duplicate;
        duplicate.name = fn.source_tasks.front().name;
        fn.locals.push_back(std::move(duplicate));
    });
    must_reject_mutated_task_envelope([](auto& fn) {
        oreslang::LocalBinding duplicate_await;
        duplicate_await.name = "second_consumer";
        duplicate_await.task_await_name = fn.source_tasks.front().name;
        fn.locals.push_back(std::move(duplicate_await));
    });
    return 0;
}
