// Host-only coexistence contract after merged scoped-local PR #1391.
// Canonical key/store types come from ScopedLocalStorageV1.hpp.
// Characterization-only legacy keys remain separately named to avoid
// conflicting aliases or accidental transport of ownership identities.
#include "oreslang/runtime/ScopedLocalStorageV1.hpp"
#include "oreslang/runtime/LocalContextDomainsV1.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

using namespace oreslang::runtime;
static_assert(!std::is_same_v<ActorLocalKeyV1<int>,
                              LegacyActorLocalKeyV1<int>>);
static_assert(!std::is_same_v<TaskLocalKeyV1<int>,
                              LegacyTaskLocalKeyV1<int>>);
static_assert(!std::is_same_v<ThreadLocalKeyV1<int>,
                              LegacyThreadLocalKeyV1<int>>);
namespace {
int successes = 0;
void check(const char* name, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "SCOPED_COEXIST_FAIL %s\n", name);
        std::abort();
    }
    ++successes;
    std::printf("SCOPED_COEXIST_PASS %s\n", name);
}
}
int main() {
    const ScopedLocalIdentityV1 canonical{71,9,18,3,1};
    const ExecutionStampV1 legacy{9,18,71,3,1,101,1};
    ActorLocalStoreV1 canonical_actor(canonical);
    auto legacy_actor = ActorLocalScopeV1::open(legacy);
    check("01_legacy_and_canonical_actors_construct", !!legacy_actor);
    ActorLocalKeyV1<std::string> canonical_key;
    LegacyActorLocalKeyV1<std::string> legacy_key;
    check("02_canonical_actor_write",
          canonical_actor.set(canonical_key,canonical,std::string("canon")));
    check("03_legacy_actor_write",
          legacy_actor->set(legacy,legacy_key,std::string("legacy")));
    check("04_canonical_actor_read_independent",
          canonical_actor.get_copy(canonical_key,canonical)==std::string("canon"));
    check("05_legacy_actor_read_independent",
          legacy_actor->get_copy(legacy,legacy_key)==std::string("legacy"));
    auto stale_canonical=canonical; ++stale_canonical.generation;
    auto stale_legacy=legacy; ++stale_legacy.actor_generation;
    check("06_canonical_rejects_stale_actor_generation",
          !canonical_actor.get_copy(canonical_key,stale_canonical));
    check("07_legacy_rejects_stale_actor_generation",
          !legacy_actor->get_copy(stale_legacy,legacy_key));
    TaskLocalStoreV1 canonical_task(canonical);
    auto legacy_task=TaskLocalScopeV1::open(legacy);
    TaskLocalKeyV1<int> canonical_task_key;
    LegacyTaskLocalKeyV1<int> legacy_task_key;
    check("08_task_scopes_construct",!!legacy_task);
    check("09_canonical_task_write",canonical_task.set(
          canonical_task_key,canonical,13));
    check("10_legacy_task_write",legacy_task->set(
          legacy,legacy_task_key,17));
    check("11_task_stores_remain_distinct",
          canonical_task.get_copy(canonical_task_key,canonical)==13 &&
          legacy_task->get_copy(legacy,legacy_task_key)==17);
    ThreadLocalKeyV1<int> canonical_thread;
    LegacyThreadLocalKeyV1<int> legacy_thread;
    check("12_canonical_thread_write",
          PhysicalThreadLocalStoreV1::set(canonical_thread,55));
    check("13_legacy_thread_write",legacy_thread.set(66));
    check("14_thread_stores_remain_distinct",
          PhysicalThreadLocalStoreV1::get_copy(canonical_thread)==55 &&
          legacy_thread.get_copy()==66);
    check("15_thread_cleanup",
          PhysicalThreadLocalStoreV1::remove(canonical_thread) &&
          legacy_thread.erase());
    check("16_actor_cleanup",canonical_actor.close() && legacy_actor->close());
    check("17_task_cleanup",canonical_task.close() && legacy_task->close());
    if(successes!=17) return 3;
    std::puts("NATIVE_SCOPED_COEXISTENCE_17_HOST_CASES_PASS");
    return 0;
}
