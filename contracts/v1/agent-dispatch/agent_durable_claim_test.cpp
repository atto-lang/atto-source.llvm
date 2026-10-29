// POSIX host-only durability/replay checks; never a native Oreslang guest.
#include "DurableLocalDispatchClaim.hpp"
#include <cassert>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>
using namespace oreslang::agent_dispatch;
static Scope request(const char* label) {
  return {"tenant", "worker", std::string(40, 'a'), label,
          std::string(64, 'b'), 7, 9, 10000, 1024, 24};
}
int main() {
  char path[] = "/tmp/ores-agent-claim-XXXXXX";
  char* name = ::mkdtemp(path);
  assert(name != nullptr);
  const std::string dir(name);
  DurableLocalDispatchClaim store(dir);
  auto req=request("dispatch");
  assert(local_claim_filename(req)=="0d308e5482aa6701e9dc55b9c1e70e311eb83188e26c8bf86befa838c2d11e21.claim");
  assert(store.claim_once(req));
  assert(!store.claim_once(req));
  auto changed=req;changed.context_sha256=std::string(64,'c');
  assert(!store.claim_once(changed)); // different payload, same identity
  auto reincarnated=req;
  reincarnated.generation=8;reincarnated.epoch=10;
  reincarnated.exact_revision=std::string(40,'c');
  assert(local_claim_filename(req)==local_claim_filename(reincarnated));
  assert(!store.claim_once(reincarnated));
  auto independent=request("independent");
  assert(store.claim_once(independent));
  auto invalid=request("../invalid"); // ID may contain '/' but filename is opaque hash
  assert(store.claim_once(invalid));
  // Atomic CREATE_NEW survives races between independent processes.
  auto racing=request("racing");
  int winning=0;
  for (int i=0; i<12; ++i) {
    pid_t pid=::fork(); assert(pid>=0);
    if(pid==0) ::_exit(store.claim_once(racing) ? 0 : 2);
  }
  for(int i=0;i<12;++i){int status=0; assert(::wait(&status)>0);
    assert(WIFEXITED(status));
    if(WEXITSTATUS(status)==0) ++winning;
    else assert(WEXITSTATUS(status)==2);
  }
  assert(winning==1);
  // SIGKILL after durable claim: process cannot cause duplicate dispatch.
  auto killed=request("killed");
  pid_t pid=::fork();assert(pid>=0);
  if(pid==0){if(!store.claim_once(killed)) ::_exit(9);::kill(::getpid(),SIGKILL);::_exit(8);}
  int status=0;assert(::waitpid(pid,&status,0)==pid);
  assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
  assert(!store.claim_once(killed));
  // HostGate -> durable store integration. The second process-equivalent gate
  // cannot submit the same logical effect, even with a different digest.
  auto integrated=request("integrated");
  int sends=0;
  TrustedHost callbacks{
    [](const Scope&){return true;}, [](const Scope&){return true;},
    [&](const Scope& item){return store.claim_once(item);},
    [&](const Scope&){sends++;return true;},
    [](){return std::uint64_t{9000};}};
  HostGate first("tenant","worker",7,9,std::string(40,'a'),100);
  assert(first.dispatch(integrated,callbacks,9000));
  HostGate second("tenant","worker",7,9,std::string(40,'a'),100);
  integrated.context_sha256=std::string(64,'c');
  assert(!second.dispatch(integrated,callbacks,9000));
  assert(sends==1);
  // Owner-only private root is mandatory.
  assert(::chmod(dir.c_str(),0755)==0);
  assert(!store.claim_once(request("insecure-dir")));
  assert(::chmod(dir.c_str(),0700)==0);
  const std::string link=dir+"-link";
  assert(::symlink(dir.c_str(),link.c_str())==0);
  assert(!DurableLocalDispatchClaim(link).claim_once(request("symlink-dir")));
  assert(::unlink(link.c_str())==0);
  // A symlink in an ancestor directory must be rejected too.
  const std::string actual_parent = dir + "/real";
  const std::string inner = actual_parent + "/private";
  assert(::mkdir(actual_parent.c_str(), 0700) == 0);
  assert(::mkdir(inner.c_str(), 0700) == 0);
  const std::string parent_link = dir + "/linked-parent";
  assert(::symlink(actual_parent.c_str(), parent_link.c_str()) == 0);
  assert(!DurableLocalDispatchClaim(parent_link + "/private")
      .claim_once(request("ancestor-link")));
  assert(!DurableLocalDispatchClaim(inner + "/../private")
      .claim_once(request("dotdot-path")));
  assert(!DurableLocalDispatchClaim("relative-private")
      .claim_once(request("relative-path")));
  assert(!DurableLocalDispatchClaim(inner + "/")
      .claim_once(request("trailing-slash")));
  std::filesystem::remove_all(dir);
  return 0;
}
