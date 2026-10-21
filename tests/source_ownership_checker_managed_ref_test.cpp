#include "oreslang/types/SourceOwnershipChecker.hpp"

#include "oreslang/backend/ManagedRefMir.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

using namespace oreslang;
using namespace oreslang::backend;

#define CHECK(cond) do { if (!(cond)) throw std::runtime_error("failed: " #cond); } while (false)

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
    static_assert(!std::is_default_constructible<SourceManagedRefAuthorization>::value,
                  "source authorization must be site-bound");
    static_assert(!std::is_constructible<SourceManagedRefAuthorization,
                                         ActorRefOwnershipEvidence,
                                         std::string, std::string,
                                         std::string, std::string,
                                         std::size_t>::value,
                  "ordinary callers cannot wrap/replay raw evidence");
    static_assert(!std::is_copy_constructible<SourceManagedRefAuthorization>::value,
                  "source authorization must not be replayable by copy");

    const auto native64 = TargetAbi::parse("x86_64-unknown-linux-gnu");

    {
        const auto authorization =
            SourceOwnershipChecker::authorize_actor_managed_ref(
                READ_SOURCE, "RefReader", "root", native64);
        const auto claims = authorization.claims();
        CHECK(claims.origin == ManagedRefOrigin::ActorOwned);
        CHECK(claims.escape == ManagedRefEscape::ActorLocal);
        CHECK(claims.access == ManagedRefAccess::Read);
        CHECK(claims.layout == ManagedRefStorage::Compressed32);
        CHECK(claims.recycle == ManagedRefRecycle::QuarantinePublished);
        CHECK(claims.inside_actor_critical_state_loan);
        CHECK(!claims.mutable_loan);
        CHECK(claims.allocation_is_actor_private);
        CHECK(claims.actor_region_capacity_bytes == UINT64_C(64) * 1024 * 1024);
        CHECK(authorization.target_triple() == "x86_64-unknown-linux-gnu");
        CHECK(authorization.actor_name() == "RefReader");
        CHECK(authorization.ref_field() == "root");
        CHECK(authorization.use_offset() != 0);
    }

    {
        const auto authorization =
            SourceOwnershipChecker::authorize_actor_managed_ref(
                WRITE_SOURCE, "RefWriter", "root", native64);
        const auto claims = authorization.claims();
        CHECK(claims.access == ManagedRefAccess::Write);
        CHECK(claims.mutable_loan);
        CHECK(authorization.actor_name() == "RefWriter");
        CHECK(authorization.ref_field() == "root");
        CHECK(authorization.use_offset() != 0);
    }

    rejects([&] {
        (void)SourceOwnershipChecker::authorize_actor_managed_ref(
            READ_SOURCE, "MissingActor", "root", native64);
    });
    rejects([&] {
        (void)SourceOwnershipChecker::authorize_actor_managed_ref(
            READ_SOURCE, "RefReader", "missing", native64);
    });
    rejects([&] {
        const auto native32 = TargetAbi::parse("i686-unknown-linux-gnu");
        (void)SourceOwnershipChecker::authorize_actor_managed_ref(
            READ_SOURCE, "RefReader", "root", native32);
    });

    // A caller cannot ask for write evidence from read source: access is
    // derived exclusively from the parsed terminal managed-ref operation.
    {
        const auto authorization =
            SourceOwnershipChecker::authorize_actor_managed_ref(
                READ_SOURCE, "RefReader", "root", native64);
        CHECK(authorization.claims().access == ManagedRefAccess::Read);
    }

    return 0;
}
