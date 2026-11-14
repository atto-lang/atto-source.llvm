#pragma once

#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * HOST-ONLY bounded local call/reply correlation. No UUIDs, guest-accessible
 * handles, or pointer-to-stack across await. The future adapter must schedule
 * guest resumption on the logical caller actor lane, not block a carrier on
 * std::future::get(). These capabilities MUST NEVER be serialized.
 */
class NativeGenerationalReplySlots final {
public:
    enum class Role { Actor, SupervisorApplication };
    class Endpoint final {
        friend class NativeGenerationalReplySlots;
        explicit Endpoint(Role role) : role_(role) {}
        Role role_;
    public:
        Role role() const noexcept { return role_; }
    };
    using EndpointRef = std::shared_ptr<const Endpoint>;

private:
    struct Pending {
        virtual ~Pending() = default;
        virtual void abandon() = 0;
    };
    template<typename T> struct TypedPending final : Pending {
        std::promise<T> promise;
        void abandon() override {
            promise.set_exception(std::make_exception_ptr(
                std::runtime_error("reply endpoint retired")));
        }
    };
    struct Slot {
        EndpointRef caller;
        EndpointRef callee;
        std::shared_ptr<Pending> pending;
    };
    static constexpr std::uint32_t invalid_index_ = std::numeric_limits<std::uint32_t>::max();
    struct Core {
        mutable std::mutex mu;
        std::vector<Slot> slots;
        std::vector<std::uint32_t> generations;
        std::vector<std::uint32_t> next;
        std::unordered_map<const Endpoint*, std::weak_ptr<const Endpoint>> endpoints;
        std::uint32_t free;
        std::size_t active = 0;
        bool closed = false;

        explicit Core(std::size_t capacity)
            : slots(capacity), generations(capacity, 0), next(capacity),
              free(capacity ? 0 : invalid_index_) {
            if (capacity == 0 || capacity > 65'536) {
                throw std::invalid_argument("invalid reply capacity");
            }
            for (std::size_t i = 0; i < capacity; ++i) {
                next[i] = i + 1 == capacity ? invalid_index_ :
                               static_cast<std::uint32_t>(i + 1);
            }
        }
        bool owns(const EndpointRef& e) const {
            if (!e) return false;
            const auto it = endpoints.find(e.get());
            return it != endpoints.end() && it->second.lock() == e;
        }
        void release(std::uint32_t index) {
            slots[index] = {};
            next[index] = free;
            free = index;
            --active;
        }
    };

public:
    template<typename T> class Ticket final {
        friend class NativeGenerationalReplySlots;
        std::weak_ptr<Core> core_;
        EndpointRef caller_;
        EndpointRef callee_;
        std::shared_ptr<TypedPending<T>> pending_;
        std::uint32_t index_;
        std::uint32_t generation_;
        Ticket(const std::shared_ptr<Core>& core, EndpointRef caller,
               EndpointRef callee, std::shared_ptr<TypedPending<T>> pending,
               std::uint32_t index, std::uint32_t generation)
            : core_(core), caller_(std::move(caller)), callee_(std::move(callee)),
              pending_(std::move(pending)), index_(index), generation_(generation) {}
    public:
        std::uint64_t local_handle() const noexcept {
            return (static_cast<std::uint64_t>(generation_) << 32) |
                   (static_cast<std::uint64_t>(index_) + 1U);
        }
        bool reply(const EndpointRef& responder, T value) const {
            if (!NativeGenerationalReplySlots::claim(*this, responder, true)) return false;
            pending_->promise.set_value(std::move(value));
            return true;
        }
        bool reject(const EndpointRef& responder, std::exception_ptr failure) const {
            if (!failure) throw std::invalid_argument("empty failure");
            if (!NativeGenerationalReplySlots::claim(*this, responder, true)) return false;
            pending_->promise.set_exception(std::move(failure));
            return true;
        }
        /** VM mailbox admission rollback. Only original caller endpoint
         * can fail its own slot; this cannot fabricate a successful reply. */
        bool fail_admission(const EndpointRef& caller, std::exception_ptr failure) const {
            if (!failure) throw std::invalid_argument("empty admission failure");
            if (!NativeGenerationalReplySlots::claim(*this, caller, false)) return false;
            pending_->promise.set_exception(std::move(failure));
            return true;
        }
        bool cancel(const EndpointRef& caller) const {
            if (!NativeGenerationalReplySlots::claim(*this, caller, false)) return false;
            pending_->promise.set_exception(std::make_exception_ptr(
                std::runtime_error("actor call cancelled")));
            return true;
        }
        bool timeout(const EndpointRef& caller) const {
            if (!NativeGenerationalReplySlots::claim(*this, caller, false)) return false;
            pending_->promise.set_exception(std::make_exception_ptr(
                std::runtime_error("actor call timed out")));
            return true;
        }
    };
    template<typename T> struct Call final {
        Ticket<T> ticket;
        std::future<T> future;
    };

    explicit NativeGenerationalReplySlots(std::size_t capacity)
        : core_(std::make_shared<Core>(capacity)) {}
    NativeGenerationalReplySlots(const NativeGenerationalReplySlots&) = delete;
    NativeGenerationalReplySlots& operator=(const NativeGenerationalReplySlots&) = delete;
    ~NativeGenerationalReplySlots() { close(); }

    EndpointRef endpoint(Role kind) {
        auto core = core_;
        std::lock_guard<std::mutex> lock(core->mu);
        if (core->closed) throw std::logic_error("reply registry closed");
        EndpointRef ref(new Endpoint(kind));
        core->endpoints.emplace(ref.get(), ref);
        return ref;
    }

    template<typename T> Call<T> reserve(const EndpointRef& caller, const EndpointRef& callee) {
        static_assert(!std::is_void<T>::value, "unary reply must not be void");
        if (!caller || !callee || caller == callee) {
            throw std::invalid_argument("invalid endpoint or self-call");
        }
        auto pending = std::make_shared<TypedPending<T>>();
        auto future = pending->promise.get_future();
        auto core = core_;
        std::lock_guard<std::mutex> lock(core->mu);
        if (core->closed) throw std::logic_error("reply registry closed");
        if (!core->owns(caller) || !core->owns(callee)) {
            throw std::logic_error("foreign or retired reply endpoint");
        }
        while (core->free != invalid_index_ &&
               core->generations[core->free] == std::numeric_limits<std::uint32_t>::max()) {
            core->free = core->next[core->free];  // Retire exhausted slot, not wrap.
        }
        if (core->free == invalid_index_) throw std::runtime_error("reply capacity exhausted");
        const auto index = core->free;
        core->free = core->next[index];
        const auto generation = ++core->generations[index];
        core->slots[index] = {caller, callee, pending};
        ++core->active;
        return Call<T>{Ticket<T>(core, caller, callee, pending, index, generation),
                       std::move(future)};
    }

    std::size_t pending_count() const {
        auto core = core_;
        std::lock_guard<std::mutex> lock(core->mu);
        return core->active;
    }

    void retire(const EndpointRef& endpoint) {
        auto core = core_;
        std::vector<std::shared_ptr<Pending>> abandoned;
        {
            std::lock_guard<std::mutex> lock(core->mu);
            if (!core->owns(endpoint)) throw std::logic_error("foreign or retired endpoint");
            core->endpoints.erase(endpoint.get());
            for (std::size_t i = 0; i < core->slots.size(); ++i) {
                const auto& slot = core->slots[i];
                if (slot.pending && (slot.caller == endpoint || slot.callee == endpoint)) {
                    abandoned.push_back(slot.pending);
                    core->release(static_cast<std::uint32_t>(i));
                }
            }
        }
        for (auto& p : abandoned) p->abandon();
    }

    void close() {
        auto core = core_;
        std::vector<std::shared_ptr<Pending>> abandoned;
        {
            std::lock_guard<std::mutex> lock(core->mu);
            if (core->closed) return;
            core->closed = true;
            core->endpoints.clear();
            for (std::size_t i = 0; i < core->slots.size(); ++i) {
                if (core->slots[i].pending) {
                    abandoned.push_back(core->slots[i].pending);
                    core->release(static_cast<std::uint32_t>(i));
                }
            }
        }
        for (auto& p : abandoned) p->abandon();
    }

private:
    std::shared_ptr<Core> core_;
    template<typename T> static bool claim(const Ticket<T>& ticket,
                                            const EndpointRef& endpoint, bool fromCallee) {
        if (endpoint != (fromCallee ? ticket.callee_ : ticket.caller_)) {
            throw std::logic_error("reply endpoint capability mismatch");
        }
        const auto core = ticket.core_.lock();
        if (!core) return false;  // Ticket may outlive registry safely.
        std::lock_guard<std::mutex> lock(core->mu);
        if (core->closed || ticket.index_ >= core->slots.size() ||
            core->generations[ticket.index_] != ticket.generation_ ||
            core->slots[ticket.index_].pending != ticket.pending_) return false;
        core->release(ticket.index_);
        return true;
    }
};

}  // namespace oreslang::runtime
