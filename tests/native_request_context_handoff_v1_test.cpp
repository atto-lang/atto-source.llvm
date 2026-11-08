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
    std::unique_ptr<NativeRequestLifecycleRegistryV1> lifecycle;

    explicit Fixture(std::size_t capacity = 8, bool registered = false,
                     bool attach_recipient = true) {
        w.scheduler = ores_native_actor_scheduler_open(3, 8, 2, on_message, &w);
        CHECK(w.scheduler);
        w.a = ores_native_actor_scheduler_spawn(w.scheduler, 0);
        w.b = ores_native_actor_scheduler_spawn(w.scheduler, 1);
        w.c = ores_native_actor_scheduler_spawn(w.scheduler, 2);
        if (registered) {
            lifecycle = std::make_unique<NativeRequestLifecycleRegistryV1>(w.scheduler);
            CHECK(lifecycle->attach(w.a));
            if (attach_recipient) CHECK(lifecycle->attach(w.b));
            CHECK(lifecycle->attach(w.c));
            request = lifecycle->open_registered(
                {7, 8, 9001, 1, w.a}, request_cancel);
            table = std::make_unique<RequestContextHandoffTableV1>(
                w.scheduler, capacity, *lifecycle);
        } else {
            request = RequestContextLocalV1::open(
                {7, 8, 9001, 1, w.a}, request_cancel);
            table = std::make_unique<RequestContextHandoffTableV1>(
                w.scheduler, capacity,
                RequestContextHandoffTableV1::UnregisteredHostOnly{});
        }
        CHECK(request);
    }
    ~Fixture() {
        for (auto actor : {w.a, w.b, w.c}) {
            ores_native_actor_scheduler_stop(w.scheduler, actor);
            ores_native_actor_scheduler_wait_done(w.scheduler, actor);
        }
        if (lifecycle) {
            // The table borrows the registry; tear down the table FIRST.
            // Pending envelope values are destroyed before registry teardown.
            table.reset();
            lifecycle.reset();
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
        {
            // A heap-backed alias is explicitly released BEFORE seal().
            // ASan must detect UAF if Selection ever stores its address.
            auto ephemeral_alias = std::make_unique<
                RequestLocalKeyV1<std::string>>(trace);
            s.include(*ephemeral_alias);
            ephemeral_alias.reset();
        }
        s.include(budget);
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
// Host-only adversarial value. Its copy triggers request cancellation while
// seal() is building its snapshot; no guest type with hidden pointers may use
// this transport without compiler-owned descriptors.
struct CancelRequestOnCopy {
    TaskCancellation* cancellation = nullptr;
    explicit CancelRequestOnCopy(TaskCancellation* value) : cancellation(value) {}
    CancelRequestOnCopy(CancelRequestOnCopy&&) noexcept = default;
    CancelRequestOnCopy(const CancelRequestOnCopy& other)
        : cancellation(other.cancellation) {
        if (cancellation) (void)cancellation->cancel("cancel-during-copy");
    }
};

void test_reentrant_cancellation_during_seal_denied() {
    Fixture f;
    RequestLocalKeyV1<CancelRequestOnCopy> key;
    on(f.w, 0, [&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(key, CancelRequestOnCopy(f.request_cancel.get())));
        RequestContextHandoffTableV1::Selection selection;
        selection.include(key);
        CHECK(!f.table->seal(f.w.a, *f.request, f.w.b, selection));
    });
    send(f.w, f.w.a, 1);
    CHECK(!f.request->active());
    CHECK(f.table->pending() == 0);
}

void test_stopped_recipient_revokes_unredeemed_ticket() {
    Fixture f;
    std::uint64_t ticket = 0;
    on(f.w, 0, [&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(f.trace, std::string("protected")));
        ticket = f.table->seal(
            f.w.a, *f.request, f.w.b, f.selection()).value_or(0);
        CHECK(ticket != 0);
    });
    send(f.w, f.w.a, 1);
    CHECK(f.table->pending() == 1);
    // No callback can run for a sealed stopped actor. Retirement also
    // rejects a late attempt to redeem the old recipient generation.
    CHECK(ores_native_actor_scheduler_stop_sealed(f.w.scheduler, f.w.b));
    CHECK(ores_native_actor_scheduler_wait_done(f.w.scheduler, f.w.b));
    CHECK(f.table->revoke_recipient(f.w.b) == 1);
    CHECK(f.table->pending() == 0);
}

struct HandoffCopyPause {
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false, released = false;
    bool await() {
        std::unique_lock<std::mutex> guard(mu);
        return cv.wait_for(guard, std::chrono::seconds(8),
                           [&] { return entered; });
    }
    void resume() {
        std::lock_guard<std::mutex> guard(mu);
        released = true;
        cv.notify_all();
    }
};
struct BlockingHandoffValue {
    std::shared_ptr<HandoffCopyPause> pause;
    bool armed = true;
    explicit BlockingHandoffValue(std::shared_ptr<HandoffCopyPause> p)
        : pause(std::move(p)) {}
    BlockingHandoffValue(const BlockingHandoffValue& other)
        : pause(other.pause), armed(false) {
        if (!other.armed) return;
        std::unique_lock<std::mutex> guard(pause->mu);
        pause->entered = true;
        pause->cv.notify_all();
        pause->cv.wait(guard, [&] { return pause->released; });
    }
    BlockingHandoffValue(BlockingHandoffValue&& other) noexcept
        : pause(std::move(other.pause)), armed(other.armed) {
        other.armed = false;
    }
};

// This is a real native scheduler DATA callback. An outside CONTROL thread
// stops actor A while its selected value copy is suspended. The in-flight
// snapshot cannot yield a new handoff ticket after scheduler STOP won.
void test_direct_STOP_during_handoff_copy_rejects_ticket() {
    Fixture f;
    auto pause = std::make_shared<HandoffCopyPause>();
    RequestLocalKeyV1<BlockingHandoffValue> key;
    on(f.w, 0, [&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(key, BlockingHandoffValue(pause)));
        RequestContextHandoffTableV1::Selection selected;
        selected.include(key);
        CHECK(!f.table->seal(f.w.a, *f.request, f.w.b, selected));
    });
    CHECK(ores_native_actor_scheduler_try_send(
        f.w.scheduler, f.w.a, 1) == ORES_NATIVE_SEND_ACCEPTED);
    const bool entered = pause->await();
    CHECK(entered);
    CHECK(ores_native_actor_scheduler_stop_sealed(f.w.scheduler, f.w.a));
    pause->resume();
    {
        std::unique_lock<std::mutex> guard(f.w.mutex);
        CHECK(f.w.cv.wait_for(guard, std::chrono::seconds(8),
                             [&] { return f.w.completed >= 1; }));
    }
    CHECK(f.table->pending() == 0);
    CHECK(ores_native_actor_scheduler_wait_done(f.w.scheduler, f.w.a));
}

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

// Precisely named host-only registry-integrated handoff assertions. They
// require real DATA-mail delivery on the native scheduler. The legacy table
// constructor above remains a compatibility-only, unregistered host API.
int registry_handoff_cases = 0;
void registered_case(const char* name, bool ok) {
    ++registry_handoff_cases;
    if (!ok) {
        std::fprintf(stderr, "HANDOFF_REGISTRY_CASE_FAIL %s\n", name);
        ++failures;
    } else {
        std::printf("HANDOFF_REGISTRY_CASE_PASS %s\n", name);
    }
}

void test_registered_handoff_stop_reclaims_derived_scope() {
    Fixture f(8, true);
    std::uint64_t ticket = 0;
    std::optional<RedeemedRequestContextV1> held;
    registered_case("01_three_actor_generations_attached",
                    f.lifecycle && f.lifecycle->attached_actor_count() == 3);
    registered_case("02_source_admitted_in_registry", f.request->active());
    on(f.w, 0, [&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(f.trace, std::string("registered-r")));
        ticket = f.table->seal(f.w.a, *f.request, f.w.b,
                               f.selection()).value_or(0);
    });
    send(f.w, f.w.a, 1);
    registered_case("03_source_sealed_one_ticket",
                    ticket && f.table->pending() == 1);
    on(f.w, 1, [&](std::uint64_t payload) {
        auto r = f.table->redeem(f.w.b, payload);
        if (r) held.emplace(std::move(*r));
    });
    send(f.w, f.w.b, ticket);
    registered_case("04_recipient_redeemed", held.has_value());
    registered_case("05_derived_belongs_only_to_recipient",
                    held && held->context().belongs_to(f.w.b) &&
                    !held->context().belongs_to(f.w.a));
    registered_case("06_derived_has_selected_copy",
                    held && held->context().get_copy(f.trace) ==
                      std::string("registered-r"));
    registered_case("07_ticket_consumed_once", f.table->pending() == 0);
    bool stopped = ores_native_actor_scheduler_stop_sealed(f.w.scheduler,f.w.b);
    registered_case("08_recipient_STOP_and_cleanup_receipt",
                    stopped && f.lifecycle->wait_cleaned(
                        f.w.b, std::chrono::milliseconds(8000)));
    registered_case("09_derived_scope_closed_and_quota_reclaimed",
                    held && !held->context().active() &&
                    held->context().used_bytes() == 0 &&
                    !held->context().get_copy(f.trace));
    registered_case("10_source_isolated_from_recipient_STOP",
                    f.request->active() &&
                    f.request->get_copy(f.trace) == std::string("registered-r"));
}

void test_unattached_recipient_cannot_receive_derived_context() {
    Fixture f(8, true, false);
    std::uint64_t ticket = 0;
    bool rejected = false;
    registered_case("11_only_sender_and_sibling_attached",
                    f.lifecycle && f.lifecycle->attached_actor_count() == 2);
    registered_case("12_source_scope_admitted", f.request->active());
    on(f.w, 0, [&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(f.trace, std::string("no-recipient")));
        ticket = f.table->seal(f.w.a, *f.request, f.w.b,
                               f.selection()).value_or(0);
    });
    send(f.w, f.w.a, 1);
    registered_case("13_unattached_B_ticket_denied_before_slot_charge",
                    ticket == 0 && f.table->pending() == 0);
    on(f.w, 1, [&](std::uint64_t payload) {
        rejected = !f.table->redeem(f.w.b, payload);
    });
    send(f.w, f.w.b, ticket);
    registered_case("14_unattached_B_redeem_fail_closed", rejected);
    registered_case("15_rejected_ticket_never_charged_a_slot",
                    !ticket && f.table->pending() == 0);
    registered_case("16_rejection_does_not_affect_sender_or_create_B_registry",
                    f.request->active() &&
                    !f.lifecycle->wait_cleaned(
                        f.w.b, std::chrono::milliseconds(0)));
}

void test_parent_cancellation_reaches_registered_derived_scope() {
    Fixture f(8, true);
    std::uint64_t ticket = 0;
    std::optional<RedeemedRequestContextV1> held;
    registered_case("17_parent_active_before_handoff", f.request->active());
    on(f.w, 0, [&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(f.trace, std::string("cancelled-parent")));
        ticket = f.table->seal(f.w.a, *f.request, f.w.b,
                               f.selection()).value_or(0);
    });
    send(f.w, f.w.a, 1);
    registered_case("18_parent_handoff_ticket_available", ticket != 0);
    on(f.w, 1, [&](std::uint64_t payload) {
        auto r = f.table->redeem(f.w.b, payload);
        if (r) held.emplace(std::move(*r));
    });
    send(f.w, f.w.b, ticket);
    registered_case("19_derived_registered_and_active",
                    held && held->context().active());
    registered_case("20_copy_before_cancel_preserved",
                    held && held->context().get_copy(f.trace) ==
                        std::string("cancelled-parent"));
    registered_case("21_parent_cancel_wins",
                    f.request_cancel->cancel("parent-closed"));
    registered_case("22_derived_revoked_on_parent_cancel",
                    held && !held->context().active() &&
                    held->context().used_bytes() == 0);
    registered_case("23_B_still_running_but_cannot_write_revoked_scope",
                    held && !held->context().set(f.budget, 11, f.w.scheduler) &&
                    ores_native_actor_scheduler_poll_ready(
                        f.w.scheduler, f.w.b));
}

void test_sender_STOP_does_not_corrupt_derived_recipient() {
    Fixture f(8, true);
    std::uint64_t ticket = 0;
    std::optional<RedeemedRequestContextV1> held;
    on(f.w, 0, [&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(f.trace, std::string("survivor")));
        ticket = f.table->seal(f.w.a, *f.request, f.w.b,
                               f.selection()).value_or(0);
    });
    send(f.w, f.w.a, 1);
    registered_case("24_cross_actor_snapshot_sealed", ticket != 0);
    on(f.w, 1, [&](std::uint64_t payload) {
        auto r = f.table->redeem(f.w.b, payload);
        if (r) held.emplace(std::move(*r));
    });
    send(f.w, f.w.b, ticket);
    registered_case("25_B_redeemed_before_sender_STOP", held.has_value());
    registered_case("26_B_owned_scope_active",
                    held && held->context().belongs_to(f.w.b) &&
                    held->context().active());
    const bool stopped = ores_native_actor_scheduler_stop_sealed(
        f.w.scheduler, f.w.a);
    registered_case("27_sender_STOP_cleanup_completed",
                    stopped && f.lifecycle->wait_cleaned(
                        f.w.a, std::chrono::milliseconds(8000)));
    registered_case("28_sender_scope_closed_and_unreadable",
                    !f.request->active() && !f.request->get_copy(f.trace));
    registered_case("29_B_copy_survives_independent_sender_STOP",
                    held && held->context().active() &&
                    held->context().get_copy(f.trace) ==
                        std::string("survivor") &&
                    ores_native_actor_scheduler_poll_ready(
                        f.w.scheduler, f.w.b));
    registered_case("30_B_local_cancel_does_not_revive_A",
                    held && held->cancel_local() &&
                    !held->context().active() && !f.request->active());
}

void test_pending_ticket_automatically_reclaimed_on_recipient_STOP() {
    Fixture f(8, true);
    std::uint64_t b_ticket = 0, c_ticket = 0;
    registered_case("31_source_live_before_pending_ticket",
                    f.request && f.request->active());
    on(f.w, 0, [&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(f.trace, std::string("pending")));
        b_ticket=f.table->seal(f.w.a,*f.request,f.w.b,f.selection()).value_or(0);
        c_ticket=f.table->seal(f.w.a,*f.request,f.w.c,f.selection()).value_or(0);
    });
    send(f.w,f.w.a,1);
    registered_case("32_two_distinct_pending_tickets",
                    b_ticket && c_ticket && b_ticket!=c_ticket &&
                    f.table->pending()==2);
    auto stale=f.w.b; ++stale.generation;
    registered_case("33_stale_revoke_cannot_destroy_live_B_ticket",
                    f.table->revoke_recipient(stale)==0 &&
                    f.table->pending()==2);
    registered_case("34_recipient_sealed_STOP_commits",
                    ores_native_actor_scheduler_stop_sealed(f.w.scheduler,f.w.b));
    registered_case("35_recipient_CONTROL_cleanup_receipt",
                    f.lifecycle->wait_cleaned(
                        f.w.b,std::chrono::milliseconds(8000)));
    registered_case("36_pending_B_ticket_auto_reclaimed",
                    f.table->pending()==1);
    registered_case("37_source_remains_open_after_B_STOP",
                    f.request->active());
    bool c_read=false;
    on(f.w,2,[&](std::uint64_t payload){
        auto value=f.table->redeem(f.w.c,payload);
        c_read=value && value->context().get_copy(f.trace)==std::string("pending");
    });
    send(f.w,f.w.c,c_ticket);
    registered_case("38_unrelated_C_ticket_survives_B_STOP",c_read);
    registered_case("39_all_pending_envelopes_accounted",
                    f.table->pending()==0);
}

void test_recipient_STOP_before_seal_denies_ticket_publication() {
    Fixture f(8,true);
    registered_case("40_stop_B_before_sender_DATA",
                    ores_native_actor_scheduler_stop_sealed(f.w.scheduler,f.w.b));
    registered_case("41_B_CONTROL_cleanup_finishes",
                    f.lifecycle->wait_cleaned(
                        f.w.b,std::chrono::milliseconds(8000)));
    bool b_denied=false, c_allowed=false;
    std::uint64_t c_ticket=0;
    on(f.w,0,[&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(f.trace,std::string("source-alive")));
        b_denied=!f.table->seal(
            f.w.a,*f.request,f.w.b,f.selection());
        c_ticket=f.table->seal(
            f.w.a,*f.request,f.w.c,f.selection()).value_or(0);
        c_allowed=c_ticket!=0;
    });
    send(f.w,f.w.a,1);
    registered_case("42_stopped_B_cannot_receive_new_ticket",b_denied);
    registered_case("43_live_C_still_accepts_ticket",c_allowed);
    registered_case("44_only_C_is_pending",f.table->pending()==1);
    bool received=false;
    on(f.w,2,[&](std::uint64_t payload){
        received=f.table->redeem(f.w.c,payload).has_value();
    });
    send(f.w,f.w.c,c_ticket);
    registered_case("45_live_C_redeems_without_B_revival",
                    received && f.table->pending()==0 &&
                    !ores_native_actor_scheduler_poll_ready(
                        f.w.scheduler,f.w.b));
}

void test_recipient_STOP_wins_during_sender_snapshot_copy() {
    Fixture f(8,true);
    auto pause=std::make_shared<HandoffCopyPause>();
    RequestLocalKeyV1<BlockingHandoffValue> key;
    bool denied=false;
    on(f.w,0,[&](std::uint64_t){
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(key,BlockingHandoffValue(pause)));
        RequestContextHandoffTableV1::Selection selection;
        selection.include(key);
        denied=!f.table->seal(f.w.a,*f.request,f.w.b,selection);
    });
    registered_case("46_sender_DATA_mail_accepted",
        ores_native_actor_scheduler_try_send(f.w.scheduler,f.w.a,88)
            == ORES_NATIVE_SEND_ACCEPTED);
    const bool entered=pause->await();
    registered_case("47_snapshot_copy_paused_before_publish",entered);
    const bool stopped=ores_native_actor_scheduler_stop_sealed(
        f.w.scheduler,f.w.b);
    registered_case("48_recipient_STOP_wins_during_snapshot",stopped);
    registered_case("49_recipient_cleanup_without_sender_carrier_block",
        f.lifecycle->wait_cleaned(f.w.b,std::chrono::milliseconds(8000)));
    pause->resume();
    {
        std::unique_lock<std::mutex> guard(f.w.mutex);
        CHECK(f.w.cv.wait_for(guard,std::chrono::seconds(8),
                             [&]{return f.w.completed>=1;}));
    }
    registered_case("50_late_two_actor_commit_rejected",denied);
    registered_case("51_no_orphan_ticket_after_STOP_race",
                    f.table->pending()==0 && f.request->active());
}

void test_destroyed_table_leaves_no_dangling_CONTROL_callback() {
    Fixture f(8,true);
    std::uint64_t ticket=0;
    on(f.w,0,[&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        ticket=f.table->seal(f.w.a,*f.request,f.w.b,f.selection()).value_or(0);
    });
    send(f.w,f.w.a,1);
    registered_case("52_ticket_created_before_table_destruction",
                    ticket!=0 && f.table->pending()==1);
    f.table.reset();
    registered_case("53_table_destroyed_before_STOP",!f.table);
    registered_case("54_B_STOP_after_table_destruction",
                    ores_native_actor_scheduler_stop_sealed(f.w.scheduler,f.w.b));
    registered_case("55_weak_callback_survives_expired_table",
                    f.lifecycle->wait_cleaned(
                        f.w.b,std::chrono::milliseconds(8000)));
    registered_case("56_A_still_live_after_weak_cleanup",
                    f.request->active());
}

void test_new_cleanup_hooks_after_unrelated_actor_STOP() {
    Fixture f(8,true);
    registered_case("57_stop_B_before_late_cleanup_registration",
                    ores_native_actor_scheduler_stop_sealed(f.w.scheduler,f.w.b));
    registered_case("58_post_STOP_safe_new_cleanup_hook_admitted",
                    f.lifecycle->wait_cleaned(
                        f.w.b,std::chrono::milliseconds(8000)) &&
                    f.lifecycle->register_stop_cleanup(
                        [](OresNativeActorRef){}));
}

void test_forged_recipient_cannot_seal_ticket() {
    Fixture f(8,true);
    bool stale_denied=false, slot_denied=false, domain_denied=false;
    on(f.w,0,[&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        auto stale=f.w.b; ++stale.generation;
        auto slot=f.w.b; slot.actor_slot=99;
        auto domain=f.w.b; ++domain.scheduler_domain;
        stale_denied=!f.table->seal(f.w.a,*f.request,stale,f.selection());
        slot_denied=!f.table->seal(f.w.a,*f.request,slot,f.selection());
        domain_denied=!f.table->seal(f.w.a,*f.request,domain,f.selection());
    });
    send(f.w,f.w.a,1);
    registered_case("59_stale_generation_denied_at_ticket_commit",
                    stale_denied);
    registered_case("60_invalid_recipient_slot_denied_at_commit",slot_denied);
    registered_case("61_wrong_scheduler_domain_denied_at_commit",
                    domain_denied && f.table->pending()==0);
}

void test_old_cancel_does_not_sweep_new_generation_ticket() {
    Fixture f(1,true);
    auto old_parent=f.request_cancel;
    std::uint64_t first=0,second=0;
    bool first_ok=false,second_ok=false;
    on(f.w,0,[&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        first=f.table->seal(f.w.a,*f.request,f.w.b,f.selection()).value_or(0);
    });
    send(f.w,f.w.a,1);
    registered_case("62_first_slot_ticket_issued",first!=0);
    on(f.w,1,[&](std::uint64_t payload) {
        first_ok=f.table->redeem(f.w.b,payload).has_value();
    });
    send(f.w,f.w.b,first);
    registered_case("63_first_generation_consumed",
                    first_ok && f.table->pending()==0);
    on(f.w,0,[&](std::uint64_t) {
        f.request_cancel=TaskCancellation::root();
        f.request=f.lifecycle->open_registered(
            {7,8,9002,1,f.w.a},f.request_cancel);
        CHECK(f.request && f.bind_source_mail());
        second=f.table->seal(f.w.a,*f.request,f.w.b,f.selection()).value_or(0);
    });
    send(f.w,f.w.a,2);
    registered_case("64_new_request_uses_same_slot_with_new_generation",
                    second && first!=second &&
                    (first & 0xffff)==(second & 0xffff));
    registered_case("65_new_ticket_is_pending",f.table->pending()==1);
    registered_case("66_cancel_old_parent_only",old_parent->cancel("old-done"));
    registered_case("67_cancelled_old_generation_does_not_sweep_new_ticket",
                    f.table->sweep()==0 && f.table->pending()==1);
    on(f.w,1,[&](std::uint64_t payload) {
        second_ok=f.table->redeem(f.w.b,payload).has_value();
    });
    send(f.w,f.w.b,second);
    registered_case("68_new_generation_ticket_redeems",second_ok);
    bool replay_denied=false;
    on(f.w,1,[&](std::uint64_t payload) {
        replay_denied=!f.table->redeem(f.w.b,payload);
    });
    send(f.w,f.w.b,second);
    registered_case("69_after_redeem_no_pending_or_replay",
                    f.table->pending()==0 && replay_denied);
    registered_case("70_new_request_not_poisoned_by_old_cancellation",
                    f.request->active() && !f.request_cancel->is_cancelled());
    old_parent->retire();
}

void test_repeated_table_shutdown_reclaims_registration_quota() {
    Fixture f(4,true);
    bool churn_ok=true;
    for (int i=0;i<96;++i) {
        try {
            RequestContextHandoffTableV1 short_lived(
                f.w.scheduler,1,*f.lifecycle);
        } catch (...) {
            churn_ok=false;
            break;
        }
    }
    registered_case("71_weak_cleanup_tokens_recycled_past_64_limit",
                    churn_ok);
    std::uint64_t ticket=0;
    bool redeemed=false;
    on(f.w,0,[&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        ticket=f.table->seal(f.w.a,*f.request,f.w.b,f.selection()).value_or(0);
    });
    send(f.w,f.w.a,1);
    on(f.w,1,[&](std::uint64_t payload) {
        redeemed=f.table->redeem(f.w.b,payload).has_value();
    });
    send(f.w,f.w.b,ticket);
    registered_case("72_original_long_lived_handoff_still_works",
                    ticket && redeemed && f.table->pending()==0);
    NativeRequestLifecycleRegistryV1::StopCleanupTokenV1 token;
    registered_case("73_explicit_STOP_hook_token_registered",
                    f.lifecycle->register_stop_cleanup(
                        [](OresNativeActorRef){},&token) && !!token);
    registered_case("74_registered_hook_token_released",
                    f.lifecycle->unregister_stop_cleanup(token));
    registered_case("75_double_hook_release_denied",
                    !f.lifecycle->unregister_stop_cleanup(token));
}

void test_live_hook_quota_and_release() {
    Fixture f(4,true);
    std::vector<std::unique_ptr<RequestContextHandoffTableV1>> holders;
    bool admitted=true;
    for (int i=0;i<63;++i) {
        try {
            holders.push_back(std::make_unique<RequestContextHandoffTableV1>(
                f.w.scheduler,1,*f.lifecycle));
        } catch (...) {
            admitted=false;
            break;
        }
    }
    registered_case("76_exactly_64_total_live_handoff_hooks_admitted",
                    admitted && holders.size()==63);
    bool over_limit=false;
    try {
        RequestContextHandoffTableV1 overflow(
            f.w.scheduler,1,*f.lifecycle);
    } catch (const std::invalid_argument&) {
        over_limit=true;
    }
    registered_case("77_65th_live_handoff_cleanup_hook_rejected",
                    over_limit);
    holders.pop_back();
    registered_case("78_destroying_table_releases_one_hook_slot",
                    holders.size()==62);
    bool reused=false;
    try {
        RequestContextHandoffTableV1 replacement(
            f.w.scheduler,1,*f.lifecycle);
        reused=true;
    } catch (...) {
        reused=false;
    }
    registered_case("79_released_hook_slot_reusable",reused);
    holders.clear();
    registered_case("80_bulk_teardown_does_not_close_parent_request",
                    f.request->active());
}

// An unrelated stopped generation cannot forever poison new ticket-table
// attachment for actors still alive. The new table has no old B envelopes;
// its dual-actor publish gate still rejects B, while A->C remains valid.
void test_new_table_after_unrelated_STOP_still_services_survivors() {
    Fixture f(8,true);
    registered_case("81_B_STOP_admitted_before_new_table",
                    ores_native_actor_scheduler_stop_sealed(f.w.scheduler,f.w.b));
    registered_case("82_B_cleanup_completed_before_new_table",
                    f.lifecycle->wait_cleaned(f.w.b,std::chrono::milliseconds(8000)));
    std::unique_ptr<RequestContextHandoffTableV1> fresh;
    try {
        fresh=std::make_unique<RequestContextHandoffTableV1>(
            f.w.scheduler,4,*f.lifecycle);
    } catch (...) {}
    registered_case("83_new_table_admitted_despite_prior_B_STOP",!!fresh);
    registered_case("84_A_and_C_remain_live",
                    ores_native_actor_scheduler_poll_ready(f.w.scheduler,f.w.a) &&
                    ores_native_actor_scheduler_poll_ready(f.w.scheduler,f.w.c));
    std::uint64_t c_ticket=0;
    bool rejected_b=false;
    on(f.w,0,[&](std::uint64_t) {
        CHECK(f.bind_source_mail());
        CHECK(f.request->set(f.trace,std::string("survivor-ticket")));
        if (fresh) {
            rejected_b=!fresh->seal(f.w.a,*f.request,f.w.b,f.selection());
            c_ticket=fresh->seal(f.w.a,*f.request,f.w.c,f.selection()).value_or(0);
        }
    });
    send(f.w,f.w.a,1);
    registered_case("85_new_table_rejects_stopped_B",rejected_b);
    registered_case("86_new_table_can_publish_to_live_C",
                    fresh && c_ticket && fresh->pending()==1);
    bool c_redeemed=false;
    on(f.w,2,[&](std::uint64_t payload) {
        if (fresh) {
            auto r=fresh->redeem(f.w.c,payload);
            c_redeemed=r && r->context().get_copy(f.trace) ==
                std::string("survivor-ticket");
        }
    });
    send(f.w,f.w.c,c_ticket);
    registered_case("87_live_C_redeems_new_table",
                    c_redeemed && fresh && fresh->pending()==0);
    registered_case("88_C_STOP_admitted_to_revoke_new_table",
                    ores_native_actor_scheduler_stop_sealed(f.w.scheduler,f.w.c));
    registered_case("89_C_cleanup_receipt_after_B_STOP",
                    f.lifecycle->wait_cleaned(f.w.c,std::chrono::milliseconds(8000)));
    registered_case("90_A_still_live_and_original_request_accessible",
                    f.request->active() &&
                    f.request->get_copy(f.trace)==std::string("survivor-ticket"));
    fresh.reset();
}

void test_lock_free_attached_generation_snapshot() {
    Fixture f(8,true,false);
    registered_case("91_registered_sender_snapshot_visible",
                    f.lifecycle->attached_generation(f.w.a));
    registered_case("92_registered_sibling_snapshot_visible",
                    f.lifecycle->attached_generation(f.w.c));
    registered_case("93_unattached_B_snapshot_absent",
                    !f.lifecycle->attached_generation(f.w.b));
    auto stale=f.w.a; ++stale.generation;
    registered_case("94_stale_actor_generation_not_enrolled",
                    !f.lifecycle->attached_generation(stale));
    auto foreign=f.w.c; ++foreign.scheduler_domain;
    registered_case("95_foreign_scheduler_domain_not_enrolled",
                    !f.lifecycle->attached_generation(foreign));
    auto badslot=f.w.c; badslot.actor_slot=99;
    registered_case("96_invalid_actor_slot_not_enrolled",
                    !f.lifecycle->attached_generation(badslot));
    registered_case("97_unattached_B_can_stop_without_listener",
                    ores_native_actor_scheduler_stop_sealed(
                        f.w.scheduler,f.w.b));
    registered_case("98_stopped_unattached_B_cannot_retroactively_attach",
                    !f.lifecycle->attach(f.w.b));
    registered_case("99_stopping_B_does_not_remove_other_attachments",
                    f.lifecycle->attached_generation(f.w.a) &&
                    f.lifecycle->attached_generation(f.w.c));
    registered_case("100_stopped_B_still_not_attached",
                    !f.lifecycle->attached_generation(f.w.b));
}

}  // namespace

int main() {
    struct { const char* name; void (*fn)(); } tests[] = {
        {"snapshot_handoff_is_independent", test_snapshot_handoff_is_independent},
        {"misdirected_forged_offturn_tickets", test_misdirected_forged_offturn_tickets},
        {"cancellation_flows_one_way", test_cancellation_flows_one_way},
        {"sweep_revoke_and_capacity", test_sweep_revoke_and_capacity},
        {"slot_reuse_bumps_generation", test_slot_reuse_bumps_generation},
        {"reentrant_cancellation_during_seal_denied",
            test_reentrant_cancellation_during_seal_denied},
        {"stopped_recipient_revokes_unredeemed_ticket",
            test_stopped_recipient_revokes_unredeemed_ticket},
        {"direct_STOP_during_handoff_copy_rejects_ticket",
            test_direct_STOP_during_handoff_copy_rejects_ticket},
        {"concurrent_cancel_race", test_concurrent_cancel_race},
        {"registered_handoff_stop_reclaims_derived_scope",
            test_registered_handoff_stop_reclaims_derived_scope},
        {"unattached_recipient_cannot_receive_derived_context",
            test_unattached_recipient_cannot_receive_derived_context},
        {"parent_cancellation_reaches_registered_derived_scope",
            test_parent_cancellation_reaches_registered_derived_scope},
        {"sender_STOP_does_not_corrupt_derived_recipient",
            test_sender_STOP_does_not_corrupt_derived_recipient},
        {"pending_ticket_automatically_reclaimed_on_recipient_STOP",
            test_pending_ticket_automatically_reclaimed_on_recipient_STOP},
        {"recipient_STOP_before_seal_denies_ticket_publication",
            test_recipient_STOP_before_seal_denies_ticket_publication},
        {"recipient_STOP_wins_during_sender_snapshot_copy",
            test_recipient_STOP_wins_during_sender_snapshot_copy},
        {"destroyed_table_leaves_no_dangling_CONTROL_callback",
            test_destroyed_table_leaves_no_dangling_CONTROL_callback},
        {"new_cleanup_hooks_after_unrelated_actor_STOP",
            test_new_cleanup_hooks_after_unrelated_actor_STOP},
        {"forged_recipient_cannot_seal_ticket",
            test_forged_recipient_cannot_seal_ticket},
        {"old_cancel_does_not_sweep_new_generation_ticket",
            test_old_cancel_does_not_sweep_new_generation_ticket},
        {"repeated_table_shutdown_reclaims_registration_quota",
            test_repeated_table_shutdown_reclaims_registration_quota},
        {"live_hook_quota_and_release",
            test_live_hook_quota_and_release},
        {"new_table_after_unrelated_STOP_still_services_survivors",
            test_new_table_after_unrelated_STOP_still_services_survivors},
        {"lock_free_attached_generation_snapshot",
            test_lock_free_attached_generation_snapshot},
    };
    for (auto& t : tests) {
        const int before = failures;
        t.fn();
        std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", t.name);
    }
    if (registry_handoff_cases != 100) {
        std::fprintf(stderr, "HANDOFF_REGISTRY_CASE_COUNT_FAILED got=%d expected=100\n",
                     registry_handoff_cases);
        ++failures;
    }
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("HANDOFF_REGISTRY_100_HOST_CASES_PASS");
    std::printf("REQUEST_CONTEXT_HANDOFF_V1_ALL_PASS\n");
    return 0;
}
