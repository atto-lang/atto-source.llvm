#pragma once
// Trusted-host only: compact generation-checked context, not guest authority.
// Context is pinned to logical HTTP exchange + Task leases, never a carrier.
// Await/callback producers may retain a lease; only admitted actor turns may
// install the temporary carrier TLS lookup hint (not an authorization token).
#include "oreslang/runtime/NativeHttpExchange.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {
// Per-process monotonic identity. Independent context registries must never
// confuse identical compact slot/generation values across registry boundaries.
inline std::uint64_t next_request_registry_id() noexcept {
    static std::atomic<std::uint64_t> next{1};
    const auto n=next.fetch_add(1,std::memory_order_relaxed);
    if(!n || n==UINT64_MAX)std::terminate();
    return n;
}
template <std::size_t Capacity>
class NativeRequestContext final {
    static_assert(Capacity > 0 && Capacity <= UINT32_MAX, "bounded context capacity");
public:
    struct Handle { std::uint64_t value = 0; };
    struct Metadata {
        std::uint64_t request_id = 0, trace_id = 0, tenant_id = 0;
        std::uint64_t deadline_tick = 0;
    };
    static_assert(sizeof(Handle) == 8, "compact request handle");
    static_assert(sizeof(Metadata) == 32, "fixed-width request metadata");
private:
    enum class Phase : std::uint8_t { free, open, closing, retired };
    struct Slot {
        std::uint32_t generation = 0;
        Phase phase = Phase::free;
        OresNativeActorRef owner{0,0,0};
        Metadata meta{};
        std::size_t leases = 0;
    };
    struct Frame {
        NativeRequestContext* table;
        Handle handle;
        OresNativeActorRef owner;
        Frame* previous;
    };
    inline static thread_local Frame* current_frame_ = nullptr;
    const std::uint64_t registry_id_ = next_request_registry_id();
    mutable std::mutex mutex_;
    std::array<Slot, Capacity> slots_{};
    static bool same(OresNativeActorRef a, OresNativeActorRef b) noexcept {
        return a.scheduler_domain == b.scheduler_domain && a.actor_slot == b.actor_slot
            && a.generation == b.generation;
    }
    static bool valid(OresNativeActorRef a) noexcept {
        return a.scheduler_domain && a.generation;
    }
    static bool valid(Metadata m) noexcept {
        return m.request_id && m.trace_id && m.tenant_id && m.deadline_tick;
    }
    static Handle make(std::size_t i, std::uint32_t generation) noexcept {
        return {(std::uint64_t(generation) << 32) | std::uint64_t(i + 1)};
    }
    Slot* locate(Handle h) noexcept { // caller holds mutex_
        const auto low = std::uint32_t(h.value);
        if (!low || low > Capacity || !std::uint32_t(h.value >> 32)) return nullptr;
        auto& s=slots_[low-1];
        return s.generation == std::uint32_t(h.value >> 32)
            && s.phase != Phase::free && s.phase != Phase::retired ? &s : nullptr;
    }
    void recycle(Slot& s) noexcept { // caller holds mutex_
        if (s.phase != Phase::closing || s.leases) std::terminate();
        s.meta={};s.owner={0,0,0};
        s.phase=s.generation == UINT32_MAX ? Phase::retired : Phase::free;
    }
    void release(Handle h) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* s=locate(h);
        if (!s || !s->leases) std::terminate();
        --s->leases;
        if (!s->leases && s->phase == Phase::closing) recycle(*s);
    }
public:
    class Lease final {
        friend class NativeRequestContext;
        NativeRequestContext* table_ = nullptr;
        Handle handle_{};
        Lease(NativeRequestContext* table, Handle handle) noexcept:
            table_(table), handle_(handle) {}
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& rhs) noexcept:
            table_(std::exchange(rhs.table_, nullptr)),handle_(rhs.handle_) {}
        Lease& operator=(Lease&& rhs) noexcept {
            if (this != &rhs) { reset();table_=std::exchange(rhs.table_,nullptr);handle_=rhs.handle_; }
            return *this;
        }
        ~Lease(){reset();}
        void reset() noexcept {if (auto* p=std::exchange(table_,nullptr))p->release(handle_);}
        Handle handle() const noexcept {return table_ ? handle_ : Handle{};}
        explicit operator bool() const noexcept {return table_!=nullptr;}
    };
    // Trusted reactor owns a physical lifetime pin from admission until
    // the exchange is completed/rolled back and fully reclaimed.
    struct ExchangePin final {
        NativeHttpExchange::Token token;
        Lease pin;
        ExchangePin(NativeHttpExchange::Token t, Lease&& p) noexcept:
            token(t), pin(std::move(p)) {}
        ExchangePin(const ExchangePin&) = delete;
        ExchangePin& operator=(const ExchangePin&) = delete;
        ExchangePin(ExchangePin&&) noexcept = default;
        ExchangePin& operator=(ExchangePin&&) noexcept = default;
    };
    // Pin before publishing a weak context handle to the exchange.
    std::optional<ExchangePin> attach_exchange(NativeHttpExchange& exchange,
            Handle handle, OresNativeActorRef owner) noexcept {
        auto pin=retain(handle,owner);
        if(!pin)return std::nullopt;
        if(!exchange.enqueue(owner,handle.value,registry_id_))return std::nullopt;
        return ExchangePin(exchange.token(),std::move(*pin));
    }
    // Durable move-only suspension link: stored in Task/continuation frame,
    // not worker TLS. Lease survives physical carrier exit.
    struct Suspension final {
        Handle context{};
        OresNativeActorRef owner{0,0,0};
        std::uint64_t exchange_generation = 0;
        Lease pin{};
        Suspension(Handle h, OresNativeActorRef a, std::uint64_t epoch,
                   Lease&& owned) noexcept:
            context(h), owner(a), exchange_generation(epoch), pin(std::move(owned)) {}
        Suspension(const Suspension&) = delete;
        Suspension& operator=(const Suspension&) = delete;
        Suspension(Suspension&&) noexcept = default;
        Suspension& operator=(Suspension&&) noexcept = default;
    };
    // Capture ONLY from an actual admitted physical turn. A foreign callback
    // producer with no active scope cannot manufacture a physical pin.
    std::optional<Suspension> suspend_current(
            const NativeHttpExchange& exchange, NativeHttpExchange::Token token) {
        Frame* f=current_frame_;
        if (!f || f->table!=this || !same(f->owner,token.actor)
                || f->handle.value!=token.request_context
                || token.request_registry!=registry_id_ || !token.generation
                || exchange.phase()!=NativeHttpExchange::Phase::actor)
            return std::nullopt;
        const auto observed=exchange.token();
        if(observed.generation!=token.generation
                || !same(observed.actor,token.actor)
                || observed.request_context!=token.request_context
                || observed.request_registry!=registry_id_)return std::nullopt;
        auto pin=retain(f->handle,f->owner);
        if(!pin)return std::nullopt;
        return Suspension(f->handle,f->owner,token.generation,std::move(*pin));
    }
    template<typename Fn>
    bool resume_admitted(const NativeHttpExchange& exchange,
            NativeHttpExchange::Token token, const Suspension& suspended, Fn&& fn) {
        if(!suspended.pin || suspended.context.value!=suspended.pin.handle().value
                || suspended.context.value!=token.request_context
                || token.request_registry!=registry_id_
                || suspended.exchange_generation!=token.generation
                || !same(suspended.owner,token.actor))return false;
        return with_admitted_turn(exchange,token,suspended.context,suspended.owner,
                                  std::forward<Fn>(fn));
    }
    std::uint64_t registry_id() const noexcept {return registry_id_;}
    NativeRequestContext()=default;
    NativeRequestContext(const NativeRequestContext&)=delete;
    NativeRequestContext& operator=(const NativeRequestContext&)=delete;
    ~NativeRequestContext(){
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& s:slots_)
            if (s.leases || (s.phase != Phase::free && s.phase != Phase::retired))
                std::terminate(); // physical lifetime proof required
    }
    std::optional<Handle> open(OresNativeActorRef owner, Metadata meta) noexcept {
        if (!valid(owner)||!valid(meta))return std::nullopt;
        std::lock_guard<std::mutex> lock(mutex_);
        // Detect duplicate live request identity, including closing/pinned slots.
        for (const auto& s:slots_)
            if ((s.phase == Phase::open || s.phase == Phase::closing)
                    && s.meta.tenant_id == meta.tenant_id
                    && s.meta.request_id == meta.request_id) return std::nullopt;
        for (std::size_t i=0;i<Capacity;++i){
            auto& s=slots_[i];
            if(s.phase!=Phase::free)continue;
            if(s.generation==UINT32_MAX)std::terminate();
            ++s.generation;s.owner=owner;s.meta=meta;s.phase=Phase::open;s.leases=0;
            return make(i,s.generation);
        }
        return std::nullopt;
    }
    std::optional<Lease> retain(Handle h, OresNativeActorRef owner) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* s=locate(h);
        if (!s || s->phase!=Phase::open || !valid(owner) || !same(s->owner,owner)
                || s->leases == SIZE_MAX) return std::nullopt;
        ++s->leases;
        return Lease(this,h);
    }
    bool close(Handle h, OresNativeActorRef owner) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* s=locate(h);
        if(!s||s->phase!=Phase::open||!valid(owner)||!same(s->owner,owner))return false;
        s->phase=Phase::closing; // revoke new acquisitions immediately
        if(!s->leases)recycle(*s);
        return true;
    }
    std::optional<Metadata> lookup(Handle h, OresNativeActorRef owner) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        // Copy only: no pointer escapes into slots that can recycle.
        const auto low=std::uint32_t(h.value);
        if(!low||low>Capacity)return std::nullopt;
        const auto& s=slots_[low-1];
        if(s.phase!=Phase::open || s.generation!=std::uint32_t(h.value>>32)
                || !valid(owner) || !same(s.owner,owner)) return std::nullopt;
        return s.meta;
    }
    std::size_t available() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t count=0;for(const auto& s:slots_)count+=s.phase==Phase::free;
        return count;
    }
    // Call only from a scheduler-authenticated actor DATA turn. Matching
    // the owner tuple is necessary but NOT sufficient for guest authority.
    template<typename Fn>
    bool with_admitted_turn(const NativeHttpExchange& exchange,
            NativeHttpExchange::Token token, Handle handle,
            OresNativeActorRef owner, Fn&& callback) {
        static_assert(std::is_void_v<std::invoke_result_t<Fn&>>,
            "carrier binding must not return an escaped resource or awaitable");
        if(!token.generation || !valid(owner) || !same(token.actor,owner)
                || token.request_context!=handle.value
                || token.request_registry!=registry_id_
                || exchange.phase()!=NativeHttpExchange::Phase::actor)return false;
        const auto observed=exchange.token();
        if(observed.generation!=token.generation || !same(observed.actor,owner)
                || observed.request_context!=handle.value
                || observed.request_registry!=registry_id_)return false;
        auto lease=retain(handle,owner);
        if(!lease)return false;
        // Store lease prevents reuse; close revokes current() immediately.
        if(!lookup(handle,owner))return false;
        Frame frame{this,handle,owner,current_frame_};
        current_frame_=&frame;
        const auto carrier=std::this_thread::get_id();
        struct Restore {
            Frame* frame;std::thread::id carrier;
            ~Restore(){
                if(current_frame_!=frame || std::this_thread::get_id()!=carrier)
                    std::terminate();
                current_frame_=frame->previous;
            }
        } restore{&frame,carrier};
        callback();
        return true;
    }
    // Convenience only, never an actor, heap or permissions accessor.
    static std::optional<Metadata> current() noexcept {
        auto* frame=current_frame_;
        return frame ? frame->table->lookup(frame->handle,frame->owner) : std::nullopt;
    }
};
} // namespace oreslang::runtime
