// RequestLocal handoff across actors through the REAL native scheduler
// mailbox (u64 payload). Host-only: not compiled .atto guest code.
#include "oreslang/runtime/RequestContextHandoffV1.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::runtime;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

// Each delivered message runs the next queued step for that actor, on that
// actor's authenticated scheduler turn.
struct World {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef a{}, b{}, c{};
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::function<void(std::uint64_t)>> steps[3];
    std::size_t next_step[3] = {0, 0, 0};
    int completed = 0;
};

void on_message(void* userdata, std::size_t actor_index, std::uint64_t payload) {
    auto& w = *static_cast<World*>(userdata);
    std::function<void(std::uint64_t)> step;
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        step = w.steps[actor_index].at(w.next_step[actor_index]++);
    }
    step(payload);
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        ++w.completed;
    }
    w.cv.notify_all();
}

void send(World& w, OresNativeActorRef to, std::uint64_t payload) {
    int before;
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        before = w.completed;
    }
    CHECK(ores_native_actor_scheduler_try_send(w.scheduler, to, payload) == ORES_NATIVE_SEND_ACCEPTED);
    std::unique_lock<std::mutex> lock(w.mutex);
    CHECK(w.cv.wait_for(lock, std::chrono::seconds(8), [&] { return w.completed > before; }));
}

void on(World& w, int actor, std::function<void(std::uint64_t)> fn) {
    std::lock_guard<std::mutex> lock(w.mutex);
    w.steps[actor].push_back(std::move(fn));
}

struct Fixture {
    World w;
    RequestLocalKeyV1<std::string> trace;
    RequestLocalKeyV1<int> budget;
    RequestLocalKeyV1<std::string> secret;  // never selected for handoff
    TaskCancellation::Ptr request_cancel = TaskCancellation::root();
    RequestContextLocalV1::Ptr request;  // owned by actor A
    std::unique_ptr<RequestContextHandoffTableV1> table;

    explicit Fixture(std::size_t capacity = 8) {
        w.scheduler = ores_native_actor_scheduler_open(3, 8, 2, on_message, &w);
        CHECK(w.scheduler);
        w.a = ores_native_actor_scheduler_spawn(w.scheduler, 0);
        w.b = ores_native_actor_scheduler_spawn(w.scheduler, 1);
        w.c = ores_native_actor_scheduler_spawn(w.scheduler, 2);
        request = RequestContextLocalV1::open({7, 8, 9001, 1, w.a}, request_cancel);
        CHECK(request);
        table = std::make_unique<RequestContextHandoffTableV1>(w.scheduler, capacity);
    }
    ~Fixture() {
        for (auto actor : {w.a, w.b, w.c}) {
            ores_native_actor_scheduler_stop(w.scheduler, actor);
            ores_native_actor_scheduler_wait_done(w.scheduler, actor);
        }
        ores_native_actor_scheduler_retire(w.scheduler);
        ores_native_actor_scheduler_drain(w.scheduler);
        ores_native_actor_scheduler_try_destroy(w.scheduler);
        request->close();
        request_cancel->retire();
    }
    // A trusted test ingress invokes this only from A's real DATA callback;
    // integer mail IDs supplied by a guest are never an authority source.
    bool bind_source_mail() {
        std::uint64_t current = 0;
        return ores_native_actor_scheduler_current_data_mail_sequence(
                   w.scheduler, w.a, &current) &&
               request->bind_original_mail_once(current);
    }
    RequestContextHandoffTableV1::Selection selection() {
        RequestContextHandoffTableV1::Selection s;
        s.include(trace).include(budget);
        return s;
    }
};

void test_snapshot_handoff_is_independent() {
    Fixture f;
    std::uint64_t ticket = 0;
    std::optional<RedeemedRequestContextV1> held;
    on(f.w, 0, [&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(f.trace, std::string("trace-1")));
        CHECK(f.request->set(f.budget, 100));
        CHECK(f.request->set(f.secret, std::string("A-only")));
        auto t = f.table->seal(f.w.a, *f.request, f.w.b, f.selection());
        CHECK(t.has_value());
        ticket = t.value_or(0);
        // A keeps writing after seal; B must see the sealed snapshot only.
        CHECK(f.request->set(f.trace, std::string("trace-1-after-seal")));
    });
    send(f.w, f.w.a, 1);
    CHECK(f.table->pending() == 1);

    on(f.w, 1, [&](std::uint64_t payload) {
        // B cannot touch A's context even on its own turn.
        CHECK(!request_local_get_on_actor_turn_v1(f.w.scheduler, f.w.b, *f.request, f.trace));
        auto redeemed = f.table->redeem(f.w.b, payload);
        CHECK(redeemed.has_value());
        if (!redeemed) return;
        auto& ctx = redeemed->context();
        CHECK(ctx.belongs_to(f.w.b) && !ctx.belongs_to(f.w.a));
        CHECK(ctx.identity().request == 9001 && ctx.identity().tenant == 7);
        CHECK(request_local_get_on_actor_turn_v1(f.w.scheduler, f.w.b, ctx, f.trace) == std::string("trace-1"));
        CHECK(ctx.get_copy(f.budget) == 100);
        CHECK(!ctx.get_copy(f.secret));  // not selected => not inherited
        CHECK(ctx.set(f.budget, 5));      // B's write stays in B
        CHECK(!f.table->redeem(f.w.b, payload));  // one-shot: replay rejected
        held.emplace(std::move(*redeemed));
    });
    send(f.w, f.w.b, ticket);
    CHECK(f.table->pending() == 0);
    CHECK(f.request->get_copy(f.budget) == 100);
    CHECK(f.request->get_copy(f.trace) == std::string("trace-1-after-seal"));

    // B gives up locally: R and A are unaffected.
    CHECK(held->cancel_local());
    CHECK(!held->context().active());
    CHECK(f.request->active());
    held.reset();
    CHECK(f.request->active());
}

void test_misdirected_forged_offturn_tickets() {
    Fixture f;
    std::uint64_t ticket = 0;
    on(f.w, 0, [&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(f.trace, std::string("t")));
        ticket = f.table->seal(f.w.a, *f.request, f.w.b, f.selection()).value_or(0);
        CHECK(ticket != 0);
        CHECK(!f.table->redeem(f.w.a, ticket));  // sender is not the recipient
    });
    send(f.w, f.w.a, 1);
    // Off-turn: main thread can neither seal nor redeem.
    CHECK(!f.table->redeem(f.w.b, ticket));
    CHECK(!f.table->seal(f.w.a, *f.request, f.w.b, f.selection()));
    // C intercepts the ticket (forwarded mail): rejected and NOT burned.
    on(f.w, 2, [&](std::uint64_t p) {
        CHECK(!f.table->redeem(f.w.c, p));
        CHECK(!f.table->redeem(f.w.c, p + (1ull << 16)));  // forged generation
        CHECK(!f.table->redeem(f.w.c, p ^ 1));              // forged slot
        CHECK(!f.table->redeem(f.w.c, 0));
        // C cannot seal A's request either.
        CHECK(!f.table->seal(f.w.c, *f.request, f.w.b, f.selection()));
    });
    send(f.w, f.w.c, ticket);
    CHECK(f.table->pending() == 1);
    // B received a different DATA payload on its own real turn. Possessing
    // the old ticket off-envelope must not redeem or consume the message.
    on(f.w, 1, [&](std::uint64_t) {
        CHECK(!f.table->redeem(f.w.b, ticket));
    });
    send(f.w, f.w.b, ticket + 123);
    CHECK(f.table->pending() == 1);
    // Restarted B incarnation (different generation) is a different recipient.
    on(f.w, 1, [&](std::uint64_t p) {
        OresNativeActorRef stale = f.w.b;
        ++stale.generation;
        CHECK(!f.table->redeem(stale, p));
        auto r = f.table->redeem(f.w.b, p);
        CHECK(r && r->context().get_copy(f.trace) == std::string("t"));
    });
    send(f.w, f.w.b, ticket);
    CHECK(f.table->pending() == 0);
}

void test_cancellation_flows_one_way() {
    Fixture f;
    std::uint64_t t1 = 0, t2 = 0;
    std::optional<RedeemedRequestContextV1> held;
    on(f.w, 0, [&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(f.trace, std::string("t")));
        t1 = f.table->seal(f.w.a, *f.request, f.w.b, f.selection()).value_or(0);
        t2 = f.table->seal(f.w.a, *f.request, f.w.b, f.selection()).value_or(0);
        CHECK(t1 && t2 && t1 != t2);
    });
    send(f.w, f.w.a, 1);
    on(f.w, 1, [&](std::uint64_t p) {
        auto r = f.table->redeem(f.w.b, p);
        CHECK(r.has_value());
        if (r) held.emplace(std::move(*r));
    });
    send(f.w, f.w.b, t1);
    CHECK(held && held->context().active());
    // HTTP client disconnects: R is cancelled. B's derived copy closes too,
    // and the still-pending envelope can no longer be redeemed.
    CHECK(f.request_cancel->cancel("http-client-disconnected"));
    CHECK(!held->context().active());
    CHECK(!held->context().get_copy(f.trace));
    on(f.w, 1, [&](std::uint64_t p) { CHECK(!f.table->redeem(f.w.b, p)); });
    send(f.w, f.w.b, t2);
    CHECK(f.table->sweep() == 0);  // the failed redeem already consumed it
    held.reset();
}

void test_sweep_revoke_and_capacity() {
    Fixture f(2);
    std::uint64_t t[3] = {0, 0, 0};
    on(f.w, 0, [&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        t[0] = f.table->seal(f.w.a, *f.request, f.w.b, f.selection()).value_or(0);
        t[1] = f.table->seal(f.w.a, *f.request, f.w.c, f.selection()).value_or(0);
        t[2] = f.table->seal(f.w.a, *f.request, f.w.b, f.selection()).value_or(0);
        CHECK(t[0] && t[1] && !t[2]);  // bounded table
        OresNativeActorRef other_domain = f.w.b;
        ++other_domain.scheduler_domain;
        CHECK(!f.table->seal(f.w.a, *f.request, other_domain, f.selection()));
    });
    send(f.w, f.w.a, 1);
    // B terminates: its envelopes are dropped; C's survives.
    CHECK(f.table->revoke_recipient(f.w.b) == 1);
    CHECK(f.table->pending() == 1);
    CHECK(f.request_cancel->cancel("done"));
    CHECK(f.table->sweep() == 1);
    CHECK(f.table->pending() == 0);
    // Cancelled request cannot be sealed at all.
    on(f.w, 0, [&](std::uint64_t) { CHECK(!f.table->seal(f.w.a, *f.request, f.w.b, f.selection())); });
    send(f.w, f.w.a, 2);
}

void test_slot_reuse_bumps_generation() {
    Fixture f(1);
    std::uint64_t first = 0, second = 0;
    on(f.w, 0, [&](std::uint64_t) { CHECK(f.bind_source_mail()); first = f.table->seal(f.w.a, *f.request, f.w.b, f.selection()).value_or(0); });
    send(f.w, f.w.a, 1);
    on(f.w, 1, [&](std::uint64_t p) { CHECK(f.table->redeem(f.w.b, p).has_value()); });
    send(f.w, f.w.b, first);
    on(f.w, 0, [&](std::uint64_t) {
        // Original request R cannot seal from the later DATA mail. New
        // request R2 must be admitted and bound independently.
        CHECK(!f.table->seal(f.w.a, *f.request, f.w.b, f.selection()));
        f.request = RequestContextLocalV1::open(
            {7, 8, 9002, 1, f.w.a}, f.request_cancel);
        CHECK(f.request && f.bind_source_mail());
        second = f.table->seal(f.w.a, *f.request, f.w.b, f.selection()).value_or(0);
    });
    send(f.w, f.w.a, 2);
    CHECK(first && second && first != second && (first & 0xffff) == (second & 0xffff));
    on(f.w, 1, [&](std::uint64_t) {
        CHECK(!f.table->redeem(f.w.b, first));  // old ticket for reused slot
        CHECK(f.table->redeem(f.w.b, second).has_value());
    });
    send(f.w, f.w.b, second);
}

// Many handoffs in flight from two sender turns to a recipient while the
// request is cancelled concurrently. Must be race-free under TSan.
void test_concurrent_cancel_race() {
    for (int round = 0; round < 50; ++round) {
        Fixture f(64);
        std::vector<std::uint64_t> tickets;
        on(f.w, 0, [&](std::uint64_t) {
            CHECK(f.bind_source_mail());
            CHECK(f.request->set(f.trace, std::string(64, 'x')));
            for (int i = 0; i < 32; ++i)
                if (auto t = f.table->seal(f.w.a, *f.request, f.w.b, f.selection())) tickets.push_back(*t);
        });
        send(f.w, f.w.a, 1);
        std::thread canceller([&] { std::this_thread::yield(); f.request_cancel->cancel("race"); });
        for (auto t : tickets) {
            on(f.w, 1, [&](std::uint64_t p) {
                auto r = f.table->redeem(f.w.b, p);
                if (r && r->context().active()) {
                    auto v = r->context().get_copy(f.trace);
                    CHECK(!v || *v == std::string(64, 'x'));
                }
            });
            send(f.w, f.w.b, t);
        }
        canceller.join();
        f.table->sweep();
        CHECK(f.table->pending() == 0);
    }
}

}  // namespace

int main() {
    struct { const char* name; void (*fn)(); } tests[] = {
        {"snapshot_handoff_is_independent", test_snapshot_handoff_is_independent},
        {"misdirected_forged_offturn_tickets", test_misdirected_forged_offturn_tickets},
        {"cancellation_flows_one_way", test_cancellation_flows_one_way},
        {"sweep_revoke_and_capacity", test_sweep_revoke_and_capacity},
        {"slot_reuse_bumps_generation", test_slot_reuse_bumps_generation},
        {"concurrent_cancel_race", test_concurrent_cancel_race},
    };
    for (auto& t : tests) {
        const int before = failures;
        t.fn();
        std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", t.name);
    }
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("REQUEST_CONTEXT_HANDOFF_V1_ALL_PASS\n");
    return 0;
}
