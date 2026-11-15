#include "oreslang/runtime/ActorReliabilityKernel.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using K=oreslang::runtime::ActorReliabilityKernel;
int main(){
 std::atomic<std::uint64_t> clock{1};
 K k(8,[&]{return clock.fetch_add(1);});
 for(int i=0;i<100;++i)k.record(K::EventKind::Failed,7,1);
 auto s=k.snapshot();
 assert(s.recent.size()==8&&s.overwritten==92);
 assert(s.counters[(std::size_t)K::EventKind::Failed]==100);

 std::atomic<std::uint64_t> rc{0};
 K::RestartBudget budget(4,100,50,[&]{return rc.load();});
 assert(budget.try_acquire(3).admitted);
 rc=10;assert(budget.try_acquire().admitted);
 rc=100;auto denied=budget.try_acquire();
 assert(!denied.admitted&&denied.retry_after==50);
 rc=149;assert(!budget.try_acquire().admitted);
 rc=151;assert(budget.try_acquire().admitted);

 K::OverloadGate gate(100,80,40);
 assert(gate.admit(79,false)==K::OverloadGate::Admission::Accept);
 assert(gate.admit(80,false)==K::OverloadGate::Admission::Shed);
 assert(gate.admit(60,false)==K::OverloadGate::Admission::Shed);
 assert(gate.admit(100,true)==K::OverloadGate::Admission::ControlAccept);
 assert(gate.admit(40,false)==K::OverloadGate::Admission::Accept);

 K::BackoffPolicy backoff(10,1000);
 bool different=false;
 for(std::uint32_t i=1;i<=30;++i){
  const auto a=backoff.delay(i,42),b=backoff.delay(i,42),c=backoff.delay(i,43);
  assert(a==b&&a>=10&&a<=1000);different|=a!=c;
 }
 try{(void)backoff.delay(0,42);assert(false);}catch(const std::invalid_argument&){}
 assert(different);

 std::uint64_t observed=10;
 K observation(4,[&]{return observed;});
 observation.record(K::EventKind::Failed,11,1);
 observed=9;
 observation.record(K::EventKind::Failed,11,1);
 auto os=observation.snapshot();
 assert(os.recent.size()==1&&os.clock_regressions==1&&os.dropped_observer_events==1);

 std::atomic<std::uint64_t> cc{1000};
 K concurrent(64,[&]{return cc.fetch_add(1);});
 std::vector<std::thread> threads;
 for(int w=0;w<8;++w)threads.emplace_back([&]{for(int i=0;i<2000;++i)concurrent.record(K::EventKind::Failed,9,1);});
 for(auto& th:threads)th.join();
 auto cs=concurrent.snapshot();
 assert(cs.counters[(std::size_t)K::EventKind::Failed]==16000);
 assert(cs.recent.size()==64&&cs.overwritten==15936);
 for(std::size_t i=1;i<cs.recent.size();++i){
  assert(cs.recent[i-1].seq<cs.recent[i].seq);
  assert(cs.recent[i-1].time<=cs.recent[i].time);
 }
 std::cout<<"NATIVE_ACTOR_RELIABILITY_KERNEL_PASS\n";
}
