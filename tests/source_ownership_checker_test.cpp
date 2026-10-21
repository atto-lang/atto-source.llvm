#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/SourceOwnershipChecker.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace oreslang;
using namespace oreslang::backend;

#define CHECK(x) do { if (!(x)) throw std::runtime_error("failed: " #x); } while (false)

template <typename F>
static void rejects(F&& fn) {
    bool denied = false;
    try { fn(); } catch (const std::exception&) { denied = true; }
    CHECK(denied);
}

static constexpr std::uint64_t REGION = UINT64_C(64) * 1024 * 1024;

int main() {
    const std::string read_source = R"(
define actor Box as {
  state {
    root: Ref<int> = null;
  }
  pub fnc on_message(int n): int with state.read {
    const result = self.handle_state(critical read |state| -> {
      return state.root.value;
    });
    return result;
  }
}
)";
    Program read = parse_program(read_source);
    auto read_evidence = SourceOwnershipChecker::issue_actor_state_ref_evidence(
        read, "Box", "root", ManagedRefAccess::Read, REGION, true);
    auto read_verified = verify_actor_compressed32(read_evidence);
    CHECK(read_verified.access() == ManagedRefAccess::Read);

    const std::string write_source = R"(
define actor Box as {
  state {
    root: Ref<int> = null;
  }
  pub fnc on_message(int n): int with state.write {
    const result = self.handle_state(critical |state| -> {
      state.root.value = n;
      return n;
    });
    return result;
  }
}
)";
    Program write = parse_program(write_source);
    auto write_evidence = SourceOwnershipChecker::issue_actor_state_ref_evidence(
        write, "Box", "root", ManagedRefAccess::Write, REGION, true);
    auto write_verified = verify_actor_compressed32(write_evidence);
    CHECK(write_verified.access() == ManagedRefAccess::Write);

    rejects([&] {
        (void) SourceOwnershipChecker::issue_actor_state_ref_evidence(
            write, "Box", "root", ManagedRefAccess::Read, REGION, true);
    });
    rejects([&] {
        (void) SourceOwnershipChecker::issue_actor_state_ref_evidence(
            read, "Box", "root", ManagedRefAccess::Read, 0, true);
    });
    rejects([&] {
        (void) SourceOwnershipChecker::issue_actor_state_ref_evidence(
            read, "Box", "root", ManagedRefAccess::Read, REGION, false);
    });

    const std::string noncritical = R"(
define actor Box as {
  state { root: Ref<int> = null; }
  pub fnc on_message(int n): int with state.read {
    const result = self.handle_state(|state| -> {
      return state.root.value;
    });
    return result;
  }
}
)";
    Program ordinary = parse_program(noncritical);
    rejects([&] {
        (void) SourceOwnershipChecker::issue_actor_state_ref_evidence(
            ordinary, "Box", "root", ManagedRefAccess::Read, REGION, true);
    });

    const std::string scalar = R"(
define actor Box as {
  state { root: int = 0; }
  pub fnc on_message(int n): int with state.read {
    const result = self.handle_state(critical read |state| -> {
      return state.root;
    });
    return result;
  }
}
)";
    Program scalar_program = parse_program(scalar);
    rejects([&] {
        (void) SourceOwnershipChecker::issue_actor_state_ref_evidence(
            scalar_program, "Box", "root", ManagedRefAccess::Read, REGION, true);
    });

    static_assert(!std::is_constructible<ActorRefOwnershipEvidence,
                                         ActorManagedRefMir>::value,
                  "MIR claims must not become source evidence");
    return 0;
}
