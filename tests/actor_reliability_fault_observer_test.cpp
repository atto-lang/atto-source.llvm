#include "oreslang/runtime/ActorReliabilityKernel.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using K=oreslang::runtime::ActorReliabilityKernel;

int main(){
 // Invalid construction fails before attempting unbounded allocation.
 for (const auto cap : {std::size_t{0},std::size_t{65537}}) {
  try { K invalid(cap, []{return std::uint64_t{0};}); assert(false); }
  catch(const std::invalid_argument&){}
 }

 // Invalid restart-burst quota must fail before constructing a giant vector.
 for (const auto quota : {std::size_t{0}, std::size_t{1025},
                          std::numeric_limits<std::size_t>::max()}) {
  try { K::RestartBudget invalid(quota, 100, 5,
                                []{return std::uint64_t{1};}); assert(false); }
  catch(const std::invalid_argument&){}
 }
 K::RestartBudget valid(1024, 100, 5, []{return std::uint64_t{1};});
 assert(valid.try_acquire(1024).admitted);
 assert(!valid.try_acquire().admitted);

 std::uint64_t clock=0;
 K k(3,[&]()->std::uint64_t{if(clock==4)throw 7;return clock++;});
 for(unsigned i=0;i<4;++i) k.record(K::EventKind::Failed,10,i+1);
 auto before=k.snapshot();
 assert(before.next==4&&before.overwritten==1&&before.recent.size()==3);
 for(unsigned i=0;i<3;++i){
  assert(before.recent[i].seq==i+1);
  assert(before.recent[i].actor==10);
  assert(before.recent[i].incarnation==i+2);
 }
 // Exceptions not derived from std::exception must not escape observation.
 k.record(K::EventKind::Failed,10,6);
 auto after=k.snapshot();
 assert(after.next==before.next&&after.overwritten==before.overwritten);
 assert(after.dropped_observer_events==1&&after.counters[0]==4);
 clock=5;
 k.record(K::EventKind::RestartAdmitted,20,7);
 after=k.snapshot();
 assert(after.next==5&&after.overwritten==2&&after.recent.size()==3);
 assert(after.recent[0].seq==2&&after.recent[1].seq==3&&after.recent[2].seq==4);

 // Invalid enum must be rejected before calling arbitrary clock() or
 // perturbing sequence/time/observer counters.
 const auto old_clock=clock;
 const auto old=after;
 try { k.record(static_cast<K::EventKind>(255),20,7);assert(false); }
 catch(const std::invalid_argument&){}
 assert(clock==old_clock);
 after=k.snapshot();
 assert(after.next==old.next&&after.overwritten==old.overwritten);
 assert(after.dropped_observer_events==old.dropped_observer_events);

 // Concurrent writes must remain contiguous and chronologically ordered.
 std::atomic<std::uint64_t> ticks{1};
 K concurrent(31,[&]{return ticks.fetch_add(1);});
 std::vector<std::thread> writers;
 for(int t=0;t<4;++t)writers.emplace_back([&]{
  for(int i=0;i<4000;++i)concurrent.record(K::EventKind::Failed,22,1);
 });
 for(auto& writer:writers)writer.join();
 auto snap=concurrent.snapshot();
 assert(snap.next==16000&&snap.overwritten==15969&&snap.recent.size()==31);
 assert(snap.counters[0]==16000&&snap.dropped_observer_events==0);
 for(std::size_t i=0;i<snap.recent.size();++i){
  assert(snap.recent[i].seq==15969+i);
  if(i)assert(snap.recent[i-1].time<snap.recent[i].time);
 }
 // Model-check the observer ring against a separately maintained FIFO
 // across small/large capacities, wraparound, and every event category.
 for (const std::size_t cap : {std::size_t{1}, std::size_t{2},
                               std::size_t{7}, std::size_t{64},
                               std::size_t{1024}}) {
  std::uint64_t tick=100;
  K oracle(cap,[&]{return tick++;});
  std::deque<K::Event> expected;
  std::array<std::uint64_t,(std::size_t)K::EventKind::Count> counts{};
  for(std::uint64_t i=0;i<10000;++i){
   const auto kind=static_cast<K::EventKind>(i%counts.size());
   const auto actor=100+i%13;
   const auto incarnation=1+i%97;
   oracle.record(kind,actor,incarnation);
   ++counts[(std::size_t)kind];
   if(expected.size()==cap)expected.pop_front();
   expected.push_back({i,100+i,kind,actor,incarnation});
   if(i%113==0||i==9999){
    const auto snapshot=oracle.snapshot();
    assert(snapshot.next==i+1);
    assert(snapshot.overwritten==(i+1>cap?i+1-cap:0));
    assert(snapshot.dropped_observer_events==0);
    assert(snapshot.clock_regressions==0);
    assert(snapshot.counters==counts);
    assert(snapshot.recent.size()==expected.size());
    for(std::size_t n=0;n<expected.size();++n){
     const auto& a=snapshot.recent[n];
     const auto& b=expected[n];
     assert(a.seq==b.seq&&a.time==b.time&&a.kind==b.kind);
     assert(a.actor==b.actor&&a.incarnation==b.incarnation);
    }
   }
  }
  // Bad incarnation is a pre-clock input failure, not an observer event.
  const auto stopped_at=tick;
  const auto before_bad=oracle.snapshot();
  try { oracle.record(K::EventKind::Failed,1,0);assert(false); }
  catch(const std::invalid_argument&){}
  assert(tick==stopped_at);
  const auto after_bad=oracle.snapshot();
  assert(after_bad.next==before_bad.next);
  assert(after_bad.overwritten==before_bad.overwritten);
  assert(after_bad.counters==before_bad.counters);
 }

 // Concurrent restart decisions must account for exactly the admitted burst,
 // not race between a read of size_ and a later write of its timestamp ring.
 std::atomic<std::uint64_t> held_clock{17};
 K::RestartBudget racing(7, 100, 25, [&]{return held_clock.load();});
 std::atomic<int> restart_admitted{0};
 std::atomic<int> restart_suppressed{0};
 std::vector<std::thread> racers;
 for(int t=0;t<8;++t)racers.emplace_back([&]{
  for(int j=0;j<100;++j){
   const auto result=racing.try_acquire();
   if(result.admitted)restart_admitted.fetch_add(1);
   else restart_suppressed.fetch_add(1);
  }
 });
 for(auto& racer:racers)racer.join();
 assert(restart_admitted==7&&restart_suppressed==793);
 assert(!racing.try_acquire().admitted); // still quarantined
 held_clock=43; // quarantine elapsed, but old window still full
 assert(!racing.try_acquire().admitted);
 held_clock=118; // original attempts are now outside the window
 assert(racing.try_acquire().admitted);

 // A broken clock must never grant a restart or poison the lock. Time
 // regression also fails closed and is recoverable after a valid tick.
 std::uint64_t phase_clock=100;
 bool fail_clock=false;
 K::RestartBudget clock_guard(2, 100, 10, [&]() -> std::uint64_t {
  if(fail_clock) throw std::runtime_error("clock unavailable");
  return phase_clock;
 });
 assert(clock_guard.try_acquire().admitted);
 fail_clock=true;
 try { (void)clock_guard.try_acquire();assert(false); }
 catch(const std::runtime_error&){}
 fail_clock=false;
 phase_clock=99;
 try { (void)clock_guard.try_acquire();assert(false); }
 catch(const std::logic_error&){}
 phase_clock=101;
 assert(clock_guard.try_acquire().admitted);
 assert(!clock_guard.try_acquire().admitted);

}
