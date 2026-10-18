#include "oreslang/runtime/TrustedHierarchicalNetworkGate.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

using oreslang::runtime::TrustedHierarchicalNetworkGate;
using Gate = TrustedHierarchicalNetworkGate;
using Status = Gate::Status;
static void check(bool okay) { if (!okay) std::abort(); }

static Gate::Quota::Policy policy(std::uint32_t max_http = 10,
                                  std::uint32_t max_tcp = 10) {
    Gate::Quota::Policy p{};
    p.max_http_in_flight = max_http;
    p.max_tcp_connections = max_tcp;
    return p;
}

static void hierarchical_http_rolls_back_after_supervisor_denial() {
    Gate gate;
    Gate::Scope child(gate, policy(), 0), supervisor(gate, policy(1), 0);
    Gate::Scope tenant(gate, policy(), 0);
    auto first = gate.try_start_http(child, supervisor, tenant, 0);
    check(first.status == Status::admitted);
    check(first.lease.commit_dispatch());
    check(gate.try_start_http(child, supervisor, tenant, 0).status
          == Status::too_many_in_flight);
    first.lease.release();
    auto second = gate.try_start_http(child, supervisor, tenant, 0);
    check(second.status == Status::admitted);
    second.lease.release();
    check(gate.try_start_http(child, supervisor, tenant, 0).status
          == Status::rate_limited);
}

static void tenant_rollback_and_once_only_pre_dispatch_cancel() {
    Gate gate;
    Gate::Scope child(gate, policy(), 0), supervisor(gate, policy(), 0);
    Gate::Scope tenant(gate, policy(1), 0);
    auto first = gate.try_start_http(child, supervisor, tenant, 0);
    check(first.status == Status::admitted);
    check(gate.try_start_http(child, supervisor, tenant, 0).status
          == Status::too_many_in_flight);
    check(first.lease.abort_before_dispatch(0));
    check(!first.lease.abort_before_dispatch(0));
    first.lease.release();
    auto second = gate.try_start_http(child, supervisor, tenant, 0);
    check(second.status == Status::admitted);
    check(second.lease.commit_dispatch());
    check(!second.lease.abort_before_dispatch(0));
    second.lease.release();
}

static void child_fanout_cannot_bypass_shared_supervisor_tcp_cap() {
    Gate gate;
    Gate::Scope a(gate, policy(), 0), b(gate, policy(), 0);
    Gate::Scope supervisor(gate, policy(10, 1), 0);
    Gate::Scope tenant(gate, policy(), 0);
    auto first = gate.try_open_tcp(a, supervisor, tenant, 0);
    check(first.status == Status::admitted);
    check(gate.try_open_tcp(b, supervisor, tenant, 0).status
          == Status::too_many_connections);
    first.lease.release();
    auto second = gate.try_open_tcp(b, supervisor, tenant, 0);
    check(second.status == Status::admitted);
    second.lease.release();
}

static void invalid_or_closed_scope_fails_closed() {
    Gate gate, foreign;
    Gate::Scope a(gate, policy(), 0), b(gate, policy(), 0);
    Gate::Scope c(gate, policy(), 0), outsider(foreign, policy(), 0);
    check(gate.try_open_tcp(a, a, c, 0).status == Status::invalid_policy);
    check(gate.try_open_tcp(a, b, outsider, 0).status == Status::invalid_policy);
    b.close();
    check(gate.try_open_tcp(a, b, c, 0).status == Status::closed);
}

static void concurrent_tenant_cap_cannot_overspend() {
    Gate gate;
    auto p = policy();
    p.tcp_burst = 10;
    Gate::Scope child(gate, p, 0), supervisor(gate, p, 0), tenant(gate, p, 0);
    std::atomic<int> ready{0}, finished{0}, accepted{0};
    std::atomic<bool> start{false}, release{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 24; ++i) {
        threads.emplace_back([&] {
            ++ready;
            while (!start.load()) std::this_thread::yield();
            auto attempt = gate.try_open_tcp(child, supervisor, tenant, 0);
            if (attempt.status == Status::admitted) ++accepted;
            ++finished;
            while (!release.load()) std::this_thread::yield();
        });
    }
    while (ready.load() != 24) std::this_thread::yield();
    start = true;
    while (finished.load() != 24) std::this_thread::yield();
    check(accepted.load() == 10);
    release = true;
    for (auto& thread : threads) thread.join();
    // The start budget remains spent even though live connections closed.
    check(gate.try_open_tcp(child, supervisor, tenant, 0).status
          == Status::rate_limited);
}

static void retired_scope_cannot_partially_commit_http_or_tcp() {
    for (int retired = 0; retired != 3; ++retired) {
        for (int http = 0; http != 2; ++http) {
            Gate gate;
            Gate::Scope child(gate, policy(), 0);
            Gate::Scope supervisor(gate, policy(), 0);
            Gate::Scope tenant(gate, policy(), 0);
            auto admitted = http
                ? gate.try_start_http(child, supervisor, tenant, 0)
                : gate.try_open_tcp(child, supervisor, tenant, 0);
            check(admitted.status == Status::admitted);
            if (retired == 0) child.close();
            else if (retired == 1) supervisor.close();
            else tenant.close();
            // Before preflight this aborted OresVM after partial commit.
            check(!admitted.lease.commit_dispatch());
            check(admitted.lease.abort_before_dispatch(0));
            check(!admitted.lease.abort_before_dispatch(0));
            admitted.lease.release();
        }
    }
}

static void concurrent_revoke_commit_has_once_only_settlement() {
    for (int round = 0; round < 64; ++round) {
        Gate gate;
        Gate::Scope child(gate, policy(), 0);
        Gate::Scope supervisor(gate, policy(), 0);
        Gate::Scope tenant(gate, policy(), 0);
        auto admitted = gate.try_start_http(child, supervisor, tenant, 0);
        check(admitted.status == Status::admitted);
        std::atomic<bool> start{false};
        std::thread closer([&] {
            while (!start.load()) std::this_thread::yield();
            supervisor.close();
        });
        start.store(true);
        const bool committed = admitted.lease.commit_dispatch();
        closer.join();
        if (!committed) check(admitted.lease.abort_before_dispatch(0));
        admitted.lease.release();
        check(!admitted.lease.commit_dispatch());
    }
}

int main() {
    hierarchical_http_rolls_back_after_supervisor_denial();
    tenant_rollback_and_once_only_pre_dispatch_cancel();
    child_fanout_cannot_bypass_shared_supervisor_tcp_cap();
    invalid_or_closed_scope_fails_closed();
    concurrent_tenant_cap_cannot_overspend();
    retired_scope_cannot_partially_commit_http_or_tcp();
    concurrent_revoke_commit_has_once_only_settlement();
    std::cout << "HIERARCHICAL_NETWORK_GATE_HOST_ONLY_PASS\n";
}
