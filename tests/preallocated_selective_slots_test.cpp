#include "oreslang/runtime/PreallocatedSelectiveSlots.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <limits>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <type_traits>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif
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

// Simulate an actor arena's physical-release hook. The fixed slot pool must
// transfer this callback-bearing unique owner without changing its deleter,
// refunding ownership prematurely, or calling cleanup on selective checkout.
struct ArenaCleanup {
  std::atomic<int>* released;
  void operator()(int* value) const noexcept {
    released->fetch_add(1,std::memory_order_relaxed);
    delete value;
  }
};
static_assert(std::is_nothrow_move_constructible_v<ArenaCleanup>);
static_assert(std::is_nothrow_invocable_v<ArenaCleanup&, int*>);
static void arena_owner_release_gate() {
  using ArenaSlots=PreallocatedSelectiveSlots<int,ArenaCleanup>;
  std::atomic<int> released{0};
  {
    ArenaSlots inbox(2);
    ArenaSlots::Owner one(new int(41),ArenaCleanup{&released});
    ArenaSlots::Owner two(new int(42),ArenaCleanup{&released});
    REQUIRE(inbox.push(one,8,9,1));
    REQUIRE(inbox.push(two,8,7,2));
    REQUIRE(!one && !two && released.load()==0);
    auto selected=inbox.select(7,2);
    REQUIRE(selected.status==ArenaSlots::Status::Matched && selected.frame);
    REQUIRE(*selected.frame->payload==42 && released.load()==0);
    REQUIRE(inbox.size()==1 && released.load()==0);
    selected.frame.reset(); // caller's physical quiescence/ACK boundary
    REQUIRE(released.load()==1);
    auto remaining=inbox.pop_front();
    REQUIRE(remaining && *remaining->payload==41);
    REQUIRE(released.load()==1);
    remaining.reset();
    REQUIRE(released.load()==2 && inbox.size()==0);
  }
  REQUIRE(released.load()==2);
}

// The slot pool deliberately has no internal mutex. This exercises its
// actual contract: a trusted host scheduler must serialize mutations under
// the physical mailbox lock. Payload ownership moves exactly once on publish.
static void concurrent_external_lock_oracle() {
  constexpr int senders=4, per_sender=1500;
  Q inbox(32);
  std::mutex physical_lock;
  std::atomic<int> admitted{0}, received{0};
  std::uint64_t next_sequence=1;
  std::array<std::thread, senders> producers;
  for(int sender=0;sender<senders;sender++) {
    producers[sender]=std::thread([&,sender] {
      for(int seq=0;seq<per_sender;seq++) {
        auto item=std::make_unique<int>(sender*65536+seq);
        bool published=false;
        while(!published) {
          {
            std::lock_guard<std::mutex> guard(physical_lock);
            published=inbox.push(item,8,std::uint32_t(sender+1),next_sequence);
            if(published) {
              ++next_sequence;
              admitted.fetch_add(1,std::memory_order_relaxed);
            }
          }
          if(!published) std::this_thread::yield();
        }
        REQUIRE(!item);
      }
    });
  }
  std::array<int, senders> last_seen{-1,-1,-1,-1};
  while(received.load(std::memory_order_relaxed)<senders*per_sender) {
    std::optional<Q::Frame> current;
    {
      std::lock_guard<std::mutex> guard(physical_lock);
      current=inbox.pop_front();
    }
    if(!current) { std::this_thread::yield(); continue; }
    int encoded=*current->payload;
    int sender=encoded/65536, seq=encoded%65536;
    REQUIRE(sender>=0 && sender<senders);
    REQUIRE(seq==last_seen[sender]+1);
    REQUIRE(current->tag==std::uint32_t(sender+1));
    last_seen[sender]=seq;
    received.fetch_add(1,std::memory_order_relaxed);
  }
  for(auto& producer:producers) producer.join();
  REQUIRE(admitted.load()==senders*per_sender);
  REQUIRE(received.load()==senders*per_sender);
  for(auto last:last_seen) REQUIRE(last==per_sender-1);
  REQUIRE(inbox.size()==0);
}

// Losing the last queued slot owner must not silently destroy an actor
// payload before its supervisor/ACK teardown fence. The child uses its own
// terminate handler so the parent verifies this fail-closed contract without
// killing the test runner. On non-POSIX platforms, execute this as a dedicated
// negative subprocess test rather than spawning an unchecked runner thread.
static void incomplete_teardown_aborts() {
#if defined(__unix__) || defined(__APPLE__)
  const pid_t pid=fork();
  REQUIRE(pid>=0);
  if(pid==0) {
    std::set_terminate([] { std::_Exit(97); });
    {
      Q queue(1);
      auto owned=std::make_unique<int>(42);
      if(!queue.push(owned,8,1,1)) std::_Exit(15);
    }
    std::_Exit(16);
  }
  int status=0;
  REQUIRE(waitpid(pid,&status,0)==pid);
  REQUIRE(WIFEXITED(status) && WEXITSTATUS(status)==97);
#endif
}

int main() {
  incomplete_teardown_aborts();
  arena_owner_release_gate();
  concurrent_external_lock_oracle();
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
