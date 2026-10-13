#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// This is the native *control-plane policy*, not source-language exception
// lowering or actor supervision. Keep ranks identical to OresExceptionPolicy
// on GraalVM: host/process fatal > cancellation > panic > raise > throw.
enum class FailureKind : std::uint8_t {
    Throw, Raise, Panic, Cancelled, ProcessExit, HostFatal
};

constexpr int failure_priority(FailureKind kind) noexcept {
    switch (kind) {
        case FailureKind::HostFatal:
        case FailureKind::ProcessExit: return 5;
        case FailureKind::Cancelled: return 4;
        case FailureKind::Panic: return 3;
        case FailureKind::Raise: return 2;
        case FailureKind::Throw: return 1;
    }
    return 0;
}

// Guest catch and trap admission differ from native supervisor priority.
// Origin is assigned by the runtime, never derived from diagnostic text.
// This host policy is not yet wired to native guest exception lowering.
enum class GuestFailureBoundary : std::uint8_t { Trap, ExplicitCatch };
enum class GuestFailureOrigin : std::uint8_t {
    OrdinaryThrow, PermissionDenied, DeadlineExceeded, Cancellation,
    SchedulerRejected, Interrupted, Panic, Raise, ProcessExit, HostFatal, Unknown
};

constexpr bool admits_guest_failure(
        GuestFailureBoundary boundary, GuestFailureOrigin origin) noexcept {
    switch (boundary) {
        case GuestFailureBoundary::Trap:
            return origin == GuestFailureOrigin::OrdinaryThrow;
        case GuestFailureBoundary::ExplicitCatch:
            return origin == GuestFailureOrigin::OrdinaryThrow
                || origin == GuestFailureOrigin::PermissionDenied
                || origin == GuestFailureOrigin::DeadlineExceeded;
    }
    return false; // malformed native boundary tag fails closed
}

constexpr std::size_t max_control_diagnostic_bytes = 1024;

// Immutable-on-delivery, value-only metadata for a native supervisor.
// Invalid UTF-8 bytes become '?' rather than crossing actor boundaries.
// Truncation never cuts a valid Unicode scalar in half.
inline std::string bounded_control_diagnostic(std::string_view message) {
    std::string result;
    result.reserve(std::min(max_control_diagnostic_bytes, message.size()));
    for (std::size_t i = 0; i < message.size();) {
        const auto first = static_cast<unsigned char>(message[i]);
        std::size_t width = 0;
        if (first <= 0x7f) width = 1;
        else if (first >= 0xc2 && first <= 0xdf) width = 2;
        else if (first >= 0xe0 && first <= 0xef) width = 3;
        else if (first >= 0xf0 && first <= 0xf4) width = 4;
        bool valid = width != 0 && width <= message.size() - i;
        if (valid && width > 1) {
            for (std::size_t j = 1; j < width; ++j) {
                if ((static_cast<unsigned char>(message[i + j]) & 0xc0) != 0x80) {
                    valid = false;
                    break;
                }
            }
        }
        if (valid && width == 3) {
            const auto second = static_cast<unsigned char>(message[i + 1]);
            if ((first == 0xe0 && second < 0xa0) ||
                (first == 0xed && second >= 0xa0)) valid = false;
        }
        if (valid && width == 4) {
            const auto second = static_cast<unsigned char>(message[i + 1]);
            if ((first == 0xf0 && second < 0x90) ||
                (first == 0xf4 && second > 0x8f)) valid = false;
        }
        if (!valid) {
            if (result.size() == max_control_diagnostic_bytes) break;
            result.push_back('?');
            ++i;
            continue;
        }
        if (width > max_control_diagnostic_bytes - result.size()) break;
        result.append(message.substr(i, width));
        i += width;
    }
    return result;
}

// Copies no Throwable/exception_ptr or mutable cross-heap payload. Suppressed
// cleanup failures are recursively preserved as native-owned diagnostic data.
class Failure {
public:
    static Failure make(FailureKind kind, std::string_view message) {
        // Native/FFI callers can construct invalid enum values by cast.
        // Never let an unknown tag enter the failure priority lattice.
        if (failure_priority(kind) == 0) {
            throw std::invalid_argument("unknown native failure kind");
        }
        return Failure(kind, bounded_control_diagnostic(message));
    }

    FailureKind kind() const noexcept { return kind_; }
    const std::string& diagnostic() const noexcept { return diagnostic_; }
    const std::vector<Failure>& suppressed() const noexcept { return suppressed_; }

private:
    Failure(FailureKind kind, std::string diagnostic)
        : kind_(kind), diagnostic_(std::move(diagnostic)) {}

    FailureKind kind_;
    std::string diagnostic_;
    std::vector<Failure> suppressed_;

    friend Failure combine_failure(Failure pending, Failure cleanup);
};

// Attempt each cleanup independently, then combine the resulting Failure.
// Higher priority replaces lower priority; equal priority retains the first
// failure, matching the JVM exception policy and Java suppressed ordering.
inline Failure combine_failure(Failure pending, Failure cleanup) {
    if (failure_priority(cleanup.kind()) > failure_priority(pending.kind())) {
        cleanup.suppressed_.push_back(std::move(pending));
        return cleanup;
    }
    pending.suppressed_.push_back(std::move(cleanup));
    return pending;
}

inline std::optional<Failure> combine_failure(
        std::optional<Failure> pending, std::optional<Failure> cleanup) {
    if (!cleanup) return pending;
    if (!pending) return cleanup;
    return combine_failure(std::move(*pending), std::move(*cleanup));
}

} // namespace oreslang::runtime
