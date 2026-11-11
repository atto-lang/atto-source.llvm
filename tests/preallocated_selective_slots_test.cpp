#include "oreslang/runtime/PreallocatedSelectiveSlots.hpp"
#include <algorithm>
#include <cassert>
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
static_assert(!std::is_aggregate_v<Q::Cursor>);
static_assert(!std::is_move_constructible_v<Q>);
int main() {
  Q q(64), other(64);
  std::deque<std::pair<std::uint64_t,std::uint32_t>> model;
  std::uint64_t next=1;std::minstd_rand rng(20261110);
  for(int step=0;step<100000;step++){
    int op=rng()%4;
    if(op<=1){
      auto value=std::make_unique<int>(static_cast<int>(next));
      auto tag=std::uint32_t(1+rng()%5);
      bool added=q.push(value,8,tag,next);
      assert(added==(model.size()<64));
      if(added){assert(!value);model.emplace_back(next++,tag);}else assert(value);
    }else if(op==2){
      auto tag=std::uint32_t(1+rng()%5);
      auto it=std::find_if(model.begin(),model.end(),[&](auto e){return e.second==tag;});
      bool expect=it!=model.end();
      Q::Cursor cursor;bool ended=false;
      for(std::size_t tries=0;tries<66&&!ended;tries++){
        auto res=q.select(tag,1,cursor);
        assert(res.inspected<=1);
        if(res.status==Q::Status::Continue){assert(!res.frame);cursor=res.cursor;continue;}
        ended=true;
        if(expect){assert(res.status==Q::Status::Matched&&res.frame);
          assert(res.frame->sequence==it->first&&*res.frame->payload==static_cast<int>(it->first));
          model.erase(it);
        }else assert(res.status==Q::Status::NoMatch&&!res.frame);
      }
      assert(ended);
    }else{
      auto got=q.pop_front();
      assert(bool(got)==!model.empty());
      if(got){assert(got->sequence==model.front().first);model.pop_front();}
    }
    assert(q.size()==model.size()&&q.size()<=q.capacity());
  }
  while(auto got=q.pop_front()) {assert(got->sequence==model.front().first);model.pop_front();}
  assert(model.empty());
  auto a=std::make_unique<int>(1),b=std::make_unique<int>(2);
  assert(q.push(a,8,1,next++)); assert(q.push(b,8,2,next++));
  auto pending=q.select(7,1);assert(pending.status==Q::Status::Continue);
  assert(pending.cursor.through_sequence()==next-2);
  bool denied=false;
  try{other.select(7,1,pending.cursor);}catch(const std::invalid_argument&){denied=true;}assert(denied);
  auto removed=q.select(2,2);assert(removed.status==Q::Status::Matched);
  auto reused=std::make_unique<int>(3);assert(q.push(reused,8,4,next++));
  denied=false;try{q.select(7,1,pending.cursor);}catch(const std::invalid_argument&){denied=true;}assert(denied);
  assert(q.pop_front());assert(q.pop_front());
  auto left=std::make_unique<int>(11),middle=std::make_unique<int>(12),right=std::make_unique<int>(13);
  assert(q.push(left,8,1,next++));assert(q.push(middle,8,2,next++));
  assert(q.push(right,8,3,next++));
  auto prior=q.select(3,1);assert(prior.status==Q::Status::Continue);
  auto earlier=q.pop_front();assert(earlier); // removing inspected prefix must not invalidate next slot
  auto after_prior=q.select(3,2,prior.cursor);
  assert(after_prior.status==Q::Status::Matched&&after_prior.frame);
  assert(*after_prior.frame->payload==13);
  assert(q.pop_front());assert(q.size()==0);
  std::cout<<"PASS_PREALLOCATED_SELECTIVE_SLOTS 100000 operations + stale-cursor denial\n";
}
