#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/** Bounded host-side stream reply lease. Not a guest actor message or CONTROL.
 * Unary responses continue to use NativeGenerationalReplySlots; this stream
 * companion deliberately provides no blocking wait or guest continuation.
 * T is an inline, trivially copyable host payload until ownership-aware
 * Sendable/Serializable lowering and byte quotas are implemented.
 */
template <typename T> class NativeActorReplyStreamsV1 final {
    static_assert(std::is_trivially_copyable<T>::value &&
                      std::is_default_constructible<T>::value &&
                      std::is_copy_assignable<T>::value &&
                      !std::is_pointer<T>::value && sizeof(T) <= 256,
                  "stream reply V1 requires bounded inline non-pointer payload");
public:
    enum class Role { Actor, SupervisorApplication };  // Never CONTROL.
    class Endpoint final {
        friend class NativeActorReplyStreamsV1;
        explicit Endpoint(Role role) : role_(role) {}
        Role role_;
    public:
        Role role() const noexcept { return role_; }
    };
    using EndpointRef = std::shared_ptr<const Endpoint>;
    enum class WriteStatus { Written, Full, Terminal, Stale };
    enum class ReadStatus { Item, Pending, End, Failed, Stale };
    struct Read final { ReadStatus status; T value{}; };

private:
    static constexpr std::uint32_t invalid_ = std::numeric_limits<std::uint32_t>::max();
    struct Slot {
        EndpointRef caller;
        EndpointRef callee;
        std::vector<T> ring;
        std::size_t head = 0;
        std::size_t count = 0;
        bool terminal = false;
        bool failed = false;
        bool active = false;
    };
    struct Core {
        std::mutex mu;
        std::vector<Slot> slots;
        std::vector<std::uint32_t> generations, next;
        std::unordered_map<const Endpoint*, std::weak_ptr<const Endpoint>> endpoints;
        std::uint32_t free = 0;
        std::size_t active = 0;
        bool closed = false;
        static std::size_t validated(std::size_t capacity, std::size_t items) {
            if (capacity == 0 || capacity > 65536 || items == 0 || items > 256 ||
                capacity > 65536 / items)
                throw std::invalid_argument("invalid bounded reply stream budget");
            return capacity;
        }
        explicit Core(std::size_t capacity, std::size_t items)
            : slots(validated(capacity, items)), generations(capacity, 0), next(capacity) {
            for (std::size_t i = 0; i < capacity; ++i) {
                slots[i].ring.resize(items);
                next[i] = (i + 1 == capacity) ? invalid_ : static_cast<std::uint32_t>(i + 1);
            }
        }
        bool owns(const EndpointRef& e) const {
            if (!e) return false;
            auto it = endpoints.find(e.get());
            return it != endpoints.end() && it->second.lock() == e;
        }
        void release(std::uint32_t index) {
            auto& s = slots[index];
            std::fill(s.ring.begin(), s.ring.end(), T{});
            s.caller.reset(); s.callee.reset();
            s.head = s.count = 0;
            s.terminal = s.failed = s.active = false;
            next[index] = free;
            free = index;
            --active;
        }
    };
public:
    class Lease final {
        friend class NativeActorReplyStreamsV1;
        std::weak_ptr<Core> core_;
        EndpointRef caller_, callee_;
        std::uint32_t index_ = invalid_, generation_ = 0;
        Lease(const std::shared_ptr<Core>& core, EndpointRef caller, EndpointRef callee,
              std::uint32_t index, std::uint32_t generation)
            : core_(core), caller_(std::move(caller)), callee_(std::move(callee)),
              index_(index), generation_(generation) {}
        template <typename Fn> auto with_slot(const EndpointRef& authority,
                                              bool producer, Fn fn) const -> decltype(fn(std::declval<Core&>(), std::declval<Slot&>())) {
            if (authority != (producer ? callee_ : caller_))
                throw std::logic_error("reply capability mismatch");
            const auto core = core_.lock();
            if (!core) return fn_stale(fn);
            std::lock_guard<std::mutex> lock(core->mu);
            if (core->closed || !core->owns(authority) || index_ >= core->slots.size() ||
                core->generations[index_] != generation_ || !core->slots[index_].active ||
                core->slots[index_].caller != caller_ ||
                core->slots[index_].callee != callee_) return fn_stale(fn);
            return fn(*core, core->slots[index_]);
        }
        template<typename Fn> static auto fn_stale(Fn /* unused */) -> decltype(std::declval<Fn>()(std::declval<Core&>(), std::declval<Slot&>())) {
            using R = decltype(std::declval<Fn>()(std::declval<Core&>(), std::declval<Slot&>()));
            if constexpr (std::is_same<R, Read>::value) return Read{ReadStatus::Stale, T{}};
            else return WriteStatus::Stale;
        }
    public:
        std::uint64_t local_handle() const noexcept {
            return (static_cast<std::uint64_t>(generation_) << 32) |
                   (static_cast<std::uint64_t>(index_) + 1u);
        }
        WriteStatus try_send(const EndpointRef& producer, const T& value) const {
            return with_slot(producer, true, [&](Core&, Slot& s) {
                if (s.terminal) return WriteStatus::Terminal;
                if (s.count == s.ring.size()) return WriteStatus::Full;
                s.ring[(s.head + s.count) % s.ring.size()] = value;
                ++s.count;
                return WriteStatus::Written;
            });
        }
        WriteStatus close_stream(const EndpointRef& producer) const {
            return finish(producer, false);
        }
        WriteStatus fail_stream(const EndpointRef& producer) const {
            return finish(producer, true);
        }
        Read poll(const EndpointRef& caller) const {
            return with_slot(caller, false, [&](Core& core, Slot& s) -> Read {
                if (s.count != 0) {
                    const T value = s.ring[s.head];
                    s.ring[s.head] = T{};
                    s.head = (s.head + 1) % s.ring.size();
                    --s.count;
                    return {ReadStatus::Item, value};
                }
                if (!s.terminal) return {ReadStatus::Pending, T{}};
                const ReadStatus result = s.failed ? ReadStatus::Failed : ReadStatus::End;
                core.release(index_);
                return {result, T{}};
            });
        }
        bool cancel(const EndpointRef& caller) const {
            // A successful cancel takes precedence over all later writes.
            return with_slot(caller, false, [&](Core& core, Slot&) {
                core.release(index_);
                return WriteStatus::Written;
            }) == WriteStatus::Written;
        }
    private:
        WriteStatus finish(const EndpointRef& producer, bool failed) const {
            return with_slot(producer, true, [&](Core&, Slot& s) {
                if (s.terminal) return WriteStatus::Terminal;
                s.terminal = true; s.failed = failed;
                return WriteStatus::Written;
            });
        }
    };
    explicit NativeActorReplyStreamsV1(std::size_t slots, std::size_t per_stream_items)
        : core_(std::make_shared<Core>(slots, per_stream_items)) {}
    NativeActorReplyStreamsV1(const NativeActorReplyStreamsV1&) = delete;
    NativeActorReplyStreamsV1& operator=(const NativeActorReplyStreamsV1&) = delete;
    ~NativeActorReplyStreamsV1() { close(); }

    EndpointRef endpoint(Role role) {
        if (role != Role::Actor && role != Role::SupervisorApplication)
            throw std::invalid_argument("CONTROL is not a reply endpoint");
        std::lock_guard<std::mutex> lock(core_->mu);
        if (core_->closed) throw std::logic_error("reply stream registry closed");
        EndpointRef e(new Endpoint(role));
        core_->endpoints.emplace(e.get(), e);
        return e;
    }
    Lease reserve(const EndpointRef& caller, const EndpointRef& callee) {
        std::lock_guard<std::mutex> lock(core_->mu);
        if (core_->closed || !core_->owns(caller) || !core_->owns(callee) || caller == callee)
            throw std::logic_error("invalid or retired reply endpoint");
        while (core_->free != invalid_ &&
               core_->generations[core_->free] == invalid_)
            core_->free = core_->next[core_->free];  // no ABA wrap.
        if (core_->free == invalid_) throw std::runtime_error("reply stream slots full");
        const auto i = core_->free;
        core_->free = core_->next[i];
        auto& s = core_->slots[i];
        s.caller = caller; s.callee = callee;
        s.active = true; s.terminal = s.failed = false;
        s.head = s.count = 0;
        ++core_->active;
        return Lease(core_, caller, callee, i, ++core_->generations[i]);
    }
    std::size_t pending_count() const {
        std::lock_guard<std::mutex> lock(core_->mu);
        return core_->active;
    }
    void retire(const EndpointRef& endpoint) {
        std::lock_guard<std::mutex> lock(core_->mu);
        if (!core_->owns(endpoint)) throw std::logic_error("foreign reply endpoint");
        core_->endpoints.erase(endpoint.get());
        for (std::size_t i = 0; i < core_->slots.size(); ++i) {
            const auto& s = core_->slots[i];
            if (s.active && (s.caller == endpoint || s.callee == endpoint))
                core_->release(static_cast<std::uint32_t>(i));
        }
    }
    void close() {
        std::lock_guard<std::mutex> lock(core_->mu);
        if (core_->closed) return;
        core_->closed = true;
        core_->endpoints.clear();
        for (std::size_t i = 0; i < core_->slots.size(); ++i)
            if (core_->slots[i].active) core_->release(static_cast<std::uint32_t>(i));
    }
private:
    std::shared_ptr<Core> core_;
};
} // namespace oreslang::runtime
