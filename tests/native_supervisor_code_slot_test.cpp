#include "oreslang/runtime/NativeSupervisorCodeSlot.hpp"
#include "oreslang/runtime/NativeSupervisorRegistry.hpp"
#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

using namespace oreslang::runtime;

static void check(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}


/** Failed admission must own/reclaim code images outside the CONTROL lock. */
static void exercise_rejected_hot_reload_reclamation() {
    std::atomic<int> rejected_reclaims{0};
    std::atomic<int> accepted_reclaims{0};
    NativeSupervisorCodeSlot<std::string> slot(
            "actor-abi-v1", "actor-policy-v1", [&] { ++accepted_reclaims; }, 2);

    auto rejected_image = [&] {
        ++rejected_reclaims;
        // A callback that re-enters the slot exposes teardown-under-lock bugs.
        (void)slot.live_revisions();
    };
    bool denied = false;
    try {
        (void)slot.activate(0, "actor-abi-v1", "stale", rejected_image);
    } catch (const std::logic_error&) { denied = true; }
    check(denied && rejected_reclaims == 1,
          "stale candidate must reclaim exactly once outside the slot lock");

    denied = false;
    try {
        (void)slot.activate(1, "changed-actor-abi", "incompatible", rejected_image);
    } catch (const std::invalid_argument&) { denied = true; }
    check(denied && rejected_reclaims == 2,
          "ABI mismatch must reclaim candidate without replacing active code");
    check(slot.current().revision == 1 && slot.pin().code() == "actor-policy-v1",
          "denied upgrade corrupted active revision");

    auto pinned_v1 = slot.pin();
    check(slot.activate(1, "actor-abi-v1", "actor-policy-v2",
                        [&] { ++accepted_reclaims; }).revision == 2,
          "valid code-only upgrade must publish");
    denied = false;
    try {
        (void)slot.activate(2, "actor-abi-v1", "over-cap", rejected_image);
    } catch (const std::length_error&) { denied = true; }
    check(denied && rejected_reclaims == 3,
          "pinned generation limit must reclaim denied candidate");
    check(pinned_v1.code() == "actor-policy-v1" && slot.pin().code() == "actor-policy-v2",
          "pinned invocation observed a different generation");
    pinned_v1.close();

    // Two simultaneous candidates at the same expected epoch: one winner.
    std::atomic<int> wins{0}, conflicts{0};
    std::thread a([&] {
        try {
            slot.activate(2, "actor-abi-v1", "competing-a", rejected_image);
            ++wins;
        } catch (const std::logic_error&) { ++conflicts; }
    });
    std::thread b([&] {
        try {
            slot.activate(2, "actor-abi-v1", "competing-b", rejected_image);
            ++wins;
        } catch (const std::logic_error&) { ++conflicts; }
    });
    a.join();
    b.join();
    check(wins == 1 && conflicts == 1 && slot.current().revision == 3,
          "concurrent candidate publication was not single-winner CAS");
    check(rejected_reclaims == 4, "losing concurrent candidate leaked");
    slot.close();
    denied = false;
    try {
        (void)slot.activate(3, "actor-abi-v1", "after-close", rejected_image);
    } catch (const std::logic_error&) { denied = true; }
    check(denied && rejected_reclaims == 5,
          "terminal slot close must reject and reclaim a late candidate");
    check(accepted_reclaims == 2,
          "successfully installed old images must each be reclaimed once");
    // Winning competing candidate also uses rejected_image as its retirement
    // callback; no active generation may be retained after close.
    check(rejected_reclaims == 6,
          "latest revision cleanup did not run when slot closed");
}

int main(){
    std::atomic<int> reclaimed{0};
    NativeSupervisorRegistry supervisors(42, 8, 8);
    auto root = supervisors.root();
    auto daddy = supervisors.create_child(root);
    auto group = supervisors.create_group(daddy, 2);
    const auto group_id = group->group_id();
    const auto supervisor_id = group->supervisor_id();
    auto installed = std::make_shared<NativeSupervisorCodeSlot<std::string>>(
        "abi-root", "Daddy-live-v1", [&]{reclaimed++;}, 3);
    supervisors.install_policy(daddy, installed);
    check(supervisors.policy_revision(daddy).revision==1,
          "native registry did not attach supervisor code");
    bool denied_slot_reuse=false;
    try { supervisors.install_policy(root, installed); }
    catch (const std::logic_error&) { denied_slot_reuse=true; }
    check(denied_slot_reuse, "one native code slot attached to two supervisors");
    installed->activate(1, "abi-root", "Daddy-live-v2", [&]{reclaimed++;});
    check(supervisors.policy_revision(daddy).revision==2,
          "native registry did not observe activated policy revision");
    check(group->group_id()==group_id && group->supervisor_id()==supervisor_id,
          "native registry policy activation changed group/supervisor identity");
    installed->close();
    // This registry slot is independent from the direct slot exercised below.
    reclaimed.store(0);
    {
        NativeSupervisorCodeSlot<std::string> slot("abi1", "Daddy-v1",
                [&]{reclaimed++;}, 2);
        auto old = slot.pin();
        auto next = slot.activate(1,"abi1","Daddy-v2",[&]{reclaimed++;});
        check(next.revision==2,"new code revision not committed");
        check(old.code()=="Daddy-v1","old pinned continuation was modified");
        check(slot.pin().code()=="Daddy-v2","new turn got old policy revision");
        check(reclaimed.load()==0,"old policy reclaimed while a lease was live");
        bool rejected=false;
        auto second = slot.pin();
        try{(void)slot.activate(2,"abi1","Daddy-v3",[&]{reclaimed++;});}
        catch(const std::length_error&){rejected=true;}
        check(rejected,"pinned code revision cap not enforced");
        second.close();
        old.close();
        check(reclaimed.load()==1,"old code must reclaim exactly once");
        rejected=false;
        try{(void)slot.activate(99,"abi1","bad",[&]{reclaimed++;});}
        catch(const std::logic_error&){rejected=true;}
        check(rejected,"stale activation admitted");
        rejected=false;
        try{(void)slot.activate(2,"abi2","bad",[&]{reclaimed++;});}
        catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"unchecked ABI migration admitted");
        check(slot.activate(2,"abi1","Daddy-v3",[&]{reclaimed++;}).revision==3,
              "new policy revision could not activate after releasing old pin");
        check(reclaimed.load()==2,"unreferenced generation not reclaimed");
        check(group->group_id()==group_id && group->supervisor_id()==supervisor_id,
              "code reload changed identity or group association");
        auto pinned = slot.pin();
        slot.close();
        check(reclaimed.load()==2,"close freed a pinned new supervisor policy");
        rejected=false;
        try{(void)slot.pin();}catch(const std::logic_error&){rejected=true;}
        check(rejected,"closed policy admitted another turn");
        pinned.close();
    }
    check(reclaimed.load()==3,"new policy leaked after final pinned turn");
    group->close();
    exercise_rejected_hot_reload_reclamation();
}
