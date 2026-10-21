#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/SourceOwnershipChecker.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

using namespace oreslang;
using namespace oreslang::backend;

#define CHECK(cond) do { if (!(cond)) throw std::runtime_error("failed: " #cond); } while (false)

template <typename F>
static void rejects(F&& fn) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

static Program write_program() {
    return parse_program(R"ORES(
define actor Counter as {
  state {
    base: int = 40;
    total: int = 0;
  }
  pub fnc on_message(int n): int with state.write {
    const result = self.handle_state(critical |state| -> {
      state.total = state.total + state.base + n;
      return state.total;
    });
    return result;
  }
}
)ORES");
}

static Program read_program() {
    return parse_program(R"ORES(
define actor Counter as {
  state {
    base: int = 40;
  }
  pub fnc on_message(int n): int with state.read {
    const result = self.handle_state(critical read |state| -> {
      return state.base + n;
    });
    return result;
  }
}
)ORES");
}

int main() {
    constexpr std::uint64_t REGION = UINT64_C(64) * 1024 * 1024;

    {
        const auto program = write_program();
        const auto claims = SourceOwnershipChecker::derive_actor_state_ref_claims(
            program, "Counter", ManagedRefAccess::Write, REGION, true);
        CHECK(claims.origin == ManagedRefOrigin::ActorOwned);
        CHECK(claims.escape == ManagedRefEscape::ActorLocal);
        CHECK(claims.access == ManagedRefAccess::Write);
        CHECK(claims.inside_actor_critical_state_loan);
        CHECK(claims.mutable_loan);
        CHECK(claims.recycle == ManagedRefRecycle::QuarantinePublished);
    }

    {
        const auto program = read_program();
        const auto claims = SourceOwnershipChecker::derive_actor_state_ref_claims(
            program, "Counter", ManagedRefAccess::Read, REGION, true);
        CHECK(claims.access == ManagedRefAccess::Read);
        CHECK(!claims.mutable_loan);
    }

    // Direct AST mutation cannot turn a noncritical callback into compact-ref authority.
    rejects([&] {
        auto program = write_program();
        program.actors.front().on_message.actor_state_loan->critical = false;
        (void)SourceOwnershipChecker::derive_actor_state_ref_claims(
            program, "Counter", ManagedRefAccess::Write, REGION, true);
    });

    // The caller cannot relabel a read loan as a write proof.
    rejects([&] {
        auto program = read_program();
        (void)SourceOwnershipChecker::derive_actor_state_ref_claims(
            program, "Counter", ManagedRefAccess::Write, REGION, true);
    });

    // A forged legacy write beside the sealed state loan is rejected.
    rejects([&] {
        auto program = write_program();
        ActorFieldWrite forged;
        forged.field = "total";
        forged.expression = std::make_unique<Expression>();
        forged.expression->kind = Expression::Kind::Integer;
        forged.expression->integer = 9;
        program.actors.front().on_message.actor_field_writes.push_back(std::move(forged));
        (void)SourceOwnershipChecker::derive_actor_state_ref_claims(
            program, "Counter", ManagedRefAccess::Write, REGION, true);
    });

    // A forged call node inside an already-parsed callback cannot bypass the checker.
    rejects([&] {
        auto program = read_program();
        program.actors.front().on_message.actor_state_loan->result->kind =
            Expression::Kind::Call;
        (void)SourceOwnershipChecker::derive_actor_state_ref_claims(
            program, "Counter", ManagedRefAccess::Read, REGION, true);
    });

    // Suspension/FFI declarations are always compact-reference escape boundaries.
    rejects([&] {
        auto program = read_program();
        program.actors.front().on_message.declared_effects.bits |=
            static_cast<std::uint32_t>(FunctionEffect::Suspend);
        (void)SourceOwnershipChecker::derive_actor_state_ref_claims(
            program, "Counter", ManagedRefAccess::Read, REGION, true);
    });
    rejects([&] {
        auto program = read_program();
        program.actors.front().on_message.declared_effects.bits |=
            static_cast<std::uint32_t>(FunctionEffect::Ffi);
        (void)SourceOwnershipChecker::derive_actor_state_ref_claims(
            program, "Counter", ManagedRefAccess::Read, REGION, true);
    });

    // Physical compact-layout preconditions are still checked downstream.
    rejects([&] {
        const auto program = read_program();
        (void)SourceOwnershipChecker::derive_actor_state_ref_claims(
            program, "Counter", ManagedRefAccess::Read, 0, true);
    });
    rejects([&] {
        const auto program = read_program();
        (void)SourceOwnershipChecker::derive_actor_state_ref_claims(
            program, "Counter", ManagedRefAccess::Read, REGION, false);
    });

    rejects([&] {
        const auto program = read_program();
        (void)SourceOwnershipChecker::derive_actor_state_ref_claims(
            program, "Missing", ManagedRefAccess::Read, REGION, true);
    });

    return 0;
}
