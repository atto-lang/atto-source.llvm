#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * Bounded, single-roundtrip pipeline state machine. Host CONTROL/actor
 * scheduler code supplies actor identities and nominal mailbox domains; guest
 * payloads must never supply these authorities. Not a substitute for actual
 * actor dispatch, mailbox serialization, or physical STOP receipts.
 *
 * Phases are 0 (unadmitted), 1 (root DATA published), N (sink DATA turn),
 * 2*N-1 (result delivered to the source), 2*N (source finished).
 * Claims reserve one phase before publication. The owning scheduler MUST
 * commit at its mailbox-publication linearization point, before the recipient
 * can execute the message. A failed root prepublication can be retried; a
 * failed directional publication aborts the route irreversibly.
 */
class NativeActorStreamRoute final {
public:
    enum class Direction { Root, Forward, Backward };
    enum class Done { Pending, Succeeded, Failed };
    struct Claim {
        std::uint64_t token;
        std::uint64_t destination;
        Direction direction;
    };

    NativeActorStreamRoute(std::vector<std::uint64_t> actor_ids,
                           std::string closed_nominal_domain)
        : actors_(std::move(actor_ids)), domain_(std::move(closed_nominal_domain)),
          physically_done_(actors_.size(), false) {
        if (actors_.size() < 2 || actors_.size() > 16)
            throw std::invalid_argument("ActorStream requires 2..16 actors");
        if (domain_.empty() || domain_.size() > 256)
            throw std::invalid_argument("ActorStream requires a bounded closed nominal domain");
        std::unordered_set<std::uint64_t> seen;
        for (auto id : actors_) {
            if (id == 0 || !seen.insert(id).second)
                throw std::invalid_argument("ActorStream requires distinct nonzero actor identities");
        }
    }

    NativeActorStreamRoute(const NativeActorStreamRoute&) = delete;
    NativeActorStreamRoute& operator=(const NativeActorStreamRoute&) = delete;

    std::size_t size() const noexcept { return actors_.size(); }

    // The scheduler must obtain this root claim before publishing source DATA.
    Claim claim_root(std::uint64_t source, const std::string& domain) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_ready(domain);
        if (phase_ != 0 || actors_.front() != source)
            throw std::logic_error("ActorStream root DATA ingress unauthorized or already used");
        return claim(actors_.front(), Direction::Root);
    }

    // Caller/sender are trusted scheduler identities, not data inside a frame.
    Claim claim_directional(std::uint64_t caller, std::uint64_t actual_sender,
                            Direction direction, const std::string& domain) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_ready(domain);
        if (direction == Direction::Root)
            throw std::invalid_argument("Root direction requires claim_root");
        const auto found = std::find(actors_.begin(), actors_.end(), caller);
        if (found == actors_.end())
            throw std::logic_error("actor is not a member of the route");
        const auto pos = static_cast<std::size_t>(found - actors_.begin());
        const auto last = actors_.size() - 1;
        if (physically_done_[pos])
            throw std::logic_error("physically stopped actor cannot route DATA");
        if (direction == Direction::Forward) {
            if (pos == last ||
                (pos == 0 ? actual_sender != 0 : actors_[pos - 1] != actual_sender) ||
                phase_ != pos + 1)
                throw std::logic_error("unauthorized forward direction, sender or phase");
            return claim(actors_[pos + 1], direction);
        }
        if (pos == 0 ||
            (pos == last ? actors_[pos - 1] : actors_[pos + 1]) != actual_sender ||
            phase_ != 2 * actors_.size() - 1 - pos)
            throw std::logic_error("unauthorized backward direction, sender or phase");
        return claim(actors_[pos - 1], direction);
    }

    // Commit strictly at scheduler-owned DATA mailbox publication.
    void commit_published(const Claim& reserved) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_active_claim(reserved);
        ++phase_;
        claimed_ = false;
    }

    // Only the uncommitted root may be safely retried. Every failed
    // directional emission poisons the complete flow, never phase-rolls back.
    void reject_prepublication(const Claim& reserved) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_active_claim(reserved);
        claimed_ = false;
        if (reserved.direction != Direction::Root) aborted_ = true;
    }

    // Terminal action is authorized only on a physical source DATA turn
    // returning from its actual first downstream neighbor.
    void complete_source(std::uint64_t actor, std::uint64_t actual_sender) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (aborted_ || claimed_ || actor != actors_.front() ||
            actual_sender != actors_[1] || phase_ != 2 * actors_.size() - 1 ||
            physically_done_[0])
            throw std::logic_error("source completion lacks verified return DATA");
        ++phase_;
    }

    // Physical finalization releases ONLY that actor's routing fence. A
    // completed source must never unregister still-live downstream actors.
    void physical_done(std::uint64_t actor, bool successful) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = std::find(actors_.begin(), actors_.end(), actor);
        if (found == actors_.end())
            throw std::invalid_argument("physical DONE from nonmember actor");
        const auto pos = static_cast<std::size_t>(found - actors_.begin());
        if (physically_done_[pos])
            throw std::logic_error("duplicate physical DONE receipt");
        physically_done_[pos] = true;
        ++done_count_;
        const auto required_phase = pos == 0 ? 2 * actors_.size()
                                             : 2 * actors_.size() - pos;
        if (!successful || claimed_ || phase_ < required_phase) aborted_ = true;
    }

    void fail() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
    }

    Done done() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (aborted_) return Done::Failed;
        if (done_count_ != actors_.size()) return Done::Pending;
        return phase_ == 2 * actors_.size() ? Done::Succeeded : Done::Failed;
    }

    bool fenced(std::uint64_t actor) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = std::find(actors_.begin(), actors_.end(), actor);
        if (found == actors_.end()) return false;
        return !physically_done_[static_cast<std::size_t>(found - actors_.begin())];
    }

    std::size_t phase() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return phase_;
    }

private:
    Claim claim(std::uint64_t destination, Direction direction) {
        if (claimed_) throw std::logic_error("ActorStream concurrent publication claim");
        claimed_ = true;
        reserved_destination_ = destination;
        reserved_direction_ = direction;
        ++next_token_;
        return Claim{next_token_, destination, direction};
    }

    void require_ready(const std::string& domain) const {
        if (aborted_) throw std::logic_error("ActorStream route was aborted");
        if (domain != domain_)
            throw std::logic_error("ActorStream exact nominal mailbox domain mismatch");
        if (claimed_) throw std::logic_error("ActorStream has a pending publication");
    }

    void require_active_claim(const Claim& candidate) const {
        if (aborted_ || !claimed_ || candidate.token != next_token_ ||
            candidate.direction != reserved_direction_ ||
            candidate.destination != reserved_destination_)
            throw std::logic_error("ActorStream forged, stale or aborted publication claim");
    }

    mutable std::mutex mutex_;
    const std::vector<std::uint64_t> actors_;
    const std::string domain_;
    std::vector<bool> physically_done_;
    std::size_t done_count_ = 0;
    std::size_t phase_ = 0;
    std::uint64_t next_token_ = 0;
    bool claimed_ = false;
    bool aborted_ = false;
    Direction reserved_direction_ = Direction::Root;
    std::uint64_t reserved_destination_ = 0;
};

} // namespace oreslang::runtime
