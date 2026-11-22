#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace oreslang::runtime {
class ActorReliabilityKernel final {
public:
 enum class EventKind:std::size_t { Failed, RestartAdmitted, RestartSuppressed, OverloadShed, Count };
 struct Event { std::uint64_t seq; std::uint64_t time; EventKind kind; std::uint64_t actor; std::uint64_t incarnation; };
 using Clock=std::function<std::uint64_t()>;
 struct Snapshot {
  std::uint64_t next{}; std::uint64_t overwritten{};
  std::uint64_t dropped_observer_events{}; std::uint64_t clock_regressions{};
  std::vector<Event> recent;
  std::array<std::uint64_t,(std::size_t)EventKind::Count> counters{};
 };
 // Reserve event storage at construction. Recording failures must not
 // allocate for every event or when evicting a ring entry.
 ActorReliabilityKernel(std::size_t cap,Clock clock)
   :cap_(checked_capacity(cap)),clock_(std::move(clock)),ring_(cap_){
  if(!clock_) throw std::invalid_argument("invalid reliability kernel");
 }
 void record(EventKind kind,std::uint64_t actor,std::uint64_t incarnation){
  if(incarnation==0) throw std::invalid_argument("invalid incarnation");
  const auto i=(std::size_t)kind;
  if(i>=counters_.size()) throw std::invalid_argument("invalid event kind");
  std::lock_guard<std::mutex> g(mu_);
  std::uint64_t now{};
  try { now=clock_(); }
  catch(...){ dropped_observer_events_=sat_inc(dropped_observer_events_); return; }
  if(has_time_&&now<last_time_){
   clock_regressions_=sat_inc(clock_regressions_);
   dropped_observer_events_=sat_inc(dropped_observer_events_);
   return;
  }
  has_time_=true; last_time_=now;
  if(sequence_exhausted_){dropped_observer_events_=sat_inc(dropped_observer_events_);return;}
  const auto seq=next_;
  if(next_==UINT64_MAX)sequence_exhausted_=true; else ++next_;
  counters_[i]=sat_inc(counters_[i]);
  if(size_==cap_) overwritten_=sat_inc(overwritten_);
  else ++size_;
  ring_[write_] = {seq,now,kind,actor,incarnation};
  write_=(write_+1)%cap_;
 }
 Snapshot snapshot()const{
  std::lock_guard<std::mutex> g(mu_);
  Snapshot result{};
  result.next=next_;
  result.overwritten=overwritten_;
  result.dropped_observer_events=dropped_observer_events_;
  result.clock_regressions=clock_regressions_;
  result.counters=counters_;
  result.recent.reserve(size_);
  const std::size_t oldest = size_==cap_ ? write_ : 0;
  for(std::size_t i=0;i<size_;++i)
   result.recent.push_back(ring_[(oldest+i)%cap_]);
  return result;
 }
 class RestartBudget final {
 public:
  struct Decision { bool admitted; std::uint64_t retry_after; std::size_t in_window; };
  RestartBudget(std::size_t max,std::uint64_t window,std::uint64_t quarantine,Clock clock)
   :max_(max),window_(window),quarantine_(quarantine),clock_(std::move(clock)),times_(checked_max(max)){
   if(window_==0||quarantine_==0||!clock_) throw std::invalid_argument("invalid restart budget");
  }
  Decision try_acquire(std::size_t requested=1){
   if(requested==0)throw std::invalid_argument("requested restarts must be positive");
   std::lock_guard<std::mutex> g(mu_); const auto now=checked();
   while(size_&&now>=times_[start_]&&now-times_[start_]>window_){start_=(start_+1)%times_.size();--size_;}
   if(now<blocked_) return {false,blocked_-now,size_};
   if(requested>max_||size_>max_-requested){
    blocked_=sat_add(now,quarantine_);return {false,quarantine_,size_};
   }
   for(std::size_t i=0;i<requested;++i){times_[(start_+size_)%times_.size()]=now;++size_;}
   return {true,0,size_};
  }
 private:
  // Validate before allocating the restart ring. A huge, invalid max must
  // never attempt an unbounded allocation on a supervisor failure path.
  static std::size_t checked_max(std::size_t max){
   if(max==0||max>1024) throw std::invalid_argument("invalid restart budget");
   return max;
  }
  std::uint64_t checked(){const auto n=clock_();if(has_&&n<last_)throw std::logic_error("monotonic clock regressed");has_=true;last_=n;return n;}
  static std::uint64_t sat_add(std::uint64_t a,std::uint64_t b){return UINT64_MAX-a<b?UINT64_MAX:a+b;}
  std::size_t max_,start_{},size_{}; std::uint64_t window_,quarantine_,blocked_{},last_{}; bool has_{};
  Clock clock_; std::vector<std::uint64_t> times_; std::mutex mu_;
 };
 class OverloadGate final {
 public:
  enum class Admission { Accept,Shed,ControlAccept };
  OverloadGate(std::size_t cap,std::size_t high,std::size_t low):cap_(cap),high_(high),low_(low){
   if(cap_==0||low_>=high_||high_>cap_)throw std::invalid_argument("invalid overload gate");
  }
  Admission admit(std::size_t q,bool control){
   std::lock_guard<std::mutex> g(mu_); if(q>cap_)throw std::invalid_argument("queue depth");
   if(control)return Admission::ControlAccept;
   if(shed_&&q<=low_)shed_=false;
   if(!shed_&&q>=high_)shed_=true;
   return shed_?Admission::Shed:Admission::Accept;
  }
 private: std::size_t cap_,high_,low_; bool shed_{}; std::mutex mu_;
 };
 class BackoffPolicy final {
 public:
  BackoffPolicy(std::uint64_t base,std::uint64_t max):base_(base),max_(max){if(!base_||max_<base_)throw std::invalid_argument("invalid backoff");}
  std::uint64_t delay(std::uint32_t attempt,std::uint64_t salt)const{
   if(attempt==0)throw std::invalid_argument("attempt must be positive");
   auto delay=base_;
   for(std::uint32_t i=1;i<attempt&&delay<max_;++i)
    delay=delay>max_/2?max_:delay*2;
   const auto remaining=max_-delay;
   const auto spread_cap=std::min(delay>>3,remaining);
   if(spread_cap==0)return delay;
   return delay+mix(salt^(UINT64_C(0x9E3779B97F4A7C15)*attempt))%(spread_cap+1);
  }
 private:
  static std::uint64_t mix(std::uint64_t z){z=(z^(z>>30))*UINT64_C(0xBF58476D1CE4E5B9);z=(z^(z>>27))*UINT64_C(0x94D049BB133111EB);return z^(z>>31);}
  std::uint64_t base_,max_;
 };
private:
 static std::uint64_t sat_inc(std::uint64_t v){return v==UINT64_MAX?UINT64_MAX:v+1;}
 static std::size_t checked_capacity(std::size_t cap){
  if(cap==0||cap>65536) throw std::invalid_argument("invalid reliability kernel");
  return cap;
 }
 std::size_t cap_; Clock clock_; std::vector<Event> ring_; mutable std::mutex mu_;
 std::size_t size_{},write_{};
 std::array<std::uint64_t,(std::size_t)EventKind::Count> counters_{};
 std::uint64_t next_{},overwritten_{},dropped_observer_events_{},clock_regressions_{},last_time_{};
 bool has_time_{},sequence_exhausted_{};
};
}
