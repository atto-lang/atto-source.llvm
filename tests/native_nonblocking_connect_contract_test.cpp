#include "oreslang/runtime/NonblockingTcpLinux.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>

using oreslang::runtime::ConnectDisposition;
using oreslang::runtime::EpollReadiness;
using oreslang::runtime::NonblockingTcpListener;
using oreslang::runtime::OwnedSocketFd;
using oreslang::runtime::finish_connect_after_writable;
using oreslang::runtime::try_connect_ipv4;

static void check(bool value) {
    if (!value) std::abort();
}

static constexpr std::uint32_t loopback_host_order =
        (UINT32_C(127) << 24) | UINT32_C(1);

static void require_fd_contract(int fd) {
    const int status = ::fcntl(fd, F_GETFL, 0);
    const int descriptor = ::fcntl(fd, F_GETFD, 0);
    check(status >= 0 && descriptor >= 0);
    check((status & O_NONBLOCK) != 0);
    check((descriptor & FD_CLOEXEC) != 0);
}

static oreslang::runtime::ConnectAttempt finish_if_needed(
        OwnedSocketFd& client,
        oreslang::runtime::ConnectAttempt attempt,
        std::uint64_t token) {
    if (attempt.disposition != ConnectDisposition::in_progress) return attempt;

    EpollReadiness readiness;
    readiness.add(client.get(), EPOLLOUT, token);
    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> events{};

    for (int round = 0; round < 20; ++round) {
        const std::size_t count = readiness.wait(events, 100);
        if (count == 0) continue;
        check(count == 1);
        check(events[0].token == token);
        check((events[0].events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0);
        readiness.remove(client.get());
        return finish_connect_after_writable(client.get());
    }
    return {ConnectDisposition::error, ETIMEDOUT};
}

static void successful_connect_uses_epollout_and_so_error() {
    auto listener = NonblockingTcpListener::loopback(0, 8);
    auto client = OwnedSocketFd::create_tcp();
    require_fd_contract(client.get());

    auto outcome = try_connect_ipv4(
            client.get(), loopback_host_order, listener.local_port());
    outcome = finish_if_needed(client, outcome, UINT64_C(0x5101));
    check(outcome.disposition == ConnectDisposition::connected);
    check(outcome.error_number == 0);

    auto accepted = listener.try_accept();
    for (int round = 0; accepted.would_block && round < 20; ++round) {
        ::usleep(1000);
        accepted = listener.try_accept();
    }
    check(!accepted.would_block);
    check(accepted.error_number == 0);
    check(accepted.client.valid());
    require_fd_contract(accepted.client.get());
}

static void refused_connect_finishes_as_error_and_releases_fd_by_raii() {
    std::uint16_t port = 0;
    {
        auto reservation = NonblockingTcpListener::loopback(0, 1);
        port = reservation.local_port();
    }

    auto client = OwnedSocketFd::create_tcp();
    auto outcome = try_connect_ipv4(client.get(), loopback_host_order, port);
    outcome = finish_if_needed(client, outcome, UINT64_C(0x5102));
    check(outcome.disposition == ConnectDisposition::error);
    check(outcome.error_number != 0);
    check(outcome.error_number != EINPROGRESS);
    check(outcome.error_number != EALREADY);
}

static void invalid_connect_fails_before_kernel_admission() {
    auto client = OwnedSocketFd::create_tcp();
    const auto outcome = try_connect_ipv4(
            client.get(), loopback_host_order, 0);
    check(outcome.disposition == ConnectDisposition::error);
    check(outcome.error_number == EINVAL);
}

int main() {
    successful_connect_uses_epollout_and_so_error();
    refused_connect_finishes_as_error_and_releases_fd_by_raii();
    invalid_connect_fails_before_kernel_admission();
    std::cout << "NATIVE_NONBLOCKING_CONNECT_CONTRACT_PASS\n";
    return 0;
}
