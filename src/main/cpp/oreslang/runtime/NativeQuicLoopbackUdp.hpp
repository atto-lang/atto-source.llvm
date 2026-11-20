#pragma once
#if !defined(__linux__)
#error "NativeQuicLoopbackUdp currently supports Linux only"
#endif
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace oreslang::native_quic {

// Host-only bounded, nonblocking loopback UDP test substrate. Not a QUIC
// engine, egress authorization or guest socket API. Production networking
// requires a source-issued permit and actor-generation retirement receipts.
class NativeQuicLoopbackUdp final {
public:
    static constexpr std::size_t MaxDatagram = 1350;
    enum class Outcome { Ok, WouldBlock, Invalid, Closed, SystemError };
    struct Received {
        Outcome outcome;
        std::size_t size;
        std::uint16_t source_port;
    };

private:
    int descriptor_ = -1;
    std::uint16_t bound_port_ = 0;

public:
    NativeQuicLoopbackUdp() = default;
    NativeQuicLoopbackUdp(const NativeQuicLoopbackUdp&) = delete;
    NativeQuicLoopbackUdp& operator=(const NativeQuicLoopbackUdp&) = delete;
    NativeQuicLoopbackUdp(NativeQuicLoopbackUdp&&) = delete;
    NativeQuicLoopbackUdp& operator=(NativeQuicLoopbackUdp&&) = delete;
    ~NativeQuicLoopbackUdp() { close(); }

    bool open() noexcept {
        if (descriptor_ != -1) return false;
        const int socket_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (socket_fd < 0) return false;
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(0);
        bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(socket_fd, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
            ::close(socket_fd);
            return false;
        }
        sockaddr_in actual{};
        socklen_t actual_length = sizeof(actual);
        if (::getsockname(socket_fd, reinterpret_cast<sockaddr*>(&actual), &actual_length) != 0 ||
            actual_length != sizeof(actual) || actual.sin_family != AF_INET ||
            ntohl(actual.sin_addr.s_addr) != INADDR_LOOPBACK || ntohs(actual.sin_port) == 0) {
            ::close(socket_fd);
            return false;
        }
        descriptor_ = socket_fd;
        bound_port_ = ntohs(actual.sin_port);
        return true;
    }
    void close() noexcept {
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = -1;
        bound_port_ = 0;
    }
    std::uint16_t port() const noexcept { return bound_port_; }
    bool is_open() const noexcept { return descriptor_ >= 0; }

    Outcome send_to_loopback(std::uint16_t peer_port, const std::uint8_t* data,
                             std::size_t size) noexcept {
        if (!is_open()) return Outcome::Closed;
        if (peer_port == 0 || !data || size == 0 || size > MaxDatagram) return Outcome::Invalid;
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(peer_port);
        peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const auto bytes = ::sendto(descriptor_, data, size, MSG_DONTWAIT | MSG_NOSIGNAL,
                                    reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
        if (bytes == static_cast<ssize_t>(size)) return Outcome::Ok;
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return Outcome::WouldBlock;
        return Outcome::SystemError;
    }

    Received receive(std::uint8_t* output, std::size_t capacity) noexcept {
        if (!is_open()) return {Outcome::Closed, 0, 0};
        if (!output || capacity == 0 || capacity > MaxDatagram)
            return {Outcome::Invalid, 0, 0};
        // Stage into a fixed-capacity private buffer. Invalid/truncated packets
        // are consumed but never publish attacker-controlled prefixes into the
        // caller's output buffer.
        std::array<std::uint8_t, MaxDatagram> staged{};
        sockaddr_in sender{};
        iovec data{staged.data(), staged.size()};
        msghdr message{};
        message.msg_name = &sender;
        message.msg_namelen = sizeof(sender);
        message.msg_iov = &data;
        message.msg_iovlen = 1;
        // MSG_TRUNC returns the real UDP datagram length on Linux. Oversize
        // packets are consumed and rejected rather than accepted truncated.
        const auto bytes = ::recvmsg(descriptor_, &message, MSG_DONTWAIT | MSG_TRUNC);
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return {Outcome::WouldBlock, 0, 0};
            return {Outcome::SystemError, 0, 0};
        }
        if (message.msg_namelen != sizeof(sender) || sender.sin_family != AF_INET ||
            ntohl(sender.sin_addr.s_addr) != INADDR_LOOPBACK || ntohs(sender.sin_port) == 0 ||
            (message.msg_flags & MSG_TRUNC) ||
            static_cast<std::size_t>(bytes) > staged.size() ||
            static_cast<std::size_t>(bytes) > capacity || bytes == 0)
            return {Outcome::Invalid, 0, 0};
        std::memcpy(output, staged.data(), static_cast<std::size_t>(bytes));
        return {Outcome::Ok, static_cast<std::size_t>(bytes), ntohs(sender.sin_port)};
    }
};

} // namespace oreslang::native_quic
