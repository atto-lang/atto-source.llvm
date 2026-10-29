#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// HOST-ONLY reference substrate. Actor identity must come from an authenticated
// scheduler turn, never untrusted guest-supplied integers. This pool is neither
// a tenant sandbox nor an actor mailbox. No guest/native pointer escapes.
struct TrustedBufferActor {
    std::uint64_t id = 0;
    std::uint64_t incarnation = 0;
    friend bool operator==(TrustedBufferActor a, TrustedBufferActor b) noexcept {
        return a.id == b.id && a.incarnation == b.incarnation;
    }
};

// A pool belongs to exactly one tenant incarnation. Movement is zero-payload-
// copy: the same vector allocation changes exclusive actor ownership.
// Cross-pool/cross-tenant requests, stale generations and active loans fail.
class TrustedActorBufferPool final {
    struct Slot final {
        explicit Slot(std::size_t bytes, TrustedBufferActor actor, std::uint64_t key)
            : data(bytes, 0), owner(actor), id(key) {}
        std::vector<std::uint8_t> data;
        TrustedBufferActor owner;
        std::uint64_t id = 0;
        std::uint64_t generation = 1;
        std::size_t read_loans = 0;
        // Pin both the owner and EACH reader's actor incarnation until release.
        // Retiring a reader early must not leave its old ticket usable.
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> reader_pins;
    };
    struct State final {
        State(std::uint64_t epoch, std::size_t bytes, std::size_t slots)
            : tenant_epoch(epoch), byte_limit(bytes), slot_limit(slots) {}
        std::mutex mutex;
        const std::uint64_t tenant_epoch;
        const std::size_t byte_limit;
        const std::size_t slot_limit;
        std::size_t used = 0;
        std::uint64_t next_id = 1;
        bool retired = false;
        bool scheduler_claimed = false;
        std::unordered_map<std::uint64_t, std::shared_ptr<Slot>> slots;
    };

public:
    class Handle final {
        friend class TrustedActorBufferPool;
        std::weak_ptr<State> state_;
        std::weak_ptr<Slot> slot_;
        std::uint64_t id_ = 0;
        std::uint64_t generation_ = 0;
        Handle(const std::shared_ptr<State>& state, const std::shared_ptr<Slot>& slot)
            : state_(state), slot_(slot), id_(slot->id), generation_(slot->generation) {}
    public:
        Handle() = default;
    };

    class ReadLoan final {
        friend class TrustedActorBufferPool;
        std::shared_ptr<State> state_;
        std::shared_ptr<Slot> slot_;
        std::uint64_t generation_ = 0;
        TrustedBufferActor reader_{};
        bool active_ = false;
        ReadLoan(std::shared_ptr<State> state, std::shared_ptr<Slot> slot,
                 TrustedBufferActor reader)
            : state_(std::move(state)), slot_(std::move(slot)),
              generation_(slot_->generation), reader_(reader), active_(true) {}
    public:
        ReadLoan(const ReadLoan&) = delete;
        ReadLoan& operator=(const ReadLoan&) = delete;
        ReadLoan(ReadLoan&& other) noexcept
            : state_(std::move(other.state_)), slot_(std::move(other.slot_)),
              generation_(other.generation_), reader_(other.reader_),
              active_(std::exchange(other.active_, false)) {}
        ReadLoan& operator=(ReadLoan&&) = delete;
        ~ReadLoan() { release(); }

        std::uint8_t read(TrustedBufferActor reader, std::size_t offset) const {
            if (!(reader == reader_)) throw std::logic_error("loan reader mismatch");
            if (!state_) throw std::logic_error("loan is moved");
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (!active_ || state_->retired || slot_->generation != generation_)
                throw std::logic_error("loan is revoked");
            if (offset >= slot_->data.size()) throw std::out_of_range("loan offset");
            return slot_->data[offset];
        }
        void release() noexcept {
            if (!active_) return;
            std::lock_guard<std::mutex> guard(state_->mutex);
            const auto it = slot_->reader_pins.find({reader_.id, reader_.incarnation});
            if (it != slot_->reader_pins.end()) {
                if (--it->second == 0) slot_->reader_pins.erase(it);
            }
            --slot_->read_loans;
            active_ = false;
        }
    };

    TrustedActorBufferPool(std::uint64_t tenant_epoch, std::size_t max_bytes,
                           std::size_t max_slots)
        : state_(std::make_shared<State>(tenant_epoch, max_bytes, max_slots)) {
        if (!tenant_epoch || !max_bytes || !max_slots)
            throw std::invalid_argument("positive tenant incarnation and budgets required");
    }
    TrustedActorBufferPool(const TrustedActorBufferPool&) = delete;
    TrustedActorBufferPool& operator=(const TrustedActorBufferPool&) = delete;
    ~TrustedActorBufferPool() { retire(); }

    // One trusted scheduler control plane per tenant pool. A second gate
    // could otherwise close/revoke another gate's actor-owned bytes.
    void claim_scheduler_owner() {
        std::lock_guard<std::mutex> guard(state_->mutex);
        require_live(*state_);
        if (state_->scheduler_claimed)
            throw std::logic_error("buffer pool already has a scheduler gate");
        state_->scheduler_claimed = true;
    }

    Handle allocate(TrustedBufferActor actor, std::size_t bytes) {
        validate_actor(actor);
        auto state = state_;
        std::lock_guard<std::mutex> guard(state->mutex);
        require_live(*state);
        if (!bytes || bytes > state->byte_limit - state->used ||
            state->slots.size() >= state->slot_limit)
            throw std::bad_alloc();
        if (state->next_id == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("buffer slot identities exhausted");
        auto slot = std::make_shared<Slot>(bytes, actor, state->next_id);
        const auto h = Handle(state, slot);
        state->slots.emplace(state->next_id, std::move(slot));
        ++state->next_id;
        state->used += bytes;
        return h;
    }

    std::uint8_t read(const Handle& h, TrustedBufferActor actor, std::size_t offset) const {
        validate_actor(actor);
        std::lock_guard<std::mutex> guard(state_->mutex);
        const auto slot = checked(h, actor);
        if (offset >= slot->data.size()) throw std::out_of_range("buffer offset");
        return slot->data[offset];
    }

    void write(const Handle& h, TrustedBufferActor actor,
               std::size_t offset, std::uint8_t value) {
        validate_actor(actor);
        std::lock_guard<std::mutex> guard(state_->mutex);
        const auto slot = checked(h, actor);
        if (slot->read_loans) throw std::logic_error("active immutable read loans");
        if (offset >= slot->data.size()) throw std::out_of_range("buffer offset");
        slot->data[offset] = value;
    }

    // This is a metadata-only ownership move: no payload copying or allocating.
    // Mailbox reservation/recipient validation must precede this commit.
    Handle take(const Handle& h, TrustedBufferActor from, TrustedBufferActor to) {
        validate_actor(from);
        validate_actor(to);
        if (from == to) throw std::logic_error("move requires distinct actors");
        std::lock_guard<std::mutex> guard(state_->mutex);
        const auto slot = checked(h, from);
        if (slot->read_loans) throw std::logic_error("cannot move a borrowed buffer");
        if (slot->generation == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("buffer generation exhausted");
        slot->owner = to;
        ++slot->generation; // invalidates all sender-handle aliases
        return Handle(state_, slot);
    }

    // Read-only cross-actor loan inside a trusted tenant; no mutable alias.
    // No pointer/span is exposed. Leases must not cross guest suspension
    // without a scheduler-enforced lifetime pin.
    ReadLoan borrow_read(const Handle& h, TrustedBufferActor owner,
                         TrustedBufferActor reader) {
        validate_actor(reader);
        validate_actor(owner);
        std::lock_guard<std::mutex> guard(state_->mutex);
        const auto slot = checked(h, owner);
        if (slot->read_loans == std::numeric_limits<std::size_t>::max())
            throw std::overflow_error("read loan counter exhausted");
        const auto key = std::make_pair(reader.id, reader.incarnation);
        auto pin = slot->reader_pins.find(key);
        if (pin == slot->reader_pins.end())
            slot->reader_pins.emplace(key, 1);
        else {
            if (pin->second == std::numeric_limits<std::size_t>::max())
                throw std::overflow_error("reader loan counter exhausted");
            ++pin->second;
        }
        ++slot->read_loans;
        return ReadLoan(state_, slot, reader);
    }

    void drop(const Handle& h, TrustedBufferActor owner) {
        validate_actor(owner);
        std::lock_guard<std::mutex> guard(state_->mutex);
        const auto slot = checked(h, owner);
        if (slot->read_loans) throw std::logic_error("cannot drop a borrowed buffer");
        const auto bytes = slot->data.size();
        std::fill(slot->data.begin(), slot->data.end(), 0); // best effort, not secure erasure
        state_->slots.erase(slot->id);
        state_->used -= bytes;
    }

    // Trusted scheduler retirement preflight: loan pins cannot be silently
    // invalidated by an actor incarnation's normal retirement.
    void require_actor_quiescent(TrustedBufferActor actor) const {
        validate_actor(actor);
        std::lock_guard<std::mutex> guard(state_->mutex);
        require_live(*state_);
        for (const auto& entry : state_->slots)
            if ((entry.second->owner == actor && entry.second->read_loans) ||
                entry.second->reader_pins.count({actor.id, actor.incarnation}))
                throw std::logic_error("actor still has immutable buffer loans");
    }

    // Called only after scheduler quiescence and mailbox closure. All
    // receiver-owned outstanding buffers are reclaimed, not just queued DATA.
    void reclaim_actor(TrustedBufferActor actor) {
        validate_actor(actor);
        std::lock_guard<std::mutex> guard(state_->mutex);
        require_live(*state_);
        for (const auto& entry : state_->slots)
            if ((entry.second->owner == actor && entry.second->read_loans) ||
                entry.second->reader_pins.count({actor.id, actor.incarnation}))
                throw std::logic_error("actor still has immutable buffer loans");
        for (auto it = state_->slots.begin(); it != state_->slots.end();) {
            const auto& slot = it->second;
            if (!(slot->owner == actor)) { ++it; continue; }
            std::fill(slot->data.begin(), slot->data.end(), 0);
            state_->used -= slot->data.size();
            it = state_->slots.erase(it);
        }
    }

    // Called by scheduler shutdown before retiring the pool. A live loan
    // pins both its owner and reader, even when neither has an active turn.
    bool has_active_loans() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        for (const auto& entry : state_->slots)
            if (entry.second->read_loans) return true;
        return false;
    }

    std::size_t used_bytes() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->used;
    }

    void retire() noexcept {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->retired) return;
        state_->retired = true;
        // Outstanding loans keep slot storage physically alive, but further
        // reads fail. This is a logical revocation, NOT secure page erasure.
        for (auto& entry : state_->slots)
            std::fill(entry.second->data.begin(), entry.second->data.end(), 0);
        state_->slots.clear();
        state_->used = 0;
    }

private:
    static void validate_actor(TrustedBufferActor actor) {
        if (!actor.id || !actor.incarnation)
            throw std::invalid_argument("invalid authenticated actor identity");
    }
    static void require_live(const State& state) {
        if (state.retired) throw std::logic_error("buffer pool is retired");
    }
    std::shared_ptr<Slot> checked(const Handle& h, TrustedBufferActor actor) const {
        require_live(*state_);
        if (h.state_.lock() != state_) throw std::logic_error("foreign tenant buffer");
        const auto slot = h.slot_.lock();
        if (!slot || h.id_ != slot->id || h.generation_ != slot->generation)
            throw std::logic_error("stale buffer handle");
        const auto it = state_->slots.find(h.id_);
        if (it == state_->slots.end() || it->second != slot)
            throw std::logic_error("buffer already dropped");
        if (!(slot->owner == actor)) throw std::logic_error("actor ownership mismatch");
        return slot;
    }
    std::shared_ptr<State> state_;
};

} // namespace oreslang::runtime
