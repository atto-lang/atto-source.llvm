#include "oreslang/runtime/ActorGraphSweepPlan.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

using oreslang::runtime::detail::plan_actor_graph_sweep;
static unsigned cases = 0;
static void check(bool yes) { assert(yes); ++cases; }

template<std::size_t N>
struct Fixture {
    std::array<bool,N> live{};
    std::array<std::vector<std::size_t>,N> edges;
    std::vector<std::size_t> roots;
    std::atomic<bool> cancel{false};
    auto plan(std::size_t work = 1000000) {
        return plan_actor_graph_sweep<N>(roots.size(), work, cancel,
            [&](std::size_t id) { return live[id]; },
            [&](std::size_t id) { return edges[id].size(); },
            [&](std::size_t id, std::size_t e) { return edges[id][e]; },
            [&](std::size_t i) { return roots[i]; });
    }
};

int main() {
    Fixture<6> f;
    f.live = {true,true,true,true,true,false};
    f.edges[0] = {1,1}; f.edges[1] = {0}; f.edges[2] = {3};
    f.edges[3] = {2,4}; f.edges[4] = {4,0}; f.roots = {0,0};
    auto p = f.plan();
    check(p && p->nodes == 3 && p->edges == 5);
    check(!p->reclaim[0] && !p->reclaim[1] && p->reclaim[2] && p->reclaim[3] && p->reclaim[4] && !p->reclaim[5]);
    // 12 slot visits + 8 validation edges + 2 roots + 2 reachable nodes + 3 edges.
    check(bool(f.plan(27))); check(!f.plan(26));
    f.cancel.store(true); check(!f.plan()); f.cancel.store(false);
    f.roots.clear(); p = f.plan(); check(p && p->nodes == 5 && p->edges == 8);
    f.roots = {5}; bool denied = false;
    try { (void)f.plan(); } catch (const std::logic_error&) { denied = true; }
    check(denied);
    f.roots.clear(); f.edges[4].push_back(5); denied = false;
    try { (void)f.plan(); } catch (const std::logic_error&) { denied = true; }
    check(denied); f.edges[4].pop_back(); f.edges[4].push_back(6); denied = false;
    try { (void)f.plan(); } catch (const std::logic_error&) { denied = true; }
    check(denied);
    Fixture<2> empty; p.reset(); auto none = empty.plan(4); check(none && none->nodes == 0);
    check(!empty.plan(3)); empty.roots = {0,0,0}; check(!empty.plan());
    auto large = std::make_unique<Fixture<10000>>(); large->live.fill(true);
    for (std::size_t i=0;i<10000;++i) large->edges[i] = {(i+1)%10000};
    auto lp = large->plan(); check(lp && lp->nodes == 10000 && lp->edges == 10000);
    large->roots = {123}; lp = large->plan(); check(lp && lp->nodes == 0);
    std::mt19937 rng(731);
    for (unsigned t=0;t<1000;++t) {
        Fixture<32> g; g.live.fill(true);
        std::array<std::array<bool,32>,32> closure{};
        for (std::size_t i=0;i<32;++i) {
            closure[i][i]=true;
            for (unsigned k=0;k<rng()%5;++k) {
                auto j=static_cast<std::size_t>(rng()%32); g.edges[i].push_back(j); closure[i][j]=true;
            }
        }
        for (unsigned i=0;i<rng()%6;++i) g.roots.push_back(rng()%32);
        for (std::size_t k=0;k<32;++k) for (std::size_t i=0;i<32;++i) for (std::size_t j=0;j<32;++j)
            closure[i][j] = closure[i][j] || (closure[i][k] && closure[k][j]);
        auto actual = g.plan(); assert(actual);
        std::size_t nodes=0, edges=0;
        for (std::size_t i=0;i<32;++i) {
            bool reachable=false; for (auto r:g.roots) reachable=reachable || closure[r][i];
            assert(actual->reclaim[i] == !reachable);
            if (!reachable) { ++nodes; edges+=g.edges[i].size(); }
        }
        check(actual->nodes==nodes && actual->edges==edges);
    }
    std::cout << "PASS " << cases << " assertions (1000 independent closure-model graphs, 10000-node cycle)\n";
}
