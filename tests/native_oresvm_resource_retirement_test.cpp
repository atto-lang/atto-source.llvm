#include "oreslang/runtime/OresVM.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <thread>
using oreslang::runtime::OresVM;
using oreslang::runtime::IsolateResourceKind;
template<class F> bool denied(F&& fn) { try { fn(); } catch (const std::exception&) { return true; } return false; }
int main() {
    OresVM vm;
    auto a=vm.create_isolate({"tenant-a", 64, 2, oreslang::runtime::IsolateMode::TrustedInProcess, 64, 2});
    auto b=vm.create_isolate({"tenant-b", 64, 2, oreslang::runtime::IsolateMode::TrustedInProcess, 64, 2});
    vm.start(a); vm.start(b);
    const auto actor_a=vm.register_actor(a, 1);
    const auto actor_a2=vm.register_actor(a, 2);
    const auto actor_b=vm.register_actor(b, 1);
    const auto io=vm.pin_actor_resource(a, actor_a, IsolateResourceKind::IoRegistration);
    const auto cb=vm.pin_actor_resource(a, actor_a, IsolateResourceKind::Callback);
    const auto bcont=vm.pin_actor_resource(b, actor_b, IsolateResourceKind::Continuation);
    assert(vm.snapshot(a).active_resource_pins==2);
    assert(vm.snapshot(b).active_resource_pins==1);
    assert(denied([&]{vm.pin_actor_resource(a,actor_a2,IsolateResourceKind::MailboxLoan);}));
    assert(denied([&]{vm.retire_actor_resource(b,actor_b,io);}));
    assert(denied([&]{vm.retire_actor_resource(a,actor_a2,io);}));
    assert(denied([&]{vm.retire_actor(a,actor_a);}));
    assert(denied([&]{vm.retire_actor(a,actor_b);}));
    vm.begin_shutdown(a);
    assert(denied([&]{vm.pin_actor_resource(a,actor_a,IsolateResourceKind::IoRegistration);}));
    assert(denied([&]{vm.complete_shutdown(a);}));
    vm.retire_actor_resource(a,actor_a,io);
    assert(denied([&]{vm.retire_actor_resource(a,actor_a,io);}));
    assert(denied([&]{vm.retire_actor(a,actor_a);}));
    vm.retire_actor_resource(a,actor_a,cb);
    vm.retire_actor(a,actor_a);
    vm.retire_actor(a,actor_a2);
    vm.complete_shutdown(a);
    vm.destroy(a);
    assert(denied([&]{vm.snapshot(a);}));
    const auto anew=vm.create_isolate({"tenant-a",64,1});
    vm.start(anew);
    const auto actor_new=vm.register_actor(anew,1);
    assert(denied([&]{vm.retire_actor_resource(anew,actor_new,io);}));
    assert(denied([&]{vm.pin_actor_resource(anew,actor_a,IsolateResourceKind::Callback);}));
    vm.retire_actor(anew,actor_new);
    vm.begin_shutdown(anew);vm.complete_shutdown(anew);vm.destroy(anew);
    // Other tenant progress is independent of tenant A's quiescence/restart.
    assert(vm.snapshot(b).active_resource_pins==1);
    vm.begin_shutdown(b);
    assert(denied([&]{vm.retire_actor(b,actor_b);}));
    vm.retire_actor_resource(b,actor_b,bcont);
    vm.retire_actor(b,actor_b);
    vm.complete_shutdown(b);vm.destroy(b);
}
