#pragma once
// Dedicated I/O driver only. Kernel notifications carry no actor messages or FDs.
// Actor DATA remains in the in-process mailbox; completions in a bounded memory ring.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/event.h>
#elif defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#else
#error "Native HTTP readiness requires kqueue (macOS) or epoll (Linux)"
#endif
namespace oreslang::runtime {
class NativeSocketReadiness final {
public:
    struct Event { std::uint64_t token; bool readable, writable, error; };
    static constexpr std::size_t batch = 256;
    NativeSocketReadiness() {
#if defined(__APPLE__)
        fd_ = ::kqueue();
        if (fd_ < 0) fail("kqueue");
        struct kevent event;
        EV_SET(&event, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        if (::kevent(fd_, &event, 1, nullptr, 0, nullptr) < 0) { ::close(fd_); fail("kqueue user event"); }
#else
        fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (fd_ < 0) fail("epoll_create1");
        wake_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_ < 0) { ::close(fd_); fail("eventfd"); }
        epoll_event event{}; event.events = EPOLLIN; event.data.u64 = 0;
        if (::epoll_ctl(fd_, EPOLL_CTL_ADD, wake_, &event) < 0) {
            ::close(wake_); ::close(fd_); fail("epoll wake registration");
        }
#endif
    }
    ~NativeSocketReadiness() {
#if defined(__linux__)
        ::close(wake_);
#endif
        ::close(fd_);
    }
    NativeSocketReadiness(const NativeSocketReadiness&) = delete;
    NativeSocketReadiness& operator=(const NativeSocketReadiness&) = delete;
    void set(int socket, std::uint64_t token, bool read, bool write, bool added) {
        if (!token) throw std::invalid_argument("reserved readiness token");
#if defined(__APPLE__)
        (void)added;
        struct kevent changes[2];
        EV_SET(&changes[0], socket, EVFILT_READ, EV_ADD | (read ? EV_ENABLE : EV_DISABLE),
               0, 0, reinterpret_cast<void*>(static_cast<std::uintptr_t>(token)));
        EV_SET(&changes[1], socket, EVFILT_WRITE, EV_ADD | (write ? EV_ENABLE : EV_DISABLE),
               0, 0, reinterpret_cast<void*>(static_cast<std::uintptr_t>(token)));
        if (::kevent(fd_, changes, 2, nullptr, 0, nullptr) < 0) fail("kevent set");
#else
        epoll_event event{};
        event.events = (read ? EPOLLIN : 0u) | (write ? EPOLLOUT : 0u);
        event.data.u64 = token;
        if (::epoll_ctl(fd_, added ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, socket, &event) < 0) fail("epoll set");
#endif
    }
    // close(socket) removes kernel registration; generation-tagged events already
    // returned by wait() are still checked by the reactor before touching a slot.
    void wake() noexcept {
#if defined(__APPLE__)
        struct kevent event;
        EV_SET(&event, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        while (::kevent(fd_, &event, 1, nullptr, 0, nullptr) < 0 && errno == EINTR) {}
#else
        const std::uint64_t one = 1;
        while (::write(wake_, &one, sizeof(one)) < 0 && errno == EINTR) {}
#endif
    }
    std::size_t wait(std::array<Event, batch>& output, int timeout_ms) {
        if (timeout_ms < 0) throw std::invalid_argument("finite reactor deadline required");
        std::size_t count = 0;
#if defined(__APPLE__)
        std::array<struct kevent, batch> events{};
        timespec timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
        int n = ::kevent(fd_, nullptr, 0, events.data(), static_cast<int>(batch), &timeout);
        if (n < 0) { if (errno == EINTR) return 0; fail("kevent wait"); }
        for (int i = 0; i < n; ++i) {
            const auto& e = events[i];
            if (e.filter == EVFILT_USER) continue;
            output[count++] = {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(e.udata)),
                e.filter == EVFILT_READ, e.filter == EVFILT_WRITE, (e.flags & EV_ERROR) != 0};
        }
#else
        std::array<epoll_event, batch> events{};
        int n = ::epoll_wait(fd_, events.data(), static_cast<int>(batch), timeout_ms);
        if (n < 0) { if (errno == EINTR) return 0; fail("epoll_wait"); }
        for (int i = 0; i < n; ++i) {
            const auto& e = events[i];
            if (!e.data.u64) { std::uint64_t value; while (::read(wake_, &value, sizeof(value)) > 0) {} continue; }
            output[count++] = {e.data.u64, (e.events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP)) != 0,
                (e.events & EPOLLOUT) != 0, (e.events & EPOLLERR) != 0};
        }
#endif
        return count;
    }
private:
    [[noreturn]] static void fail(const char* op) { throw std::system_error(errno, std::generic_category(), op); }
    int fd_ = -1;
#if defined(__linux__)
    int wake_ = -1;
#endif
};
} // namespace oreslang::runtime
