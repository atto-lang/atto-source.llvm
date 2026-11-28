#include "oreslang/runtime/NonblockingTcpLinux.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace oreslang::runtime;

static void require_fd_flags(int fd) {
    const int status = ::fcntl(fd, F_GETFL, 0);
    const int descriptor = ::fcntl(fd, F_GETFD, 0);
    assert(status >= 0);
    assert(descriptor >= 0);
    assert((status & O_NONBLOCK) != 0);
    assert((descriptor & FD_CLOEXEC) != 0);
}

int main() {
#if !defined(__linux__)
    return 0;
#else
    {
        auto socket = OwnedSocketFd::create_tcp();
        require_fd_flags(socket.get());
    }

    {
        auto listener = NonblockingTcpListener::loopback(0, 8);
        require_fd_flags(listener.fd());
        const auto accepted = listener.try_accept();
        assert(accepted.would_block);
        assert(!accepted.client.valid());
        assert(accepted.error_number == 0);
    }

    int pair[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    OwnedSocketFd left(pair[0]);
    OwnedSocketFd right(pair[1]);
    require_fd_flags(left.get());
    require_fd_flags(right.get());

    std::array<std::uint8_t, 16> buffer{};
    const auto empty = try_read(left.get(), buffer.data(), buffer.size());
    assert(empty.disposition == IoDisposition::would_block);

    const char payload[] = "ores";
    const auto sent = try_write(right.get(), payload, sizeof(payload) - 1);
    assert(sent.disposition == IoDisposition::transferred);
    assert(sent.size == sizeof(payload) - 1);

    const auto read = try_read(left.get(), buffer.data(), buffer.size());
    assert(read.disposition == IoDisposition::transferred);
    assert(read.size == sizeof(payload) - 1);
    assert(std::memcmp(buffer.data(), payload, sizeof(payload) - 1) == 0);

    EpollReadiness readiness;
    constexpr std::uint64_t token = 7;
    readiness.add(left.get(), EPOLLIN, token);

    const auto sent2 = try_write(right.get(), payload, sizeof(payload) - 1);
    assert(sent2.disposition == IoDisposition::transferred);

    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
    const auto first = readiness.wait(ready, 1000);
    assert(first == 1);
    assert(ready[0].token == token);
    assert((ready[0].events & EPOLLIN) != 0);

    // EPOLLONESHOT must suppress repeat delivery until explicit rearm.
    const auto suppressed = readiness.wait(ready, 0);
    assert(suppressed == 0);
    readiness.rearm(left.get(), EPOLLIN, token);
    const auto rearmed = readiness.wait(ready, 1000);
    assert(rearmed == 1);
    assert(ready[0].token == token);

    const auto drain = try_read(left.get(), buffer.data(), buffer.size());
    assert(drain.disposition == IoDisposition::transferred);

    // Wakeups are reactor control events and must never escape as guest readiness.
    readiness.wakeup();
    const auto user_events = readiness.wait(ready, 1000);
    assert(user_events == 0);

    // Saturating the peer's send buffer must report would_block instead of
    // parking a carrier. Partial transfers are legal and must preserve progress.
    {
        int pressure[2] = {-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pressure) == 0);
        OwnedSocketFd writer(pressure[0]);
        OwnedSocketFd stalled_reader(pressure[1]);
        int small_buffer = 4096;
        assert(::setsockopt(writer.get(), SOL_SOCKET, SO_SNDBUF,
                            &small_buffer, sizeof(small_buffer)) == 0);
        std::array<std::uint8_t, 65536> payload_bytes{};
        bool saw_would_block = false;
        std::size_t transferred = 0;
        for (int attempt = 0; attempt < 4096; ++attempt) {
            const auto result = try_write(writer.get(), payload_bytes.data(), payload_bytes.size());
            if (result.disposition == IoDisposition::transferred) {
                assert(result.size > 0);
                transferred += result.size;
                continue;
            }
            if (result.disposition == IoDisposition::would_block) {
                saw_would_block = true;
                break;
            }
            assert(false && "nonblocking saturated write returned unexpected error");
        }
        assert(transferred > 0);
        assert(saw_would_block);
    }

    // Peer retirement must become EOF on reads and an error on writes without
    // SIGPIPE or a blocking retry loop.
    {
        int eof_pair[2] = {-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, eof_pair) == 0);
        OwnedSocketFd reader(eof_pair[0]);
        OwnedSocketFd peer(eof_pair[1]);
        peer.reset();
        std::array<std::uint8_t, 8> eof_buffer{};
        const auto eof = try_read(reader.get(), eof_buffer.data(), eof_buffer.size());
        assert(eof.disposition == IoDisposition::eof);
        const char byte = 'x';
        const auto write_after_close = try_write(reader.get(), &byte, 1);
        assert(write_after_close.disposition == IoDisposition::error);
        assert(write_after_close.error_number == EPIPE
               || write_after_close.error_number == ECONNRESET);
    }

    // Removing a registration must prevent later readiness delivery for that token.
    {
        int removed_pair[2] = {-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, removed_pair) == 0);
        OwnedSocketFd watched(removed_pair[0]);
        OwnedSocketFd peer(removed_pair[1]);
        constexpr std::uint64_t removed_token = 19;
        readiness.add(watched.get(), EPOLLIN, removed_token);
        readiness.remove(watched.get());
        const char byte = 'z';
        const auto wrote = try_write(peer.get(), &byte, 1);
        assert(wrote.disposition == IoDisposition::transferred);
        assert(readiness.wait(ready, 0) == 0);
    }

    bool invalid_registration = false;
    try {
        EpollReadiness::validate_registration(left.get(), EPOLLIN, 0);
    } catch (const std::invalid_argument&) {
        invalid_registration = true;
    }
    assert(invalid_registration);

    return 0;
#endif
}
