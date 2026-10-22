#include "oreslang/runtime/SupervisorLifecycleFsm.hpp"
#include <deque>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using F = oreslang::runtime::SupervisorLifecycleFsm;
#define CHECK(x) do { if (!(x)) throw std::logic_error("failed: " #x); } while (false)

static F::Event event(F::Kind kind, std::uint64_t epoch = 1) {
    return F::Event{kind, epoch};
}
static F::Event shutdown(std::initializer_list<F::Child> children = {}, std::uint64_t epoch = 1) {
    auto e = event(F::Kind::shutdown_requested, epoch);
    for (auto child : children) e.children[e.child_count++] = child;
    return e;
}
static F::Event child(F::Kind kind, F::Child token, std::uint64_t epoch = 1) {
    auto e = event(kind, epoch); e.child = token; return e;
}
static void rejects(const F::State& s, const F::Event& e) {
    bool rejected = false;
    try { (void)F::reduce(s, e); } catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
}
static F::State stopped(std::initializer_list<F::Child> children = {}) {
    auto s = F::reduce(F::State{}, shutdown(children));
    s = F::reduce(s, event(F::Kind::before_finished));
    for (auto c : children) s = F::reduce(s, child(F::Kind::child_finished, c));
    s = F::reduce(s, event(F::Kind::actors_finished));
    return F::reduce(s, event(F::Kind::after_finished));
}
static void focused() {
    auto s = F::reduce(F::State{}, shutdown({{11, 7}, {22, 9}}));
    rejects(s, event(F::Kind::restart_requested));
    rejects(s, event(F::Kind::restart_committed));
    rejects(s, child(F::Kind::child_finished, {11, 7}));
    s = F::reduce(s, event(F::Kind::actors_finished));
    s = F::reduce(s, event(F::Kind::physical_release));
    s = F::reduce(s, event(F::Kind::before_finished));
    CHECK(F::reduce(s, child(F::Kind::child_finished, {11, 6})).revision == s.revision);
    rejects(s, child(F::Kind::child_finished, {11, 8}));
    rejects(s, child(F::Kind::child_finished, {99, 7}));
    s = F::reduce(s, child(F::Kind::child_finished, {11, 7}));
    CHECK(F::reduce(s, child(F::Kind::child_finished, {11, 7})).revision == s.revision);
    s = F::reduce(s, child(F::Kind::child_finished, {22, 9}));
    CHECK(s.phase == F::Phase::after_hook);
    s = F::reduce(s, event(F::Kind::after_finished));
    CHECK(s.phase == F::Phase::stopped);
    CHECK(F::reduce(s, event(F::Kind::deadline)).revision == s.revision);
    s = F::reduce(s, child(F::Kind::child_reclaimed, {11, 7}));
    CHECK(s.phase == F::Phase::stopped);
    s = F::reduce(s, child(F::Kind::child_reclaimed, {22, 9}));
    CHECK(s.phase == F::Phase::reclaimed);

    auto r = F::reduce(F::State{}, event(F::Kind::restart_requested));
    rejects(r, event(F::Kind::restart_committed));
    r = F::reduce(r, event(F::Kind::actors_finished));
    r = F::reduce(r, event(F::Kind::physical_release));
    r = F::reduce(r, event(F::Kind::restart_committed));
    CHECK(r.epoch == 2 && r.admissions_open());
    CHECK(F::reduce(r, event(F::Kind::actors_finished, 1)).revision == r.revision);
    rejects(r, event(F::Kind::actors_finished, 3));
    r = F::reduce(r, shutdown({}, 2));
    r = F::reduce(r, event(F::Kind::deadline, 2));
    CHECK(r.timed_out && !r.logically_stopped());
    auto failed = event(F::Kind::coordinator_failed, 2);
    CHECK(F::reduce(r, failed).failed);
    rejects(F::State{}, event(static_cast<F::Kind>(255)));

    auto full = shutdown(); full.child_count = F::max_children;
    for (std::size_t i = 0; i < full.child_count; ++i) full.children[i] = {i + 1, 1};
    auto wide = F::reduce(F::State{}, full);
    wide = F::reduce(wide, event(F::Kind::before_finished));
    for (auto c : full.children) wide = F::reduce(wide, child(F::Kind::child_finished, c));
    CHECK(wide.completed == ~std::uint64_t{0});
    full.children[63] = full.children[0]; rejects(F::State{}, full);
    full.child_count = 65; rejects(F::State{}, full);
    auto overflow = F::State{}; overflow.revision = ~std::uint64_t{0};
    rejects(overflow, event(F::Kind::actors_finished));
    overflow = F::State{}; overflow.epoch = ~std::uint64_t{0};
    overflow = F::reduce(overflow, event(F::Kind::restart_requested, overflow.epoch));
    overflow = F::reduce(overflow, event(F::Kind::actors_finished, overflow.epoch));
    overflow = F::reduce(overflow, event(F::Kind::physical_release, overflow.epoch));
    rejects(overflow, event(F::Kind::restart_committed, overflow.epoch));
}
static void hierarchies() {
    // Two siblings: local and one child receipt cannot reclaim the root.
    auto root = stopped({{11, 1}, {22, 1}});
    root = F::reduce(root, event(F::Kind::physical_release));
    root = F::reduce(root, child(F::Kind::child_reclaimed, {11, 1}));
    CHECK(root.phase == F::Phase::stopped);
    root = F::reduce(root, child(F::Kind::child_reclaimed, {22, 1}));
    CHECK(root.phase == F::Phase::reclaimed);
    // Depth three: each parent waits on the whole child subtree's receipt.
    auto leaf = stopped(); auto middle = stopped({{22, 1}});
    root = stopped({{11, 1}});
    root = F::reduce(root, event(F::Kind::physical_release));
    middle = F::reduce(middle, event(F::Kind::physical_release));
    CHECK(root.phase == F::Phase::stopped && middle.phase == F::Phase::stopped);
    leaf = F::reduce(leaf, event(F::Kind::physical_release));
    CHECK(leaf.phase == F::Phase::reclaimed);
    middle = F::reduce(middle, child(F::Kind::child_reclaimed, {22, 1}));
    CHECK(middle.phase == F::Phase::reclaimed);
    root = F::reduce(root, child(F::Kind::child_reclaimed, {11, 1}));
    CHECK(root.phase == F::Phase::reclaimed);
}
static std::string key(const F::State& s) {
    std::ostringstream out;
    out << unsigned(s.phase) << ':' << s.epoch << ':' << s.child_count << ':'
        << s.completed << ':' << s.released_children << ':' << s.before_done
        << s.actors_done << s.after_done << s.physical_released << s.failed << s.timed_out;
    return out.str(); // revision omitted: observable counter, not enabling state
}
static void explore() {
    std::deque<F::State> queue{F::State{}};
    std::set<std::string> seen{key(queue.front())};
    std::size_t edges = 0;
    while (!queue.empty()) {
        auto s = queue.front(); queue.pop_front();
        std::vector<F::Event> events;
        for (unsigned i = 0; i <= unsigned(F::Kind::coordinator_failed); ++i) {
            auto e = event(static_cast<F::Kind>(i), s.epoch);
            if (e.kind == F::Kind::shutdown_requested) e = shutdown({{11, 7}, {22, 9}}, s.epoch);
            if (e.kind == F::Kind::child_finished || e.kind == F::Kind::child_reclaimed) {
                e.child = {22, 9}; events.push_back(e); e.child = {11, 7};
            }
            events.push_back(e); e.failed = true; events.push_back(e);
        }
        for (const auto& e : events) {
            F::State n;
            try {
                n = F::reduce(s, e);
            } catch (const std::logic_error&) {
                // Illegal candidates are disabled edges; focused checks assert guards.
                continue;
            }
            if (n.epoch > 2 || n.revision == s.revision) continue;
            if (s.logically_stopped()) CHECK(n.logically_stopped());
            if (s.phase != F::Phase::running && s.phase != F::Phase::restarting)
                CHECK(!n.admissions_open());
            ++edges;
            if (seen.insert(key(n)).second) queue.push_back(n);
        }
        CHECK(seen.size() < 100000);
    }
    CHECK(seen.size() > 100);
    std::cout << "PASS native lifecycle BFS states=" << seen.size() << " edges=" << edges << '\n';
}
int main() { focused(); hierarchies(); explore(); }
