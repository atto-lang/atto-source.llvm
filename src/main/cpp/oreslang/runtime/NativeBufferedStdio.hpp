#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace oreslang::runtime {

// Serialized trusted descriptor owner only. No scheduler or guest ABI.
// Pending receipt slots are reserved before publication; promotion splices
// the existing payload node without allocating or invoking guest callbacks.
class NativeBufferedStdio final {
public:
    struct Owner { std::uint64_t execution{}, generation{}; };
    struct Ticket { std::uint64_t descriptor{}, id{}; Owner owner{}; };
    struct SinkTicket { std::uint64_t descriptor{}, id{}; };
    enum class Result { Accepted, Pending, Quota, Invalid, Oversize, Closed, Failed, Stale };
    struct Admission { Result result; Ticket ticket{}; };
    struct Output { SinkTicket ticket; std::string payload; };
private:
    enum class State { Free, Pending, Ready };
    struct Receipt { State state = State::Free; Ticket ticket{}; Result result = Result::Stale; };
    struct Record { std::uint64_t id; std::string payload; std::size_t slot; };
    static constexpr std::size_t no_slot = 64;
    inline static std::atomic<std::uint64_t> descriptor_ids{0};
    const std::uint64_t descriptor_;
    const std::size_t records_limit_, bytes_limit_, waiter_limit_, pending_limit_;
    std::list<Record> accepted_, waiting_;
    std::array<Receipt,64> receipts_{};
    std::optional<SinkTicket> active_;
    std::size_t active_bytes_=0, accepted_bytes_=0, pending_bytes_=0;
    std::uint64_t next_=0;
    bool sealed_=false, failed_=false;

    static std::uint64_t descriptor_id() {
        auto current=descriptor_ids.load(std::memory_order_relaxed);
        for (;;) {
            if (current==std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("stdio descriptor IDs exhausted");
            if (descriptor_ids.compare_exchange_weak(current,current+1,std::memory_order_relaxed))
                return current+1;
        }
    }
    static bool same(Owner a, Owner b) noexcept {
        return a.execution==b.execution && a.generation==b.generation;
    }
    bool fits(std::size_t n) const noexcept {
        return accepted_records()<records_limit_ && n<=bytes_limit_-accepted_bytes_;
    }
    static bool valid_utf8(std::string_view text) noexcept {
        std::size_t i=0;
        while(i<text.size()) {
            const auto a=static_cast<unsigned char>(text[i++]);
            if(a<0x80) continue;
            unsigned n=0, lo=0x80, hi=0xbf;
            if(a>=0xc2 && a<=0xdf) n=1;
            else if(a>=0xe0 && a<=0xef) { n=2; if(a==0xe0)lo=0xa0; if(a==0xed)hi=0x9f; }
            else if(a>=0xf0 && a<=0xf4) { n=3; if(a==0xf0)lo=0x90; if(a==0xf4)hi=0x8f; }
            else return false;
            if(n>text.size()-i) return false;
            auto b=static_cast<unsigned char>(text[i++]);
            if(b<lo || b>hi) return false;
            for(unsigned j=1;j<n;++j) {
                b=static_cast<unsigned char>(text[i++]);
                if(b<0x80 || b>0xbf) return false;
            }
        }
        return true;
    }
    std::size_t find(Owner owner,Ticket ticket) const noexcept {
        if(ticket.descriptor!=descriptor_ || !same(owner,ticket.owner)) return no_slot;
        for(std::size_t i=0;i<waiter_limit_;++i) {
            const auto& r=receipts_[i];
            if(r.state!=State::Free && r.ticket.id==ticket.id && same(r.ticket.owner,owner)) return i;
        }
        return no_slot;
    }
    void promote() noexcept {
        if(sealed_ || failed_) return;
        while(!waiting_.empty() && fits(waiting_.front().payload.size())) {
            auto& r=waiting_.front();
            pending_bytes_-=r.payload.size(); accepted_bytes_+=r.payload.size();
            auto& receipt=receipts_[r.slot];
            receipt.state=State::Ready; receipt.result=Result::Accepted;
            r.slot=no_slot;
            accepted_.splice(accepted_.end(),waiting_,waiting_.begin());
        }
    }
    void reject_pending(Result result) noexcept {
        for(const auto& r:waiting_) {
            auto& receipt=receipts_[r.slot];
            receipt.state=State::Ready; receipt.result=result;
        }
        waiting_.clear(); pending_bytes_=0;
    }
public:
    explicit NativeBufferedStdio(std::size_t records=1024,std::size_t bytes=4194304,
                                  std::size_t waiters=64,std::size_t pending=1048576)
      :descriptor_(descriptor_id()),records_limit_(records),bytes_limit_(bytes),
       waiter_limit_(waiters),pending_limit_(pending) {
        if(!records || records>1024 || !bytes || bytes>4194304 ||
           !waiters || waiters>64 || !pending || pending>1048576)
            throw std::invalid_argument("stdio limits");
    }
    NativeBufferedStdio(const NativeBufferedStdio&)=delete;
    NativeBufferedStdio& operator=(const NativeBufferedStdio&)=delete;
    NativeBufferedStdio(NativeBufferedStdio&&)=delete;
    NativeBufferedStdio& operator=(NativeBufferedStdio&&)=delete;
    std::size_t accepted_records() const noexcept {return accepted_.size()+(active_.has_value()?1:0);}
    std::size_t accepted_bytes() const noexcept {return accepted_bytes_;}
    std::size_t pending_records() const noexcept {return waiting_.size();}
    std::size_t pending_bytes() const noexcept {return pending_bytes_;}
    bool drained() const noexcept {return accepted_records()==0;}
    Admission admit(Owner owner,std::string_view text) {
        if(!owner.execution || !owner.generation) return {Result::Invalid};
        if(failed_) return {Result::Failed};
        if(sealed_) return {Result::Closed};
        if(text.size()>65536 || text.size()>bytes_limit_) return {Result::Oversize};
        if(!valid_utf8(text)) return {Result::Invalid};
        if(next_==std::numeric_limits<std::uint64_t>::max()) return {Result::Quota};
        const auto id=next_+1;
        if(waiting_.empty() && fits(text.size())) {
            accepted_.push_back({id,std::string(text),no_slot});
            next_=id; accepted_bytes_+=text.size();
            return {Result::Accepted};
        }
        if(text.size()>pending_limit_-pending_bytes_) return {Result::Quota};
        std::size_t slot=0;
        while(slot<waiter_limit_ && receipts_[slot].state!=State::Free) ++slot;
        if(slot==waiter_limit_) return {Result::Quota};
        waiting_.push_back({id,std::string(text),slot}); // allocation before publishing slot
        next_=id; pending_bytes_+=text.size();
        Ticket ticket{descriptor_,id,owner};
        receipts_[slot]={State::Pending,ticket,Result::Pending};
        return {Result::Pending,ticket};
    }
    Result take_ready(Owner owner,Ticket ticket) noexcept {
        auto slot=find(owner,ticket);
        if(slot==no_slot) return Result::Stale;
        auto& r=receipts_[slot];
        if(r.state==State::Pending) return Result::Pending;
        auto result=r.result; r=Receipt{}; return result;
    }
    bool cancel(Owner owner,Ticket ticket) noexcept {
        auto slot=find(owner,ticket);
        if(slot==no_slot || receipts_[slot].state!=State::Pending) return false;
        for(auto i=waiting_.begin();i!=waiting_.end();++i) {
            if(i->slot==slot) {
                pending_bytes_-=i->payload.size(); waiting_.erase(i);
                receipts_[slot]=Receipt{}; promote(); return true;
            }
        }
        return false;
    }
    void retire(Owner owner) noexcept {
        for(auto i=waiting_.begin();i!=waiting_.end();) {
            auto& r=receipts_[i->slot];
            if(same(r.ticket.owner,owner)) {
                pending_bytes_-=i->payload.size(); r=Receipt{}; i=waiting_.erase(i);
            } else ++i;
        }
        for(auto& r:receipts_) if(r.state==State::Ready && same(r.ticket.owner,owner))r=Receipt{};
        promote();
    }
    std::optional<Output> take_sink() noexcept {
        if(active_ || failed_ || accepted_.empty())return std::nullopt;
        auto& r=accepted_.front();
        active_=SinkTicket{descriptor_,r.id}; active_bytes_=r.payload.size();
        Output out{*active_,std::move(r.payload)};
        accepted_.pop_front(); return out;
    }
    Result finish_sink(SinkTicket ticket,bool success) noexcept {
        if(!active_ || active_->descriptor!=ticket.descriptor || active_->id!=ticket.id)return Result::Stale;
        active_.reset(); accepted_bytes_-=active_bytes_; active_bytes_=0;
        if(!success){abort();return Result::Failed;}
        if(failed_)return Result::Failed;
        promote();return Result::Accepted;
    }
    void seal() noexcept {sealed_=true;reject_pending(Result::Closed);}
    void abort() noexcept {
        if(failed_)return;
        failed_=true;sealed_=true;reject_pending(Result::Failed);
        accepted_.clear();accepted_bytes_=active_bytes_;
    }
};
} // namespace oreslang::runtime
