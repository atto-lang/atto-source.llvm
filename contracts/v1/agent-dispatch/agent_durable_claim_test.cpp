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
  assert(local_claim_filename(req)=="094cebe33e39cfe9c78133d726cbe2ea792b663bd24c596b77b903dc1dbf3e66.claim");
  assert(store.claim_once(req));
  assert(!store.claim_once(req));
  auto changed=req;changed.context_sha256=std::string(64,'c');
  assert(!store.claim_once(changed)); // different payload, same identity
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
  // Owner-only private root is mandatory.
  assert(::chmod(dir.c_str(),0755)==0);
  assert(!store.claim_once(request("insecure-dir")));
  assert(::chmod(dir.c_str(),0700)==0);
  const std::string link=dir+"-link";
  assert(::symlink(dir.c_str(),link.c_str())==0);
  assert(!DurableLocalDispatchClaim(link).claim_once(request("symlink-dir")));
  assert(::unlink(link.c_str())==0);
  std::filesystem::remove_all(dir);
  return 0;
}
