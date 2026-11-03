#pragma once
#include <array>
#include <cstdint>
#include <deque>
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
  std::vector<Event> recent;
  std::array<std::uint64_t,(std::size_t)EventKind::Count> counters{};
 };
 ActorReliabilityKernel(std::size_t cap,Clock clock):cap_(cap),clock_(std::move(clock)){
  if(cap_==0||cap_>65536||!clock_) throw std::invalid_argument("invalid reliability kernel");
 }
 void record(EventKind kind,std::uint64_t actor,std::uint64_t incarnation){
  if(incarnation==0) throw std::invalid_argument("invalid incarnation");
  std::lock_guard<std::mutex> g(mu_);
  const auto now=clock_();
  if(has_time_&&now<last_time_) throw std::logic_error("monotonic clock regressed");
  has_time_=true; last_time_=now;
  const auto i=(std::size_t)kind;
  if(i>=counters_.size()) throw std::invalid_argument("invalid event kind");
  ++counters_[i];
  if(recent_.size()==cap_){recent_.pop_front();++overwritten_;}
  recent_.push_back({next_++,now,kind,actor,incarnation});
 }
 Snapshot snapshot()const{
  std::lock_guard<std::mutex> g(mu_);
  return {next_,overwritten_,std::vector<Event>(recent_.begin(),recent_.end()),counters_};
 }
 class RestartBudget final {
 public:
  struct Decision { bool admitted; std::uint64_t retry_after; std::size_t in_window; };
  RestartBudget(std::size_t max,std::uint64_t window,std::uint64_t quarantine,Clock clock)
   :max_(max),window_(window),quarantine_(quarantine),clock_(std::move(clock)),times_(max){
   if(max_==0||max_>1024||window_==0||quarantine_==0||!clock_) throw std::invalid_argument("invalid restart budget");
  }
  Decision try_acquire(){
   std::lock_guard<std::mutex> g(mu_); const auto now=checked();
   while(size_&&now>=times_[start_]&&now-times_[start_]>=window_){start_=(start_+1)%times_.size();--size_;}
   if(now<blocked_) return {false,blocked_-now,size_};
   if(size_>=max_){blocked_=sat_add(now,quarantine_);return {false,quarantine_,size_};}
   times_[(start_+size_)%times_.size()]=now;++size_;return {true,0,size_};
  }
 private:
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
   if(shed_&&q<=low_)shed_=false; if(!shed_&&q>=high_)shed_=true;
   return shed_?Admission::Shed:Admission::Accept;
  }
 private: std::size_t cap_,high_,low_; bool shed_{}; std::mutex mu_;
 };
 class BackoffPolicy final {
 public:
  BackoffPolicy(std::uint64_t base,std::uint64_t max):base_(base),max_(max){if(!base_||max_<base_)throw std::invalid_argument("invalid backoff");}
  std::uint64_t delay(std::uint32_t attempt,std::uint64_t salt)const{
   auto cap=base_;for(std::uint32_t i=0;i<attempt&&cap<max_;++i)cap=cap>max_/2?max_:cap*2;
   const auto floor=cap/2?cap/2:1; const auto span=cap-floor+1;
   return floor+mix(salt^(UINT64_C(0x9E3779B97F4A7C15)*(attempt+1ULL)))%span;
  }
 private:
  static std::uint64_t mix(std::uint64_t z){z=(z^(z>>30))*UINT64_C(0xBF58476D1CE4E5B9);z=(z^(z>>27))*UINT64_C(0x94D049BB133111EB);return z^(z>>31);}
  std::uint64_t base_,max_;
 };
private:
 std::size_t cap_; Clock clock_; mutable std::mutex mu_; std::deque<Event> recent_;
 std::array<std::uint64_t,(std::size_t)EventKind::Count> counters_{};
 std::uint64_t next_{},overwritten_{},last_time_{}; bool has_time_{};
};
}
