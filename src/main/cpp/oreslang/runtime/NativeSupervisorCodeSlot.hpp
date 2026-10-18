#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
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
    // Native images must not throw while moving their ownership wrappers:
    // otherwise the callback may unload an image before a failed move
    // finishes destroying the wrapper still referencing that image.
    static_assert(std::is_nothrow_move_constructible_v<Policy>,
                  "native code image must be nothrow-move-constructible");
    static_assert(std::is_nothrow_destructible_v<Policy>,
                  "native code image destructor must not throw");
    // Runs *after* the Policy object has been destroyed. Running dlclose /
    // ORC removal first could unload Policy's destructor or captured vtable.
    struct ReclaimOwner final {
        std::function<void()> callback;
        explicit ReclaimOwner(std::function<void()> cb) : callback(std::move(cb)) {}
        ~ReclaimOwner() noexcept {
            try { if (callback) callback(); }
            catch (...) { /* cleanup must not throw from a final code pin */ }
        }
    };
    struct Generation {
        ReclaimOwner owner; // Declared first => destroyed last, after code.
        std::uint64_t number;
        std::string abi;
        Policy code;
        Generation(std::uint64_t revision, std::string signature, Policy image,
                   std::function<void()> cleanup)
            : owner(std::move(cleanup)), number(revision),
              abi(std::move(signature)), code(std::move(image)) {}
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
        // Take ownership even when initial admission or allocation fails.
        // The initial image may already be a loaded native shared object.
        ReclaimOwner rejected(std::move(reclaim));
        Policy staged(std::move(initial)); // Destroy before rejected cleanup on failure.
        if (initial_abi.empty() || maximum_live < 2 || !rejected.callback)
            throw std::invalid_argument("invalid native supervisor code slot admission");
        current_ = std::make_shared<Generation>(
                1, std::move(initial_abi), std::move(staged),
                rejected.callback);
        rejected.callback = nullptr;
        retained_.push_back(current_);
    }

    NativeSupervisorCodeSlot(const NativeSupervisorCodeSlot&) = delete;
    NativeSupervisorCodeSlot& operator=(const NativeSupervisorCodeSlot&) = delete;
    ~NativeSupervisorCodeSlot() { close(); }

    /**
     * One slot may be installed on exactly one supervisor in one runtime
     * registry. Same numeric supervisor IDs in another registry are not
     * interchangeable.
     */
    void bind_owner(const void* runtime_domain, std::uint64_t supervisor_id) {
        if (runtime_domain == nullptr || supervisor_id == 0)
            throw std::invalid_argument("invalid supervisor code owner");
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) throw std::logic_error("native supervisor code slot closed");
        if (owner_domain_ != nullptr)
            throw std::logic_error("native supervisor code slot already installed");
        owner_domain_ = runtime_domain;
        owner_supervisor_ = supervisor_id;
    }

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

    /** Atomic, ABI-exact publication of a fully prepared image.
     *
     * A rejected candidate owns its cleanup just like an accepted one.
     * Construct the candidate before acquiring the slot lock so that
     * reclamation (including allocator failures) cannot call back under
     * the CONTROL activation mutex. A guest callable must never execute
     * while this mutex is held.
     */
    Descriptor activate(std::uint64_t expected_revision, std::string abi,
            Policy code, std::function<void()> reclaim) {
        // Callback ownership begins before ABI validation and allocation.
        // If moving/copying an image or allocating a generation throws, the
        // guard still schedules image unload exactly once, outside CONTROL.
        ReclaimOwner rejected(std::move(reclaim));
        Policy staged(std::move(code)); // Teardown precedes callback on failure.
        if (abi.empty() || !rejected.callback)
            throw std::invalid_argument("invalid native supervisor code image");

        // On stale revision, incompatible ABI, close, or live-cap failure the
        // candidate is destroyed AFTER unwinding the scoped activation lock.
        auto candidate = std::make_shared<Generation>(
                0, std::move(abi), std::move(staged), rejected.callback);
        rejected.callback = nullptr;
        std::shared_ptr<Generation> previous;
        Descriptor activated;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) throw std::logic_error("native supervisor code slot closed");
            if (current_->number != expected_revision)
                throw std::logic_error("stale native supervisor code revision");
            if (current_->number == UINT64_MAX)
                throw std::overflow_error("native supervisor code revision space exhausted");
            if (current_->abi != candidate->abi)
                throw std::invalid_argument("native supervisor policy ABI migration required");

            prune_locked();
            // The current revision is already included in retained_. If nobody
            // else pins it, it will disappear after this activation, making
            // room for the candidate even when the vector is at capacity.
            if (retained_.size() >= max_live_ && current_.use_count() > 1)
                throw std::length_error("pinned native supervisor code revision cap exceeded");

            // vector growth may throw; candidate cleanup still runs outside
            // this mutex because candidate was declared before guard.
            retained_.push_back(candidate);
            candidate->number = current_->number + 1;
            activated = {candidate->number, candidate->abi};
            previous = std::move(current_);
            current_ = candidate;
        }
        // Never reclaim code with a supervisor policy lock held. Old leases
        // keep their exact revision alive until their last borrower exits.
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
    const void* owner_domain_ = nullptr;
    std::uint64_t owner_supervisor_ = 0;
    bool closed_ = false;
};

} // namespace oreslang::runtime
