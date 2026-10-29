// Host-only callback-contract tests, not actual actor guest or trusted provider.
#include "AgentHostDispatchGate.hpp"
#include <cassert>
#include <stdexcept>
using namespace oreslang::agent_dispatch;
static Scope valid() {
  return {"tenant", "worker", std::string(40, 'a'), "dispatch", std::string(64, 'b'),
          7, 9, 10000, 1024, 24};
}
static HostGate gate() { return {"tenant", "worker", 7, 9, std::string(40,'a'), 100}; }
int main() {
  const auto req = valid();
  int current=0, auth=0, claimed=0, submitted=0;
  TrustedHost host {
    [&](const Scope&){ ++current; return true; },
    [&](const Scope&){ ++auth; return true; },
    [&](const Scope&){ ++claimed; return claimed == 1; },
    [&](const Scope&){ ++submitted; return true; },
    [](){return std::uint64_t{9000};}
  };
  auto g=gate();
  assert(g.dispatch(req,host,9000)); assert(!g.dispatch(req,host,9000));
  assert(current==2 && auth==2 && claimed==1 && submitted==1);
  {auto h=gate(); auto bad=req; bad.generation++; assert(!h.dispatch(bad,host,9000));}
  {auto h=gate(); auto bad=req; bad.epoch++; assert(!h.dispatch(bad,host,9000));}
  {auto h=gate(); auto bad=req; bad.tenant_id="other"; assert(!h.dispatch(bad,host,9000));}
  {auto h=gate(); auto bad=req; bad.context_sha256[0]='X'; assert(!h.dispatch(bad,host,9000));}
  {auto h=gate(); auto bad=req; bad.expires_at_ms=9000; assert(!h.dispatch(bad,host,9000));}
  {auto h=gate(); auto bad=req; bad.measured_tokens=101; assert(!h.dispatch(bad,host,9000));}
  {auto h=gate(); auto bad=req; bad.utf8_bytes=65537; assert(!h.dispatch(bad,host,9000));}
  {auto h=gate(); TrustedHost bad=host; bad.claim_once={}; assert(!h.dispatch(req,bad,9000));}
  {auto h=gate(); TrustedHost bad=host; bad.authorize=[](const Scope&){return false;}; assert(!h.dispatch(req,bad,9000));}
  {auto h=gate(); int calls=0; TrustedHost bad=host;
   bad.current_revision=[&](const Scope&){return ++calls==1;};
   int sends=0; bad.claim_once=[](const Scope&){return true;};
   bad.submit_data_only=[&](const Scope&){sends++;return true;};
   assert(!h.dispatch(req,bad,9000)); assert(calls==2 && sends==0);}
  {auto h=gate(); int calls=0; TrustedHost bad=host;
   bad.claim_once=[](const Scope&){return true;};
   bad.authorize=[&](const Scope&){return ++calls==1;};
   int sends=0;bad.submit_data_only=[&](const Scope&){sends++;return true;};
   assert(!h.dispatch(req,bad,9000));assert(calls==2 && sends==0);}
  {auto h=gate(); int sends=0;TrustedHost bad=host;
   bad.claim_once=[](const Scope&){throw std::runtime_error("IO");return true;};
   bad.submit_data_only=[&](const Scope&){sends++;return true;};
   assert(!h.dispatch(req,bad,9000));assert(sends==0);}
  {auto h=gate();int sends=0;TrustedHost bad=host;
   bad.claim_once=[](const Scope&){return true;};
   bad.submit_data_only=[&](const Scope&){sends++;throw std::runtime_error("unknown outcome");return true;};
   assert(!h.dispatch(req,bad,9000));assert(!h.dispatch(req,bad,9000));assert(sends==1);}
  {auto h=gate(); std::uint64_t clock=9000; int sends=0;
   TrustedHost bad=host;
   bad.claim_once=[&](const Scope&){clock=10000;return true;};
   bad.now_ms=[&](){return clock;};
   bad.submit_data_only=[&](const Scope&){sends++;return true;};
   assert(!h.dispatch(req,bad,9000));assert(sends==0);}
  {auto h=gate(); TrustedHost bad=host; bad.now_ms={};
   assert(!h.dispatch(req,bad,9000));}
  {auto h=gate(); TrustedHost bad=host;
   bad.now_ms=[]()->std::uint64_t {throw std::runtime_error("clock unavailable");};
   assert(!h.dispatch(req,bad,9000));}
  return 0;
}
