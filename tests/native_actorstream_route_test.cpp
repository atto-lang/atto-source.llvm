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
int main() {
    for (auto n: {2u, 3u, 4u, 15u, 16u}) roundtrip(n);
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
