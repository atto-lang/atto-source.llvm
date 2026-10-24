#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>

namespace oreslang::runtime {

// Trusted native transport accounting, NOT an Oreslang guest capability.
// Every queued callback, timer, waker and I/O completion that might touch
// actor-owned storage must hold one move-only permit from enqueue/admission
// until its last physical callback/cleanup finishes. Registration and freeze
// share one atomic word, so a drained receipt cannot race admitted permits.
// No per-callback heap allocation or blocking wait is performed by this gate.
class NativeTransportCallbackGate final {
public:
    struct Binding final {
        std::uint64_t domain = 0;
        std::uint64_t slot = 0;
        std::uint64_t generation = 0;
        std::uint64_t slab = 0;
    };
    // Immutable routing metadata, NOT an authorization token. Trusted host
    // retains the actual counted callback permit until final cleanup.
    Binding binding() const noexcept {
        return Binding{domain_, slot_, generation_, identity_};
    }
    class Permit final {
    public:
        Permit() noexcept = default;
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        Permit(Permit&& other) noexcept : gate_(std::exchange(other.gate_, nullptr)) {}
        Permit& operator=(Permit&& other) noexcept {
            if (this != &other) {
                release();
                gate_ = std::exchange(other.gate_, nullptr);
            }
            return *this;
        }
        ~Permit() { release(); }
        explicit operator bool() const noexcept { return gate_ != nullptr; }
        void release() noexcept {
            auto* gate = std::exchange(gate_, nullptr);
            if (gate) gate->release_one();
        }
    private:
        friend class NativeTransportCallbackGate;
        explicit Permit(NativeTransportCallbackGate* gate) noexcept : gate_(gate) {}
        NativeTransportCallbackGate* gate_ = nullptr;
    };

    class DrainReceipt final {
    public:
        DrainReceipt(const DrainReceipt&) = default;
        DrainReceipt& operator=(const DrainReceipt&) = default;
    private:
        friend class NativeTransportCallbackGate;
        DrainReceipt(const NativeTransportCallbackGate* gate,
                     std::uint64_t domain, std::uint64_t slot,
                     std::uint64_t generation, std::uint64_t identity) noexcept
            : gate_(gate), domain_(domain), slot_(slot),
              generation_(generation), identity_(identity) {}
        const NativeTransportCallbackGate* gate_;
        std::uint64_t domain_;
        std::uint64_t slot_;
        std::uint64_t generation_;
        std::uint64_t identity_;
    };

    NativeTransportCallbackGate(std::uint64_t domain, std::uint64_t slot,
                                std::uint64_t generation,
                                std::uint64_t slab_identity) noexcept
        : domain_(domain), slot_(slot), generation_(generation),
          identity_(slab_identity) {}
    NativeTransportCallbackGate(const NativeTransportCallbackGate&) = delete;
    NativeTransportCallbackGate& operator=(const NativeTransportCallbackGate&) = delete;
    NativeTransportCallbackGate(NativeTransportCallbackGate&&) = delete;
    NativeTransportCallbackGate& operator=(NativeTransportCallbackGate&&) = delete;
    ~NativeTransportCallbackGate() {
        // Prevent destroying the gate while a registered callback
        // can still touch its associated actor arena. This is not proof that
        // an unregistered external callback cannot exist (host must wire it).
        if ((state_.load(std::memory_order_acquire) & kCountMask) != 0)
            std::abort();
    }

    [[nodiscard]] Permit try_admit() noexcept {
        auto observed = state_.load(std::memory_order_acquire);
        for (;;) {
            // Reserve the high bit for terminal shutdown; count cannot wrap
            // or consume the stop bit even under hostile callback volume.
            if ((observed & kStopped) != 0 ||
                    (observed & kCountMask) == kCountMask) return {};
            if (state_.compare_exchange_weak(observed, observed + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return Permit(this);
            }
        }
    }

    void stop_admission() noexcept {
        // Atomic with concurrent admissions: a racing CAS either precedes
        // this shutdown and owns a counted permit, or fails to admit.
        state_.fetch_or(kStopped, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool accepting() const noexcept {
        return (state_.load(std::memory_order_acquire) & kStopped) == 0;
    }

    [[nodiscard]] std::optional<DrainReceipt> try_drain() const noexcept {
        // Acquire from the final permit release before allowing page scrub.
        if (state_.load(std::memory_order_acquire) != kStopped)
            return std::nullopt;
        return DrainReceipt(this, domain_, slot_, generation_, identity_);
    }

    [[nodiscard]] bool verifies(const DrainReceipt& receipt) const noexcept {
        return state_.load(std::memory_order_acquire) == kStopped
            && receipt.gate_ == this && receipt.domain_ == domain_
            && receipt.slot_ == slot_ && receipt.generation_ == generation_
            && receipt.identity_ == identity_;
    }

private:
    void release_one() noexcept {
        const auto prior = state_.fetch_sub(1, std::memory_order_acq_rel);
        if ((prior & kCountMask) == 0) std::abort();
    }
    static constexpr std::uint32_t kStopped = std::uint32_t{1} << 31;
    static constexpr std::uint32_t kCountMask = kStopped - 1;
    const std::uint64_t domain_;
    const std::uint64_t slot_;
    const std::uint64_t generation_;
    const std::uint64_t identity_;
    std::atomic<std::uint32_t> state_{0};
};

} // namespace oreslang::runtime
