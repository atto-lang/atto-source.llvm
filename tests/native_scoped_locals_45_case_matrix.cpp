// 45 named host-level scoped-local regression assertions.
// This is not a compiled .atto guest / provenance attestation.
#include "oreslang/runtime/LocalContextDomainsV1.hpp"
#include "oreslang/runtime/RequestContextLocalV1.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::runtime;
namespace {
unsigned passed = 0;
void check(const char* name, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "SCOPED_LOCAL_CASE_FAILED: %s\n", name);
        std::abort();
    }
    std::printf("SCOPED_LOCAL_CASE_PASS: %s\n", name);
    ++passed;
}
}
int main() {
    const ExecutionStampV1 owner{9, 18, 71, 3, 1, 101, 1};
    auto other_tenant = owner; other_tenant.tenant = 10;
    auto other_isolate = owner; other_isolate.isolate = 19;
    auto other_actor = owner; other_actor.actor_slot = 4;
    auto reincarnated = owner; reincarnated.actor_generation = 2;
    auto sibling = owner; sibling.task_id = 102;
    auto reused_task = owner; reused_task.task_generation = 2;

    check("01_invalid_actor_owner", !ActorLocalScopeV1::open({}));
    check("02_invalid_task_owner", !TaskLocalScopeV1::open({}));
    auto actor = ActorLocalScopeV1::open(owner);
    auto task = TaskLocalScopeV1::open(owner);
    check("03_actor_open", !!actor);
    check("04_task_open", !!task);
    LegacyActorLocalKeyV1<std::string> akey;
    LegacyTaskLocalKeyV1<std::string> tkey;
    check("05_actor_write", actor->set(owner, akey, std::string("actor")));
    check("06_actor_read", actor->get_copy(owner, akey) == std::string("actor"));
    check("07_actor_wrong_tenant", !actor->get_copy(other_tenant, akey));
    check("08_actor_wrong_isolate", !actor->get_copy(other_isolate, akey));
    check("09_actor_restarted_generation", !actor->get_copy(reincarnated, akey));
    check("10_actor_foreign_slot", !actor->set(other_actor, akey, std::string("bad")));
    check("11_task_write", task->set(owner, tkey, std::string("parent")));
    check("12_task_read", task->get_copy(owner, tkey) == std::string("parent"));
    check("13_task_sibling_denied", !task->get_copy(sibling, tkey));
    check("14_task_aba_generation_denied", !task->get_copy(reused_task, tkey));
    auto child = TaskLocalScopeV1::open(sibling);
    check("15_child_task_open", !!child);
    check("16_task_not_implicitly_inherited", !child->get_copy(sibling, tkey));
    check("17_explicit_snapshot_copy", task->copy_to_child(owner, *child, sibling, tkey));
    check("18_child_snapshot_visible", child->get_copy(sibling, tkey) == std::string("parent"));
    check("19_parent_replacement", task->set(owner, tkey, std::string("updated")));
    check("20_child_not_mutated_by_parent", child->get_copy(sibling, tkey) == std::string("parent"));

    LegacyThreadLocalKeyV1<int> tls;
    check("21_tls_initially_empty", !tls.get_copy());
    check("22_tls_set", tls.set(44));
    check("23_tls_main_read", tls.get_copy() == 44);
    std::atomic<bool> foreign_empty{false}, foreign_independent{false};
    std::thread foreign([&] {
        foreign_empty.store(!tls.get_copy(), std::memory_order_release);
        const bool wrote = tls.set(72);
        foreign_independent.store(wrote && tls.get_copy() == 72, std::memory_order_release);
        (void)tls.erase();
    });
    foreign.join();
    check("24_tls_foreign_thread_empty", foreign_empty.load(std::memory_order_acquire));
    check("25_tls_foreign_thread_writes_separate", foreign_independent.load(std::memory_order_acquire));
    check("26_tls_main_unchanged", tls.get_copy() == 44);
    check("27_tls_erase", tls.erase() && !tls.get_copy());

    auto cancellation = TaskCancellation::root();
    const OresNativeActorRef actor_ref{71, 3, 1};
    auto request = RequestContextLocalV1::open(
        RequestIdentityV1{9, 18, 501, 1, actor_ref}, cancellation);
    check("28_request_open", !!request);
    check("29_request_first_mail_binding", request->bind_original_mail_once(1));
    check("30_request_rebind_fails", !request->bind_original_mail_once(2));
    check("31_request_zero_mail_denied", !request->bind_original_mail_once(0));
    check("32_request_same_mail_idempotent", request->bind_original_mail_once(1));
    RequestLocalKeyV1<std::string> rkey;
    check("33_request_exact_value_limit", request->set(rkey, std::string(1024, 'z')));
    check("34_request_quota_measured", request->used_bytes() == 1024);
    check("35_request_oversized_value_denied", !request->set(rkey, std::string(1025, 'q')));
    check("36_request_malformed_utf8_denied", !request->set(
        rkey, std::string("\xed\xa0\x80", 3)));
    check("37_request_rejected_write_preserves_old",
          request->get_copy(rkey) == std::string(1024, 'z'));
    check("38_request_erase", request->erase(rkey));
    check("39_request_erase_returns_quota", request->used_bytes() == 0);
    check("40_request_close", request->close());
    check("41_request_stale_read_denied", !request->get_copy(rkey));
    check("42_request_stale_bind_denied", !request->bind_original_mail_once(1));
    check("43_actor_close", actor->close());
    check("44_actor_after_close_denied", !actor->get_copy(owner, akey));
    check("45_task_close_and_stale_denied",
          task->close() && !task->get_copy(owner, tkey) && child->close());
    cancellation->retire();
    if (passed != 45) return 2;
    std::puts("NATIVE_SCOPED_LOCALS_45_CASE_HOST_PASS");
}
