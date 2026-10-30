// Real PostgreSQL host-only integration test, not a native Oreslang guest.
#include "PostgresActorDispatchClaim.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
using namespace oreslang::agent_dispatch;
static Scope request(const std::string& dispatch, const char digest='b') {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  return {"tenant", "worker", std::string(40,'a'), dispatch,
          std::string(64,digest), 7, 9,
          static_cast<std::uint64_t>(now+300000), 1024, 24};
}
int main(int argc, char** argv) {
  const char* conninfo = std::getenv("ORES_TEST_PG_CONNINFO");
  assert(conninfo && *conninfo); // Test harness provides isolated DB credentials.
  PostgresActorDispatchClaim db(conninfo);
  if (argc > 1 && (std::string(argv[1]) == "probe" ||
                   std::string(argv[1]) == "probe-hold" ||
                   std::string(argv[1]) == "probe-newgen")) {
    assert(argc == 3);
    auto candidate=request(argv[2]);
    if (std::string(argv[1]) == "probe-newgen") {
      candidate.generation=8;candidate.epoch=10;
      candidate.exact_revision=std::string(40,'c');
    }
    if (!db.claim_once(candidate)) return 2;
    if (std::string(argv[1]) == "probe-hold") {
      std::cout << "CLAIM_COMMITTED\n" << std::flush;
      std::this_thread::sleep_for(std::chrono::seconds(30));
    }
    return 0;
  }
  auto unbound = request("native-unbound");unbound.tenant_id="unbound-tenant";
  assert(!db.claim_once(unbound));
  auto stale = request("native-stale");stale.generation=8;
  assert(!db.claim_once(stale));
  auto expired = request("native-expired");expired.expires_at_ms=1;
  assert(!db.claim_once(expired));
  auto invalid = request("native-invalid");invalid.context_sha256="x";
  assert(!db.claim_once(invalid));
  const auto scope = request("native-hundred");
  std::atomic<int> winners{0}, next{0};
  std::vector<std::thread> workers;
  for (int i=0; i<16; ++i) workers.emplace_back([&]{
    while(next.fetch_add(1)<100) if(db.claim_once(scope)) winners.fetch_add(1);
  });
  for (auto& t:workers) t.join();
  assert(winners.load()==1);
  assert(!db.claim_once(request("native-hundred",'c')));
  std::cout << "PG_DURABLE_CLAIM_NATIVE_PASS 100_candidates_one_winner=true\n";
  return 0;
}
