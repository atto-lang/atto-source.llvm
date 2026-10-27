#include "oreslang/runtime/NativeTcpConnectionLinux.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>

using oreslang::runtime::ConnectDisposition;
using oreslang::runtime::EpollReadiness;
using oreslang::runtime::NativeTcpConnectionLinux;
using oreslang::runtime::NonblockingTcpListener;
using oreslang::runtime::OwnedSocketFd;
using oreslang::runtime::finish_connect_after_writable;
using oreslang::runtime::try_connect_ipv4;

static void check(bool value) {
    if (!value) std::abort();
}

static constexpr std::uint32_t loopback_host_order =
        (UINT32_C(127) << 24) | UINT32_C(1);

static OwnedSocketFd connect_to(NonblockingTcpListener& listener) {
    auto client = OwnedSocketFd::create_tcp();
    auto outcome = try_connect_ipv4(
            client.get(), loopback_host_order, listener.local_port());
    if (outcome.disposition == ConnectDisposition::in_progress) {
        EpollReadiness readiness;
        readiness.add(client.get(), EPOLLOUT, UINT64_C(0x7101));
        std::array<EpollReadiness::Ready, EpollReadiness::max_batch> events{};
        bool ready = false;
        for (int round = 0; round < 20 && !ready; ++round) {
            const std::size_t count = readiness.wait(events, 100);
            if (count == 0) continue;
            check(count == 1 && events[0].token == UINT64_C(0x7101));
            check((events[0].events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0);
            ready = true;
        }
        check(ready);
        readiness.remove(client.get());
        outcome = finish_connect_after_writable(client.get());
    }
    check(outcome.disposition == ConnectDisposition::connected);
    return client;
}

static void independent_direction_leases_and_deferred_close() {
    auto listener = NonblockingTcpListener::loopback(0, 8);
    auto client = connect_to(listener);
    auto accepted = listener.try_accept();
    for (int round = 0; accepted.would_block && round < 1000; ++round) {
        ::usleep(1000);
        accepted = listener.try_accept();
    }
    check(!accepted.would_block && accepted.error_number == 0);
    check(accepted.client.valid());

    NativeTcpConnectionLinux connection(std::move(client));
    const int physical_fd = connection.fd();
    check(physical_fd >= 0);

    NativeTcpConnectionLinux::Lease read1{};
    NativeTcpConnectionLinux::Lease write1{};
    NativeTcpConnectionLinux::Lease duplicate{};
    check(connection.acquire_read(&read1));
    check(connection.acquire_write(&write1));
    check(connection.read_leased() && connection.write_leased());
    check(!connection.acquire_read(&duplicate));
    check(!connection.acquire_write(&duplicate));

    check(connection.release(read1));
    check(!connection.release(read1));

    NativeTcpConnectionLinux::Lease read2{};
    check(connection.acquire_read(&read2));
    check(read2.direction_generation > read1.direction_generation);
    check(read2.descriptor_generation == read1.descriptor_generation);

    connection.close();
    check(connection.close_requested());
    check(!connection.retired());
    check(::fcntl(physical_fd, F_GETFD) >= 0);

    check(connection.release(write1));
    check(!connection.retired());
    check(::fcntl(physical_fd, F_GETFD) >= 0);

    check(!connection.release(read1));
    check(connection.release(read2));
    check(connection.retired());
    check(connection.fd() == -1);
    check(::fcntl(physical_fd, F_GETFD) == -1);
    check(!connection.acquire_read(&duplicate));
    check(!connection.acquire_write(&duplicate));
}

static void descriptor_generation_rejects_cross_connection_stale_token() {
    NativeTcpConnectionLinux::Lease stale{};
    std::uint64_t first_generation = 0;
    {
        auto listener = NonblockingTcpListener::loopback(0, 8);
        auto client = connect_to(listener);
        auto accepted = listener.try_accept();
        for (int round = 0; accepted.would_block && round < 1000; ++round) {
            ::usleep(1000);
            accepted = listener.try_accept();
        }
        check(accepted.client.valid());

        NativeTcpConnectionLinux first(std::move(client));
        check(first.acquire_read(&stale));
        first_generation = first.descriptor_generation();
        first.close();
        check(first.release(stale));
        check(first.retired());
    }

    auto listener = NonblockingTcpListener::loopback(0, 8);
    auto client = connect_to(listener);
    auto accepted = listener.try_accept();
    for (int round = 0; accepted.would_block && round < 1000; ++round) {
        ::usleep(1000);
        accepted = listener.try_accept();
    }
    check(accepted.client.valid());

    NativeTcpConnectionLinux second(std::move(client));
    NativeTcpConnectionLinux::Lease fresh{};
    check(second.acquire_read(&fresh));
    check(second.descriptor_generation() != first_generation);
    check(fresh.descriptor_generation != stale.descriptor_generation);
    check(!second.release(stale));
    check(second.read_leased());
    check(second.release(fresh));
    second.close();
    check(second.retired());
}

int main() {
    independent_direction_leases_and_deferred_close();
    descriptor_generation_rejects_cross_connection_stale_token();
    return 0;
}
