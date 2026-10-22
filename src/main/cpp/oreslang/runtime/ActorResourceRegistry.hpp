#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <exception>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::runtime {

class NativeActorResourceQuarantine;

// Trusted native runtime substrate.  No raw resource or callback may be sent
// across a guest actor boundary.  A registry is owned by one actor incarnation.
class ActorResourceRegistry final {
public:
    enum class Kind { socket, file, timer, ffi_callback, native_allocation };
    enum class Phase { live, retiring, closing, reclaimed, failed };
    enum class Reclaim { not_retired, busy, closing, complete, failed };
    enum class Activity { turn, loan, callback };

    class Ticket final {
    public:
        Ticket(const Ticket&) = default;
        Ticket& operator=(const Ticket&) = default;
    private:
        friend class ActorResourceRegistry;
        Ticket(std::uint64_t actor, std::uint64_t generation,
               std::uint64_t sequence, Activity role)
            : actor_id(actor), incarnation(generation), nonce(sequence),
              activity(role) {}
        std::uint64_t actor_id;
        std::uint64_t incarnation;
        std::uint64_t nonce;
        Activity activity;
    };

    struct Result {
        Reclaim status;
        // Number of closers that returned successfully. Not the number attempted.
        std::size_t closed;
        std::vector<std::string> errors;
        // Includes failures whose diagnostic text could not be allocated.
        std::size_t failures = 0;
        std::size_t attempted = 0;
        // Skipped allocations stay quarantined after unsafe revocation failure.
        std::size_t skipped = 0;
    };

    explicit ActorResourceRegistry(std::uint64_t actor_id,
                                   std::uint64_t incarnation,
                                   std::size_t max_resources = 256)
        : actor_id_(actor_id), incarnation_(incarnation), capacity_(max_resources) {
        if (!actor_id || !incarnation || max_resources == 0 || max_resources > 4096)
            throw std::invalid_argument("invalid actor resource registry identity/capacity");
        entries_.reserve(max_resources);
    }

    // Releasing a FAILED registry's callback captures can trigger native
    // RAII reclamation while an unrevoked FFI callback still references them.
    // Destructor fail-closes by retaining unresolved captures for process
    // lifetime. The trusted quarantine must explicitly discharge after
    // externally verified isolate shutdown; a timeout is not sufficient.
    ~ActorResourceRegistry() noexcept;

    ActorResourceRegistry(const ActorResourceRegistry&) = delete;
    ActorResourceRegistry& operator=(const ActorResourceRegistry&) = delete;

    std::uint64_t actor_id() const noexcept { return actor_id_; }
    std::uint64_t incarnation() const noexcept { return incarnation_; }

    // Host-only registration. Callbacks must be trusted, idempotent close or
    // revocation actions. Guest functions and blocking I/O are not admitted.
    void register_resource(Kind kind, std::function<void()> close) {
        if (!close) throw std::invalid_argument("resource closer is required");
        std::lock_guard<std::mutex> guard(mutex_);
        require_live();
        if (entries_.size() >= capacity_)
            throw std::length_error("actor resource registry capacity exceeded");
        entries_.push_back({kind, std::move(close)});
    }

    // Scheduler-only tokens. Tokens are observations of outstanding activity,
    // never authority to dereference actor memory on foreign threads.
    Ticket acquire_turn() { return acquire(turns_, Activity::turn); }
    Ticket acquire_loan() { return acquire(loans_, Activity::loan); }
    Ticket acquire_callback() { return acquire(callbacks_, Activity::callback); }
    void release_turn(Ticket token) { release(turns_, Activity::turn, token); }
    void release_loan(Ticket token) { release(loans_, Activity::loan, token); }
    void release_callback(Ticket token) { release(callbacks_, Activity::callback, token); }

    void retire() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ == Phase::live) phase_ = Phase::retiring;
    }

    Phase phase() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return phase_;
    }

    // Terminal failures retain captured native ownership in the registry.
    // The supervisor MUST keep the failed registry alive in its quarantine
    // until resource-specific safety/revocation can be established.
    std::size_t quarantined_resources() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return quarantined_.size();
    }

    // Physical reclamation must only follow scheduler/FFI quiescence.
    // Never invoke a resource closer with the registry mutex held.
    Result try_reclaim() {
        std::vector<Entry> pending;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (phase_ == Phase::live) return {Reclaim::not_retired, 0, {}};
            if (phase_ == Phase::closing) return {Reclaim::closing, 0, {}};
            if (phase_ == Phase::reclaimed || phase_ == Phase::failed) return last_result_;
            if (!turns_.empty() || !loans_.empty() || !callbacks_.empty())
                return {Reclaim::busy, 0, {}};
            phase_ = Phase::closing;
            pending.swap(entries_);
        }
        Result result{Reclaim::complete, 0, {}};
        const auto invoke = [&result](Entry& resource) {
            ++result.attempted;
            try {
                resource.close();
                ++result.closed;
            } catch (const std::exception& e) {
                ++result.failures;
                append_error(result, e.what());
            } catch (...) {
                ++result.failures;
                append_error(result, "unknown closer failure");
            }
        };
        // Revoke I/O, timers and FFI callbacks before freeing ANY arena.
        // Even if registrations are out of dependency order, a late callback
        // must never be able to touch an already released native allocation.
        for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
            if (it->kind != Kind::native_allocation) invoke(*it);
        }
        if (result.failures == 0) {
            for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
                if (it->kind != Kind::native_allocation) continue;
                if (result.failures == 0) invoke(*it);
                else ++result.skipped;  // do not free dependent arenas after failure
            }
        } else {
            // Unknown revocation outcome. Quarantine allocation ownership
            // rather than risk UAF. The supervisor records terminal failure.
            for (const auto& resource : pending) {
                if (resource.kind == Kind::native_allocation) ++result.skipped;
            }
        }
        // A throwing revoker might leave a live socket, native loan or FFI
        // callback. Do not report COMPLETE or authorize arena destruction.
        // Failure is terminal; retrying a partially run closer violates
        // exactly-once semantics. Supervisor must quarantine/reconcile.
        if (result.failures != 0) result.status = Reclaim::failed;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            // Vector move cannot allocate. Terminal failure is intentionally
            // distinct from safe quiescent reclamation. Both states prevent
            // a second close attempt and leave failure visible to supervisor.
            const bool safely_reclaimed = result.failures == 0;
            last_result_ = std::move(result);
            if (!safely_reclaimed) {
                // Preserve callback captures, including RAII ownership of
                // skipped allocations. Letting pending destruct here could
                // otherwise free supposedly quarantined native state.
                quarantined_.swap(pending);
            }
            phase_ = safely_reclaimed ? Phase::reclaimed : Phase::failed;
            return last_result_;
        }
    }

private:
    friend class NativeActorResourceQuarantine;

    void discharge_after_verified_isolate_shutdown() {
        std::vector<Entry> released;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (phase_ != Phase::failed) return;
            released.swap(quarantined_);
        }
        // Deallocate trusted RAII captures only after a supervisor has
        // independently proven that no external callback can reach them.
    }

    static void append_error(Result& result, const char* message) noexcept {
        if (result.errors.size() >= 16) return;
        const char* text = message != nullptr ? message : "unknown closer failure";
        std::size_t size = 0;
        while (size < 256 && text[size] != '\0') ++size;
        try {
            result.errors.emplace_back(text, size);
        } catch (...) {
            // OOM while formatting a diagnostic must never interrupt the
            // remaining closers. The integer failure count is authoritative.
        }
    }

    struct Entry {
        Kind kind;
        std::function<void()> close;
    };

    void require_live() const {
        if (phase_ != Phase::live) throw std::logic_error("actor resource registry retired");
    }

    Ticket acquire(std::unordered_set<std::uint64_t>& set, Activity role) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_live();
        if (set.size() >= 4096)
            throw std::length_error("actor activity ticket capacity exceeded");
        if (next_token_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor resource token exhausted");
        const auto nonce = ++next_token_;
        set.insert(nonce);
        return Ticket{actor_id_, incarnation_, nonce, role};
    }

    void release(std::unordered_set<std::uint64_t>& set, Activity role, Ticket token) {
        std::lock_guard<std::mutex> guard(mutex_);
        // Never allow foreign actor-incarnation or cross-role tickets to
        // release one of this actor's loans, callbacks, or turns.
        if (token.actor_id != actor_id_ || token.incarnation != incarnation_
                || token.activity != role || set.erase(token.nonce) != 1)
            throw std::logic_error("invalid or duplicate actor resource lease release");
    }

    const std::uint64_t actor_id_;
    const std::uint64_t incarnation_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    Phase phase_ = Phase::live;
    std::uint64_t next_token_ = 0;
    std::vector<Entry> entries_;
    std::vector<Entry> quarantined_;
    std::unordered_set<std::uint64_t> turns_, loans_, callbacks_;
    Result last_result_{Reclaim::not_retired, 0, {}};
};

inline ActorResourceRegistry::~ActorResourceRegistry() noexcept {
    // Losing the final registry owner is NOT proof of native quiescence. A
    // LIVE/RETIRING registry could still hold FFI callbacks or arenas, just as
    // a FAILED one could after bounded quarantine overflow. Only a verified
    // RECLAIMED registry, or a FAILED registry explicitly discharged after
    // isolate shutdown, may destroy such captures automatically.
    if (phase_ != Phase::reclaimed &&
            (!entries_.empty() || !quarantined_.empty())) {
        struct EmergencyRetention {
            std::vector<Entry> outstanding;
            std::vector<Entry> failed;
        };
        auto* retained = new (std::nothrow) EmergencyRetention();
        if (retained == nullptr) std::terminate();
        retained->outstanding.swap(entries_);
        retained->failed.swap(quarantined_);
        // Intentionally retained until process termination. Allocation
        // failure is fatal instead of attempting unsafe RAII destruction.
        // Supervisors must still own and retire registries normally; this
        // defensive fallback is not a replacement for bounded quarantine.
    }
}

} // namespace oreslang::runtime
