#pragma once

// Host-only AttoLang request-local context. Logical request identity is NOT
// physical pthread TLS. Compiler-produced .atto source lowering, TaskLocal,
// ActorLocal, and guest-visible capabilities are separate release gates.

#include "oreslang/runtime/LocalValueAdmissionV1.hpp"
#include "oreslang/runtime/LocalPayloadBudgetV1.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/TaskCancellation.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace oreslang::runtime {

struct RequestIdentityV1 {
    std::uint64_t tenant = 0;
    std::uint64_t isolate = 0;
    std::uint64_t request = 0;
    std::uint64_t generation = 0;
    OresNativeActorRef actor{};
    bool valid() const noexcept {
        return tenant && isolate && request && generation &&
               actor.scheduler_domain && actor.generation;
    }
};

// Runtime-minted key identity. Keys cannot be forged from guest integers, and
// each key is tied to its original C++ T by the type system. This does not
// authorize arbitrary guest values: compiler ownership/Send checks are still
// required before exposing this API to AttoLang source.
inline std::uint64_t allocate_request_local_key_v1() {
    static std::atomic<std::uint64_t> next{1};
    std::uint64_t value = next.load(std::memory_order_relaxed);
    while (value != 0 && value != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(
                value, value + 1, std::memory_order_relaxed,
                std::memory_order_relaxed))
            return value;
    }
    throw std::overflow_error("request_local_key_exhausted");
}

template <typename T>
class RequestLocalKeyV1 final {
    static_assert(local_value_allowed_v1<T>,
                  "RequestLocal requires owned copyable values with "
                  "nothrow cleanup; raw references and borrowed views denied");
public:
    RequestLocalKeyV1() : id_(allocate_request_local_key_v1()) {}
    RequestLocalKeyV1(const RequestLocalKeyV1&) = default;
    RequestLocalKeyV1& operator=(const RequestLocalKeyV1&) = default;
private:
    friend class RequestContextLocalV1;
    const std::uint64_t id_;
};

// The host must mint request identity from trusted ingress and bind it to an
// authenticated actor turn. A request context is NOT a bearer capability:
// the fact that code has a shared_ptr does not prove guest authorization.
// Callers must verify scheduler-current-actor before exposing locals to guest.
//
// Store is bounded to 32 keys. All operations use an explicit context handle,
// never thread_local/ThreadLocal or actor-global current_request. A host Task
// continuation may retain the shared scope across physical worker migration.
// Reading returns a COPY so no mutable reference escapes across await.
class RequestContextLocalV1 final
    : public std::enable_shared_from_this<RequestContextLocalV1> {
    struct CellBase {
        explicit CellBase(std::size_t payload) : bytes(payload) {}
        virtual ~CellBase() = default;
        const std::size_t bytes;
    };
    template <typename T>
    struct Cell final : CellBase {
        Cell(T value, std::size_t payload)
            : CellBase(payload), value(std::move(value)) {}
        T value;
    };
public:
    using Ptr = std::shared_ptr<RequestContextLocalV1>;
    static constexpr std::size_t max_slots = 32;
    static constexpr std::size_t max_value_bytes = 1024;
    static constexpr std::size_t max_bytes = 8192;

    static Ptr open(RequestIdentityV1 identity,
                    TaskCancellation::Ptr cancellation) {
        if (!identity.valid() || !cancellation)
            return {};
        auto result = Ptr(new RequestContextLocalV1(
            identity, std::move(cancellation)));
        const std::weak_ptr<RequestContextLocalV1> weak = result;
        result->revocation_ = result->cancellation_->on_revoke([weak]() noexcept {
            if (auto live = weak.lock()) live->close();
        });
        return result;
    }

    RequestContextLocalV1(const RequestContextLocalV1&) = delete;
    RequestContextLocalV1& operator=(const RequestContextLocalV1&) = delete;

    ~RequestContextLocalV1() {
        close();
        revocation_.close();
    }

    RequestIdentityV1 identity() const noexcept { return identity_; }

    bool belongs_to(OresNativeActorRef actor) const noexcept {
        return actor.scheduler_domain == identity_.actor.scheduler_domain &&
               actor.actor_slot == identity_.actor.actor_slot &&
               actor.generation == identity_.actor.generation;
    }

    bool active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        // Read cancellation under the same lock as slot admission; cancellation
        // marks TaskCancellation before delivering the revocation callback.
        return cancellation_->with_active_authority([&] { return open_; });
    }

    // Trusted-ingress one-shot binding to scheduler-minted original DATA mail.
    // This identity is immutable once set; neither a later pooled-actor mail
    // nor a wake callback may silently rebind an existing request.
    bool bind_original_mail_once(std::uint64_t admitted_mail,
                                 OresNativeActorScheduler* scheduler = nullptr) {
        if (!admitted_mail) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        // This tiny mutation is serialized with cancellation and retirement
        // by the cancellation graph's authority mutex. No user callbacks.
        return cancellation_->with_active_authority([&] {
            auto commit = [&]() -> bool {
                if (!open_) return false;
                if (!original_mail_sequence_) original_mail_sequence_ = admitted_mail;
                return original_mail_sequence_ == admitted_mail;
            };
            return !scheduler ? commit()
                : ores_native_actor_scheduler_commit_if_live_v1(
                    scheduler, identity_.actor,
                    [](void* ptr) -> int {
                        return (*static_cast<decltype(commit)*>(ptr))() ? 1 : 0;
                    }, &commit) != 0;
        });
    }

    // Trusted-host predicate: a copied request handle does not authorize a
    // different DATA mail even when the pooled actor identity still matches.
    // The caller must obtain 'current_mail' from the physical scheduler,
    // never from guest source, a payload field or a previous carrier's TLS.
    bool bound_to_original_mail(std::uint64_t current_mail) const {
        if (!current_mail) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        return cancellation_->with_active_authority([&] {
            return open_ && original_mail_sequence_ == current_mail;
        });
    }

    std::size_t used_bytes() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return used_bytes_;
    }

    template <typename T>
    bool set(const RequestLocalKeyV1<T>& key, T value,
             OresNativeActorScheduler* scheduler = nullptr) {
        auto operation = begin_operation();
        if (!operation) return false;
        // Host-only measured payload limit; composite deep-size descriptors
        // and compiler ownership checks remain mandatory for guest access.
        const std::size_t bytes = local_measured_payload_bytes_v1(value);
        if (bytes > max_value_bytes) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || cancellation_->is_cancelled()) return false;
            auto found = slots_.find(key.id_);
            const std::size_t old = found == slots_.end() ? 0 : found->second->bytes;
            if ((found == slots_.end() && slots_.size() >= max_slots) ||
                bytes > max_bytes - (used_bytes_ - old)) return false;
        }
        auto next = std::make_shared<Cell<T>>(std::move(value), bytes);
        // Preallocate an owned map node outside TaskCancellation's authority
        // mutex. Every scope reserves all 32 bucket positions at open, so
        // node insertion cannot rehash/allocate during the final commit.
        std::unordered_map<std::uint64_t, std::shared_ptr<CellBase>> staged;
        staged.emplace(key.id_, next);
        auto staged_node = staged.extract(key.id_);
        std::shared_ptr<CellBase> previous;
        bool committed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Linearization is at this final slot mutation. A cancellation
            // that acquires graph authority first denies the commit; an
            // earlier commit finishes before cancellation can become visible.
            // No guest copy/move/dtor or user callback runs under graph lock.
            committed = cancellation_->with_active_authority([&] {
                auto commit = [&]() -> bool {
                    if (!open_) return false;
                    auto found = slots_.find(key.id_);
                    const std::size_t old =
                        found == slots_.end() ? 0 : found->second->bytes;
                    if ((found == slots_.end() && slots_.size() >= max_slots) ||
                        bytes > max_bytes - (used_bytes_ - old)) return false;
                    if (found == slots_.end()) {
                        // Node and bucket array allocated before the gate.
                        slots_.insert(std::move(staged_node));
                    } else {
                        previous.swap(found->second);
                        found->second = std::move(next);
                    }
                    used_bytes_ = used_bytes_ - old + bytes;
                    return true;
                };
                // TRUSTED ADAPTER ONLY: this bounded commit executes while
                // scheduler STOP/retire holds the same lifecycle mutex.
                // Compiler/guest APIs must always supply the scheduler.
                return !scheduler ? commit()
                    : ores_native_actor_scheduler_commit_if_live_v1(
                        scheduler, identity_.actor,
                        [](void* ptr) -> int {
                            return (*static_cast<decltype(commit)*>(ptr))() ? 1 : 0;
                        }, &commit) != 0;
            });
        }
        previous.reset(); // reentrant destructors never run with the scope locked
        return committed;
    }

    template <typename T>
    std::optional<T> get_copy(const RequestLocalKeyV1<T>& key) const {
        auto operation = begin_operation();
        if (!operation) return std::nullopt;
        std::shared_ptr<const Cell<T>> pinned;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || cancellation_->is_cancelled())
                return std::nullopt;
            const auto found = slots_.find(key.id_);
            if (found == slots_.end())
                return std::nullopt;
            // Unique typed key IDs are host minted, never guest supplied.
            // Keep the original cell alive while a user-defined T copy runs
            // after dropping our mutex. A reentrant copy can close/cancel,
            // replace or erase the request without invalidating this source.
            pinned = std::static_pointer_cast<const Cell<T>>(found->second);
        }
        // User-defined copies run outside BOTH locks. After the copy, a
        // reentrant close, cancellation, retirement or teardown invalidates
        // the snapshot instead of releasing it as an authorized result.
        T snapshot = pinned->value;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!cancellation_->with_active_authority(
                    [&] { return open_; }))
                return std::nullopt;
        }
        return snapshot;
    }

    // Explicit per-key retirement frees one quota slot. A retired value's
    // destructor must not execute under the request lock, since it may
    // synchronously close/cancel/inspect this same request.
    template <typename T>
    bool erase(const RequestLocalKeyV1<T>& key,
               OresNativeActorScheduler* scheduler = nullptr) {
        auto operation = begin_operation();
        if (!operation) return false;
        std::shared_ptr<CellBase> retired;
        std::unordered_map<std::uint64_t,
            std::shared_ptr<CellBase>>::node_type retired_node;
        bool committed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Deletion and quota return linearize against the graph's
            // cancellation and retirement state, without running T::~T.
            committed = cancellation_->with_active_authority([&] {
                auto commit = [&]() -> bool {
                    if (!open_) return false;
                    const auto found = slots_.find(key.id_);
                    if (found == slots_.end()) return false;
                    retired = std::move(found->second);
                    used_bytes_ -= retired->bytes;
                    // No deallocation under graph/scheduler lifecycle lock.
                    retired_node = slots_.extract(found);
                    return true;
                };
                return !scheduler ? commit()
                    : ores_native_actor_scheduler_commit_if_live_v1(
                        scheduler, identity_.actor,
                        [](void* ptr) -> int {
                            return (*static_cast<decltype(commit)*>(ptr))() ? 1 : 0;
                        }, &commit) != 0;
            });
        }
        retired.reset();
        retired_node = {}; // release map allocation AFTER all locks
        return committed;
    }

    bool close() noexcept {
        std::unordered_map<std::uint64_t, std::shared_ptr<CellBase>> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_) return false;
            open_ = false;
            retired.swap(slots_);
            used_bytes_ = 0;
        }
        // Host-owned request locals are destroyed after the closing fence and
        // outside the lock. Caller must fence child Tasks and pinned borrows
        // before dropping request-owned arenas or other external resources.
        retired.clear();
        return true;
    }

private:
    // Cross-actor handoff links a recipient's derived context to this
    // request's cancellation (never the reverse) and needs read access.
    friend class RequestContextHandoffTableV1;
    friend class RequestLocalStopGroupV1;

    // Host-owned operation leases are exact-scope and cannot be copied.
    // They pin the logical operation during allocation/value copying outside
    // the store lock, even if STOP marks the scope closed concurrently.
    class OperationLease final {
    public:
        explicit OperationLease(const RequestContextLocalV1* owner) noexcept
            : owner_(owner) {}
        OperationLease(const OperationLease&) = delete;
        OperationLease& operator=(const OperationLease&) = delete;
        OperationLease(OperationLease&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)) {}
        OperationLease& operator=(OperationLease&&) = delete;
        ~OperationLease() {
            if (owner_) owner_->release_operation();
        }
    private:
        const RequestContextLocalV1* owner_;
    };

    std::optional<OperationLease> begin_operation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cancellation_->with_active_authority([&] {
                if (!open_ ||
                    in_flight_ == std::numeric_limits<std::size_t>::max())
                    return false;
                ++in_flight_;
                return true;
            })) return std::nullopt;
        return std::optional<OperationLease>(std::in_place, this);
    }

    void release_operation() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (in_flight_ == 0) std::terminate();
        if (--in_flight_ == 0) quiescent_.notify_all();
    }

    // CONTROL-plane only, after close(): waiting here on a carrier would
    // deadlock if the actor's continuation owns the outstanding operation.
    bool wait_closed_quiescent() const {
        std::unique_lock<std::mutex> lock(mutex_);
        if (open_) return false;
        quiescent_.wait(lock, [&] { return in_flight_ == 0; });
        return true;
    }

    RequestContextLocalV1(RequestIdentityV1 identity,
                          TaskCancellation::Ptr cancellation)
        : identity_(identity), cancellation_(std::move(cancellation)) {
        // Fixed maximum slot count, reserved before request admission. This
        // excludes hash-table allocation/rehashing from the cancellation
        // graph's locked commit path.
        slots_.reserve(max_slots);
    }

    const RequestIdentityV1 identity_;
    const TaskCancellation::Ptr cancellation_;
    mutable std::mutex mutex_;
    bool open_ = true;
    mutable std::size_t in_flight_ = 0;
    mutable std::condition_variable quiescent_;
    std::uint64_t original_mail_sequence_ = 0;
    std::size_t used_bytes_ = 0;
    std::unordered_map<std::uint64_t, std::shared_ptr<CellBase>> slots_;
    TaskCancellation::Subscription revocation_;
};

} // namespace oreslang::runtime
