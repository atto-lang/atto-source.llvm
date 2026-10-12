#include "oreslang/runtime/NativeSupervisorCodeSlot.hpp"
#include "oreslang/runtime/NativeSupervisorRegistry.hpp"
#include <atomic>
#include <stdexcept>
#include <string>

using namespace oreslang::runtime;

static void check(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}

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
}
