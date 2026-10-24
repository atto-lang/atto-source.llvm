#pragma once

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace oreslang::runtime {

// Trusted native transport accounting, NOT an Oreslang guest capability.
// Every queued callback, timer, waker and I/O completion that might touch
// actor-owned storage must hold one move-only permit from enqueue/admission
// until its last physical callback/cleanup finishes. Registration and freeze
// share one mutex, so a drained receipt cannot race a newly admitted permit.
// No per-callback heap allocation or blocking wait is performed by this gate.
class NativeTransportCallbackGate final {
public:
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
        // Prevent destroying the accounting mutex while a registered callback
        // can still touch its associated actor arena. This is not proof that
        // an unregistered external callback cannot exist (host must wire it).
        std::lock_guard<std::mutex> guard(mu_);
        if (active_ != 0) std::abort();
    }

    [[nodiscard]] Permit try_admit() noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        if (stopped_ || active_ == std::numeric_limits<std::uint64_t>::max()) {
            return {};
        }
        ++active_;
        return Permit(this);
    }

    void stop_admission() noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        stopped_ = true;
    }

    [[nodiscard]] bool accepting() const noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        return !stopped_;
    }

    [[nodiscard]] std::optional<DrainReceipt> try_drain() const noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        if (!stopped_ || active_ != 0) return std::nullopt;
        return DrainReceipt(this, domain_, slot_, generation_, identity_);
    }

    [[nodiscard]] bool verifies(const DrainReceipt& receipt) const noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        return stopped_ && active_ == 0 && receipt.gate_ == this
            && receipt.domain_ == domain_ && receipt.slot_ == slot_
            && receipt.generation_ == generation_
            && receipt.identity_ == identity_;
    }

private:
    void release_one() noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        if (active_ == 0) std::abort();
        --active_;
    }
    const std::uint64_t domain_;
    const std::uint64_t slot_;
    const std::uint64_t generation_;
    const std::uint64_t identity_;
    mutable std::mutex mu_;
    std::uint64_t active_ = 0;
    bool stopped_ = false;
};

} // namespace oreslang::runtime
