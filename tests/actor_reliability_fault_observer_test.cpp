#include "oreslang/runtime/ActorReliabilityKernel.h"
#include <atomic>
#include <cassert>
#include <cstdint>
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
}
