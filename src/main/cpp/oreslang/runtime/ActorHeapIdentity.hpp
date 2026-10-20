#pragma once
#include <cstdint>

namespace oreslang::runtime {
// Authenticated by the supervisor at native actor admission, not a capability
// exposed to untrusted C++ or arbitrary guest source.
struct ActorHeapIdentity final {
    std::uint64_t runtime;
    std::uint64_t actor;
    std::uint64_t epoch;
};
inline bool operator==(ActorHeapIdentity a, ActorHeapIdentity b) noexcept {
    return a.runtime == b.runtime && a.actor == b.actor && a.epoch == b.epoch;
}
inline bool operator!=(ActorHeapIdentity a, ActorHeapIdentity b) noexcept {
    return !(a == b);
}
} // namespace oreslang::runtime
