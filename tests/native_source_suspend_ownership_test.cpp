#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/OwnershipChecker.hpp"

#include <cstdlib>
#include <exception>
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
    return 0;
}
