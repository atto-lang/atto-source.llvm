#include "oreslang/runtime/NativeActorStreamRoute.hpp"
#include <atomic>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using oreslang::runtime::NativeActorStreamRoute;
using Route = NativeActorStreamRoute;
static void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
template<typename F> static void rejected(F&& f) {
    try { f(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("unauthorized transition was accepted");
}
static std::vector<std::uint64_t> ids(std::size_t n) {
    std::vector<std::uint64_t> v;
    for (std::size_t i = 0; i < n; ++i) v.push_back(i + 100);
    return v;
}
static void roundtrip(std::size_t n) {
    const auto a = ids(n);
    Route route(a, "PipeMessages@sha256:abcdef");
    auto entry = route.claim_root(a[0], "PipeMessages@sha256:abcdef");
    rejected([&] { route.claim_root(a[0], "PipeMessages@sha256:abcdef"); });
    auto forged = entry;
    ++forged.token;
    rejected([&] { route.commit_published(forged); });
    route.commit_published(entry);
    check(route.phase() == 1, "root publication phase");
    for (std::size_t i = 0; i < n-1; ++i) {
        rejected([&] { route.claim_directional(a[i], 9999, Route::Direction::Forward, "PipeMessages@sha256:abcdef"); });
        auto c = route.claim_directional(a[i], i == 0 ? 0 : a[i-1],
            Route::Direction::Forward, "PipeMessages@sha256:abcdef");
        check(c.destination == a[i+1], "wrong downstream recipient");
        route.commit_published(c);
    }
    for (std::size_t i = n-1; i > 0; --i) {
        auto c = route.claim_directional(a[i], i == n-1 ? a[i-1] : a[i+1],
            Route::Direction::Backward, "PipeMessages@sha256:abcdef");
        check(c.destination == a[i-1], "wrong upstream recipient");
        route.commit_published(c);
    }
    rejected([&] { route.complete_source(a[0], 9999); });
    route.complete_source(a[0], a[1]);
    rejected([&] { route.claim_root(a[0], "PipeMessages@sha256:abcdef"); });
    route.physical_done(a[0], true);
    check(route.done() == Route::Done::Pending, "source-only DONE falsely completed route");
    check(!route.fenced(a[0]), "departed source fence leaked");
    for (std::size_t i = 1; i < n; ++i) {
        check(route.fenced(a[i]), "source DONE released live sibling fence");
        route.physical_done(a[i], true);
    }
    check(route.done() == Route::Done::Succeeded, "complete roundtrip not acknowledged");
    rejected([&] { route.physical_done(a[0], true); });
}

static void advance_to(Route& route, const std::vector<std::uint64_t>& a,
                       std::size_t target, const std::string& domain) {
    const auto n = a.size();
    if (target >= 1) route.commit_published(route.claim_root(a[0], domain));
    for (std::size_t i = 0; i < n - 1 && route.phase() < target; ++i) {
        auto claim = route.claim_directional(a[i], i ? a[i-1] : 0,
            Route::Direction::Forward, domain);
        route.commit_published(claim);
    }
    for (std::size_t i = n - 1; i > 0 && route.phase() < target; --i) {
        auto claim = route.claim_directional(a[i],
            i == n - 1 ? a[i-1] : a[i+1],
            Route::Direction::Backward, domain);
        route.commit_published(claim);
    }
    if (route.phase() < target) route.complete_source(a[0], a[1]);
    check(route.phase() == target, "failed to construct requested phase");
}
static void physical_done_phase_matrix(std::size_t n) {
    const auto a = ids(n);
    for (std::size_t pos = 0; pos < n; ++pos) {
        const std::size_t required = pos ? 2*n-pos : 2*n;
        for (std::size_t phase = 0; phase <= 2*n; ++phase) {
            Route route(a, "closed-domain");
            advance_to(route, a, phase, "closed-domain");
            route.physical_done(a[pos], true);
            check(route.done() == (phase < required
                  ? Route::Done::Failed : Route::Done::Pending),
                  "premature physical DONE threshold mismatch");
            check(!route.fenced(a[pos]),
                  "completed stage retained its own admission fence");
            for (std::size_t peer = 0; peer < n; ++peer)
                if (peer != pos)
                    check(route.fenced(a[peer]),
                          "stage DONE unregistered a live sibling's fence");
        }
    }
}
static void authenticated_typed_reply(std::size_t n) {
    auto actors = ids(n);
    Route route(actors, "canonical-domain-v2");
    advance_to(route, actors, 2*n-1, "canonical-domain-v2");
    std::vector<std::uint8_t> reply{0x92, 0xa1, 0x52, 0x13};
    rejected([&] { route.completed_reply(); });
    rejected([&] { route.complete_source_with_reply(
        actors[0], 99999, "canonical-domain-v2", reply); });
    rejected([&] { route.complete_source_with_reply(
        actors[0], actors[1], "wrong-domain", reply); });
    rejected([&] { route.complete_source_with_reply(
        actors[0], actors[1], "canonical-domain-v2", {}); });
    rejected([&] { route.complete_source_with_reply(
        actors[0], actors[1], "canonical-domain-v2",
        std::vector<std::uint8_t>(1024 * 1024 + 1, 0x42)); });
    check(route.phase() == 2*n-1, "invalid reply advanced source terminal phase");
    route.complete_source_with_reply(
        actors[0], actors[1], "canonical-domain-v2", reply);
    check(route.phase() == 2*n, "authenticated result did not finish source");
    rejected([&] { route.complete_source_with_reply(
        actors[0], actors[1], "canonical-domain-v2", reply); });
    reply[0] = 0x00; // corrupt caller-owned buffer after publication
    rejected([&] { route.completed_reply(); });
    for (auto id : actors) route.physical_done(id, true);
    auto result = route.completed_reply();
    check(result.size() == 4 && result[0] == 0x92,
          "source's immutable owned reply was corrupted by caller alias");
    result[1] = 0x00;
    check(route.completed_reply()[1] == 0xa1,
          "caller modified runtime-owned completed reply through an alias");
    check(route.done() == Route::Done::Succeeded,
          "typed final payload prevented whole-pipeline DONE");
    route.fail();
    check(route.completed_reply()[0] == 0x92,
          "late post-success failure invalidated already-finalized result");
}
static void typed_completion_races_abort() {
    // A source return may race a fatal CONTROL event. Even if the return
    // claims the terminal source phase first, failure before whole-route
    // physical DONE MUST permanently prevent publishing typed bytes.
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto a = ids(3);
        Route route(a, "domain");
        advance_to(route, a, 2 * a.size() - 1, "domain");
        std::atomic<bool> start{false};
        std::thread publisher([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try {
                route.complete_source_with_reply(
                    a[0], a[1], "domain", {0x92, 0xa1, 0x52});
            } catch (const std::logic_error&) {
                // Failure won the source-publish race.
            }
        });
        std::thread failure([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            route.fail();
        });
        start.store(true, std::memory_order_release);
        publisher.join();
        failure.join();
        for (auto actor : a) route.physical_done(actor, true);
        check(route.done() == Route::Done::Failed,
              "terminal failure lost the typed-result publication race");
        rejected([&] { route.completed_reply(); });
    }
}
static void reply_failure_is_not_a_result() {
    auto actors = ids(3);
    Route route(actors, "domain");
    advance_to(route, actors, 5, "domain");
    route.complete_source_with_reply(
        actors[0], actors[1], "domain", {0x92, 0xa1, 0x52});
    route.physical_done(actors[0], true);
    route.fail();
    route.physical_done(actors[1], true);
    route.physical_done(actors[2], true);
    check(route.done() == Route::Done::Failed, "failure did not abort reply");
    rejected([&] { route.completed_reply(); });

    Route legacy(actors, "domain");
    advance_to(legacy, actors, 6, "domain");
    for (auto actor : actors) legacy.physical_done(actor, true);
    check(legacy.done() == Route::Done::Succeeded,
          "legacy control-plane route stopped working");
    rejected([&] { legacy.completed_reply(); });
}
int main() {
    for (auto n: {2u, 3u, 4u, 15u, 16u}) {
        roundtrip(n);
        physical_done_phase_matrix(n);
        authenticated_typed_reply(n);
    }
    reply_failure_is_not_a_result();
    typed_completion_races_abort();
    for (auto n: {1u, 17u}) {
        try { Route invalid(ids(n), "PipeMessages"); }
        catch (const std::invalid_argument&) { continue; }
        throw std::runtime_error("stage bound bypassed");
    }
    try { Route invalid({100, 100}, "PipeMessages"); }
    catch (const std::invalid_argument&) { goto duplicate_ok; }
    throw std::runtime_error("duplicate actor identity accepted");
    duplicate_ok:
    {
        auto a = ids(3);
        Route route(a, "closed-domain");
        rejected([&] { route.claim_root(a[0], "wrong-domain"); });
        auto c = route.claim_root(a[0], "closed-domain");
        route.reject_prepublication(c);
        check(route.phase() == 0, "rejected root consumed single admission credit");
        route.commit_published(route.claim_root(a[0], "closed-domain"));
        auto forward = route.claim_directional(a[0], 0, Route::Direction::Forward, "closed-domain");
        route.reject_prepublication(forward);
        check(route.done() == Route::Done::Failed, "failed hop was not terminal");
        rejected([&] { route.claim_root(a[0], "closed-domain"); });
    }
    {
        auto a = ids(3);
        Route route(a, "closed-domain");
        route.commit_published(route.claim_root(a[0], "closed-domain"));
        route.physical_done(a[1], true);
        check(route.done() == Route::Done::Failed, "premature middle DONE succeeded");
        check(route.fenced(a[2]), "premature middle DONE released sibling fence");
    }
    {
        auto a = ids(3);
        Route route(a, "closed-domain");
        route.commit_published(route.claim_root(a[0], "closed-domain"));
        route.fail();
        check(route.done() == Route::Done::Failed, "actor failure did not abort route");
    }
    // A sink may finish while the transform holds its valid backward
    // publication reservation; that sibling's claim must not cause a false
    // pipeline abort. The reservation commits before source execution.
    {
        auto a = ids(3);
        Route route(a, "closed-domain");
        route.commit_published(route.claim_root(a[0], "closed-domain"));
        route.commit_published(route.claim_directional(a[0], 0,
            Route::Direction::Forward, "closed-domain"));
        route.commit_published(route.claim_directional(a[1], a[0],
            Route::Direction::Forward, "closed-domain"));
        route.commit_published(route.claim_directional(a[2], a[1],
            Route::Direction::Backward, "closed-domain"));
        auto transform_backward = route.claim_directional(a[1], a[2],
            Route::Direction::Backward, "closed-domain");
        route.physical_done(a[2], true);
        check(route.done() == Route::Done::Pending,
              "sibling pending publication falsely aborted completed sink");
        check(!route.fenced(a[2]) && route.fenced(a[1]),
              "sink DONE changed surviving transform fence");
        route.commit_published(transform_backward);
        route.complete_source(a[0], a[1]);
        route.physical_done(a[1], true);
        route.physical_done(a[0], true);
        check(route.done() == Route::Done::Succeeded,
              "legitimate overlapping physical DONE and sibling publication failed");
    }
    // Claims belong to one route object even if two routes happen to share
    // stage ids and their first token value is the same.
    {
        Route first(ids(3), "domain");
        Route second(ids(3), "domain");
        auto a = first.claim_root(100, "domain");
        auto b = second.claim_root(100, "domain");
        rejected([&] { second.commit_published(a); });
        first.commit_published(a);
        second.commit_published(b);
    }
    // Adversarial address reuse: object-pointer identity and ticket 1 alone
    // must not authenticate an old route against its replacement in-place.
    {
        alignas(Route) unsigned char backing[sizeof(Route)];
        Route* previous = new (backing) Route(ids(3), "domain");
        auto stale = previous->claim_root(100, "domain");
        previous->~Route();
        Route* replacement = new (backing) Route(ids(3), "domain");
        auto fresh = replacement->claim_root(100, "domain");
        rejected([&] { replacement->commit_published(stale); });
        check(replacement->phase() == 0,
              "stale claim advanced generation-reused route");
        replacement->commit_published(fresh);
        check(replacement->phase() == 1,
              "fresh route claim blocked by stale address reuse");
        replacement->~Route();
    }
    // A forged enum representation must not silently be interpreted as
    // Backward, including at a phase where backward delivery would be legal.
    {
        const auto a = ids(3);
        Route route(a, "domain");
        route.commit_published(route.claim_root(a[0], "domain"));
        route.commit_published(route.claim_directional(a[0], 0,
            Route::Direction::Forward, "domain"));
        route.commit_published(route.claim_directional(a[1], a[0],
            Route::Direction::Forward, "domain"));
        rejected([&] { route.claim_directional(a[2], a[1],
            static_cast<Route::Direction>(99), "domain"); });
        check(route.phase() == 3, "invalid direction changed phase");
        rejected([&] { route.fenced(9999); });
        auto valid = route.claim_directional(a[2], a[1],
            Route::Direction::Backward, "domain");
        route.commit_published(valid);
        check(route.phase() == 4, "invalid opcode damaged valid routing");
    }
    // A prepublication root rejection may be retried, but the old
    // reservation is revoked, not transferred to the new reservation.
    {
        Route route(ids(3), "domain");
        auto stale = route.claim_root(100, "domain");
        route.reject_prepublication(stale);
        auto fresh = route.claim_root(100, "domain");
        rejected([&] { route.commit_published(stale); });
        route.commit_published(fresh);
        check(route.phase() == 1, "stale root rollback consumed new credit");
    }
    // Runtime failure wins against a pending outbound publication. No late
    // publisher can resurrect an aborted route's phase or DONE state.
    {
        Route route(ids(3), "domain");
        auto pending = route.claim_root(100, "domain");
        std::thread fatal([&] { route.fail(); });
        fatal.join();
        rejected([&] { route.commit_published(pending); });
        rejected([&] { route.reject_prepublication(pending); });
        check(route.phase() == 0 && route.done() == Route::Done::Failed,
              "terminal failure was undone by a late publication");
    }

    // Terminal success cannot be revoked by a delayed post-DONE failure
    // notification. Before the final DONE, any failure remains fatal.
    {
        auto a = ids(3);
        Route finished(a, "domain");
        advance_to(finished, a, 6, "domain");
        for (auto actor : a) finished.physical_done(actor, true);
        check(finished.done() == Route::Done::Succeeded,
              "missing terminal success");
        finished.fail();
        check(finished.done() == Route::Done::Succeeded,
              "late failure revoked already-published whole-route success");

        Route incomplete(a, "domain");
        advance_to(incomplete, a, 6, "domain");
        incomplete.physical_done(a[0], true);
        incomplete.physical_done(a[2], true);
        incomplete.fail();
        incomplete.physical_done(a[1], true);
        check(incomplete.done() == Route::Done::Failed,
              "failure before final DONE was incorrectly ignored");
    }
    // Serialized final-DONE versus failure race must yield one stable
    // terminal answer, not success followed by failure.
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto a = ids(3);
        Route route(a, "domain");
        advance_to(route, a, 6, "domain");
        route.physical_done(a[0], true);
        route.physical_done(a[2], true);
        std::atomic<bool> start{false};
        std::thread done_worker([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            route.physical_done(a[1], true);
        });
        std::thread fail_worker([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            route.fail();
        });
        start.store(true, std::memory_order_release);
        done_worker.join();
        fail_worker.join();
        auto result = route.done();
        check(result == Route::Done::Failed ||
              result == Route::Done::Succeeded,
              "physical-DONE/failure race stayed pending");
        route.fail();
        check(route.done() == result,
              "terminal state changed after finalization/failure race");
    }
    // One publisher owns the claim; concurrent contenders cannot obtain
    // duplicate route authority while the first claim is pending.
    {
        auto a = ids(3);
        Route route(a, "closed-domain");
        std::atomic<int> winners{0};
        std::vector<std::thread> contenders;
        for (int i = 0; i < 16; ++i) {
            contenders.emplace_back([&] {
                try { (void) route.claim_root(a[0], "closed-domain"); ++winners; }
                catch (const std::logic_error&) { }
            });
        }
        for (auto& t : contenders) t.join();
        check(winners == 1, "concurrent duplicate root claim succeeded");
    }
}
