#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include <stdexcept>
#include <string>

using oreslang::runtime::NativeActorGroup;
static void require(bool value, const char* msg) {
    if (!value) throw std::runtime_error(msg);
}
int main() {
    NativeActorGroup group(1206, 3);
    NativeActorGroup foreign(1206, 3);
    int stopped = 0;
    auto a = group.adopt(1206, 101, 1, [&]{++stopped;});
    auto b = group.adopt_child(a, 1206, 102, 1, [&]{++stopped;});
    auto c = group.adopt_child(b, 1206, 103, 1, [&]{++stopped;});
    require(group.worker_count()==3,"nested children escaped group");
    require(!group.spawned_by(a).has_value(),"root has fabricated parent");
    require(group.spawned_by(b)==a.actor_id(),"child lineage");
    require(group.spawned_by(c)==b.actor_id(),"grandchild lineage");
    bool foreignRejected=false;
    try { (void) foreign.adopt_child(b,1206,104,1,[]{}); }
    catch (const std::logic_error&) { foreignRejected=true; }
    require(foreignRejected,"foreign group accepted parent");
    bool capacityRejected=false;
    try { (void) group.adopt_child(c,1206,104,1,[]{}); }
    catch (const std::logic_error&) { capacityRejected=true; }
    require(capacityRejected,"capacity gate bypassed");
    group.close();
    require(stopped==3 && group.worker_count()==0,"close did not stop family");
    bool closedRejected=false;
    try { (void) group.adopt_child(a,1206,105,1,[]{}); }
    catch (const std::logic_error&) { closedRejected=true; }
    require(closedRejected,"closed group admitted child");
}
