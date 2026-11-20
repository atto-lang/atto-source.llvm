#include "oreslang/runtime/NativeMlAdmission.hpp"
#include <atomic>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#define CHECK(expr) do { if (!(expr)) std::abort(); } while (false)

int main() {
    using namespace oreslang::ml;
    using Sink = VolatileFencedSink;
    using Result = Sink::Result;
    bool invalid = false;
    try { Sink rejected("", "r", {1, 0, 0}); }
    catch (const std::invalid_argument&) { invalid = true; }
    CHECK(invalid);

    Sink sink("tenant-A", "model-output", {12, 7, 0});
    CHECK(sink.try_commit("tenant-A", "model-output", 11, 7, 1) == Result::StaleFence);
    CHECK(sink.try_commit("tenant-B", "model-output", 13, 7, 1) == Result::ForeignScope);
    CHECK(sink.try_commit("tenant-A", "other", 13, 7, 1) == Result::ForeignScope);
    CHECK(sink.try_commit("tenant-A", "model-output", 0, 7, 1) == Result::Invalid);
    CHECK(sink.try_commit("tenant-A", "model-output", 13, 6, 1) == Result::VersionConflict);
    CHECK(sink.try_commit("tenant-A", "model-output", 13, 7, 9) == Result::Committed);
    CHECK(sink.try_commit("tenant-A", "model-output", 12, 8, 10) == Result::StaleFence);
    CHECK(sink.snapshot().fence == 13 && sink.snapshot().version == 8);
    CHECK(sink.snapshot().value == 9);
    // Rehydrating a state from a trustworthy checkpoint must not admit
    // old tokens, even if the previous process exited.
    Sink restored("tenant-A", "model-output", sink.snapshot());
    CHECK(restored.try_commit("tenant-A", "model-output", 12, 8, 10) == Result::StaleFence);
    CHECK(restored.try_commit("tenant-A", "model-output", 14, 8, 10) == Result::Committed);

    Sink race("tenant-A", "model-output", {1, 0, 0});
    std::atomic<int> winners{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 24; ++i) {
        workers.emplace_back([&, i] {
            if (race.try_commit("tenant-A", "model-output", 2, 0, i + 1)
                    == Result::Committed) ++winners;
        });
    }
    for (auto& worker : workers) worker.join();
    CHECK(winners == 1);
    CHECK(race.snapshot().version == 1);
    race.retire();
    CHECK(race.try_commit("tenant-A", "model-output", 3, 1, 0) == Result::Retired);
    Sink exhausted("tenant-A", "model-output",
                   {1, std::numeric_limits<std::uint64_t>::max(), 0});
    CHECK(exhausted.try_commit("tenant-A", "model-output", 2,
          std::numeric_limits<std::uint64_t>::max(), 1) == Result::VersionExhausted);

    CapabilityClaim claim{1, "tenant-A", "ml-executor", "model-output", 100, 3, 12};
    AdmissionContext ctx{"tenant-A", "ml-executor", "model-output", 50, 3, 12};
    CHECK(preflight_capability(claim, ctx) ==
          CapabilityDenial::SignatureAndRedemptionUnavailable);
    claim.version = 2;
    CHECK(preflight_capability(claim, ctx) == CapabilityDenial::UnsupportedVersion);
    claim.version = 1;
    claim.tenant = "";
    CHECK(preflight_capability(claim, ctx) == CapabilityDenial::MissingScope);
    claim.tenant = "tenant-B";
    CHECK(preflight_capability(claim, ctx) == CapabilityDenial::ScopeMismatch);
    claim.tenant = "tenant-A";
    claim.expires_at = 50;
    CHECK(preflight_capability(claim, ctx) == CapabilityDenial::Expired);
    claim.expires_at = 100;
    claim.policy_revision = 2;
    CHECK(preflight_capability(claim, ctx) == CapabilityDenial::StalePolicy);
    claim.policy_revision = 3;
    claim.resource_fence = 11;
    CHECK(preflight_capability(claim, ctx) == CapabilityDenial::StaleFence);
    claim.resource_fence = 12;
    // This must NEVER return an authorized/committed outcome.
    CHECK(preflight_capability(claim, ctx) ==
          CapabilityDenial::SignatureAndRedemptionUnavailable);
}
