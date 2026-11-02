#include "oreslang/runtime/CriticalGate.hpp"
#include "oreslang/runtime/LogicalMutex.hpp"
#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <array>
using oreslang::runtime::CriticalGate;
using oreslang::runtime::LogicalMutex;
static std::size_t checks=0;
static void expect(bool condition,const char* msg) { if(!condition) throw std::runtime_error(msg); ++checks; }
int main() {
  // 8,193 protected increments through explicit bounded FIFO handoff.
  LogicalMutex<int> lock(0);
  const auto first=lock.request();
  std::vector<CriticalGate::Token> queued;
  queued.reserve(8192);
  for(int i=0;i<8192;++i){auto r=lock.request();expect(r.admission==CriticalGate::Admission::queued,"FIFO admit");queued.push_back(r.token);}
  bool over=false; try{(void)lock.request();}catch(const std::overflow_error&){over=true;}
  expect(over,"bounded FIFO overflow");
  lock.with_owner(first.token,[](int& x){++x;});
  auto next=lock.release(first.token);
  for (std::size_t i=0;i<queued.size();++i){
    expect(next==queued[i],"FIFO token order");
    lock.with_owner(next,[](int& x){++x;});
    next=lock.release(next);
  }
  expect(!next,"FIFO drained");
  const auto last=lock.request();
  expect(lock.with_owner(last.token,[](int& x){return x;})==8193,"exact updates");
  expect(!lock.release(last.token),"final release");
  const auto done=lock.retire();
  expect(!done.owner && done.cancelled_waiters.empty(),"no phantom retirement tickets");
  expect(lock.quiescent(),"quiescent");
  // Stress retirement vs FIFO transfer: exactly one cancellation or handoff.
  for (int i=0;i<1000;++i){
    CriticalGate gate;
    const auto owner=gate.request(), waiter=gate.request();
    CriticalGate::Retirement ret;
    CriticalGate::Token transfer;
    std::thread release([&](){transfer=gate.release(owner.token);});
    std::thread retire([&](){ret=gate.retire();});
    release.join();retire.join();
    if (ret.cancelled_waiters.size()==1) {
      expect(!transfer,"cancelled ticket must not be handed off");
      expect(ret.cancelled_waiters.front()==waiter.token,"cancelled receipt");
      expect(ret.owner==owner.token,"owner receipt");
    } else {
      expect(ret.cancelled_waiters.empty(),"unexpected cancellation");
      expect(transfer==waiter.token,"transferred token");
      expect(ret.owner==waiter.token,"transferred owner receipt");
      expect(!gate.release(waiter.token),"retired active permit release");
    }
    expect(gate.quiescent(),"eventual retirement quiescence");
  }
  std::cout << "PASS native CriticalGate+LogicalMutex checks="<<checks<<"\n";
}
