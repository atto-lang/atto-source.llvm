#pragma once
#include "oreslang/runtime/NativeSelectArbiter.hpp"
#include "oreslang/runtime/NativeSelectableI64Channel.hpp"
#include "oreslang/runtime/NativeSelectableI64Future.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

namespace oreslang::runtime {

class NativeSelectExhausted final : public std::runtime_error {
public:
    NativeSelectExhausted() : std::runtime_error("SelectExhausted") {}
};

// Trusted host-only scalar owner pump. No scheduler/actor/guest lowering.
// Each round uses the existing arbiter and identity-fenced subscriptions.
// Producer callbacks retain only a round-specific advisory atomic.
template<std::size_t ChannelCapacity, std::size_t MaxWaiters=64, std::size_t MaxCases=256>
class NativeCountedI64Cursor final {
public:
    using Channel = NativeSelectableI64Channel<ChannelCapacity,MaxWaiters>;
    using Future = NativeSelectableI64Future<MaxWaiters>;
    using Source = std::variant<std::shared_ptr<Channel>,std::shared_ptr<Future>>;
    using Projection = std::function<std::int64_t(std::int64_t)>;
    enum class State { Ready, Waiting, Success, Cancelled, Failed };
private:
    using Arbiter = NativeSelectArbiter<MaxCases>;
    using Registration = typename Arbiter::Registration;
    using Token = std::variant<typename Channel::Subscription,typename Future::Subscription>;
    struct Case {
        Registration registration;
        Token token;
    };
    struct Round {
        Arbiter arbiter;
        std::shared_ptr<std::atomic_bool> dirty = std::make_shared<std::atomic_bool>(true);
        std::vector<Case> cases;
    };
    std::thread::id owner_ = std::this_thread::get_id();
    std::size_t count_, position_=0;
    bool fair_, driving_=false;
    State state_=State::Ready;
    std::vector<Source> live_;
    std::vector<Projection> projections_;
    std::vector<std::int64_t> values_;
    std::unique_ptr<Round> round_;
    std::exception_ptr failure_;

    void owner() const {
        if(std::this_thread::get_id()!=owner_ || driving_)
            throw std::logic_error("counted cursor requires serialized host owner");
    }
    struct Drive {
        bool& busy;
        explicit Drive(bool& b):busy(b){busy=true;}
        ~Drive(){busy=false;}
    };
    static bool same(const Source& a,const Source& b) {
        if(a.index()!=b.index())return false;
        if(auto c=std::get_if<std::shared_ptr<Channel>>(&a))
            return *c==std::get<std::shared_ptr<Channel>>(b);
        return std::get<std::shared_ptr<Future>>(a)->same_source(*std::get<std::shared_ptr<Future>>(b));
    }
    void cleanup() {
        if(!round_)return;
        round_->arbiter.cancel();
        auto error=round_->arbiter.cleanup_failure();
        round_.reset();
        if(error)std::rethrow_exception(error);
    }
    void fail(std::exception_ptr error) {
        try {cleanup();}catch(...){error=std::current_exception();}
        live_.clear();projections_.clear();values_.clear();failure_=std::move(error);state_=State::Failed;
    }
    void advance(std::size_t index,bool retire) {
        auto n=live_.size(), next=(index+1)%n;
        if(!retire){position_=next;return;}
        Source winner=live_[index];
        std::size_t next_index=n;
        for(std::size_t j=0;j<n;++j){
            auto k=(next+j)%n;
            if(!same(live_[k],winner)){next_index=k;break;}
        }
        position_=0;
        if(next_index!=n)
            for(std::size_t k=0;k<next_index;++k)if(!same(live_[k],winner))++position_;
        // Keep lexical arm positions aligned, even when Future wrappers
        // share the same underlying source identity.
        for(std::size_t k=n;k>0;--k){
            auto i=k-1;
            if(same(live_[i],winner)){
                live_.erase(live_.begin()+static_cast<std::ptrdiff_t>(i));
                if(!projections_.empty())
                    projections_.erase(projections_.begin()+static_cast<std::ptrdiff_t>(i));
            }
        }
    }
public:
    NativeCountedI64Cursor(std::size_t count,const std::vector<Source>& sources,bool fair=true,
            const std::vector<Projection>& projections={})
        :count_(count),fair_(fair) {
        if(count>1024)throw std::invalid_argument("SelectCountInvalid");
        if(!count){state_=State::Success;return;}
        if(sources.empty()||sources.size()>MaxCases)throw std::invalid_argument("counted source count");
        for(const auto& source:sources) {
            if(!std::visit([](const auto& s){return bool(s);},source))
                throw std::invalid_argument("null counted source");
            if(auto future=std::get_if<std::shared_ptr<Future>>(&source);
                    future && !(*future)->valid_source())
                throw std::invalid_argument("moved-from counted Future");
        }
        if(!projections.empty() && projections.size()!=sources.size())
            throw std::invalid_argument("one counted projector per lexical arm required");
        for(const auto& projector:projections)
            if(!projector)throw std::invalid_argument("empty counted projector");
        live_=sources;projections_=projections;values_.reserve(count);
    }
    NativeCountedI64Cursor(const NativeCountedI64Cursor&)=delete;
    NativeCountedI64Cursor& operator=(const NativeCountedI64Cursor&)=delete;
    ~NativeCountedI64Cursor(){try{cleanup();}catch(...){}}
    State state() const{owner();return state_;}
    bool dirty() const{owner();return round_&&round_->dirty->load();}
    std::exception_ptr failure() const{owner();return failure_;}
    std::vector<std::int64_t> result() const{
        owner();
        if(state_!=State::Success)throw std::logic_error("batch not successful");
        return values_;
    }
    void begin_round(){
        owner();
        if(state_!=State::Ready)throw std::logic_error("round not ready");
        Drive drive(driving_);
        try{
            round_=std::make_unique<Round>();
            round_->cases.reserve(live_.size());
            state_=State::Waiting;
            for(std::size_t i=0;i<live_.size();++i){
                auto registration=round_->arbiter.register_case(i);
                auto notify=[dirty=round_->dirty](const auto&){dirty->store(true);};
                Token token;
                if(auto channel=std::get_if<std::shared_ptr<Channel>>(&live_[i]))
                    token=(*channel)->subscribe_readable_advisory(notify);
                else token=std::get<std::shared_ptr<Future>>(live_[i])->subscribe_ready_advisory(notify);
                try {
                    registration.arm([token]{std::visit([](const auto& t){(void)t.close();},token);});
                }catch(...){
                    std::visit([](const auto& t){(void)t.close();},token);
                    throw;
                }
                round_->cases.push_back(Case{std::move(registration),std::move(token)});
            }
        }catch(...){fail(std::current_exception());throw;}
    }
    bool pump(){
        owner();
        if(state_!=State::Waiting||!round_)throw std::logic_error("no active round");
        Drive drive(driving_);
        try{
            round_->dirty->store(false);
            auto n=live_.size(), start=fair_?position_:0;
            for(std::size_t k=0;k<n;++k){
                auto i=(start+k)%n;
                auto& c=round_->cases[i];
                bool retire=false, eof=false;
                std::int64_t value=0;
                std::exception_ptr error;
                if(auto source=std::get_if<std::shared_ptr<Channel>>(&live_[i])){
                    typename Channel::ReadResult result;
                    typename Channel::NotificationBatch notifications;
                    bool won=c.registration.try_win_if([&]{
                        return (*source)->try_commit_read(std::get<typename Channel::Subscription>(c.token),result,notifications);
                    });
                    notifications.dispatch();
                    if(!won)continue;
                    value=result.value;eof=result.status==Channel::ReadStatus::Eof;retire=eof;
                }else{
                    auto source_future=std::get<std::shared_ptr<Future>>(live_[i]);
                    typename Future::Result result;
                    bool won=c.registration.try_win_if([&]{
                        return source_future->try_commit(std::get<typename Future::Subscription>(c.token),result);
                    });
                    if(!won)continue;
                    value=result.value;retire=true;
                    if(result.status!=Future::Status::Value)
                        error=result.failure?result.failure:std::make_exception_ptr(std::runtime_error("selected Future cancelled"));
                }
                cleanup();
                if(error){fail(error);return true;}
                // Only the serialized owner invokes trusted projections,
                // after loser revocation and notification cleanup.
                if(!eof)values_.push_back(projections_.empty()?value:projections_[i](value));
                advance(i,retire);
                if(values_.size()==count_){live_.clear();projections_.clear();state_=State::Success;}
                else if(live_.empty())fail(std::make_exception_ptr(NativeSelectExhausted()));
                else state_=State::Ready;
                return true;
            }
            return false;
        }catch(...){fail(std::current_exception());throw;}
    }
    bool cancel(){
        owner();
        if(state_!=State::Ready&&state_!=State::Waiting)return false;
        Drive drive(driving_);
        try{
            cleanup();values_.clear();live_.clear();projections_.clear();state_=State::Cancelled;return true;
        }catch(...){fail(std::current_exception());throw;}
    }
};
} // namespace oreslang::runtime
