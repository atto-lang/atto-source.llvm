#pragma once
// Linux-only kernel boundary: NOT Oreslang std/net, HTTP, actor I/O or an async continuation.
#if !defined(__linux__)
#error "NonblockingTcpLinux is Linux-only"
#endif
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <limits>
#include <sys/socket.h>
#include <unistd.h>
namespace oreslang::runtime {
class OwnedSocketFd final {
public:
    explicit OwnedSocketFd(int fd = -1) noexcept : fd_(fd) {}
    ~OwnedSocketFd() noexcept { reset(); }
    OwnedSocketFd(const OwnedSocketFd&) = delete;
    OwnedSocketFd& operator=(const OwnedSocketFd&) = delete;
    OwnedSocketFd(OwnedSocketFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    OwnedSocketFd& operator=(OwnedSocketFd&& other) noexcept {
        if (this != &other) { reset(); fd_ = std::exchange(other.fd_, -1); }
        return *this;
    }
    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    void reset() noexcept { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
    static OwnedSocketFd create_tcp() {
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "socket nonblocking");
        return OwnedSocketFd(fd);
    }
private:
    int fd_ = -1;
};
enum class IoDisposition : std::uint8_t { transferred, would_block, eof, error };
struct IoAttempt { IoDisposition disposition; std::size_t size; int error_number; };
inline IoAttempt try_read(int fd, void* bytes, std::size_t capacity) noexcept {
    if (fd < 0 || bytes == nullptr || capacity == 0) return {IoDisposition::error, 0, EINVAL};
    const ssize_t n = ::recv(fd, bytes, capacity, MSG_DONTWAIT);
    if (n > 0) return {IoDisposition::transferred, static_cast<std::size_t>(n), 0};
    if (n == 0) return {IoDisposition::eof, 0, 0};
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return {IoDisposition::would_block, 0, 0};
    return {IoDisposition::error, 0, errno};
}
inline IoAttempt try_write(int fd, const void* bytes, std::size_t size) noexcept {
    if (fd < 0 || bytes == nullptr || size == 0) return {IoDisposition::error, 0, EINVAL};
    const ssize_t n = ::send(fd, bytes, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n > 0) return {IoDisposition::transferred, static_cast<std::size_t>(n), 0};
    if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return {IoDisposition::would_block, 0, 0};
    return {IoDisposition::error, 0, errno};
}
struct TryAccept { OwnedSocketFd client; bool would_block; int error_number; };
class NonblockingTcpListener final {
public:
    static NonblockingTcpListener loopback(std::uint16_t port, int backlog) {
        if (backlog < 1 || backlog > 65535) throw std::invalid_argument("invalid listen backlog");
        auto s = OwnedSocketFd::create_tcp();
        int reuse = 1;
        if (::setsockopt(s.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0)
            throw std::system_error(errno, std::generic_category(), "SO_REUSEADDR");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        address.sin_port = ::htons(port);
        if (::bind(s.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            throw std::system_error(errno, std::generic_category(), "bind");
        if (::listen(s.get(), backlog) != 0)
            throw std::system_error(errno, std::generic_category(), "listen");
        return NonblockingTcpListener(std::move(s));
    }
    int fd() const noexcept { return listener_.get(); }
    std::uint16_t local_port() const {
        sockaddr_in address{};
        socklen_t n = sizeof(address);
        if (::getsockname(fd(), reinterpret_cast<sockaddr*>(&address), &n) != 0)
            throw std::system_error(errno, std::generic_category(), "getsockname");
        return ::ntohs(address.sin_port);
    }
    TryAccept try_accept() const noexcept {
        const int fd = ::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) return {OwnedSocketFd(fd), false, 0};
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return {OwnedSocketFd{}, true, 0};
        return {OwnedSocketFd{}, false, errno};
    }
private:
    explicit NonblockingTcpListener(OwnedSocketFd s) noexcept : listener_(std::move(s)) {}
    OwnedSocketFd listener_;
};
// A dedicated reactor thread may park on epoll_wait. A host/actor carrier
// submits nonblocking commands and calls wakeup(), never wait().
// User registrations are one-shot so an unconsumed OP_READ/EPOLLIN cannot
// create a hot event loop. Reactor re-arms only after processing a request.
// Stable generation IDs must be validated by the owning completion gate.
class EpollReadiness final {
public:
    static constexpr std::size_t max_batch = 32;
    static constexpr std::uint64_t wake_token = std::numeric_limits<std::uint64_t>::max();
    struct Ready { std::uint64_t token; std::uint32_t events; };

    EpollReadiness() {
        epoll_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_ < 0) throw std::system_error(errno, std::generic_category(), "epoll_create1");
        wake_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_ < 0) {
            const int e = errno;
            ::close(epoll_);
            epoll_ = -1;
            throw std::system_error(e, std::generic_category(), "eventfd");
        }
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.u64 = wake_token;
        if (::epoll_ctl(epoll_, EPOLL_CTL_ADD, wake_, &event) != 0) {
            const int e = errno;
            ::close(wake_);
            ::close(epoll_);
            wake_ = -1;
            epoll_ = -1;
            throw std::system_error(e, std::generic_category(), "epoll add wakeup");
        }
    }

    ~EpollReadiness() noexcept {
        if (wake_ >= 0) ::close(wake_);
        if (epoll_ >= 0) ::close(epoll_);
    }
    EpollReadiness(const EpollReadiness&) = delete;
    EpollReadiness& operator=(const EpollReadiness&) = delete;

    static void validate_registration(int fd, std::uint32_t events, std::uint64_t token) {
        if (fd < 0 || token == 0 || token == wake_token
                || !(events & (EPOLLIN | EPOLLOUT))
                || (events & ~(static_cast<std::uint32_t>(EPOLLIN | EPOLLOUT))) != 0)
            throw std::invalid_argument("invalid epoll registration");
    }
    void add(int fd, std::uint32_t events, std::uint64_t token) {
        validate_registration(fd, events, token);
        epoll_event event{};
        event.events = events | EPOLLONESHOT | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
        event.data.u64 = token;
        if (::epoll_ctl(epoll_, EPOLL_CTL_ADD, fd, &event) != 0)
            throw std::system_error(errno, std::generic_category(), "epoll add");
    }
    void rearm(int fd, std::uint32_t events, std::uint64_t token) {
        validate_registration(fd, events, token);
        epoll_event event{};
        event.events = events | EPOLLONESHOT | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
        event.data.u64 = token;
        if (::epoll_ctl(epoll_, EPOLL_CTL_MOD, fd, &event) != 0)
            throw std::system_error(errno, std::generic_category(), "epoll rearm");
    }
    void remove(int fd) noexcept {
        if (fd >= 0) ::epoll_ctl(epoll_, EPOLL_CTL_DEL, fd, nullptr);
    }

    // Cross-thread wakeup is a nonblocking eventfd write; the reactor consumes
    // wake notifications internally and returns zero user events when no socket
    // is ready. Never let the reserved token escape to guest callbacks.
    void wakeup() {
        const std::uint64_t one = 1;
        for (;;) {
            if (::write(wake_, &one, sizeof(one)) == static_cast<ssize_t>(sizeof(one))) return;
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return; // counter already readable
            throw std::system_error(errno, std::generic_category(), "reactor wakeup");
        }
    }
    std::size_t wait(std::array<Ready, max_batch>& out, int timeout_ms) {
        if (timeout_ms < -1) throw std::invalid_argument("invalid epoll timeout");
        std::array<epoll_event, max_batch> buf{};
        int n = ::epoll_wait(epoll_, buf.data(), static_cast<int>(buf.size()), timeout_ms);
        if (n < 0) {
            if (errno == EINTR) return 0;
            throw std::system_error(errno, std::generic_category(), "epoll wait");
        }
        std::size_t accepted = 0;
        for (int i = 0; i < n; ++i) {
            if (buf[i].data.u64 == wake_token) {
                std::uint64_t drained = 0;
                while (::read(wake_, &drained, sizeof(drained)) == sizeof(drained)) { }
                continue;
            }
            out[accepted++] = Ready{buf[i].data.u64, buf[i].events};
        }
        return accepted;
    }
private:
    int epoll_ = -1;
    int wake_ = -1;
};
} // namespace oreslang::runtime
