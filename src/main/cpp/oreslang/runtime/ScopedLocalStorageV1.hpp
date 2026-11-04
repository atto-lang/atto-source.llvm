#pragma once

// HOST-ONLY foundation for AttoLang ActorLocal<T>, TaskLocal<T>, ThreadLocal<T>.
// This is NOT guest syntax/lowering, an authenticated scheduler dispatch, an
// AOT key-descriptor ABI, or a security/tenant boundary. RequestLocal already
// has its separate RequestContextLocalV1 implementation. The caller MUST
// supply identity from the scheduler's authenticated logical execution lease,
// never from guest arguments, callback provenance or an OS thread-local.
//
// This host substrate bounds SLOT COUNT only. It dynamically allocates, is
// unsuitable for a sealed hard-real-time turn, and must not accept guest
// owned/raw capabilities without compiler Sendable and ownership checks.

#include <atomic>
#include <cstddef>
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

enum class ScopedLocalScopeV1 { Actor, Task, Thread };

// Struct values are identity metadata, NOT bearer capabilities. Production
// lowering must authenticate this identity on each dispatch before calling
// any store operation. Exact generation and tenant/isolate components matter.
struct ScopedLocalIdentityV1 {
    std::uint64_t runtime = 0;
    std::uint64_t tenant = 0;
    std::uint64_t isolate = 0;
    std::uint64_t owner = 0;
    std::uint64_t generation = 0;

    bool valid() const noexcept {
        return runtime && tenant && isolate && owner && generation;
    }
    bool equals(const ScopedLocalIdentityV1& other) const noexcept {
        return runtime == other.runtime && tenant == other.tenant &&
               isolate == other.isolate && owner == other.owner &&
               generation == other.generation;
    }
};

namespace scoped_local_detail_v1 {
inline std::uint64_t next_key() {
    static std::atomic<std::uint64_t> serial{1};
    auto current = serial.load(std::memory_order_relaxed);
    while (current != 0 && current != std::numeric_limits<std::uint64_t>::max()) {
        if (serial.compare_exchange_weak(current, current + 1,
                std::memory_order_relaxed, std::memory_order_relaxed))
            return current;
    }
    throw std::overflow_error("scoped_local_key_exhausted");
}

struct CellBase {
    virtual ~CellBase() = default;
};
template <class T> struct Cell final : CellBase {
    explicit Cell(T item) : value(std::move(item)) {}
    const T value;
};

template <class T> constexpr bool local_value_allowed_v =
    std::is_copy_constructible_v<T> && !std::is_pointer_v<T> &&
    !std::is_reference_v<T>;
}

template <class T, ScopedLocalScopeV1 Scope>
class ScopedLocalKeyV1 final {
    static_assert(scoped_local_detail_v1::local_value_allowed_v<T>,
        "scoped local snapshots must be copyable values, not raw pointers/references");
public:
    ScopedLocalKeyV1() : id_(scoped_local_detail_v1::next_key()) {}
    ScopedLocalKeyV1(const ScopedLocalKeyV1&) = default;
    ScopedLocalKeyV1& operator=(const ScopedLocalKeyV1&) = delete;
    std::uint64_t host_key_id_for_diagnostics() const noexcept { return id_; }
private:
    template <ScopedLocalScopeV1> friend class LogicalLocalStoreV1;
    friend class PhysicalThreadLocalStoreV1;
    const std::uint64_t id_;
};

template <class T> using ActorLocalKeyV1 = ScopedLocalKeyV1<T, ScopedLocalScopeV1::Actor>;
template <class T> using TaskLocalKeyV1 = ScopedLocalKeyV1<T, ScopedLocalScopeV1::Task>;
template <class T> using ThreadLocalKeyV1 = ScopedLocalKeyV1<T, ScopedLocalScopeV1::Thread>;

// Explicit per-owner store, never a process-global map or thread-local
// carrier context. One instance belongs to exactly one actor incarnation or
// logical task generation. Its physical lifetime is owned by that runtime.
template <ScopedLocalScopeV1 Scope>
class LogicalLocalStoreV1 final {
    static_assert(Scope == ScopedLocalScopeV1::Actor ||
                  Scope == ScopedLocalScopeV1::Task,
                  "ThreadLocal storage must use physical thread slots");
    using Base = scoped_local_detail_v1::CellBase;
    template <class T> using Cell = scoped_local_detail_v1::Cell<T>;
public:
    static constexpr std::size_t max_slots = 32;
    explicit LogicalLocalStoreV1(ScopedLocalIdentityV1 identity)
        : owner_(identity) {
        if (!owner_.valid()) throw std::invalid_argument("invalid scoped local owner");
    }
    LogicalLocalStoreV1(const LogicalLocalStoreV1&) = delete;
    LogicalLocalStoreV1& operator=(const LogicalLocalStoreV1&) = delete;

    // Caller is a scheduler-authenticated logical context, NOT an identity
    // invented from a guest value. This predicate is necessary, not sufficient,
    // for guest authorization until the compiler+runtime dispatch bridge lands.
    bool active_for(const ScopedLocalIdentityV1& caller) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_ && owner_.equals(caller);
    }

    template <class T>
    bool set(const ScopedLocalKeyV1<T, Scope>& key,
             const ScopedLocalIdentityV1& caller, T value) {
        auto next = std::make_shared<Cell<T>>(std::move(value));
        std::shared_ptr<Base> replaced;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || !owner_.equals(caller)) return false;
            const auto found = slots_.find(key.id_);
            if (found == slots_.end()) {
                if (slots_.size() >= max_slots) return false;
                slots_.emplace(key.id_, std::move(next));
            } else {
                replaced.swap(found->second);
                found->second = std::move(next);
            }
        }
        // Do not run arbitrary T destructors while the store lock is held.
        replaced.reset();
        return true;
    }

    template <class T>
    std::optional<T> get_copy(const ScopedLocalKeyV1<T, Scope>& key,
                              const ScopedLocalIdentityV1& caller) const {
        std::shared_ptr<Base> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || !owner_.equals(caller)) return std::nullopt;
            const auto found = slots_.find(key.id_);
            if (found == slots_.end()) return std::nullopt;
            snapshot = found->second;
        }
        // Never call a guest-supplied copy constructor under the store mutex;
        // the immutable Cell remains alive across concurrent replacement.
        T value = std::static_pointer_cast<const Cell<T>>(snapshot)->value;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || !owner_.equals(caller)) return std::nullopt;
        }
        return value;
    }

    // Physical owner retirement must wait for outstanding child Tasks and
    // borrowed references before reclaiming external actor/request arenas.
    bool close() noexcept {
        std::unordered_map<std::uint64_t, std::shared_ptr<Base>> old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_) return false;
            open_ = false;
            old.swap(slots_);
        }
        old.clear();
        return true;
    }

    ~LogicalLocalStoreV1() { close(); }
private:
    const ScopedLocalIdentityV1 owner_;
    mutable std::mutex mutex_;
    bool open_ = true;
    std::unordered_map<std::uint64_t, std::shared_ptr<Base>> slots_;
};

using ActorLocalStoreV1 = LogicalLocalStoreV1<ScopedLocalScopeV1::Actor>;
using TaskLocalStoreV1 = LogicalLocalStoreV1<ScopedLocalScopeV1::Task>;

// Real OS-thread storage, NOT a logical actor/task/request store. Production
// guest use must be compile-time restricted to thread-pinned/native effects;
// an actor worker reused for another tenant MUST clear this dedicated bucket.
class PhysicalThreadLocalStoreV1 final {
    using Base = scoped_local_detail_v1::CellBase;
    template <class T> using Cell = scoped_local_detail_v1::Cell<T>;
    static std::unordered_map<std::uint64_t, std::shared_ptr<Base>>& slots() {
        static thread_local std::unordered_map<std::uint64_t,
                                               std::shared_ptr<Base>> local;
        return local;
    }
public:
    static constexpr std::size_t max_slots = 32;
    template <class T>
    static bool set(const ThreadLocalKeyV1<T>& key, T value) {
        auto next = std::make_shared<Cell<T>>(std::move(value));
        auto& entries = slots();
        auto it = entries.find(key.id_);
        if (it == entries.end()) {
            if (entries.size() >= max_slots) return false;
            entries.emplace(key.id_, std::move(next));
        } else {
            auto previous = std::move(it->second);
            it->second = std::move(next);
            previous.reset();
        }
        return true;
    }
    template <class T>
    static std::optional<T> get_copy(const ThreadLocalKeyV1<T>& key) {
        const auto& entries = slots();
        const auto it = entries.find(key.id_);
        if (it == entries.end()) return std::nullopt;
        return std::static_pointer_cast<const Cell<T>>(it->second)->value;
    }
    static void clear_current_thread() {
        std::unordered_map<std::uint64_t, std::shared_ptr<Base>> old;
        old.swap(slots());
        old.clear(); // not on an arbitrary actor-callback lane
    }
};

} // namespace oreslang::runtime
