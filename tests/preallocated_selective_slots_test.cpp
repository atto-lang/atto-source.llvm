#include "oreslang/runtime/PreallocatedSelectiveSlots.hpp"
#include <algorithm>
#include <limits>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <type_traits>
#include <vector>
using oreslang::runtime::PreallocatedSelectiveSlots;
using Q=PreallocatedSelectiveSlots<int>;
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error("preallocated selective contract: " #x); } while(false)
template<class F> void require_invalid(F&& f) {
  bool rejected=false;
  try { f(); } catch (const std::invalid_argument&) { rejected=true; }
  REQUIRE(rejected);
}
static_assert(!std::is_aggregate_v<Q::Cursor>);
static_assert(!std::is_move_constructible_v<Q>);
int main() {
  // Reject bad capacities before allocation, rather than requesting an
  // enormous vector and accidentally throwing std::bad_alloc/length_error.
  require_invalid([] { Q bad(0); });
  require_invalid([] { Q bad(Q::max_capacity+1); });
  require_invalid([] { Q bad(std::numeric_limits<std::size_t>::max()); });
  Q q(64), other(64);
  REQUIRE(q.capacity()==64 && q.size()==0);
  auto first=std::make_unique<int>(42);
  require_invalid([&] { (void)q.push(first,0,1,1); });
  require_invalid([&] { (void)q.push(first,8,1,0); });
  REQUIRE(first && *first==42 && q.size()==0);
  std::deque<std::pair<std::uint64_t,std::uint32_t>> model;
  std::uint64_t next=1;std::minstd_rand rng(20261110);
  for(int step=0;step<100000;step++){
    int op=rng()%4;
    if(op<=1){
      auto value=std::make_unique<int>(static_cast<int>(next));
      auto tag=std::uint32_t(1+rng()%5);
      bool added=q.push(value,8,tag,next);
      REQUIRE(added==(model.size()<64));
      if(added){REQUIRE(!value);model.emplace_back(next++,tag);}else REQUIRE(value);
    }else if(op==2){
      auto tag=std::uint32_t(1+rng()%5);
      auto it=std::find_if(model.begin(),model.end(),[&](auto e){return e.second==tag;});
      bool expect=it!=model.end();
      Q::Cursor cursor;bool ended=false;
      for(std::size_t tries=0;tries<66&&!ended;tries++){
        auto res=q.select(tag,1,cursor);
        REQUIRE(res.inspected<=1);
        if(res.status==Q::Status::Continue){REQUIRE(!res.frame);cursor=res.cursor;continue;}
        ended=true;
        if(expect){REQUIRE(res.status==Q::Status::Matched&&res.frame);
          REQUIRE(res.frame->sequence==it->first&&*res.frame->payload==static_cast<int>(it->first));
          model.erase(it);
        }else REQUIRE(res.status==Q::Status::NoMatch&&!res.frame);
      }
      REQUIRE(ended);
    }else{
      auto got=q.pop_front();
      REQUIRE(bool(got)==!model.empty());
      if(got){REQUIRE(got->sequence==model.front().first);model.pop_front();}
    }
    REQUIRE(q.size()==model.size()&&q.size()<=q.capacity());
  }
  while(auto got=q.pop_front()) {REQUIRE(got->sequence==model.front().first);model.pop_front();}
  REQUIRE(model.empty());
  auto a=std::make_unique<int>(1),b=std::make_unique<int>(2);
  REQUIRE(q.push(a,8,1,next++)); REQUIRE(q.push(b,8,2,next++));
  auto pending=q.select(7,1);REQUIRE(pending.status==Q::Status::Continue);
  REQUIRE(pending.cursor.through_sequence()==next-2);
  bool denied=false;
  try{other.select(7,1,pending.cursor);}catch(const std::invalid_argument&){denied=true;}REQUIRE(denied);
  auto removed=q.select(2,2);REQUIRE(removed.status==Q::Status::Matched);
  auto reused=std::make_unique<int>(3);REQUIRE(q.push(reused,8,4,next++));
  denied=false;try{q.select(7,1,pending.cursor);}catch(const std::invalid_argument&){denied=true;}REQUIRE(denied);
  REQUIRE(q.pop_front());REQUIRE(q.pop_front());
  auto left=std::make_unique<int>(11),middle=std::make_unique<int>(12),right=std::make_unique<int>(13);
  REQUIRE(q.push(left,8,1,next++));REQUIRE(q.push(middle,8,2,next++));
  REQUIRE(q.push(right,8,3,next++));
  auto prior=q.select(3,1);REQUIRE(prior.status==Q::Status::Continue);
  auto earlier=q.pop_front();REQUIRE(earlier); // removing inspected prefix must not invalidate next slot
  auto after_prior=q.select(3,2,prior.cursor);
  REQUIRE(after_prior.status==Q::Status::Matched&&after_prior.frame);
  REQUIRE(*after_prior.frame->payload==13);
  REQUIRE(q.pop_front());REQUIRE(q.size()==0);
  // A failed duplicate/old sequence request cannot steal a sender handle.
  auto invalid=std::make_unique<int>(77);
  require_invalid([&] { (void)q.push(invalid,8,1,next-1); });
  REQUIRE(invalid && *invalid==77 && q.size()==0);
  std::cout<<"PASS_PREALLOCATED_SELECTIVE_SLOTS 100000 operations + stale-cursor denial\n";
}
