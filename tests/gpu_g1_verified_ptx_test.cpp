#include "oreslang/backend/GpuG1VerifiedPtx.hpp"
#include <openssl/evp.h>
#include <array>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
using namespace oreslang::gpu::g1;
namespace {
std::uint64_t count=0;
void check(bool b){++count;if(!b)std::abort();}
template<class F> void reject(F f){bool hit=false;try{f();}catch(const std::invalid_argument&){hit=true;}check(hit);}
std::string digest(std::string_view input){
 std::array<unsigned char,32> bytes{};unsigned int n=0;
 check(EVP_Digest(input.data(),input.size(),bytes.data(),&n,EVP_sha256(),nullptr)==1&&n==32);
 const char *h="0123456789abcdef";std::string out;out.reserve(64);
 for(unsigned char b:bytes){out.push_back(h[b>>4]);out.push_back(h[b&15]);}return out;
}
std::string code=".version 7.0\n.target sm_50\n.visible .entry atto_g1_saxpy_f32() { ret; }\n";
PinnedG1Ptx pins(){return {std::string(40,'a'),std::string(40,'b'),std::string(40,'c'),std::string(64,'d'),digest(code),"atto_g1_saxpy_f32",1};}
struct FakeLoader {
 using Module=std::uint64_t;using Kernel=std::uint64_t;
 int load=0,resolve=0,unload=0;bool fail_resolve=false,fail_unload=false;
 Module load_ptx(const std::string& txt){check(txt.size()==code.size()+1&&txt.back()=='\0');++load;return 51;}
 Kernel resolve_kernel(Module m,std::string_view name){check(m==51&&name==GpuG1PtxGate::sole_entry);++resolve;if(fail_resolve)throw std::invalid_argument("resolve");return 73;}
 void unload_module(Module m){check(m==51);++unload;if(fail_unload)throw std::invalid_argument("unload");}
};
struct NoWork {bool idle=true;void require_quiescent_shutdown()const{if(!idle)throw std::invalid_argument("busy");}};
}
int main(){
 FakeLoader loader;const auto original=pins();GpuG1PtxGate::verify(code,original);check(loader.load==0);
 {auto p=original;p.entrypoint="any";reject([&]{GpuG1PtxGate::verify(code,p);});}
 {auto p=original;p.abi_version=2;reject([&]{GpuG1PtxGate::verify(code,p);});}
 {auto p=original;p.compiler_sha256[2]='X';reject([&]{GpuG1PtxGate::verify(code,p);});}
 {auto p=original;p.head_sha1[0]='A';reject([&]{GpuG1PtxGate::verify(code,p);});}
 {auto p=original;p.tree_sha1.pop_back();reject([&]{GpuG1PtxGate::verify(code,p);});}
 {auto p=original;p.fixture_blob_sha1[10]='g';reject([&]{GpuG1PtxGate::verify(code,p);});}
 {auto p=original;p.ptx_sha256[3]='z';reject([&]{GpuG1PtxGate::verify(code,p);});}
 {auto p=original;p.ptx_sha256[0]=(p.ptx_sha256[0]=='e'?'f':'e');reject([&]{GpuG1PtxGate::verify(code,p);});}
 reject([&]{GpuG1PtxGate::verify("",original);});
 reject([&]{GpuG1PtxGate::verify(std::string(1024*1024+1,'a'),original);});
 reject([&]{GpuG1PtxGate::verify(code+std::string("\0evil",5),original);});
 {auto mutated=code;mutated[0]='?';reject([&]{GpuG1PtxGate::verify(mutated,original);});}
 for(int i=0;i<1024;i++){
   auto modified=code;modified[i%modified.size()]=static_cast<char>((static_cast<unsigned char>(modified[i%modified.size()])+1)%128);
   reject([&]{GpuG1PtxGate::verify(modified,original);});
 }
 check(loader.load==0);
 NoWork busy;
 {GpuG1VerifiedPtxModule<FakeLoader> m(loader,code,original);check(loader.load==1&&loader.resolve==1);
  auto token=m.token();check(token.kernel()==73);reject([&]{m.close_after_quiescence(busy);});
 }
 check(loader.unload==0); // deliberately leak module rather than untracked unload
 {GpuG1VerifiedPtxModule<FakeLoader> m(loader,code,original);busy.idle=false;
  reject([&]{m.close_after_quiescence(busy);});busy.idle=true;
  {auto t=m.token();reject([&]{m.close_after_quiescence(busy);});check(t.kernel()==73);}
  m.close_after_quiescence(busy);check(loader.unload==1);
  reject([&]{m.close_after_quiescence(busy);});reject([&]{(void)m.token();});
 }
 {auto ld=FakeLoader{};ld.fail_resolve=true;
  reject([&]{GpuG1VerifiedPtxModule<FakeLoader> m(ld,code,original);});
  check(ld.load==1&&ld.unload==1);
 }
 {FakeLoader ld;ld.fail_resolve=true;ld.fail_unload=true;
  bool quarantined=false;
  try {GpuG1VerifiedPtxModule<FakeLoader> m(ld,code,original);}
  catch(const std::runtime_error& e) {
    quarantined=std::string(e.what()).find("GPU-G1-MODULE-008")!=std::string::npos;
  }
  check(quarantined && ld.load==1 && ld.unload==1);
 }
 // A live kernel token must fence a concurrent unload attempt. Minting and
 // closing are serialized by the module's own lock, not by caller timing.
 {GpuG1VerifiedPtxModule<FakeLoader> m(loader,code,original);
  std::atomic<unsigned> holders{0};std::atomic<bool> release{false};
  std::atomic<unsigned> wrong{0};std::vector<std::thread> tasks;
  for(unsigned j=0;j<16;++j) tasks.emplace_back([&]{
      auto token=m.token();holders.fetch_add(1,std::memory_order_release);
      while(!release.load(std::memory_order_acquire))std::this_thread::yield();
      if(token.kernel()!=73)wrong.fetch_add(1);
  });
  while(holders.load(std::memory_order_acquire)!=16)std::this_thread::yield();
  check(holders==16);
  reject([&]{m.close_after_quiescence(busy);});
  release.store(true,std::memory_order_release);
  for(auto& t:tasks)t.join();
  check(wrong==0);
  loader.fail_unload=true;
  reject([&]{m.close_after_quiescence(busy);});
  loader.fail_unload=false;
  m.close_after_quiescence(busy);
  check(loader.unload>=3);
 }
 check(loader.load==3);
 std::cout<<"PASS "<<count<<" host-only verified PTX gate and mutation assertions\n";
}
