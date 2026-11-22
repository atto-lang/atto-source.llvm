#pragma once

// HOST-ONLY foundation for AttoLang ActorLocal<T>, TaskLocal<T>, ThreadLocal<T>.
// This is NOT guest syntax/lowering, an authenticated scheduler dispatch, an
// AOT key-descriptor ABI, or a security/tenant boundary. RequestLocal already
// has its separate RequestContextLocalV1 implementation. The caller MUST
// supply identity from the scheduler's authenticated logical execution lease,
// never from guest arguments, callback provenance or an OS thread-local.
//
// This host substrate bounds 32 slots and 8 KiB of value PAYLOAD bytes per
// store. It still dynamically allocates (including container metadata) and is
// unsuitable for a sealed hard-real-time turn. Guest-owned capabilities require
// compiler Sendable/ownership admission before any host storage access.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
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
    explicit CellBase(std::size_t payload) : bytes(payload) {}
    virtual ~CellBase() = default;
    const std::size_t bytes;
};
template <class T> struct Cell final : CellBase {
    Cell(T item, std::size_t payload)
        : CellBase(payload), value(std::move(item)) {}
    const T value;
};

// Strict Unicode scalar validation: reject overlong encodings, surrogate
// codepoints, continuation-byte prefixes, >U+10FFFF and truncated tails.
// Source-level String is Unicode, not a way to smuggle arbitrary host bytes.
inline bool valid_utf8(const std::string& value) noexcept {
    const auto* data = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t offset = 0;
    while (offset < value.size()) {
        const unsigned char first = data[offset];
        if (first <= 0x7f) { ++offset; continue; }
        std::uint32_t scalar;
        std::uint32_t minimum;
        std::size_t total;
        if (first >= 0xc2 && first <= 0xdf) {
            total = 2; scalar = first & 0x1f; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            total = 3; scalar = first & 0x0f; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            total = 4; scalar = first & 0x07; minimum = 0x10000;
        } else return false;
        if (total > value.size() - offset) return false;
        for (std::size_t index = 1; index < total; ++index) {
            const unsigned char part = data[offset + index];
            if ((part & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (part & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff
                || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
        offset += total;
    }
    return true;
}

// This host entrypoint deliberately admits only primitive scalar / enum
// values and bounded valid UTF-8 strings. is_trivially_copyable<T> is NOT
// enough: a plain struct can conceal a host pointer or privileged capability.
// Struct/record values require transitive compiler-owned type descriptors.
template <class T>
std::size_t value_bytes(const T& value) noexcept {
    if constexpr (std::is_same_v<T, std::string>)
        return valid_utf8(value) ? value.size()
                                 : std::numeric_limits<std::size_t>::max();
    else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
        return sizeof(T);
    else {
        (void) value;
        return std::numeric_limits<std::size_t>::max();
    }
}

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
    static constexpr std::size_t max_bytes = 8192;
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
        const std::size_t bytes = scoped_local_detail_v1::value_bytes(value);
        if (bytes > max_bytes) return false;
        // Fail untrusted/stale writes before allocating a new cell. Recheck
        // after allocation because a concurrent close/replacement may race.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || !owner_.equals(caller)) return false;
            const auto found = slots_.find(key.id_);
            const std::size_t previous =
                found == slots_.end() ? 0 : found->second->bytes;
            if ((found == slots_.end() && slots_.size() >= max_slots)
                    || bytes > max_bytes - (used_bytes_ - previous)) return false;
        }
        auto next = std::make_shared<Cell<T>>(std::move(value), bytes);
        std::shared_ptr<Base> replaced;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || !owner_.equals(caller)) return false;
            const auto found = slots_.find(key.id_);
            const std::size_t previous =
                found == slots_.end() ? 0 : found->second->bytes;
            if ((found == slots_.end() && slots_.size() >= max_slots)
                    || bytes > max_bytes - (used_bytes_ - previous)) return false;
            if (found == slots_.end()) {
                slots_.emplace(key.id_, std::move(next));
            } else {
                replaced.swap(found->second);
                found->second = std::move(next);
            }
            used_bytes_ = used_bytes_ - previous + bytes;
        }
        replaced.reset(); // destructor outside the store mutex
        return true;
    }

    template <class T>
    bool remove(const ScopedLocalKeyV1<T, Scope>& key,
                const ScopedLocalIdentityV1& caller) {
        std::shared_ptr<Base> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || !owner_.equals(caller)) return false;
            const auto found = slots_.find(key.id_);
            if (found == slots_.end()) return false;
            used_bytes_ -= found->second->bytes;
            retired = std::move(found->second);
            slots_.erase(found);
        }
        retired.reset();
        return true;
    }

    std::size_t used_bytes() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return used_bytes_;
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
            used_bytes_ = 0;
        }
        old.clear();
        return true;
    }

    ~LogicalLocalStoreV1() { close(); }
private:
    const ScopedLocalIdentityV1 owner_;
    mutable std::mutex mutex_;
    bool open_ = true;
    std::size_t used_bytes_ = 0;
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
    struct State {
        std::unordered_map<std::uint64_t, std::shared_ptr<Base>> slots;
        std::size_t used_bytes = 0;
    };
    static State& state() {
        static thread_local State local;
        return local;
    }
public:
    static constexpr std::size_t max_slots = 32;
    static constexpr std::size_t max_bytes = 8192;
    template <class T>
    static bool set(const ThreadLocalKeyV1<T>& key, T value) {
        const std::size_t bytes = scoped_local_detail_v1::value_bytes(value);
        if (bytes > max_bytes) return false;
        auto next = std::make_shared<Cell<T>>(std::move(value), bytes);
        auto& local = state();
        auto& entries = local.slots;
        auto it = entries.find(key.id_);
        const std::size_t previous = it == entries.end() ? 0 : it->second->bytes;
        if ((it == entries.end() && entries.size() >= max_slots)
                || bytes > max_bytes - (local.used_bytes - previous)) return false;
        std::shared_ptr<Base> retired;
        if (it == entries.end()) {
            entries.emplace(key.id_, std::move(next));
        } else {
            retired = std::move(it->second);
            it->second = std::move(next);
        }
        local.used_bytes = local.used_bytes - previous + bytes;
        retired.reset();
        return true;
    }
    template <class T>
    static std::optional<T> get_copy(const ThreadLocalKeyV1<T>& key) {
        const auto& entries = state().slots;
        const auto it = entries.find(key.id_);
        if (it == entries.end()) return std::nullopt;
        return std::static_pointer_cast<const Cell<T>>(it->second)->value;
    }
    template <class T>
    static bool remove(const ThreadLocalKeyV1<T>& key) {
        auto& local = state();
        auto it = local.slots.find(key.id_);
        if (it == local.slots.end()) return false;
        local.used_bytes -= it->second->bytes;
        auto retired = std::move(it->second);
        local.slots.erase(it);
        retired.reset();
        return true;
    }
    static std::size_t used_bytes() { return state().used_bytes; }
    static void clear_current_thread() {
        std::unordered_map<std::uint64_t, std::shared_ptr<Base>> old;
        auto& local = state();
        old.swap(local.slots);
        local.used_bytes = 0;
        old.clear(); // destructor outside the bucket
    }
};

} // namespace oreslang::runtime
