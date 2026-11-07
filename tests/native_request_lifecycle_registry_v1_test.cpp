// Host-only automatic RequestLocal STOP notification + CONTROL worker.
// No native result here is compiled AttoLang guest execution.
#include "oreslang/runtime/NativeRequestLifecycleRegistryV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

using namespace oreslang::runtime;
namespace {
int passed = 0;
void check(const char* name, bool yes) {
    if (!yes) {
        std::fprintf(stderr, "LIFECYCLE_REGISTRY_FAILED %s\n", name);
        std::abort();
    }
    ++passed;
    std::printf("LIFECYCLE_REGISTRY_PASS %s\n", name);
}
constexpr auto timeout = std::chrono::milliseconds(8000);
struct World {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef a{}, b{};
    std::atomic<int> self_stopped{0};
    static void on_message(void* opaque, std::size_t, std::uint64_t value) {
        auto& self = *static_cast<World*>(opaque);
        if (value == 997) {
            const bool stopped = ores_native_actor_scheduler_stop(
                self.scheduler, self.a) != 0;
            self.self_stopped.store(stopped ? 1 : -1);
        }
    }
    World() {
        scheduler = ores_native_actor_scheduler_open(
            2, 8, 2, &on_message, this);
        assert(scheduler);
        a = ores_native_actor_scheduler_spawn(scheduler, 0);
        b = ores_native_actor_scheduler_spawn(scheduler, 1);
        assert(a.scheduler_domain && b.scheduler_domain);
    }
    ~World() {
        if (ores_native_actor_scheduler_poll_ready(scheduler,a))
            assert(ores_native_actor_scheduler_stop_sealed(scheduler,a));
        if (ores_native_actor_scheduler_poll_ready(scheduler,b))
            assert(ores_native_actor_scheduler_stop_sealed(scheduler,b));
        assert(ores_native_actor_scheduler_wait_done(scheduler,a));
        assert(ores_native_actor_scheduler_wait_done(scheduler,b));
        ores_native_actor_scheduler_retire(scheduler);
        assert(ores_native_actor_scheduler_drain(scheduler));
        assert(ores_native_actor_scheduler_try_destroy(scheduler));
    }
};
RequestContextLocalV1::Ptr request(OresNativeActorRef actor,
        std::uint64_t id, const TaskCancellation::Ptr& cancel) {
    return RequestContextLocalV1::open({23, 54, id, 1, actor}, cancel);
}
struct Barrier {
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false, released = false;
    bool await() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return started; });
    }
    void signal() {
        std::unique_lock<std::mutex> lock(mutex);
        started = true;
        cv.notify_all();
        cv.wait(lock, [&] { return released; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        cv.notify_all();
    }
};
struct PauseCopy {
    std::shared_ptr<Barrier> barrier;
    bool armed = true;
    explicit PauseCopy(std::shared_ptr<Barrier> p)
        : barrier(std::move(p)) {}
    PauseCopy(const PauseCopy& other)
        : barrier(other.barrier), armed(false) {
        if (other.armed) barrier->signal();
    }
    PauseCopy(PauseCopy&& other) noexcept
        : barrier(std::move(other.barrier)), armed(other.armed) {
        other.armed = false;
    }
};

void basic_stop_and_registration() {
    World w;
    NativeRequestLifecycleRegistryV1 reg(w.scheduler);
    auto a1cancel = TaskCancellation::root();
    auto a2cancel = TaskCancellation::root();
    auto bcancel = TaskCancellation::root();
    auto a1 = request(w.a, 101, a1cancel);
    auto a2 = request(w.a, 102, a2cancel);
    auto b1 = request(w.b, 103, bcancel);
    RequestLocalKeyV1<int> key;
    check("01_attach_actor_a", reg.attach(w.a));
    check("02_duplicate_actor_attach_denied", !reg.attach(w.a));
    check("03_wrong_actor_request_registration_denied",
          !reg.register_request(w.a,b1));
    check("04_register_first_scope", reg.register_request(w.a,a1));
    check("05_register_second_scope", reg.register_request(w.a,a2));
    check("06_duplicate_scope_registration_denied",
          !reg.register_request(w.a,a1));
    check("07_scope_store_before_stop",
          a1->set(key,41) && a2->set(key,42));
    check("08_direct_normal_STOP_accepted",
          ores_native_actor_scheduler_stop(w.scheduler,w.a));
    check("09_automatic_control_cleanup_receipt",
          reg.wait_cleaned(w.a,timeout));
    check("10_both_scopes_closed_without_manual_reconciliation",
          !a1->active() && !a2->active() &&
          a1->used_bytes() == 0 && a2->used_bytes() == 0);
    check("11_post_stop_scope_registration_denied",
          !reg.register_request(w.a,request(w.a,104,a1cancel)));
    check("12_sibling_actor_unaffected",
          ores_native_actor_scheduler_poll_ready(w.scheduler,w.b));
    a1cancel->retire();
    a2cancel->retire();
    bcancel->retire();
}
void sealed_and_forged() {
    World w;
    NativeRequestLifecycleRegistryV1 reg(w.scheduler);
    auto c = TaskCancellation::root();
    auto r = request(w.a,201,c);
    auto stale = w.a; ++stale.generation;
    auto wrong_domain = w.a; ++wrong_domain.scheduler_domain;
    auto wrong_slot = w.a; wrong_slot.actor_slot = 10;
    check("13_forged_generation_attach_denied", !reg.attach(stale));
    check("14_foreign_scheduler_domain_attach_denied", !reg.attach(wrong_domain));
    check("15_invalid_actor_slot_attach_denied", !reg.attach(wrong_slot));
    check("16_exact_incarnation_attach", reg.attach(w.a));
    check("17_exact_scope_registered", reg.register_request(w.a,r));
    check("18_sealed_STOP_admitted",
          ores_native_actor_scheduler_stop_sealed(w.scheduler,w.a));
    check("19_sealed_STOP_drains_without_carrier_block",
          reg.wait_cleaned(w.a,timeout) && !r->active());
    c->retire();
}
void global_retirement_two_actors() {
    World w;
    NativeRequestLifecycleRegistryV1 reg(w.scheduler);
    auto ca = TaskCancellation::root(), cb = TaskCancellation::root();
    auto ra = request(w.a,301,ca), rb = request(w.b,302,cb);
    check("20_attach_both_generations", reg.attach(w.a) && reg.attach(w.b)
          && reg.attached_actor_count()==2);
    check("21_register_both_scopes",
          reg.register_request(w.a,ra) && reg.register_request(w.b,rb));
    ores_native_actor_scheduler_retire(w.scheduler);
    check("22_retirement_control_cleanups_both",
          reg.wait_cleaned(w.a,timeout) && reg.wait_cleaned(w.b,timeout));
    check("23_all_request_storage_reclaimed",
          !ra->active() && !rb->active() &&
          ra->used_bytes()==0 && rb->used_bytes()==0);
    ca->retire();
    cb->retire();
}
void actor_self_stop() {
    World w;
    NativeRequestLifecycleRegistryV1 reg(w.scheduler);
    auto c=TaskCancellation::root();
    auto r=request(w.a,401,c);
    check("24_attach_and_register_self_stopping_actor",
          reg.attach(w.a) && reg.register_request(w.a,r));
    check("25_callback_STOP_DATA_send_admitted",
          ores_native_actor_scheduler_try_send(w.scheduler,w.a,997)
              == ORES_NATIVE_SEND_ACCEPTED);
    check("26_callback_STOP_and_automatic_cleanup",
          reg.wait_cleaned(w.a,timeout) && w.self_stopped.load()==1);
    check("27_CALLBACK_STOP_no_stale_request",
          !r->active() && r->used_bytes()==0);
    c->retire();
}
void paused_copy_holds_cleanup() {
    World w;
    NativeRequestLifecycleRegistryV1 reg(w.scheduler);
    auto c=TaskCancellation::root();
    auto r=request(w.a,501,c);
    RequestLocalKeyV1<PauseCopy> key;
    auto barrier=std::make_shared<Barrier>();
    check("28_attach_and_register_pinned_read",
          reg.attach(w.a) && reg.register_request(w.a,r) &&
          r->set(key,PauseCopy(barrier)));
    std::atomic<bool> copied{true};
    std::thread reader([&] {
        copied.store(r->get_copy(key).has_value());
    });
    const bool paused=barrier->await();
    if(!paused) { barrier->release(); reader.join(); }
    check("29_pinned_read_copy_entered",paused);
    const bool stopped=ores_native_actor_scheduler_stop_sealed(w.scheduler,w.a);
    check("30_STOP_signals_without_waiting_for_pinned_copy",stopped);
    // The CONTROL worker must fence new admission while the existing value
    // copy remains paused. Completion is never forged before lease release.
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    while(r->active() && std::chrono::steady_clock::now()<deadline)
        std::this_thread::yield();
    // At this point, a stopped scope is closed, but worker still waits on
    // its in-flight lease; don't call wait_cleaned on the suspended reader.
    assert(!r->active());
    barrier->release();
    reader.join();
    assert(reg.wait_cleaned(w.a,timeout));
    assert(!copied.load() && r->used_bytes()==0);
    c->retire();
}
} // namespace
int main() {
    basic_stop_and_registration();
    sealed_and_forged();
    global_retirement_two_actors();
    actor_self_stop();
    paused_copy_holds_cleanup();
    if (passed != 30) return 2;
    std::puts("NATIVE_REQUEST_LIFECYCLE_REGISTRY_30_HOST_CASES_PASS");
}
