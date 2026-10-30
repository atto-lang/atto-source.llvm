#pragma once
// Trusted-host only: compact generation-checked context, not guest authority.
// Context is pinned to logical HTTP exchange + Task leases, never a carrier.
// Await/callback producers may retain a lease; only admitted actor turns may
// install the temporary carrier TLS lookup hint (not an authorization token).
#include "oreslang/runtime/NativeHttpExchange.hpp"
#include "oreslang/runtime/RequestSlotTable.hpp"
#include <functional>
#include <string_view>
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
    auto n=next.load(std::memory_order_relaxed);
    while(n && n!=UINT64_MAX) {
        if(next.compare_exchange_weak(n,n+1,std::memory_order_relaxed))
            return n;
    }
    std::terminate();
}
// Trusted ingress-generated, nonreused DATA-envelope identity. The underlying
// payload is not a guest capability; source code must not call the scheduler
// directly or choose another request's dispatch ticket.
inline std::uint64_t next_request_dispatch_ticket() noexcept {
    static std::atomic<std::uint64_t> next{1};
    auto n=next.load(std::memory_order_relaxed);
    while(n && n!=UINT64_MAX) {
        if(next.compare_exchange_weak(n,n+1,std::memory_order_relaxed))
            return n;
    }
    return 0; // exhaustion denies HTTP admission, never wraps to stale ticket
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
        OresNativeActorScheduler* scheduler;
        const NativeHttpExchange* exchange;
        std::uint64_t exchange_generation;
        std::uint64_t dispatch_ticket;
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
    // Exact-current scheduler authority, never request token or callback provenance.
    static bool admitted(OresNativeActorScheduler* scheduler,
                         OresNativeActorRef owner) noexcept {
        if (!scheduler || !valid(owner) ||
            !ores_native_actor_scheduler_is_current_actor(scheduler, owner))
            return false;
        OresNativeExecutionContext ctx{};
        return ores_native_actor_scheduler_current_execution_context(scheduler, &ctx)
            && ctx.id.runtime_domain == owner.scheduler_domain
            && ctx.id.execution != 0
            && ctx.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR
            && ctx.allocation.domain == owner.scheduler_domain
            && ctx.allocation.owner == owner.actor_slot
            && ctx.allocation.generation == owner.generation;
    }
    static bool admitted_for(OresNativeActorScheduler* scheduler,
                             OresNativeActorRef owner,
                             std::uint64_t ticket) noexcept {
        std::uint64_t delivered = 0;
        return ticket != 0 && admitted(scheduler,owner)
            && ores_native_actor_scheduler_current_data_payload(
                    scheduler,owner,&delivered)
            && delivered == ticket;
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
    // Physical ownership of an HTTP exchange, not merely a weak token.
    // A pin may only retire after rollback/reclaim returns to reactor phase.
    struct ExchangePin final {
        NativeHttpExchange::Token token;
    private:
        friend class NativeRequestContext;
        NativeHttpExchange* exchange_ = nullptr;
        Lease pin_;
        ExchangePin(NativeHttpExchange& exchange, NativeHttpExchange::Token t,
                    Lease&& p) noexcept:token(t),exchange_(&exchange),pin_(std::move(p)){}
    public:
        ExchangePin(const ExchangePin&) = delete;
        ExchangePin& operator=(const ExchangePin&) = delete;
        ExchangePin(ExchangePin&& src) noexcept:
            token(src.token), exchange_(std::exchange(src.exchange_,nullptr)),
            pin_(std::move(src.pin_)) {}
        ExchangePin& operator=(ExchangePin&&) = delete;
        ~ExchangePin(){
            if(pin_ && (!exchange_ || !exchange_->release_physical_pin(token)))
                std::terminate(); // early release or different generation
        }
    };
    std::optional<ExchangePin> attach_exchange(NativeHttpExchange& exchange,
            Handle handle, OresNativeActorRef owner) noexcept {
        auto pin=retain(handle,owner);
        if(!pin)return std::nullopt;
        const auto ticket=next_request_dispatch_ticket();
        if(!ticket || !exchange.enqueue(owner,handle.value,registry_id_,ticket))
            return std::nullopt;
        const auto token=exchange.token();
        if(!exchange.bind_physical_pin(token)){
            if(!exchange.rollback(token))std::terminate();
            return std::nullopt;
        }
        return ExchangePin(exchange,token,std::move(*pin));
    }
    // A request's transport buffer is physically retained from the time
    // its exchange is published through terminal/reclaim. This lease is NOT
    // exposed as a guest string_view and never crosses await as a guest borrow.
    template<std::size_t Slots,std::size_t MaxBytes>
    struct BoundExchange final {
        using Table=RequestSlotTable<Slots,MaxBytes>;
        using BodyHandle=typename Table::Handle;
    private:
        friend class NativeRequestContext;
        ExchangePin exchange_pin_;
        Table* table_;
        BodyHandle body_;
        std::uint64_t transport_owner_;
        typename Table::ReadLease body_pin_;
        BoundExchange(ExchangePin&& exchange, Table& table, BodyHandle body,
                      std::uint64_t owner, typename Table::ReadLease&& loan) noexcept:
            exchange_pin_(std::move(exchange)),table_(&table),body_(body),
            transport_owner_(owner),body_pin_(std::move(loan)){}
    public:
        BoundExchange(const BoundExchange&) = delete;
        BoundExchange& operator=(const BoundExchange&) = delete;
        BoundExchange(BoundExchange&&) noexcept = default;
        BoundExchange& operator=(BoundExchange&&) = delete;
        NativeHttpExchange::Token token() const noexcept { return exchange_pin_.token; }
    };
    template<std::size_t Slots,std::size_t MaxBytes>
    std::optional<BoundExchange<Slots,MaxBytes>> attach_exchange_with_buffer(
            NativeHttpExchange& exchange, Handle handle, OresNativeActorRef owner,
            RequestSlotTable<Slots,MaxBytes>& table,
            typename RequestSlotTable<Slots,MaxBytes>::Handle body,
            std::uint64_t transport_owner) noexcept {
        // The transport owner comes from the trusted ingress/handoff. A guest
        // cannot supply arbitrary numeric owners to this host-only factory.
        auto read_pin=table.borrow(body,transport_owner);
        if(!read_pin)return std::nullopt;
        auto exchange_pin=attach_exchange(exchange,handle,owner);
        if(!exchange_pin)return std::nullopt;
        return BoundExchange<Slots,MaxBytes>(std::move(*exchange_pin),table,
                    body,transport_owner,std::move(*read_pin));
    }
    // Durable move-only suspension link: stored in Task/continuation frame,
    // not worker TLS. Lease survives physical carrier exit.
    struct Suspension final {
        Handle context{};
        OresNativeActorRef owner{0,0,0};
        std::uint64_t exchange_generation = 0;
        Lease pin{};
    private:
        friend class NativeRequestContext;
        // 0 available, 1 executing resume, 2 consumed. Only an authenticated
        // scheduler DATA turn can claim; copied/stale callbacks can never replay.
        std::atomic<unsigned> resumption_state_{0};
        Suspension(Handle h, OresNativeActorRef a, std::uint64_t epoch,
                   Lease&& owned) noexcept:
            context(h), owner(a), exchange_generation(epoch), pin(std::move(owned)) {}
    public:
        Suspension(const Suspension&) = delete;
        Suspension& operator=(const Suspension&) = delete;
        Suspension(Suspension&& rhs) noexcept:
            context(rhs.context), owner(rhs.owner),
            exchange_generation(rhs.exchange_generation), pin(std::move(rhs.pin)),
            resumption_state_(rhs.resumption_state_.exchange(2,std::memory_order_acq_rel)) {}
        Suspension& operator=(Suspension&& rhs) noexcept {
            if(this!=&rhs) {
                context=rhs.context;
                owner=rhs.owner;
                exchange_generation=rhs.exchange_generation;
                pin=std::move(rhs.pin);
                resumption_state_.store(
                    rhs.resumption_state_.exchange(2,std::memory_order_acq_rel),
                    std::memory_order_release);
            }
            return *this;
        }
    };
    // Capture ONLY from an actual admitted physical turn. A foreign callback
    // producer with no active scope cannot manufacture a physical pin.
    std::optional<Suspension> suspend_current(
            const NativeHttpExchange& exchange, NativeHttpExchange::Token token) {
        Frame* f=current_frame_;
        if (!f || f->table!=this || f->exchange!=&exchange
                || f->exchange_generation!=token.generation
                || !admitted_for(f->scheduler,f->owner,f->dispatch_ticket)
                || f->dispatch_ticket!=token.dispatch_ticket
                || !same(f->owner,token.actor)
                || f->handle.value!=token.request_context
                || token.request_registry!=registry_id_ || !token.generation
                || exchange.phase()!=NativeHttpExchange::Phase::actor)
            return std::nullopt;
        const auto observed=exchange.token();
        if(observed.generation!=token.generation
                || !same(observed.actor,token.actor)
                || observed.request_context!=token.request_context
                || observed.request_registry!=registry_id_
                || observed.dispatch_ticket!=token.dispatch_ticket)
            return std::nullopt;
        auto pin=retain(f->handle,f->owner);
        if(!pin)return std::nullopt;
        return Suspension(f->handle,f->owner,token.generation,std::move(*pin));
    }
    template<typename Fn>
    bool resume_admitted(OresNativeActorScheduler* scheduler,
            const NativeHttpExchange& exchange,
            NativeHttpExchange::Token token, Suspension& suspended, Fn&& fn) {
        if(!suspended.pin || suspended.context.value!=suspended.pin.handle().value
                || suspended.context.value!=token.request_context
                || token.request_registry!=registry_id_
                || !token.dispatch_ticket
                || suspended.exchange_generation!=token.generation
                || !same(suspended.owner,token.actor)
                || !admitted_for(scheduler,suspended.owner,token.dispatch_ticket))
            return false;
        unsigned expected=0;
        if(!suspended.resumption_state_.compare_exchange_strong(expected,1,
                std::memory_order_acq_rel))return false;
        bool ok=false;
        try {
            ok=with_verified_turn(scheduler,exchange,token,suspended.context,
                    suspended.owner,true,std::forward<Fn>(fn));
        } catch (...) {
            suspended.resumption_state_.store(2,std::memory_order_release);
            throw;
        }
        suspended.resumption_state_.store(ok?2:0,std::memory_order_release);
        return ok;
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
    // Internal bridge only. The public initial path consumes the envelope
    // exactly once; a validated Suspension uses the continuation path.
private:
    template<typename Fn>
    bool with_verified_turn(OresNativeActorScheduler* scheduler,
            const NativeHttpExchange& exchange,
            NativeHttpExchange::Token token, Handle handle,
            OresNativeActorRef owner, bool continuation_resume, Fn&& callback) {
        static_assert(std::is_void_v<std::invoke_result_t<Fn&>>,
            "carrier binding must not return an escaped resource or awaitable");
        if(!admitted_for(scheduler,owner,token.dispatch_ticket) ||
                !token.generation
                || !valid(owner) || !same(token.actor,owner)
                || token.request_context!=handle.value
                || token.request_registry!=registry_id_
                || exchange.phase()!=NativeHttpExchange::Phase::actor)return false;
        const auto observed=exchange.token();
        if(observed.generation!=token.generation || !same(observed.actor,owner)
                || observed.request_context!=handle.value
                || observed.request_registry!=registry_id_
                || observed.dispatch_ticket!=token.dispatch_ticket)return false;
        auto lease=retain(handle,owner);
        if(!lease)return false;
        // Store lease prevents reuse; close revokes current() immediately.
        if(!lookup(handle,owner))return false;
        const Frame* outer=current_frame_;
        const bool nested=outer && outer->table==this &&
            outer->exchange==&exchange &&
            outer->exchange_generation==token.generation &&
            outer->dispatch_ticket==token.dispatch_ticket;
        if(!continuation_resume && !nested && !exchange.enter_initial(token))
            return false;
        Frame frame{this,handle,owner,scheduler,&exchange,
                    token.generation,token.dispatch_ticket,current_frame_};
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
        if(!admitted_for(scheduler,owner,token.dispatch_ticket))return false;
        callback();
        return true;
    }
public:
    // A repeated scheduler DATA ticket is not a new request invocation.
    // Resume only through a valid one-shot structured Suspension.
    template<typename Fn>
    bool with_admitted_turn(OresNativeActorScheduler* scheduler,
            const NativeHttpExchange& exchange,
            NativeHttpExchange::Token token, Handle handle,
            OresNativeActorRef owner, Fn&& callback) {
        return with_verified_turn(scheduler,exchange,token,handle,owner,
                                  false,std::forward<Fn>(callback));
    }
    // No untrusted choice of transport handle/owner at lookup: use only the
    // exchange's trusted ingress-bound buffer. The short guest borrow is
    // lexical; the separate host-owned read pin survives suspension.
    template<std::size_t Slots,std::size_t MaxBytes,typename Visitor>
    bool with_request_bytes(const BoundExchange<Slots,MaxBytes>& bound,
                            Visitor&& visitor) {
        static_assert(std::is_void_v<std::invoke_result_t<Visitor&,std::string_view>>,
            "request borrowed bytes visitor must return void");
        Frame* frame=current_frame_;
        const auto token=bound.token();
        if(!frame || frame->table!=this ||
            frame->exchange!=bound.exchange_pin_.exchange_ ||
            frame->exchange_generation!=token.generation ||
            frame->exchange->phase()!=NativeHttpExchange::Phase::actor ||
            !admitted_for(frame->scheduler,frame->owner,
                          frame->dispatch_ticket) ||
            frame->dispatch_ticket!=token.dispatch_ticket ||
            frame->handle.value!=token.request_context ||
            token.request_registry!=registry_id_ ||
            !same(frame->owner,token.actor) ||
            !lookup(frame->handle,frame->owner))return false;
        return bound.table_->with_borrow(bound.body_,bound.transport_owner_,
                std::forward<Visitor>(visitor));
    }
    // Convenience only, never an actor, heap or permissions accessor.
    static std::optional<Metadata> current() noexcept {
        auto* frame=current_frame_;
        return frame && admitted_for(frame->scheduler,frame->owner,
                                         frame->dispatch_ticket)
            && frame->exchange->phase()==NativeHttpExchange::Phase::actor
            && frame->exchange->token().generation==frame->exchange_generation
            && frame->exchange->token().dispatch_ticket==frame->dispatch_ticket
            ? frame->table->lookup(frame->handle,frame->owner) : std::nullopt;
    }
};
} // namespace oreslang::runtime
