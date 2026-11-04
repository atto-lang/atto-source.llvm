#pragma once

// AttoLang native HOST substrate for ThreadLocal, TaskLocal and ActorLocal.
// The existing RequestContextLocalV1.hpp owns HTTP RequestLocal. None of
// these C++ APIs are guest intrinsics or grant actor/task execution authority.
// Scheduler admission must authenticate each ExecutionStamp before invoking
// a guest accessor. Compiler type/ownership/Send checks remain mandatory.

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

enum class LocalDomainV1 { Actor, Task };

struct ExecutionStampV1 {
    std::uint64_t tenant = 0;
    std::uint64_t isolate = 0;
    std::uint64_t actor_domain = 0;
    std::uint64_t actor_slot = 0;
    std::uint64_t actor_generation = 0;
    std::uint64_t task_id = 0;
    std::uint64_t task_generation = 0;

    bool valid_actor() const noexcept {
        return tenant && isolate && actor_domain && actor_generation;
    }
    bool valid_task() const noexcept {
        return valid_actor() && task_id && task_generation;
    }
    bool same_actor(const ExecutionStampV1& other) const noexcept {
        return tenant == other.tenant && isolate == other.isolate &&
               actor_domain == other.actor_domain &&
               actor_slot == other.actor_slot &&
               actor_generation == other.actor_generation;
    }
    bool same_task(const ExecutionStampV1& other) const noexcept {
        return same_actor(other) && task_id == other.task_id &&
               task_generation == other.task_generation;
    }
};

inline std::uint64_t next_local_key_id_v1() {
    static std::atomic<std::uint64_t> next{1};
    auto current = next.load(std::memory_order_relaxed);
    while (current && current < std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(current, current + 1,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed))
            return current;
    }
    throw std::overflow_error("local_key_exhausted");
}

struct LocalCellBaseV1 {
    virtual ~LocalCellBaseV1() = default;
};

template <class T>
struct LocalCellV1 final : LocalCellBaseV1 {
    explicit LocalCellV1(T input) : value(std::move(input)) {}
    T value;
};

template <class T>
constexpr bool local_value_allowed_v1 =
    std::is_copy_constructible_v<T> && !std::is_pointer_v<T> &&
    !std::is_reference_v<T>;

// Host-minted opaque typed key. Runtime/guest ABI must never expose raw IDs.
// These host type traits do NOT replace guest Send/Sync and ownership checks.
template <LocalDomainV1 Domain, class T>
class LogicalLocalKeyV1 final {
    static_assert(local_value_allowed_v1<T>,
                  "locals require copyable owned snapshots, not raw borrows");
public:
    LogicalLocalKeyV1() : id_(next_local_key_id_v1()) {}
    LogicalLocalKeyV1(const LogicalLocalKeyV1&) = default;
    LogicalLocalKeyV1& operator=(const LogicalLocalKeyV1&) = delete;
private:
    template <LocalDomainV1> friend class LogicalLocalScopeV1;
    const std::uint64_t id_;
};

template <LocalDomainV1 Domain>
class LogicalLocalScopeV1 final {
public:
    using Ptr = std::shared_ptr<LogicalLocalScopeV1>;
    static constexpr std::size_t max_slots = 32;

    static Ptr open(ExecutionStampV1 identity) {
        if constexpr (Domain == LocalDomainV1::Actor) {
            if (!identity.valid_actor()) return {};
        } else {
            if (!identity.valid_task()) return {};
        }
        return Ptr(new LogicalLocalScopeV1(identity));
    }

    LogicalLocalScopeV1(const LogicalLocalScopeV1&) = delete;
    LogicalLocalScopeV1& operator=(const LogicalLocalScopeV1&) = delete;
    ~LogicalLocalScopeV1() { close(); }

    bool is_open() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_;
    }

    // `current` is a SCHEDULER-AUTHENTICATED logical execution stamp,
    // *not* a freely supplied value from AttoLang source or I/O callbacks.
    bool authorized(ExecutionStampV1 current) const noexcept {
        if constexpr (Domain == LocalDomainV1::Actor)
            return current.valid_actor() && current.same_actor(identity_);
        else
            return current.valid_task() && current.same_task(identity_);
    }

    template <class T>
    bool set(ExecutionStampV1 current,
             const LogicalLocalKeyV1<Domain, T>& key, T value) {
        if (!authorized(current)) return false;
        auto incoming = std::make_shared<LocalCellV1<T>>(std::move(value));
        std::shared_ptr<LocalCellBaseV1> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_) return false;
            auto found = slots_.find(key.id_);
            if (found == slots_.end()) {
                if (slots_.size() >= max_slots) return false;
                slots_.emplace(key.id_, std::move(incoming));
            } else {
                retired.swap(found->second);
                found->second = std::move(incoming);
            }
        }
        // User-defined destructors are never run under the scope mutex.
        retired.reset();
        return true;
    }

    template <class T>
    std::optional<T> get_copy(
        ExecutionStampV1 current,
        const LogicalLocalKeyV1<Domain, T>& key) const {
        if (!authorized(current)) return std::nullopt;
        std::shared_ptr<LocalCellBaseV1> pinned;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_) return std::nullopt;
            auto found = slots_.find(key.id_);
            if (found == slots_.end()) return std::nullopt;
            pinned = found->second;
        }
        // A guest/host copy constructor must never execute under this lock.
        // Pin the cell so a reentrant close/erase cannot destroy the source.
        return static_cast<const LocalCellV1<T>&>(*pinned).value;
    }

    template <class T>
    bool erase(ExecutionStampV1 current,
               const LogicalLocalKeyV1<Domain, T>& key) {
        if (!authorized(current)) return false;
        std::shared_ptr<LocalCellBaseV1> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_) return false;
            auto found = slots_.find(key.id_);
            if (found == slots_.end()) return false;
            retired = std::move(found->second);
            slots_.erase(found);
        }
        retired.reset();
        return true;
    }

    bool close() noexcept {
        std::unordered_map<std::uint64_t,
                           std::shared_ptr<LocalCellBaseV1>> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_) return false;
            open_ = false;
            retired.swap(slots_);
        }
        retired.clear();
        return true;
    }

    // Explicit Copy-only child inheritance; never silently inherit an actor
    // borrow, a request writer, or a parent task's mutable cell. Caller owns
    // the child admission and supplies both authenticated task stamps.
    template <class T, LocalDomainV1 D = Domain,
              std::enable_if_t<D == Domain && Domain == LocalDomainV1::Task, int> = 0>
    bool copy_to_child(ExecutionStampV1 parent_current,
                       LogicalLocalScopeV1& child,
                       ExecutionStampV1 child_current,
                       const LogicalLocalKeyV1<Domain, T>& key) const {
        if (&child == this || !parent_current.same_actor(child_current) ||
            parent_current.same_task(child_current)) return false;
        auto snapshot = get_copy(parent_current, key);
        if (!snapshot) return false;
        return child.set(child_current, key, std::move(*snapshot));
    }

private:
    explicit LogicalLocalScopeV1(ExecutionStampV1 identity)
        : identity_(identity) {}
    const ExecutionStampV1 identity_;
    mutable std::mutex mutex_;
    bool open_ = true;
    std::unordered_map<std::uint64_t,
                       std::shared_ptr<LocalCellBaseV1>> slots_;
};

using ActorLocalScopeV1 = LogicalLocalScopeV1<LocalDomainV1::Actor>;
using TaskLocalScopeV1 = LogicalLocalScopeV1<LocalDomainV1::Task>;
template <class T> using ActorLocalKeyV1 = LogicalLocalKeyV1<LocalDomainV1::Actor, T>;
template <class T> using TaskLocalKeyV1 = LogicalLocalKeyV1<LocalDomainV1::Task, T>;

// Physical ThreadLocal: never carries guest actor, request or Task authority.
// Thread exit destroys the whole map; newly created threads start empty.
class PhysicalThreadLocalsV1 final {
    struct Slots {
        static constexpr std::size_t max_slots = 32;
        bool closing = false;
        std::unordered_map<std::uint64_t,
                           std::shared_ptr<LocalCellBaseV1>> values;

        ~Slots() {
            // Destructors of guest-owned host values may reenter a TLS key
            // on this SAME physical thread. Fence read/write/erase before
            // dropping any cell and empty the table before its callbacks.
            closing = true;
            decltype(values) retiring;
            retiring.swap(values);
            retiring.clear();
        }
    };
    static Slots& slots() {
        static thread_local Slots value;
        return value;
    }
public:
    template <class T>
    class Key final {
        static_assert(local_value_allowed_v1<T>,
                      "ThreadLocal requires copyable owned snapshots");
    public:
        Key() : id_(next_local_key_id_v1()) {}
        Key(const Key&) = default;
        Key& operator=(const Key&) = delete;
        bool set(T value) const {
            auto& state = slots();
            if (state.closing) return false;
            auto incoming = std::make_shared<LocalCellV1<T>>(std::move(value));
            auto& table = state.values;
            auto found = table.find(id_);
            if (found == table.end()) {
                if (table.size() >= Slots::max_slots) return false;
                table.emplace(id_, std::move(incoming));
            } else {
                auto retired = std::move(found->second);
                found->second = std::move(incoming);
                retired.reset();
            }
            return true;
        }
        std::optional<T> get_copy() const {
            const auto& state = slots();
            if (state.closing) return std::nullopt;
            const auto& table = state.values;
            auto found = table.find(id_);
            if (found == table.end()) return std::nullopt;
            auto pinned = found->second; // copy may reenter and erase this key
            return static_cast<const LocalCellV1<T>&>(*pinned).value;
        }
        bool erase() const {
            auto& state = slots();
            if (state.closing) return false;
            auto& table = state.values;
            auto found = table.find(id_);
            if (found == table.end()) return false;
            auto retired = std::move(found->second);
            table.erase(found);
            retired.reset(); // destructor runs after the slot is invisible
            return true;
        }
    private:
        const std::uint64_t id_;
    };
};

template <class T> using ThreadLocalKeyV1 = PhysicalThreadLocalsV1::Key<T>;

} // namespace oreslang::runtime
