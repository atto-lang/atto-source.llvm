#pragma once

// Host-only AttoLang request-local context. Logical request identity is NOT
// physical pthread TLS. Compiler-produced .atto source lowering, TaskLocal,
// ActorLocal, and guest-visible capabilities are separate release gates.

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/TaskCancellation.hpp"
#include "oreslang/runtime/ScopedLocalStorageV1.hpp"

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
    static_assert(std::is_copy_constructible_v<T>,
                  "request local snapshots must be safely copyable");
    static_assert(!std::is_pointer_v<T> && !std::is_reference_v<T>,
                  "borrowed pointers/references are not request locals");
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
// Store bounds 32 keys and 8 KiB of UTF-8/scalar PAYLOAD bytes, not total
// allocator overhead. All operations use an explicit context handle, never
// thread_local/ThreadLocal or actor-global current_request. The caller MUST
// authenticate ingress+source-mail origin; possession of a Ptr is insufficient.
// A host Task continuation may retain the scope across worker migration.
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
        const T value;
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

    // Trusted-ingress one-shot binding: a logical request-local context may
    // never be rebound to a later user mail on the same pooled actor. The
    // caller must supply the scheduler-issued original DATA-mail identity,
    // not guest strings or the physical carrier's previous message. This is
    // a necessary host check, NOT sufficient guest authentication by itself.
    bool bind_original_mail_once(std::uint64_t admitted_mail) {
        if (!admitted_mail) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || cancellation_->is_cancelled()) return false;
        if (!original_mail_sequence_) original_mail_sequence_ = admitted_mail;
        return original_mail_sequence_ == admitted_mail;
    }

    bool active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        // Read cancellation under the same lock as slot admission; cancellation
        // marks TaskCancellation before delivering the revocation callback.
        return open_ && !cancellation_->is_cancelled();
    }

    template <typename T>
    bool set(const RequestLocalKeyV1<T>& key, T value) {
        // Same fail-closed scalar/enum/valid-UTF-8 policy as ActorLocal and
        // TaskLocal. A trivially-copyable host struct may still carry a native
        // pointer, so reject all structs until typed AOT descriptors exist.
        const auto bytes = scoped_local_detail_v1::value_bytes(value);
        // Match Java HttpRequestLocals: 1 KiB per string/scalar value and
        // 8 KiB for the aggregate, counting UTF-8 bytes, not characters.
        if (bytes > max_value_bytes) return false;

        // Fail wrong/retired request identity and quota BEFORE allocating a
        // Cell. Recheck under mutex after allocation: STOP/cancel/replacement
        // may win concurrently, and a rejected write must not mutate quota.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || cancellation_->is_cancelled()) return false;
            auto found = slots_.find(key.id_);
            const auto old = found == slots_.end() ? 0 : found->second->bytes;
            if ((found == slots_.end() && slots_.size() >= max_slots)
                    || bytes > max_bytes - (used_bytes_ - old)) return false;
        }
        auto next = std::make_shared<Cell<T>>(std::move(value), bytes);
        std::shared_ptr<CellBase> previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || cancellation_->is_cancelled()) return false;
            auto found = slots_.find(key.id_);
            const auto old = found == slots_.end() ? 0 : found->second->bytes;
            if ((found == slots_.end() && slots_.size() >= max_slots)
                    || bytes > max_bytes - (used_bytes_ - old)) return false;
            if (found == slots_.end()) {
                slots_.emplace(key.id_, std::move(next));
            } else {
                previous.swap(found->second);
                found->second = std::move(next);
            }
            used_bytes_ = used_bytes_ - old + bytes;
        }
        // Never call guest destructor under store lock.
        previous.reset();
        return true;
    }

    template <typename T>
    std::optional<T> get_copy(const RequestLocalKeyV1<T>& key) const {
        std::shared_ptr<CellBase> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || cancellation_->is_cancelled()) return std::nullopt;
            const auto found = slots_.find(key.id_);
            if (found == slots_.end()) return std::nullopt;
            snapshot = found->second;
        }
        // Copy can allocate/throw and must not run under request mutex.
        const T copy = std::static_pointer_cast<const Cell<T>>(snapshot)->value;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Cancellation/revocation winning during copy invalidates result.
            if (!open_ || cancellation_->is_cancelled()) return std::nullopt;
        }
        return copy;
    }

    template <typename T>
    bool remove(const RequestLocalKeyV1<T>& key) {
        std::shared_ptr<CellBase> previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || cancellation_->is_cancelled()) return false;
            auto found = slots_.find(key.id_);
            if (found == slots_.end()) return false;
            used_bytes_ -= found->second->bytes;
            previous = std::move(found->second);
            slots_.erase(found);
        }
        previous.reset();
        return true;
    }

    std::size_t used_bytes() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return used_bytes_;
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
    RequestContextLocalV1(RequestIdentityV1 identity,
                          TaskCancellation::Ptr cancellation)
        : identity_(identity), cancellation_(std::move(cancellation)) {}

    const RequestIdentityV1 identity_;
    const TaskCancellation::Ptr cancellation_;
    mutable std::mutex mutex_;
    bool open_ = true;
    std::uint64_t original_mail_sequence_ = 0; // immutable once bound
    std::size_t used_bytes_ = 0;
    std::unordered_map<std::uint64_t, std::shared_ptr<CellBase>> slots_;
    TaskCancellation::Subscription revocation_;
};

} // namespace oreslang::runtime
