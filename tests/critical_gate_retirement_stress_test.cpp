#include "oreslang/runtime/CriticalGate.hpp"
#include "oreslang/runtime/LogicalMutex.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using oreslang::runtime::CriticalGate;
using oreslang::runtime::LogicalMutex;
static void require(bool ok, const char* msg) { if(!ok) throw std::runtime_error(msg); }
int main() {
  std::size_t passes=0;
  for (int round=0; round<256; ++round) {
    CriticalGate gate;
    auto owner=gate.try_request(); require(owner.has_value(),"try acquire"); ++passes;
    require(!gate.try_request().has_value(),"no double admission"); ++passes;
    auto queued1=gate.request(), queued2=gate.request();
    require(queued1.admission==CriticalGate::Admission::queued,"first queued"); ++passes;
    require(queued2.admission==CriticalGate::Admission::queued,"second queued"); ++passes;
    require(gate.release(*owner)==queued1.token,"first FIFO"); ++passes;
    require(gate.release(queued1.token)==queued2.token,"second FIFO"); ++passes;
    require(!gate.release(queued2.token),"drained FIFO"); ++passes;
    auto newOwner=gate.try_request(); require(newOwner.has_value(),"reuse"); ++passes;
    auto stale=gate.request(); require(gate.retire()==1,"retire waiter"); ++passes;
    require(!gate.quiescent(),"not quiescent until active exits"); ++passes;
    require(!gate.release(stale.token),"cancel stale"); ++passes;
    bool threw=false; try { (void)gate.request(); } catch(const std::logic_error&) {threw=true;}
    require(threw,"retired request rejected"); ++passes;
    require(!gate.release(*newOwner),"no retired handoff"); ++passes;
    require(gate.quiescent(),"quiescence fence"); ++passes;
  }
  for (int round=0;round<256;++round) {
    LogicalMutex<int> mutex(0);
    auto token=mutex.try_request(); require(token.has_value(),"mutex admission"); ++passes;
    std::atomic<bool> entered=false, finish=false;
    std::thread worker([&]{
      mutex.with_owner(*token,[&](int& value){
        entered.store(true,std::memory_order_release);
        while (!finish.load(std::memory_order_acquire)) std::this_thread::yield();
        ++value;
      });
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    require(mutex.retire()==0,"retired active mutex"); ++passes;
    require(!mutex.quiescent(),"not quiescent while body running"); ++passes;
    finish.store(true,std::memory_order_release);
    worker.join();
    bool threw=false;try{mutex.with_owner(*token,[](int& value){++value;});}catch(const std::logic_error&){threw=true;}
    require(threw,"retired mutex no callback"); ++passes;
    require(!mutex.release(*token),"retired mutex no handoff"); ++passes;
    require(mutex.quiescent(),"retired mutex quiescent"); ++passes;
  }
  std::cout << "PASS native header smoke assertions=" << passes << '\n';
}
