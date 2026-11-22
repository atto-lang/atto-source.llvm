#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang::runtime {

/**
 * HOST-ONLY state-machine proof of actor hot-upgrade fencing, not a native
 * executable-code loader or an integrated Oreslang source-actor scheduler.
 * Callers must be trusted and obtain image/ABI hashes from a verified compiler.
 */
class ActorHotUpgradeGate final {
public:
    struct Image {
        std::string code_sha256;
        std::string state_sha256;
        std::string message_abi_sha256;
        std::string capability_sha256;

        bool same_boundary(const Image& rhs) const noexcept {
            return state_sha256 == rhs.state_sha256
                && message_abi_sha256 == rhs.message_abi_sha256
                && capability_sha256 == rhs.capability_sha256;
        }
    };

private:
    struct State {
        State(std::uint64_t id, Image initial)
            : actor_id(id), active(std::move(initial)) {}
        std::mutex mutex;
        const std::uint64_t actor_id;
        Image active;
        std::uint64_t epoch = 1;
        std::uint64_t next_ticket = 1;
        std::uint64_t pending_ticket = 0;
        std::optional<Image> pending;
        std::size_t pins = 0;
        bool closed = false;
    };

    std::shared_ptr<State> state_;

    struct LeaseToken {
        LeaseToken(std::shared_ptr<State> owner, std::uint64_t generation)
            : state(std::move(owner)), epoch(generation) {}
        std::shared_ptr<State> state;
        std::uint64_t epoch;
        std::atomic<bool> released{false};
    };

    static void check_digest(const std::string& value) {
        if (value.size() != 64
                || !std::all_of(value.begin(), value.end(),
                    [](char c) { return (c >= '0' && c <= '9')
                        || (c >= 'a' && c <= 'f'); }))
            throw std::invalid_argument("expected canonical lowercase SHA-256");
    }

    static void validate(const Image& image) {
        check_digest(image.code_sha256);
        check_digest(image.state_sha256);
        check_digest(image.message_abi_sha256);
        check_digest(image.capability_sha256);
    }

public:
    class Lease final {
        friend class ActorHotUpgradeGate;
        std::shared_ptr<LeaseToken> token_;
        explicit Lease(std::shared_ptr<State> state, std::uint64_t epoch)
            : token_(std::make_shared<LeaseToken>(std::move(state), epoch)) {}
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) noexcept = default;
        Lease& operator=(Lease&& rhs) noexcept {
            if (this != &rhs) {
                close();
                token_ = std::move(rhs.token_);
            }
            return *this;
        }
        ~Lease() { close(); }

        std::uint64_t epoch() const noexcept {
            return token_ ? token_->epoch : 0;
        }
        void close() noexcept {
            if (!token_ || token_->released.exchange(true, std::memory_order_acq_rel))
                return;
            std::lock_guard<std::mutex> guard(token_->state->mutex);
            if (token_->state->pins != 0) --token_->state->pins;
        }
    };

    class Candidate final {
        friend class ActorHotUpgradeGate;
        std::shared_ptr<State> state_;
        std::uint64_t origin_epoch_ = 0;
        std::uint64_t ticket_ = 0;
        Candidate(std::shared_ptr<State> state, std::uint64_t epoch,
                  std::uint64_t ticket)
            : state_(std::move(state)), origin_epoch_(epoch), ticket_(ticket) {}
    public:
        // A prepared release seals DATA admission. Dropping its last owned
        // ticket on any exception path must never strand the actor frozen.
        Candidate(const Candidate&) = delete;
        Candidate& operator=(const Candidate&) = delete;
        Candidate(Candidate&&) noexcept = default;
        Candidate& operator=(Candidate&& other) noexcept {
            if (this != &other) {
                release();
                state_ = std::move(other.state_);
                origin_epoch_ = other.origin_epoch_;
                ticket_ = other.ticket_;
            }
            return *this;
        }
        ~Candidate() noexcept { release(); }
    private:
        void release() noexcept {
            auto state = std::move(state_);
            if (!state) return;
            std::lock_guard<std::mutex> guard(state->mutex);
            if (!state->closed && state->epoch == origin_epoch_
                    && state->pending && state->pending_ticket == ticket_) {
                state->pending.reset();
                state->pending_ticket = 0;
            }
        }
    };

    explicit ActorHotUpgradeGate(std::uint64_t actor_id, Image initial) {
        if (actor_id == 0) throw std::invalid_argument("actor id must be nonzero");
        validate(initial);
        state_ = std::make_shared<State>(actor_id, std::move(initial));
    }
    ActorHotUpgradeGate(const ActorHotUpgradeGate&) = delete;
    ActorHotUpgradeGate& operator=(const ActorHotUpgradeGate&) = delete;
    ~ActorHotUpgradeGate() { close(); }

    std::uint64_t actor_id() const noexcept { return state_->actor_id; }
    std::uint64_t epoch() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->epoch;
    }
    Image image() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->active;
    }
    std::size_t pins() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->pins;
    }
    bool frozen() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->pending.has_value();
    }
    bool retired() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->closed && state_->pins == 0;
    }

    Lease pin() {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->closed || state_->pending)
            throw std::logic_error("closed or upgrade-frozen actor");
        if (state_->pins == std::numeric_limits<std::size_t>::max())
            throw std::overflow_error("actor pins exhausted");
        // Construct before charging: allocation failure must never leak a pin.
        Lease lease(state_, state_->epoch);
        ++state_->pins;
        return lease;
    }

    bool permits(const Lease& lease) const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        const auto token = lease.token_;
        return token && token->state.get() == state_.get()
            && token->epoch == state_->epoch
            && !token->released.load(std::memory_order_acquire)
            && !state_->closed;
    }

    Candidate prepare(Image proposed) {
        validate(proposed);
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->closed || state_->pending)
            throw std::logic_error("closed or already upgrading actor");
        if (!state_->active.same_boundary(proposed))
            throw std::logic_error("actor state, message ABI or capability change denied");
        if (state_->active.code_sha256 == proposed.code_sha256)
            throw std::invalid_argument("identical image is not an upgrade");
        if (state_->next_ticket == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("upgrade ticket exhaustion");
        const auto ticket = state_->next_ticket++;
        state_->pending_ticket = ticket;
        state_->pending.emplace(std::move(proposed)); // owned copy; no mutable caller alias
        return Candidate(state_, state_->epoch, ticket);
    }

    bool publish(const Candidate& proposed) {
        std::lock_guard<std::mutex> guard(state_->mutex);
        check_candidate(proposed);
        if (state_->pins != 0) return false;
        if (state_->epoch == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor upgrade epoch exhausted");
        state_->active = std::move(*state_->pending);
        state_->pending.reset();
        state_->pending_ticket = 0;
        ++state_->epoch;
        return true;
    }

    void abort(const Candidate& proposed) {
        std::lock_guard<std::mutex> guard(state_->mutex);
        check_candidate(proposed);
        state_->pending.reset();
        state_->pending_ticket = 0;
    }

    void close() noexcept {
        std::lock_guard<std::mutex> guard(state_->mutex);
        state_->closed = true;
        state_->pending.reset();
        state_->pending_ticket = 0;
        // Pins retain State ownership and release after this object's destruction.
        // There is deliberately no native arena/code-page reclamation here.
    }

private:
    void check_candidate(const Candidate& candidate) const {
        if (state_->closed || candidate.state_.get() != state_.get()
                || candidate.origin_epoch_ != state_->epoch
                || candidate.ticket_ == 0
                || candidate.ticket_ != state_->pending_ticket || !state_->pending)
            throw std::logic_error("stale, foreign or retired upgrade candidate");
    }
};

} // namespace oreslang::runtime
