#include "oreslang/runtime/TrustedUntrustedNetworkQuota.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

using oreslang::runtime::TrustedUntrustedNetworkQuota;
using Status = TrustedUntrustedNetworkQuota::Status;
static void check(bool okay) { if (!okay) std::abort(); }

static void http_in_flight_is_distinct_from_start_rate() {
    TrustedUntrustedNetworkQuota quota({}, 0);
    std::vector<TrustedUntrustedNetworkQuota::Lease> pending;
    for (std::uint64_t i = 0; i < 10; ++i) {
        auto admitted = quota.try_start_http(i * 100000000ULL);
        check(admitted.status == Status::admitted);
        pending.emplace_back(std::move(admitted.lease));
    }
    check(quota.http_in_flight() == 10);
    check(quota.try_start_http(900000000ULL).status == Status::too_many_in_flight);
    pending[0].release();
    check(quota.try_start_http(900000000ULL).status == Status::admitted);
    // The temporary Admission's lease was released by its destructor.
    check(quota.http_in_flight() == 9);
    for (auto& lease : pending) lease.release();
    check(quota.http_in_flight() == 0);

    auto first = quota.try_start_http(900000000ULL);
    check(first.status == Status::rate_limited || first.status == Status::admitted);
    if (first.status == Status::admitted) first.lease.release();
}

static void request_rate_and_failed_connect_release() {
    TrustedUntrustedNetworkQuota quota({}, 0);
    auto a = quota.try_start_http(0);
    auto b = quota.try_start_http(0);
    check(a.status == Status::admitted && b.status == Status::admitted);
    check(quota.try_start_http(0).status == Status::rate_limited);
    a.lease.release();
    b.lease.release();
    check(quota.try_start_http(0).status == Status::rate_limited);
    auto c = quota.try_start_http(100000000);
    check(c.status == Status::admitted);
    c.lease.release();

    std::vector<TrustedUntrustedNetworkQuota::Lease> sockets;
    for (int i = 0; i < 10; ++i) {
        auto s = quota.try_open_tcp(static_cast<std::uint64_t>(i) * 100000000ULL);
        check(s.status == Status::admitted);
        sockets.emplace_back(std::move(s.lease));
    }
    check(quota.try_open_tcp(900000000ULL).status == Status::too_many_connections);
    sockets[0].release();
    sockets[0].release();
    auto failed = quota.try_open_tcp(900000000ULL);
    check(failed.status == Status::admitted);
    failed.lease.release();
    for (auto& s : sockets) s.release();
    check(quota.tcp_connections() == 0);
}

static void concurrent_tcp_open_is_bounded() {
    TrustedUntrustedNetworkQuota::Policy policy{};
    policy.tcp_burst = 10; // isolate simultaneous socket cap from dial rate
    TrustedUntrustedNetworkQuota quota(policy, 0);
    std::atomic<bool> start{false}, release{false};
    std::atomic<int> ready{0}, finished{0}, admitted{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 24; ++i) {
        threads.emplace_back([&] {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            auto attempt = quota.try_open_tcp(0);
            if (attempt.status == Status::admitted) ++admitted;
            ++finished;
            while (!release.load()) std::this_thread::yield();
        });
    }
    while (ready.load() != 24) std::this_thread::yield();
    start = true;
    while (finished.load() != 24) std::this_thread::yield();
    check(admitted.load() == 10);
    check(quota.tcp_connections() == 10);
    quota.close();
    check(quota.try_open_tcp(0).status == Status::closed);
    release = true;
    for (auto& t : threads) t.join();
    check(quota.tcp_connections() == 0);
}

static void connection_attempt_churn_is_rate_limited() {
    TrustedUntrustedNetworkQuota quota({}, 0);
    auto a = quota.try_open_tcp(0);
    auto b = quota.try_open_tcp(0);
    check(a.status == Status::admitted && b.status == Status::admitted);
    a.lease.release();
    b.lease.release(); // failures still spend dial tokens
    check(quota.tcp_connections() == 0);
    check(quota.try_open_tcp(0).status == Status::rate_limited);
    auto c = quota.try_open_tcp(100000000ULL);
    check(c.status == Status::admitted);
    c.lease.release();
}

static void published_cancellation_never_refunds_rate_tokens() {
    TrustedUntrustedNetworkQuota quota({}, 0);
    auto http = quota.try_start_http(0);
    check(http.status == Status::admitted);
    check(http.lease.abort_before_dispatch(0));
    check(!http.lease.abort_before_dispatch(0));
    check(!http.lease.commit_dispatch());
    http.lease.release();
    check(quota.http_in_flight() == 0);
    auto first = quota.try_start_http(0);
    check(first.status == Status::admitted);
    check(quota.try_start_http(0).status == Status::rate_limited);
    check(first.lease.commit_dispatch());
    check(!first.lease.abort_before_dispatch(0));
    first.lease.release();

    auto tcp = quota.try_open_tcp(0);
    check(tcp.status == Status::admitted);
    check(tcp.lease.abort_before_dispatch(0));
    check(!tcp.lease.abort_before_dispatch(0));
    check(quota.tcp_connections() == 0);
}

static void delayed_cancellation_cannot_inflate_future_burst() {
    TrustedUntrustedNetworkQuota quota({}, 0);
    auto a = quota.try_start_http(0);
    auto b = quota.try_start_http(0);
    check(a.status == Status::admitted && b.status == Status::admitted);
    auto c = quota.try_start_http(1000000000ULL);
    auto d = quota.try_start_http(1000000000ULL);
    check(c.status == Status::admitted && d.status == Status::admitted);
    check(a.lease.abort_before_dispatch(1000000000ULL));
    check(b.lease.abort_before_dispatch(1000000000ULL));
    check(quota.try_start_http(1000000000ULL).status == Status::rate_limited);
    c.lease.release();
    d.lease.release();
    check(quota.http_in_flight() == 0);

    auto t1 = quota.try_open_tcp(1000000000ULL);
    auto t2 = quota.try_open_tcp(1000000000ULL);
    check(t1.status == Status::admitted && t2.status == Status::admitted);
    auto t3 = quota.try_open_tcp(2000000000ULL);
    auto t4 = quota.try_open_tcp(2000000000ULL);
    check(t3.status == Status::admitted && t4.status == Status::admitted);
    check(t1.lease.abort_before_dispatch(2000000000ULL));
    check(t2.lease.abort_before_dispatch(2000000000ULL));
    check(quota.try_open_tcp(2000000000ULL).status == Status::rate_limited);
    t3.lease.release();
    t4.lease.release();
    check(quota.tcp_connections() == 0);
}

static void revoked_quota_rejects_undispatched_http_and_tcp() {
    TrustedUntrustedNetworkQuota http({}, 0);
    auto h = http.try_start_http(0);
    check(h.status == Status::admitted);
    http.close();
    check(!h.lease.commit_dispatch());
    check(h.lease.abort_before_dispatch(0));
    check(!h.lease.abort_before_dispatch(0));
    check(http.http_in_flight() == 0);
    check(http.try_start_http(0).status == Status::closed);

    TrustedUntrustedNetworkQuota tcp({}, 0);
    auto t = tcp.try_open_tcp(0);
    check(t.status == Status::admitted);
    tcp.close();
    check(!t.lease.commit_dispatch());
    check(t.lease.abort_before_dispatch(0));
    check(tcp.tcp_connections() == 0);
}

static void invalid_policy_fails_closed() {
    TrustedUntrustedNetworkQuota::Policy p;
    p.max_tcp_connections = 0;
    TrustedUntrustedNetworkQuota invalid(p, 0);
    check(invalid.try_open_tcp(0).status == Status::invalid_policy);
    check(invalid.try_start_http(0).status == Status::invalid_policy);
}

int main() {
    http_in_flight_is_distinct_from_start_rate();
    request_rate_and_failed_connect_release();
    concurrent_tcp_open_is_bounded();
    connection_attempt_churn_is_rate_limited();
    published_cancellation_never_refunds_rate_tokens();
    delayed_cancellation_cannot_inflate_future_burst();
    revoked_quota_rejects_undispatched_http_and_tcp();
    invalid_policy_fails_closed();
    std::cout << "UNTRUSTED_NETWORK_QUOTA_HOST_ONLY_PASS\n";
}
