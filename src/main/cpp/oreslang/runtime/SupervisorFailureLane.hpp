#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace oreslang {

/**
 * Native actor-supervision CONTROL lane, independent of data mailboxes.
 * Reserve a node before admitting a child. After reservation, publication
 * writes bounded data into that node and appends an intrusive FIFO link:
 * no per-failure queue growth, heap allocation or tracing GC.
 *
 * The caller is responsible for scheduling the supervisor's logical lane.
 * This class never runs callbacks under its mutex, and is not guest lowering.
 */
class SupervisorFailureLane {
public:
    struct ActorKey {
        std::string id;
        std::uint64_t generation;

        bool operator<(const ActorKey& rhs) const {
            return std::tie(id, generation) < std::tie(rhs.id, rhs.generation);
        }
    };

    enum class Kind { Throw, Raise, Panic, Cancel, HostFailure };

    struct Failure {
        ActorKey actor;
        Kind kind;
        // Original pre-promotion effect. Terminal actor Throw/Raise is delivered
        // as Panic, but the supervisor still receives typed provenance.
        Kind origin_kind;
        // Opaque numeric error discriminator copied before the actor is reclaimed.
        // No borrowed exception pointer or actor-owned object crosses CONTROL.
        std::uint32_t origin_code = 0;
        std::string summary;
        std::string parent_id;

        Failure(ActorKey actor, Kind kind, std::string summary,
                std::string parent_id = {},
                std::optional<Kind> origin = std::nullopt,
                std::uint32_t code = 0)
            : actor(std::move(actor)), kind(kind),
              origin_kind(origin.value_or(kind)), origin_code(code),
              summary(std::move(summary)), parent_id(std::move(parent_id)) {}
    };

    struct Handler {
        virtual ~Handler() = default;
        virtual void on_throw(const Failure&) = 0;
        virtual void on_raise(const Failure&) = 0;
        virtual void on_panic(const Failure&) = 0;
        virtual void on_cancel(const Failure&) = 0;
        virtual void on_host_failure(const Failure&) = 0;
    };

    explicit SupervisorFailureLane(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("capacity must be positive");
    }

    void reserve(const ActorKey& actor) {
        // ActorId is a host-owned printable token. Bound its allocation and
        // reject control/non-ASCII bytes before map-node admission; the child
        // reservation quota alone does not limit a malicious identifier's
        // memory footprint. Actor generations may legitimately begin at zero.
        if (actor.id.empty() || actor.id.size() > 128) {
            throw std::invalid_argument("actor identity length outside 1..128 bytes");
        }
        for (const unsigned char ch : actor.id) {
            if (ch <= 0x20 || ch >= 0x7f) {
                throw std::invalid_argument("actor identity contains nonprintable or non-ASCII bytes");
            }
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (children_.size() >= capacity_) {
            throw std::logic_error("supervision slots exhausted");
        }
        auto [it, inserted] = children_.try_emplace(actor);
        if (!inserted) throw std::logic_error("duplicate supervision reservation");
        // std::map does not relocate nodes; this pointer survives until ack.
        it->second.actor = &it->first;
    }

    bool publish(Failure failure) {
        return publish_preallocated(failure.actor, failure.kind,
                                    failure.summary, failure.parent_id,
                                    failure.origin_kind, failure.origin_code);
    }

    /**
     * Allocation-free terminal publication after pre-reservation, barring
     * platform mutex/runtime behavior. Fail closed for unknown generations.
     * Does not execute handlers or acquire an ordinary mailbox lock.
     */
    bool publish_preallocated(const ActorKey& actor, Kind kind,
                              std::string_view summary,
                              std::string_view parent_id = {},
                              std::optional<Kind> origin = std::nullopt,
                              std::uint32_t origin_code = 0) {
        std::lock_guard<std::mutex> lock(mu_);
        // Validate and normalize before considering duplicate publication:
        // even a duplicate must not let a forged invalid effect slip through
        // the low-level CONTROL entrypoint unnoticed.
        switch (kind) {
            case Kind::Throw: case Kind::Raise: case Kind::Panic:
            case Kind::Cancel: case Kind::HostFailure: break;
            default: throw std::invalid_argument("invalid supervisor failure kind");
        }
        const Kind original = origin.value_or(kind);
        switch (original) {
            case Kind::Throw: case Kind::Raise: case Kind::Panic:
            case Kind::Cancel: case Kind::HostFailure: break;
            default: throw std::invalid_argument("invalid supervisor origin kind");
        }
        // Every route into actor supervision, including direct lane writers,
        // delivers unhandled Throw/Raise as Panic. Do NOT change origin_kind.
        if (kind == Kind::Throw || kind == Kind::Raise) {
            if (original != kind) {
                throw std::invalid_argument("actor terminal origin contradicts effect");
            }
            kind = Kind::Panic;
        } else if (original != kind && !(kind == Kind::Panic
                && (original == Kind::Throw || original == Kind::Raise))) {
            throw std::invalid_argument("invalid supervisor terminal/origin pair");
        }
        Slot& slot = require(actor);
        if (slot.has_event) return false;
        slot.kind = kind;
        slot.origin_kind = original;
        slot.origin_code = origin_code;
        store_utf8(slot.summary, slot.summary_length, summary);
        store_utf8(slot.parent_id, slot.parent_length, parent_id);
        slot.has_event = true;
        enqueue(slot);
        return true;
    }

    std::optional<Failure> poll() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!head_) return std::nullopt;
        Slot* slot = head_;
        if (!slot->queued || slot->in_flight || !slot->has_event) {
            throw std::logic_error("supervisor queue invariant");
        }
        // Materialize before unlink. On allocation failure, CONTROL remains
        // queued and no in-flight message becomes inaccessible.
        Failure failure{*slot->actor, slot->kind,
            std::string(slot->summary.data(), slot->summary_length),
            std::string(slot->parent_id.data(), slot->parent_length),
            slot->origin_kind, slot->origin_code};
        unlink_head();
        slot->in_flight = true;
        return failure;
    }

    void ack(const ActorKey& actor) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = children_.find(actor);
        if (it == children_.end() || !it->second.in_flight) {
            throw std::logic_error("event is not in flight");
        }
        children_.erase(it);
    }

    void retry(const ActorKey& actor) {
        std::lock_guard<std::mutex> lock(mu_);
        Slot& slot = require(actor);
        if (!slot.in_flight) throw std::logic_error("event is not in flight");
        slot.in_flight = false;
        enqueue(slot);
    }

    void retire_gracefully(const ActorKey& actor) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = children_.find(actor);
        if (it == children_.end()) throw std::logic_error("unknown/stale actor generation");
        if (it->second.has_event) throw std::logic_error("failure requires ack");
        children_.erase(it);
    }

    // This convenience dispatcher is at-most-once, not a transactional
    // callback executor. A handler may have restarted an actor or emitted
    // an escalation before throwing. Requeueing after entry would replay
    // irreversible side effects. Higher-level SupervisorControlTurn owns
    // its own explicit pre-decision retry and restart policy.
    bool dispatch_one(Handler& handler) {
        auto event = poll();
        if (!event) return false;
        try {
            switch (event->kind) {
                case Kind::Throw: handler.on_throw(*event); break;
                case Kind::Raise: handler.on_raise(*event); break;
                case Kind::Panic: handler.on_panic(*event); break;
                case Kind::Cancel: handler.on_cancel(*event); break;
                case Kind::HostFailure: handler.on_host_failure(*event); break;
            }
        } catch (...) {
            // The callback's side-effect status is unknowable at this point.
            // Retire the event before propagating the original exception.
            ack(event->actor);
            throw;
        }
        ack(event->actor);
        return true;
    }

    std::size_t reserved_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return children_.size();
    }

    std::size_t pending_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return pending_count_;
    }

private:
    struct Slot {
        const ActorKey* actor = nullptr;
        Kind kind = Kind::HostFailure;
        Kind origin_kind = Kind::HostFailure;
        std::uint32_t origin_code = 0;
        std::array<char, 512> summary{};
        std::array<char, 128> parent_id{};
        std::size_t summary_length = 0;
        std::size_t parent_length = 0;
        bool has_event = false;
        bool in_flight = false;
        bool queued = false;
        Slot* next = nullptr;
    };

    // Allocation-free UTF-8 validation and boundary-aware truncation into
    // pre-reserved CONTROL storage. Invalid bytes become ASCII '?' individually.
    // No code point, overlong encoding, surrogate or out-of-range scalar
    // crosses the actor isolation boundary.
    template <std::size_t N>
    static void store_utf8(std::array<char, N>& buffer, std::size_t& length,
                           std::string_view text) noexcept {
        length = 0;
        for (std::size_t i = 0; i < text.size();) {
            const auto first = static_cast<unsigned char>(text[i]);
            std::size_t width = 0;
            if (first <= 0x7f) width = 1;
            else if (first >= 0xc2 && first <= 0xdf) width = 2;
            else if (first >= 0xe0 && first <= 0xef) width = 3;
            else if (first >= 0xf0 && first <= 0xf4) width = 4;
            bool valid = width != 0 && width <= text.size() - i;
            if (valid) {
                for (std::size_t j = 1; j < width; ++j) {
                    if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80) {
                        valid = false;
                        break;
                    }
                }
            }
            if (valid && width == 3) {
                const auto second = static_cast<unsigned char>(text[i + 1]);
                if ((first == 0xe0 && second < 0xa0)
                        || (first == 0xed && second >= 0xa0)) valid = false;
            }
            if (valid && width == 4) {
                const auto second = static_cast<unsigned char>(text[i + 1]);
                if ((first == 0xf0 && second < 0x90)
                        || (first == 0xf4 && second > 0x8f)) valid = false;
            }
            const std::size_t bytes = valid ? width : 1;
            if (bytes > N - length) break;
            if (valid) {
                std::memcpy(buffer.data() + length, text.data() + i, bytes);
                i += bytes;
            } else {
                buffer[length] = '?';
                ++i;
            }
            length += bytes;
        }
    }

    Slot& require(const ActorKey& actor) {
        auto it = children_.find(actor);
        if (it == children_.end()) throw std::logic_error("unknown/stale actor generation");
        return it->second;
    }

    void enqueue(Slot& slot) {
        if (slot.queued || slot.in_flight) throw std::logic_error("double queue");
        slot.next = nullptr;
        slot.queued = true;
        if (tail_) tail_->next = &slot;
        else head_ = &slot;
        tail_ = &slot;
        ++pending_count_;
    }

    void unlink_head() {
        Slot* current = head_;
        head_ = current->next;
        if (!head_) tail_ = nullptr;
        current->next = nullptr;
        current->queued = false;
        --pending_count_;
    }

    const std::size_t capacity_;
    mutable std::mutex mu_;
    std::map<ActorKey, Slot> children_;
    Slot* head_ = nullptr;
    Slot* tail_ = nullptr;
    std::size_t pending_count_ = 0;
};

} // namespace oreslang
