#pragma once

// Host-only AttoLang request-local context. Logical request identity is NOT
// physical pthread TLS. Compiler-produced .atto source lowering, TaskLocal,
// ActorLocal, and guest-visible capabilities are separate release gates.

#include "oreslang/runtime/LocalValueAdmissionV1.hpp"
#include "oreslang/runtime/LocalPayloadBudgetV1.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/TaskCancellation.hpp"

#include <atomic>
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
        return open_ && !cancellation_->is_cancelled();
    }

    // Trusted-ingress one-shot binding to scheduler-minted original DATA mail.
    // This identity is immutable once set; neither a later pooled-actor mail
    // nor a wake callback may silently rebind an existing request.
    bool bind_original_mail_once(std::uint64_t admitted_mail) {
        if (!admitted_mail) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || cancellation_->is_cancelled()) return false;
        if (!original_mail_sequence_) original_mail_sequence_ = admitted_mail;
        return original_mail_sequence_ == admitted_mail;
    }

    std::size_t used_bytes() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return used_bytes_;
    }

    template <typename T>
    bool set(const RequestLocalKeyV1<T>& key, T value) {
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
        std::shared_ptr<CellBase> previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || cancellation_->is_cancelled()) return false;
            auto found = slots_.find(key.id_);
            const std::size_t old = found == slots_.end() ? 0 : found->second->bytes;
            if ((found == slots_.end() && slots_.size() >= max_slots) ||
                bytes > max_bytes - (used_bytes_ - old)) return false;
            if (found == slots_.end()) {
                slots_.emplace(key.id_, std::move(next));
            } else {
                previous.swap(found->second);
                found->second = std::move(next);
            }
            used_bytes_ = used_bytes_ - old + bytes;
        }
        previous.reset(); // reentrant destructors never run with the scope locked
        return true;
    }

    template <typename T>
    std::optional<T> get_copy(const RequestLocalKeyV1<T>& key) const {
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
        // An already-authorized read may return its pinned snapshot after a
        // concurrent close. It MUST NOT expose a borrowed mutable reference.
        return pinned->value;
    }

    // Explicit per-key retirement frees one quota slot. A retired value's
    // destructor must not execute under the request lock, since it may
    // synchronously close/cancel/inspect this same request.
    template <typename T>
    bool erase(const RequestLocalKeyV1<T>& key) {
        std::shared_ptr<CellBase> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || cancellation_->is_cancelled())
                return false;
            const auto found = slots_.find(key.id_);
            if (found == slots_.end())
                return false;
            retired = std::move(found->second);
            used_bytes_ -= retired->bytes;
            slots_.erase(found);
        }
        retired.reset();
        return true;
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

    RequestContextLocalV1(RequestIdentityV1 identity,
                          TaskCancellation::Ptr cancellation)
        : identity_(identity), cancellation_(std::move(cancellation)) {}

    const RequestIdentityV1 identity_;
    const TaskCancellation::Ptr cancellation_;
    mutable std::mutex mutex_;
    bool open_ = true;
    std::uint64_t original_mail_sequence_ = 0;
    std::size_t used_bytes_ = 0;
    std::unordered_map<std::uint64_t, std::shared_ptr<CellBase>> slots_;
    TaskCancellation::Subscription revocation_;
};

} // namespace oreslang::runtime
