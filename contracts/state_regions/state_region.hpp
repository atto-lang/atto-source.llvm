#pragma once
#include <cstdint>
#include <limits>

namespace ores::state {
enum class Status : uint8_t { ok, retired, conflicting_access, bad_release, busy };
enum class Mode : uint8_t { read, write };

// Embedded in owner-controlled storage; no GC, heap allocation or hidden locks.
// NOT thread-safe: outer actor scheduling/lock must serialize transitions.
struct Region {
    uint32_t readers = 0;
    uint32_t pins = 0;
    bool writer = false;
    bool retired = false;
    uint64_t version = 0;

    Status acquire(Mode mode) noexcept {
        if (retired) return Status::retired;
        if (mode == Mode::write) {
            if (writer || readers) return Status::conflicting_access;
            writer = true;
        } else {
            if (writer || readers == std::numeric_limits<uint32_t>::max())
                return Status::conflicting_access;
            ++readers;
        }
        return Status::ok;
    }
    Status release(Mode mode) noexcept {
        if (mode == Mode::write) {
            if (!writer) return Status::bad_release;
            writer = false;
            ++version;
        } else {
            if (!readers) return Status::bad_release;
            --readers;
        }
        return Status::ok;
    }
    Status pin() noexcept {
        if (retired) return Status::retired;
        if (pins == std::numeric_limits<uint32_t>::max()) return Status::busy;
        ++pins;
        return Status::ok;
    }
    Status unpin() noexcept {
        if (!pins) return Status::bad_release;
        --pins;
        return Status::ok;
    }
    Status retire() noexcept {
        if (retired) return Status::retired;
        if (writer || readers || pins) return Status::busy;
        retired = true;
        return Status::ok;
    }
};
} // namespace ores::state
