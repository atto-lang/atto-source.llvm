#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * CONTROL-owned immutable supervisor code image pointer.
 *
 * The slot itself owns no actor heap. Pinning holds a revision while an
 * existing policy turn/continuation is active; a new revision is activated
 * atomically without rewriting that turn. Reclamation callbacks MUST only
 * enqueue cleanup on the owning runtime, not execute guest code directly.
 *
 * The native source compiler does not yet lower source code_change/await
 * into this substrate.
 */
template <class Policy>
class NativeSupervisorCodeSlot final {
    struct Generation {
        std::uint64_t number;
        std::string abi;
        Policy code;
        std::function<void()> reclaim;
        ~Generation() noexcept {
            try { if (reclaim) reclaim(); }
            catch (...) { /* cannot throw from final shared pin release */ }
        }
    };
public:
    struct Descriptor {
        std::uint64_t revision;
        std::string abi;
    };

    class Lease final {
    public:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) noexcept = default;
        Lease& operator=(Lease&&) noexcept = default;
        const Policy& code() const {
            if (!generation_) throw std::logic_error("native supervisor code lease released");
            return generation_->code;
        }
        Descriptor descriptor() const {
            if (!generation_) throw std::logic_error("native supervisor code lease released");
            return {generation_->number, generation_->abi};
        }
        void close() noexcept { generation_.reset(); }
    private:
        friend class NativeSupervisorCodeSlot;
        explicit Lease(std::shared_ptr<Generation> generation)
            : generation_(std::move(generation)) {}
        std::shared_ptr<Generation> generation_;
    };

    explicit NativeSupervisorCodeSlot(std::string initial_abi, Policy initial,
            std::function<void()> reclaim, std::size_t maximum_live = 8)
        : max_live_(maximum_live) {
        if (initial_abi.empty() || maximum_live < 2 || !reclaim)
            throw std::invalid_argument("invalid native supervisor code slot admission");
        current_ = std::make_shared<Generation>(
                Generation{1, std::move(initial_abi), std::move(initial),
                           std::move(reclaim)});
        retained_.push_back(current_);
    }

    NativeSupervisorCodeSlot(const NativeSupervisorCodeSlot&) = delete;
    NativeSupervisorCodeSlot& operator=(const NativeSupervisorCodeSlot&) = delete;
    ~NativeSupervisorCodeSlot() { close(); }

    Descriptor current() const {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) throw std::logic_error("native supervisor code slot closed");
        return {current_->number, current_->abi};
    }

    Lease pin() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) throw std::logic_error("native supervisor code slot closed");
        return Lease(current_);
    }

    Descriptor activate(std::uint64_t expected_revision, std::string abi,
            Policy code, std::function<void()> reclaim) {
        if (abi.empty() || !reclaim)
            throw std::invalid_argument("invalid native supervisor code image");
        std::shared_ptr<Generation> previous;
        Descriptor activated;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) throw std::logic_error("native supervisor code slot closed");
            if (current_->number != expected_revision)
                throw std::logic_error("stale native supervisor code revision");
            if (current_->abi != abi)
                throw std::invalid_argument("native supervisor policy ABI migration required");
            prune_locked();
            if (retained_.size() >= max_live_ && current_.use_count() > 1)
                throw std::length_error("pinned native supervisor code revision cap exceeded");
            auto next = std::make_shared<Generation>(Generation{
                current_->number + 1, std::move(abi),
                std::move(code), std::move(reclaim)});
            activated = {next->number, next->abi};
            retained_.push_back(next);
            previous = std::move(current_);
            current_ = std::move(next);
        }
        // Release the old code outside the slot lock; reclamation callbacks
        // must never run with the supervisor policy activation mutex held.
        previous.reset();
        return activated;
    }

    std::size_t live_revisions() const {
        std::lock_guard<std::mutex> guard(mutex_);
        prune_locked();
        return retained_.size();
    }

    void close() noexcept {
        std::shared_ptr<Generation> previous;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) return;
            closed_ = true;
            previous = std::move(current_);
        }
        previous.reset();
    }

private:
    void prune_locked() const {
        retained_.erase(std::remove_if(retained_.begin(), retained_.end(),
                [](const auto& ref) { return ref.expired(); }), retained_.end());
    }

    const std::size_t max_live_;
    mutable std::mutex mutex_;
    mutable std::vector<std::weak_ptr<Generation>> retained_;
    std::shared_ptr<Generation> current_;
    bool closed_ = false;
};

} // namespace oreslang::runtime
