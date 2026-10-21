#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/SourceOwnershipChecker.hpp"

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
    auto read_proof = SourceOwnershipChecker::validate_actor_state_ref_use(
        read_source, "Box", "root", ManagedRefAccess::Read);
    CHECK(read_proof.access() == ManagedRefAccess::Read);
    CHECK(read_proof.actor() == "Box");
    CHECK(read_proof.state_field() == "root");

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
    auto write_proof = SourceOwnershipChecker::validate_actor_state_ref_use(
        write_source, "Box", "root", ManagedRefAccess::Write);
    CHECK(write_proof.access() == ManagedRefAccess::Write);

    rejects([&] {
        (void) SourceOwnershipChecker::validate_actor_state_ref_use(
            write_source, "Box", "root", ManagedRefAccess::Read);
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
    rejects([&] {
        (void) SourceOwnershipChecker::validate_actor_state_ref_use(
            noncritical, "Box", "root", ManagedRefAccess::Read);
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
    rejects([&] {
        (void) SourceOwnershipChecker::validate_actor_state_ref_use(
            scalar, "Box", "root", ManagedRefAccess::Read);
    });

    const std::string ref_as_scalar = R"(
define actor Box as {
  state { root: Ref<int> = null; }
  pub fnc on_message(int n): int with state.read {
    const result = self.handle_state(critical read |state| -> {
      return state.root;
    });
    return result;
  }
}
)";
    rejects([&] {
        (void) SourceOwnershipChecker::validate_actor_state_ref_use(
            ref_as_scalar, "Box", "root", ManagedRefAccess::Read);
    });

    const std::string overwrite_ref = R"(
define actor Box as {
  state { root: Ref<int> = null; }
  pub fnc on_message(int n): int with state.write {
    const result = self.handle_state(critical |state| -> {
      state.root = n;
      return n;
    });
    return result;
  }
}
)";
    rejects([&] {
        (void) SourceOwnershipChecker::validate_actor_state_ref_use(
            overwrite_ref, "Box", "root", ManagedRefAccess::Write);
    });

    const std::string scalar_payload = R"(
define actor Box as {
  state { root: int = 0; }
  pub fnc on_message(int n): int with state.write {
    const result = self.handle_state(critical |state| -> {
      state.root.value = n;
      return n;
    });
    return result;
  }
}
)";
    rejects([&] {
        (void) SourceOwnershipChecker::validate_actor_state_ref_use(
            scalar_payload, "Box", "root", ManagedRefAccess::Write);
    });

    const std::string nested_payload = R"(
define actor Box as {
  state { root: Ref<int> = null; }
  pub fnc on_message(int n): int with state.read {
    const result = self.handle_state(critical read |state| -> {
      return state.root.value.value;
    });
    return result;
  }
}
)";
    rejects([&] {
        (void) SourceOwnershipChecker::validate_actor_state_ref_use(
            nested_payload, "Box", "root", ManagedRefAccess::Read);
    });

    static_assert(!std::is_default_constructible<SourceManagedRefUseProof>::value,
                  "source Ref use proof must be checker-issued");
    static_assert(!std::is_constructible<SourceManagedRefUseProof,
                                         std::string, std::string,
                                         ManagedRefAccess>::value,
                  "callers must not forge source Ref use proof");
    static_assert(!std::is_constructible<ActorRefOwnershipEvidence,
                                         ActorManagedRefMir>::value,
                  "source-use proof and MIR claims must not become compact evidence");
    return 0;
}
