#include "oreslang/types/SourceOwnershipChecker.hpp"

#include "oreslang/backend/ManagedRefMir.hpp"

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <type_traits>

using namespace oreslang;
using namespace oreslang::backend;

#define CHECK(cond) do { if (!(cond)) throw std::runtime_error("failed: " #cond); } while (false)

static constexpr std::uint64_t REGION = UINT64_C(64) * 1024 * 1024;

static constexpr std::string_view READ_SOURCE = R"ores(
define actor RefReader as
  state {
    root: Ref<int> = new Ref<int>(41);
  }
  pub fnc on_message(int n): int with state.read {
    return self.handle_state(|s| -> rt ref_read(s.root));
  }
end
)ores";

static constexpr std::string_view WRITE_SOURCE = R"ores(
define actor RefWriter as
  state {
    root: Ref<int> = new Ref<int>(0);
  }
  pub fnc on_message(int n): int with state.read, state.write {
    return self.handle_state(critical write |s| -> rt ref_write(s.root, n));
  }
end
)ores";

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

int main() {
    static_assert(!std::is_constructible<ActorRefOwnershipEvidence,
                                         ActorManagedRefMir>::value,
                  "raw MIR claims must remain unable to mint source evidence");

    {
        const auto evidence =
            SourceOwnershipChecker::issue_actor_managed_ref_evidence(
                READ_SOURCE, "RefReader", "root", REGION, true);
        const auto claims = evidence.claims();
        CHECK(claims.origin == ManagedRefOrigin::ActorOwned);
        CHECK(claims.escape == ManagedRefEscape::ActorLocal);
        CHECK(claims.access == ManagedRefAccess::Read);
        CHECK(claims.layout == ManagedRefStorage::Compressed32);
        CHECK(claims.recycle == ManagedRefRecycle::QuarantinePublished);
        CHECK(claims.inside_actor_critical_state_loan);
        CHECK(!claims.mutable_loan);
        CHECK(claims.allocation_is_actor_private);
        CHECK(verify_actor_compressed32(evidence).access()
              == ManagedRefAccess::Read);
    }

    {
        const auto evidence =
            SourceOwnershipChecker::issue_actor_managed_ref_evidence(
                WRITE_SOURCE, "RefWriter", "root", REGION, true);
        const auto claims = evidence.claims();
        CHECK(claims.access == ManagedRefAccess::Write);
        CHECK(claims.mutable_loan);
        CHECK(verify_actor_compressed32(evidence).access()
              == ManagedRefAccess::Write);
    }

    rejects([] {
        (void)SourceOwnershipChecker::issue_actor_managed_ref_evidence(
            READ_SOURCE, "MissingActor", "root", REGION, true);
    });
    rejects([] {
        (void)SourceOwnershipChecker::issue_actor_managed_ref_evidence(
            READ_SOURCE, "RefReader", "missing", REGION, true);
    });
    rejects([] {
        (void)SourceOwnershipChecker::issue_actor_managed_ref_evidence(
            READ_SOURCE, "RefReader", "root", 0, true);
    });
    rejects([] {
        (void)SourceOwnershipChecker::issue_actor_managed_ref_evidence(
            READ_SOURCE, "RefReader", "root", REGION, false);
    });

    // A caller cannot ask for write evidence from read source: access is
    // derived exclusively from the parsed terminal managed-ref operation.
    {
        const auto evidence =
            SourceOwnershipChecker::issue_actor_managed_ref_evidence(
                READ_SOURCE, "RefReader", "root", REGION, true);
        CHECK(evidence.claims().access == ManagedRefAccess::Read);
    }

    return 0;
}
