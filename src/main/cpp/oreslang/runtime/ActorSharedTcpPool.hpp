#pragma once
// Trusted host-only TCP connection pool shared by many actors.
//
// Safety model (what keeps one actor from disturbing another):
//  * Actors never see a socket FD, pointer, or buffer owned by the pool. They
//    hold a PoolLease: plain DATA {slot, generation, actor identity}.
//  * Every operation re-checks the lease against the slot under the pool
//    mutex: same generation, same owning actor id AND incarnation. A lease
//    forwarded through a mailbox, kept after release, or held by a dead actor
//    incarnation is rejected.
//  * Every lease transition bumps the slot generation, so stale leases can
//    never alias a later lease of the same physical connection.
//  * The FD is closed only when no I/O is in flight on it (busy flag). That
//    prevents the classic FD-reuse bug where a late write from actor A lands
//    on a new connection that the kernel gave the same FD number for actor B.
//  * Connections are partitioned by PoolOrigin, which includes the tenant, so
//    a physical connection never crosses a tenant boundary.
//  * A connection returns to the idle set only if the protocol layer says the
//    exchange ended cleanly. Idle connections are probed before reuse; any
//    pending byte or EOF discards the connection.
//  * Raw FD access requires PoolIoKey, which only ActorHttpAgent can mint, so
//    actor code cannot write arbitrary bytes to a shared connection.
//
// `PoolActor` must come from the scheduler's authenticated current-actor
// identity, never from guest-supplied data.
//
// This is a mutex-based POSIX reference implementation, NOT a hard-real-time
// path: connect/probe use syscalls, and responses allocate.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace oreslang::runtime {

struct PoolActor {
    std::uint64_t id = 0;
    std::uint64_t incarnation = 0;
    friend bool operator==(const PoolActor&, const PoolActor&) = default;
};

// DNS resolution is a separate trusted step; the pool only sees numeric IPv4.
struct PoolOrigin {
    std::uint64_t tenant = 0;
    std::uint32_t ipv4_host_order = 0;
    std::uint16_t port = 0;
    friend bool operator==(const PoolOrigin&, const PoolOrigin&) = default;
};

struct PoolLease {
    std::uint32_t slot = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t generation = 0;
    PoolActor actor{};
    bool reused = false;  // true when the physical connection served an earlier lease
};

enum class PoolStatus : std::uint8_t {
    ok,
    busy,            // an operation is already in flight on this lease
    limit_actor,     // caller holds max_per_actor leases
    limit_origin,    // origin already has max_per_origin connections
    pool_full,       // no free slot and nothing idle to evict
    connect_failed,
    invalid_lease,   // stale, released, revoked, or forged
    wrong_actor,     // lease belongs to another actor or incarnation
    poisoned,        // connection failed earlier in this lease; release it
    io_error,
    timeout,
    protocol_error,
    too_large,
    peer_closed,
    revoked,
    shut_down,
    bad_request,
};

inline const char* to_string(PoolStatus s) noexcept {
    switch (s) {
        case PoolStatus::ok: return "ok";
        case PoolStatus::busy: return "busy";
        case PoolStatus::limit_actor: return "limit_actor";
        case PoolStatus::limit_origin: return "limit_origin";
        case PoolStatus::pool_full: return "pool_full";
        case PoolStatus::connect_failed: return "connect_failed";
        case PoolStatus::invalid_lease: return "invalid_lease";
        case PoolStatus::wrong_actor: return "wrong_actor";
        case PoolStatus::poisoned: return "poisoned";
        case PoolStatus::io_error: return "io_error";
        case PoolStatus::timeout: return "timeout";
        case PoolStatus::protocol_error: return "protocol_error";
        case PoolStatus::too_large: return "too_large";
        case PoolStatus::peer_closed: return "peer_closed";
        case PoolStatus::revoked: return "revoked";
        case PoolStatus::shut_down: return "shut_down";
        case PoolStatus::bad_request: return "bad_request";
    }
    return "unknown";
}

struct PoolConfig {
    std::uint32_t max_connections = 64;  // fixed slot table; bounded FDs
    std::uint32_t max_per_origin = 8;
    std::uint32_t max_per_actor = 4;
    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_response_bytes = 1024 * 1024;
    int connect_timeout_ms = 1000;
    int io_timeout_ms = 2000;
    int idle_timeout_ms = 30000;
};

struct PoolStats {
    std::uint64_t connects = 0;
    std::uint64_t reuses = 0;
    std::uint64_t discarded_on_probe = 0;
    std::uint64_t closed_unclean = 0;
    std::uint64_t revoked = 0;
    std::uint32_t idle = 0;
    std::uint32_t leased = 0;
    std::uint32_t closing = 0;
};

class ActorHttpAgent;

// Passkey: only the protocol layer may touch a leased FD.
class PoolIoKey final {
    friend class ActorHttpAgent;
    PoolIoKey() = default;
};

class ActorSharedTcpPool final {
public:
    explicit ActorSharedTcpPool(PoolConfig config) : config_(config), slots_(config.max_connections) {}
    ActorSharedTcpPool(const ActorSharedTcpPool&) = delete;
    ActorSharedTcpPool& operator=(const ActorSharedTcpPool&) = delete;
    // Waits for in-flight I/O to finish so no FD is closed under a reader.
    ~ActorSharedTcpPool() {
        shutdown();
        std::unique_lock<std::mutex> lock(mutex_);
        drained_.wait(lock, [&] { return busy_ == 0; });
        for (auto& s : slots_) close_locked(s);
    }

    const PoolConfig& config() const noexcept { return config_; }

    struct Acquired {
        PoolStatus status;
        PoolLease lease;
    };

    Acquired acquire(PoolActor caller, PoolOrigin origin) {
        if (caller.id == 0) return {PoolStatus::wrong_actor, {}};
        std::unique_lock<std::mutex> lock(mutex_);
        if (shut_down_) return {PoolStatus::shut_down, {}};
        if (count_locked([&](const Slot& s) { return s.state == State::leased && s.owner == caller; })
                >= config_.max_per_actor)
            return {PoolStatus::limit_actor, {}};

        const auto now = Clock::now();
        // Reuse a clean idle connection of exactly this origin (tenant included).
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& s = slots_[i];
            if (s.state != State::idle || !(s.origin == origin)) continue;
            if (idle_expired(s, now) || !idle_socket_is_clean(s.fd)) {
                ++stats_.discarded_on_probe;
                close_locked(s);
                continue;
            }
            lease_locked(s, caller);
            ++stats_.reuses;
            return {PoolStatus::ok, PoolLease{i, s.generation, caller, true}};
        }

        if (count_locked([&](const Slot& s) { return s.state != State::free && s.origin == origin; })
                >= config_.max_per_origin)
            return {PoolStatus::limit_origin, {}};

        std::uint32_t index = find_free_or_evict_locked(now);
        if (index == kNoSlot) return {PoolStatus::pool_full, {}};

        // Reserve the slot and connect outside the lock.
        Slot& s = slots_[index];
        s.origin = origin;
        lease_locked(s, caller);
        s.busy = true;
        ++busy_;
        const std::uint64_t generation = s.generation;
        lock.unlock();

        int fd = connect_with_timeout(origin, config_.connect_timeout_ms);

        lock.lock();
        s.busy = false;
        --busy_;
        drained_.notify_all();
        if (s.state != State::leased || s.generation != generation) {
            // Revoked or shut down while connecting.
            if (fd >= 0) ::close(fd);
            if (s.state == State::closing) close_locked(s);
            return {shut_down_ ? PoolStatus::shut_down : PoolStatus::revoked, {}};
        }
        if (fd < 0) {
            close_locked(s);
            return {PoolStatus::connect_failed, {}};
        }
        s.fd = fd;
        ++stats_.connects;
        return {PoolStatus::ok, PoolLease{index, generation, caller, false}};
    }

    // `reusable` is the protocol layer's verdict; the pool still refuses to
    // pool a poisoned connection or anything during shutdown.
    PoolStatus release(PoolActor caller, const PoolLease& lease, bool reusable) {
        std::lock_guard<std::mutex> lock(mutex_);
        const PoolStatus st = validate_locked(caller, lease);
        if (st != PoolStatus::ok) return st;
        Slot& s = slots_[lease.slot];
        if (s.busy) return PoolStatus::busy;
        if (reusable && !s.poisoned && !shut_down_ && s.fd >= 0) {
            ++s.generation;
            s.state = State::idle;
            s.owner = {};
            s.idle_since = Clock::now();
        } else {
            ++stats_.closed_unclean;
            close_locked(s);
        }
        return PoolStatus::ok;
    }

    // Scheduler calls this when an actor incarnation ends (exit, crash,
    // restart). Its connections are closed, never pooled: their protocol
    // state is unknown. In-flight I/O is woken with shutdown(2) and the FD is
    // closed by whoever finishes that I/O.
    std::size_t revoke_actor(PoolActor actor) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (auto& s : slots_) {
            if (s.state != State::leased || !(s.owner == actor)) continue;
            ++n;
            ++stats_.revoked;
            retire_locked(s);
        }
        return n;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        shut_down_ = true;
        for (auto& s : slots_) {
            if (s.state == State::idle) close_locked(s);
            else if (s.state == State::leased) retire_locked(s);
        }
    }

    PoolStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        PoolStats out = stats_;
        for (const auto& s : slots_) {
            if (s.state == State::idle) ++out.idle;
            else if (s.state == State::leased) ++out.leased;
            else if (s.state == State::closing) ++out.closing;
        }
        return out;
    }

    // Runs fn(fd) with the slot marked busy. fn returns true iff the
    // connection is still protocol-clean afterwards; false poisons it.
    template <class F>
    PoolStatus with_fd(PoolIoKey, PoolActor caller, const PoolLease& lease, F&& fn) {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const PoolStatus st = validate_locked(caller, lease);
            if (st != PoolStatus::ok) return st;
            Slot& s = slots_[lease.slot];
            if (s.busy) return PoolStatus::busy;
            if (s.poisoned) return PoolStatus::poisoned;
            s.busy = true;
            ++busy_;
            fd = s.fd;
        }
        bool clean = false;
        try {
            clean = fn(fd);
        } catch (...) {
            finish_io(lease.slot, false);
            throw;
        }
        finish_io(lease.slot, clean);
        return PoolStatus::ok;
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr std::uint32_t kNoSlot = std::numeric_limits<std::uint32_t>::max();
    enum class State : std::uint8_t { free, idle, leased, closing };

    struct Slot {
        int fd = -1;
        State state = State::free;
        std::uint64_t generation = 0;
        PoolOrigin origin{};
        PoolActor owner{};
        bool busy = false;
        bool poisoned = false;
        Clock::time_point idle_since{};
    };

    PoolStatus validate_locked(PoolActor caller, const PoolLease& lease) const {
        if (lease.slot >= slots_.size()) return PoolStatus::invalid_lease;
        if (!(lease.actor == caller)) return PoolStatus::wrong_actor;
        const Slot& s = slots_[lease.slot];
        if (s.state != State::leased || s.generation != lease.generation) return PoolStatus::invalid_lease;
        if (!(s.owner == caller)) return PoolStatus::wrong_actor;
        return PoolStatus::ok;
    }

    void lease_locked(Slot& s, PoolActor caller) {
        ++s.generation;
        s.state = State::leased;
        s.owner = caller;
        s.poisoned = false;
    }

    // Invalidate the current lease. Close now if idle of I/O, else defer.
    void retire_locked(Slot& s) {
        ++s.generation;
        s.owner = {};
        if (s.busy) {
            s.state = State::closing;
            if (s.fd >= 0) ::shutdown(s.fd, SHUT_RDWR);  // wake the reader; FD stays allocated
        } else {
            close_locked(s);
        }
    }

    void close_locked(Slot& s) {
        if (s.fd >= 0) ::close(s.fd);
        s.fd = -1;
        ++s.generation;
        s.state = State::free;
        s.owner = {};
        s.poisoned = false;
    }

    void finish_io(std::uint32_t index, bool clean) {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& s = slots_[index];
        s.busy = false;
        --busy_;
        if (!clean) s.poisoned = true;
        if (s.state == State::closing) close_locked(s);
        drained_.notify_all();
    }

    template <class P>
    std::uint32_t count_locked(P pred) const {
        std::uint32_t n = 0;
        for (const auto& s : slots_) n += pred(s) ? 1u : 0u;
        return n;
    }

    bool idle_expired(const Slot& s, Clock::time_point now) const {
        return now - s.idle_since > std::chrono::milliseconds(config_.idle_timeout_ms);
    }

    std::uint32_t find_free_or_evict_locked(Clock::time_point now) {
        std::uint32_t oldest = kNoSlot;
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].state == State::free) return i;
            if (slots_[i].state == State::idle
                    && (oldest == kNoSlot || slots_[i].idle_since < slots_[oldest].idle_since))
                oldest = i;
        }
        (void)now;
        if (oldest != kNoSlot) close_locked(slots_[oldest]);
        return oldest;
    }

    // An idle HTTP/1.1 connection must have nothing to read. A pending byte
    // (late or unsolicited response) or EOF means it is unsafe to hand out.
    static bool idle_socket_is_clean(int fd) noexcept {
        if (fd < 0) return false;
        unsigned char b;
        const ssize_t n = ::recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n >= 0) return false;
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }

    static int connect_with_timeout(PoolOrigin origin, int timeout_ms) noexcept {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        auto fail = [&] { ::close(fd); return -1; };
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) return fail();
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return fail();
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(origin.port);
        addr.sin_addr.s_addr = htonl(origin.ipv4_host_order);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) return fd;
        if (errno != EINPROGRESS) return fail();
        pollfd p{fd, POLLOUT, 0};
        int r;
        do r = ::poll(&p, 1, timeout_ms); while (r < 0 && errno == EINTR);
        if (r <= 0) return fail();
        int err = 0;
        socklen_t len = sizeof err;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) return fail();
        return fd;
    }

    const PoolConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable drained_;
    std::vector<Slot> slots_;
    std::uint32_t busy_ = 0;
    bool shut_down_ = false;
    PoolStats stats_{};
};

}  // namespace oreslang::runtime
