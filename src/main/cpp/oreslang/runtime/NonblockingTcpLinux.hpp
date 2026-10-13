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
// The reactor may park on epoll_wait; actor/ROOT carriers MUST NOT call wait().
// Token identity must be checked against actor generation before completion.
class EpollReadiness final {
public:
    static constexpr std::size_t max_batch = 32;
    struct Ready { std::uint64_t token; std::uint32_t events; };
    EpollReadiness() : epoll_(::epoll_create1(EPOLL_CLOEXEC)) {
        if (epoll_ < 0) throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
    ~EpollReadiness() noexcept { if (epoll_ >= 0) ::close(epoll_); }
    EpollReadiness(const EpollReadiness&) = delete;
    EpollReadiness& operator=(const EpollReadiness&) = delete;
    void add(int fd, std::uint32_t events, std::uint64_t token) {
        if (fd < 0 || token == 0 || !(events & (EPOLLIN | EPOLLOUT)))
            throw std::invalid_argument("invalid epoll registration");
        epoll_event e{};
        e.events = events | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
        e.data.u64 = token;
        if (::epoll_ctl(epoll_, EPOLL_CTL_ADD, fd, &e) != 0)
            throw std::system_error(errno, std::generic_category(), "epoll add");
    }
    void remove(int fd) noexcept { if (fd >= 0) ::epoll_ctl(epoll_, EPOLL_CTL_DEL, fd, nullptr); }
    std::size_t wait(std::array<Ready, max_batch>& out, int timeout_ms) {
        if (timeout_ms < -1) throw std::invalid_argument("invalid epoll timeout");
        std::array<epoll_event, max_batch> buf{};
        int n = ::epoll_wait(epoll_, buf.data(), static_cast<int>(buf.size()), timeout_ms);
        if (n < 0) {
            if (errno == EINTR) return 0;
            throw std::system_error(errno, std::generic_category(), "epoll wait");
        }
        for (int i = 0; i < n; ++i) out[i] = Ready{buf[i].data.u64, buf[i].events};
        return static_cast<std::size_t>(n);
    }
private:
    int epoll_ = -1;
};
} // namespace oreslang::runtime
