#pragma once

#if !defined(__linux__)
#error "NativeTcpConnectionLinux is Linux-only"
#endif

#include "oreslang/runtime/NonblockingTcpLinux.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace oreslang::runtime {

/*
 * Allocation-free ownership state for one connected nonblocking TCP socket.
 *
 * One READ and one WRITE lease may coexist. Duplicate same-direction leases
 * fail closed. close() is logical first: the descriptor is physically closed
 * only after both directional leases retire. Every lease carries both a
 * descriptor generation and a direction generation, so stale tokens cannot
 * release a later lease or an OS-reused descriptor.
 */
class NativeTcpConnectionLinux final {
public:
    enum class Direction : std::uint8_t { read = 1, write = 2 };

    struct Lease {
        int fd = -1;
        std::uint64_t descriptor_generation = 0;
        std::uint64_t direction_generation = 0;
        Direction direction = Direction::read;
    };

    explicit NativeTcpConnectionLinux(OwnedSocketFd socket) noexcept
        : socket_(std::move(socket)),
          descriptor_generation_(next_descriptor_generation()) {
        if (!socket_.valid()) std::abort();
    }

    NativeTcpConnectionLinux(const NativeTcpConnectionLinux&) = delete;
    NativeTcpConnectionLinux& operator=(const NativeTcpConnectionLinux&) = delete;
    NativeTcpConnectionLinux(NativeTcpConnectionLinux&&) = delete;
    NativeTcpConnectionLinux& operator=(NativeTcpConnectionLinux&&) = delete;

    ~NativeTcpConnectionLinux() noexcept {
        close();
        if (!retired()) {
            /*
             * A connection owner may not disappear while reactor/completion
             * code still owns a directional lease.
             */
            std::abort();
        }
    }

    int fd() const noexcept {
        return retired() ? -1 : socket_.get();
    }

    std::uint64_t descriptor_generation() const noexcept {
        return descriptor_generation_;
    }

    bool acquire_read(Lease* output) noexcept {
        return acquire(Direction::read, output);
    }

    bool acquire_write(Lease* output) noexcept {
        return acquire(Direction::write, output);
    }

    bool release(Lease lease) noexcept {
        if (lease.fd < 0
                || lease.descriptor_generation != descriptor_generation_
                || lease.direction_generation == 0)
            return false;

        const std::uint32_t bit = direction_bit(lease.direction);
        std::atomic<std::uint64_t>& generation =
                lease.direction == Direction::read
                    ? read_generation_ : write_generation_;
        if (generation.load(std::memory_order_acquire)
                != lease.direction_generation)
            return false;

        std::uint32_t observed = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((observed & bit) == 0) return false;
            const std::uint32_t desired = observed & ~bit;
            if (state_.compare_exchange_weak(
                    observed, desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                try_retire();
                return true;
            }
        }
    }

    void close() noexcept {
        state_.fetch_or(kClosed, std::memory_order_acq_rel);
        try_retire();
    }

    bool close_requested() const noexcept {
        return (state_.load(std::memory_order_acquire) & kClosed) != 0;
    }

    bool retired() const noexcept {
        return (state_.load(std::memory_order_acquire) & kRetired) != 0;
    }

    bool read_leased() const noexcept {
        return (state_.load(std::memory_order_acquire) & kRead) != 0;
    }

    bool write_leased() const noexcept {
        return (state_.load(std::memory_order_acquire) & kWrite) != 0;
    }

private:
    static constexpr std::uint32_t kRead = 1u << 0;
    static constexpr std::uint32_t kWrite = 1u << 1;
    static constexpr std::uint32_t kClosed = 1u << 2;
    static constexpr std::uint32_t kRetired = 1u << 3;

    static std::uint32_t direction_bit(Direction direction) noexcept {
        return direction == Direction::read ? kRead : kWrite;
    }

    static std::uint64_t next_descriptor_generation() noexcept {
        const std::uint64_t generation =
                descriptor_sequence_.fetch_add(1, std::memory_order_acq_rel);
        if (generation == 0 || generation == UINT64_MAX) std::abort();
        return generation;
    }

    bool acquire(Direction direction, Lease* output) noexcept {
        if (!output) return false;
        const std::uint32_t bit = direction_bit(direction);
        std::uint32_t observed = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((observed & (kClosed | kRetired | bit)) != 0) return false;
            const std::uint32_t desired = observed | bit;
            if (!state_.compare_exchange_weak(
                    observed, desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
                continue;

            std::atomic<std::uint64_t>& generation =
                    direction == Direction::read
                        ? read_generation_ : write_generation_;
            const std::uint64_t next =
                    generation.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (next == 0 || next == UINT64_MAX) std::abort();
            *output = Lease{
                socket_.get(), descriptor_generation_, next, direction};
            return true;
        }
    }

    void try_retire() noexcept {
        std::uint32_t observed = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((observed & kClosed) == 0
                    || (observed & (kRead | kWrite)) != 0
                    || (observed & kRetired) != 0)
                return;
            const std::uint32_t desired = observed | kRetired;
            if (state_.compare_exchange_weak(
                    observed, desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                socket_.reset();
                return;
            }
        }
    }

    inline static std::atomic<std::uint64_t> descriptor_sequence_{1};

    OwnedSocketFd socket_;
    const std::uint64_t descriptor_generation_;
    std::atomic<std::uint32_t> state_{0};
    std::atomic<std::uint64_t> read_generation_{0};
    std::atomic<std::uint64_t> write_generation_{0};
};

} // namespace oreslang::runtime
