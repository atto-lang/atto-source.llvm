#include "oreslang/frontend/Parser.hpp"

#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL sealed-state AST contract: %s\\n", #condition); \
        std::abort(); \
    } \
} while (false)

using namespace oreslang;

// A sealed source schema must remain a distinct compiler AST node; the old
// actor scalar field lane does not carry lexical RW permissions.
static_assert(!std::is_same_v<SealedStateSchemaDecl, LocalBinding>);
static_assert(!std::is_same_v<CriticalStateAccessDecl, LocalBinding>);
static_assert(std::is_same_v<decltype(ActorDecl{}.sealed_state),
                             std::optional<SealedStateSchemaDecl>>);

int main() {
    ActorDecl actor;
    actor.name = "Counter";
    SealedStateSchemaDecl state;
    state.offset = 7;
    SealedStateFieldDecl field;
    field.name = "revision";
    field.type = ScalarType::I64;
    field.initial_i64 = -17;
    state.fields.push_back(field);
    actor.sealed_state = std::move(state);

    REQUIRE(actor.fields.empty());
    REQUIRE(actor.sealed_state.has_value());
    REQUIRE(actor.sealed_state->fields.size() == 1);
    REQUIRE(actor.sealed_state->fields[0].name == "revision");
    REQUIRE(actor.sealed_state->fields[0].initial_i64 == -17);

    CriticalStateAccessDecl loan;
    loan.mode = CriticalStateAccessDecl::Mode::Read;
    loan.state_parameter = "s";
    loan.offset = 43;
    loan.read_fields.push_back("revision");
    actor.critical_state_uses.push_back(loan);
    loan.mode = CriticalStateAccessDecl::Mode::Write;
    loan.offset = 64;
    loan.read_fields.clear();
    loan.written_fields.push_back("revision");
    actor.critical_state_uses.push_back(loan);

    REQUIRE(actor.critical_state_uses.size() == 2);
    REQUIRE(actor.critical_state_uses[0].mode == CriticalStateAccessDecl::Mode::Read);
    REQUIRE(actor.critical_state_uses[1].mode == CriticalStateAccessDecl::Mode::Write);
    REQUIRE(actor.critical_state_uses[0].state_parameter == "s");
    REQUIRE(actor.critical_state_uses[0].read_fields.size() == 1);
    REQUIRE(actor.critical_state_uses[0].written_fields.empty());
    REQUIRE(actor.critical_state_uses[1].read_fields.empty());
    REQUIRE(actor.critical_state_uses[1].written_fields.size() == 1);
    REQUIRE(actor.critical_state_uses[1].written_fields[0] == "revision");
    REQUIRE(actor.fields.empty());  // no fallthrough to legacy unsound fields
    std::puts("SEALED_STATE_TYPED_AST_CONTRACT_PASS");
}
