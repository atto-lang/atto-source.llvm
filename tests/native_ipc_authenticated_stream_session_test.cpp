#include "oreslang/runtime/NativeIpcAuthenticatedStreamSession.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace oreslang::runtime;

static NativeIpcWireV3::Bytes key() {
    NativeIpcWireV3::Bytes result(32u);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<unsigned char>(0x31u + i);
    return result;
}

static NativeIpcWireV3::ConnectionBinding binding() {
    NativeIpcWireV3::ConnectionBinding result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<unsigned char>(0x41u + i);
    return result;
}

static IpcReceiptEnvelope message(
        std::uint64_t sequence,
        const std::string& session,
        const NativeIpcWireV3::Bytes& payload) {
    IpcReceiptEnvelope envelope;
    envelope.domain = "tenant";
    envelope.sender = "sender";
    envelope.destination = "worker";
    envelope.session = session;
    envelope.payload_sha256 = NativeIpcWireV3::sha256_hex(payload);
    envelope.sender_incarnation = 1;
    envelope.destination_incarnation = 2;
    envelope.route_epoch = 3;
    envelope.capability_generation = 4;
    envelope.message_sequence = sequence;
    envelope.frame_sequence = sequence;
    return envelope;
}

static void write_all(int fd, const unsigned char* bytes, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::send(
            fd, bytes + offset, size - offset, MSG_NOSIGNAL);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        _exit(90);
    }
}

static void read_all(int fd, unsigned char* bytes, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::recv(fd, bytes + offset, size - offset, 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        _exit(91);
    }
}

static std::uint32_t read_u32(int fd) {
    std::array<unsigned char, 4> prefix{};
    read_all(fd, prefix.data(), prefix.size());
    return (static_cast<std::uint32_t>(prefix[0]) << 24u) |
           (static_cast<std::uint32_t>(prefix[1]) << 16u) |
           (static_cast<std::uint32_t>(prefix[2]) << 8u) |
           static_cast<std::uint32_t>(prefix[3]);
}

static void write_frame(int fd, const NativeIpcWireV3::Bytes& frame) {
    assert(frame.size() <= std::numeric_limits<std::uint32_t>::max());
    const auto size = static_cast<std::uint32_t>(frame.size());
    const std::array<unsigned char, 4> prefix{
        static_cast<unsigned char>((size >> 24u) & 0xffu),
        static_cast<unsigned char>((size >> 16u) & 0xffu),
        static_cast<unsigned char>((size >> 8u) & 0xffu),
        static_cast<unsigned char>(size & 0xffu)
    };
    write_all(fd, prefix.data(), prefix.size());
    write_all(fd, frame.data(), frame.size());
}

static NativeIpcWireV3::Bytes read_frame(int fd) {
    const auto size = read_u32(fd);
    if (size < NativeIpcWireV3::mac_bytes ||
        size > NativeIpcWireV3::max_data_frame_bytes)
        _exit(92);
    NativeIpcWireV3::Bytes frame(static_cast<std::size_t>(size));
    read_all(fd, frame.data(), frame.size());
    return frame;
}

static void wait_child(pid_t child) {
    int status = 0;
    for (int i = 0; i < 500; ++i) {
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            assert(WIFEXITED(status));
            assert(WEXITSTATUS(status) == 0);
            return;
        }
        if (result < 0 && errno != EINTR)
            break;
        ::usleep(10000);
    }
    (void)::kill(child, SIGKILL);
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    assert(false && "forked authenticated IPC peer exceeded deadline");
}

static void flush_until_complete(
        NativeIpcAuthenticatedStreamSession& session) {
    for (int i = 0; i < 100000; ++i) {
        if (!session.has_pending_write())
            return;
        const auto status = session.flush_nonblocking();
        assert(status ==
            NativeIpcAuthenticatedStreamSession::SendStatus::complete ||
            status ==
            NativeIpcAuthenticatedStreamSession::SendStatus::would_block);
        std::this_thread::yield();
    }
    assert(false && "authenticated IPC write did not drain");
}

static IpcReceiptStage poll_until_receipt(
        NativeIpcAuthenticatedStreamSession& session) {
    for (int i = 0; i < 100000; ++i) {
        auto stage = session.poll_receipt_nonblocking();
        if (stage.has_value())
            return *stage;
        assert(session.phase() ==
            NativeIpcAuthenticatedStreamSession::Phase::open);
        std::this_thread::yield();
    }
    throw std::runtime_error("authenticated IPC receipt did not arrive");
}

static void success_case() {
    int fds[2]{-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);

    const NativeIpcWireV3::Bytes payload{
        'n','a','t','i','v','e','-','v','3'};
    const auto envelope = message(1, "nativeSuccess", payload);
    const auto mac_key = key();
    const auto connection_binding = binding();
    constexpr std::uint64_t generation = 91;

    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        ::close(fds[0]);
        auto frame = read_frame(fds[1]);
        NativeIpcWireV3::InboundReplayGate replay;
        const auto decoded = NativeIpcWireV3::decode_data_frame(
            frame, envelope, generation, replay,
            connection_binding, mac_key);
        if (!(decoded.envelope == envelope) || decoded.payload != payload)
            _exit(93);

        write_frame(fds[1], NativeIpcWireV3::encode_receipt_frame(
            envelope, generation, IpcReceiptStage::remote_accepted,
            connection_binding, mac_key));
        write_frame(fds[1], NativeIpcWireV3::encode_receipt_frame(
            envelope, generation, IpcReceiptStage::handler_committed,
            connection_binding, mac_key));
        ::close(fds[1]);
        _exit(0);
    }

    ::close(fds[1]);
    NativeIpcAuthenticatedStreamSession session(
        OwnedSocketFd{fds[0]}, envelope, generation, 4,
        mac_key, connection_binding);
    const auto initial = session.send_nonblocking(envelope, payload);
    assert(initial ==
        NativeIpcAuthenticatedStreamSession::SendStatus::complete ||
        initial ==
        NativeIpcAuthenticatedStreamSession::SendStatus::would_block);
    flush_until_complete(session);
    assert(poll_until_receipt(session) == IpcReceiptStage::remote_accepted);
    assert(poll_until_receipt(session) == IpcReceiptStage::handler_committed);
    assert(session.counters() ==
        (std::pair<std::size_t, std::size_t>{0, 0}));
    assert(session.invariant());
    session.close_and_retire();
    assert(session.phase() ==
        NativeIpcAuthenticatedStreamSession::Phase::retired);
    assert(session.take_losses().empty());
    assert(session.key_zeroized());
    wait_child(child);
}

static void stale_generation_case() {
    int fds[2]{-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);

    const NativeIpcWireV3::Bytes payload{
        's','t','a','l','e'};
    const auto envelope = message(1, "nativeStale", payload);
    const auto mac_key = key();
    const auto connection_binding = binding();
    constexpr std::uint64_t generation = 92;

    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        ::close(fds[0]);
        auto frame = read_frame(fds[1]);
        NativeIpcWireV3::InboundReplayGate replay;
        (void)NativeIpcWireV3::decode_data_frame(
            frame, envelope, generation, replay,
            connection_binding, mac_key);
        write_frame(fds[1], NativeIpcWireV3::encode_receipt_frame(
            envelope, generation - 1u,
            IpcReceiptStage::remote_accepted,
            connection_binding, mac_key));
        ::close(fds[1]);
        _exit(0);
    }

    ::close(fds[1]);
    NativeIpcAuthenticatedStreamSession session(
        OwnedSocketFd{fds[0]}, envelope, generation, 2,
        mac_key, connection_binding);
    const auto initial = session.send_nonblocking(envelope, payload);
    assert(initial ==
        NativeIpcAuthenticatedStreamSession::SendStatus::complete ||
        initial ==
        NativeIpcAuthenticatedStreamSession::SendStatus::would_block);
    flush_until_complete(session);

    bool rejected = false;
    for (int i = 0; i < 100000 && !rejected; ++i) {
        try {
            (void)session.poll_receipt_nonblocking();
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        if (!rejected)
            std::this_thread::yield();
    }
    assert(rejected);
    assert(session.phase() ==
        NativeIpcAuthenticatedStreamSession::Phase::retired);
    auto losses = session.take_losses();
    assert(losses.size() == 1);
    assert(losses[0].envelope == envelope);
    assert(losses[0].last_observed ==
        IpcReceiptStage::dispatch_committed);
    assert(session.counters() ==
        (std::pair<std::size_t, std::size_t>{0, 0}));
    assert(session.key_zeroized());
    assert(session.invariant());
    wait_child(child);
}

static void oversized_receipt_case() {
    int fds[2]{-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);

    const NativeIpcWireV3::Bytes payload{'x'};
    const auto envelope = message(1, "oversizedReceipt", payload);
    const auto mac_key = key();
    const auto connection_binding = binding();

    NativeIpcAuthenticatedStreamSession session(
        OwnedSocketFd{fds[0]}, envelope, 93, 1,
        mac_key, connection_binding);

    const std::uint32_t oversized =
        static_cast<std::uint32_t>(
            NativeIpcWireV3::max_receipt_frame_bytes + 1u);
    const std::array<unsigned char, 4> prefix{
        static_cast<unsigned char>((oversized >> 24u) & 0xffu),
        static_cast<unsigned char>((oversized >> 16u) & 0xffu),
        static_cast<unsigned char>((oversized >> 8u) & 0xffu),
        static_cast<unsigned char>(oversized & 0xffu)
    };
    write_all(fds[1], prefix.data(), prefix.size());

    bool rejected = false;
    try {
        (void)session.poll_receipt_nonblocking();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    assert(session.phase() ==
        NativeIpcAuthenticatedStreamSession::Phase::retired);
    assert(session.take_losses().empty());
    assert(session.key_zeroized());
    ::close(fds[1]);
}

static void pressured_write_case() {
    int fds[2]{-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    int small = 1024;
    assert(::setsockopt(
        fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);

    NativeIpcWireV3::Bytes payload(
        NativeIpcWireV3::max_payload_bytes, 0x5au);
    const auto first = message(1, "pressuredWrite", payload);
    auto second = first;
    second.message_sequence = 2;
    second.frame_sequence = 2;
    const auto mac_key = key();
    const auto connection_binding = binding();

    NativeIpcAuthenticatedStreamSession session(
        OwnedSocketFd{fds[0]}, first, 94, 4,
        mac_key, connection_binding);
    const auto first_status = session.send_nonblocking(first, payload);
    assert(first_status ==
        NativeIpcAuthenticatedStreamSession::SendStatus::would_block);
    assert(session.has_pending_write());
    assert(session.counters() ==
        (std::pair<std::size_t, std::size_t>{1, 1}));

    // Busy is rejected before any second receipt admission or dispatch commit.
    assert(session.send_nonblocking(second, payload) ==
        NativeIpcAuthenticatedStreamSession::SendStatus::busy);
    assert(session.counters() ==
        (std::pair<std::size_t, std::size_t>{1, 1}));

    session.close_and_retire();
    assert(session.phase() ==
        NativeIpcAuthenticatedStreamSession::Phase::retired);
    auto losses = session.take_losses();
    assert(losses.size() == 1);
    assert(losses[0].envelope == first);
    assert(losses[0].last_observed ==
        IpcReceiptStage::dispatch_committed);
    assert(session.counters() ==
        (std::pair<std::size_t, std::size_t>{0, 0}));
    assert(session.key_zeroized());
    assert(session.invariant());
    ::close(fds[1]);
}

int main() {
    success_case();
    stale_generation_case();
    oversized_receipt_case();
    pressured_write_case();
    std::cout
        << "native-ipc-authenticated-stream-session PASS "
        << "(nonblocking, forked-process, v3 authenticated)\n";
}
