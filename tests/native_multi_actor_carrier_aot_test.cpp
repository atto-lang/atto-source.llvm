#include "oreslang/runtime/MultiActorCarrier.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <thread>

using namespace oreslang::runtime;
using namespace std::chrono_literals;

extern "C" {
void oreslang_rt_slow_frame_init(void*);
int oreslang_rt_slow_frame_step(void*, std::uint64_t);
void oreslang_rt_slow_frame_cancel(void*);
std::int64_t oreslang_rt_slow_frame_result(void*);
void oreslang_rt_fast_frame_init(void*);
int oreslang_rt_fast_frame_step(void*, std::uint64_t);
void oreslang_rt_fast_frame_cancel(void*);
std::int64_t oreslang_rt_fast_frame_result(void*);
}

#define REQUIRE(x) do { if (!(x)) { \
    std::fprintf(stderr, "MultiActorCarrier check failed at %s:%d: %s\n", \
                 __FILE__, __LINE__, #x); std::abort(); \
} } while (0)

struct Frame { std::uint64_t state = 0, count = 0, magic = 0; };
static_assert(sizeof(Frame) == 24, "generated frame C ABI size changed");
struct SharedEvidence {
    std::atomic<int> progress_b_while_a_incomplete{0};
    std::atomic<int> errors{0};
    std::atomic<std::uint64_t> thread_tag{0};
    std::atomic<std::uint64_t> a_dispatches{0};
    std::atomic<std::uint64_t> b_completed{0};
};
struct ActorState {
    Frame frame{};
    SharedEvidence* evidence = nullptr;
    bool slow = false;
    std::uint64_t next_message = 1; // worker-only ownership
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> cancellations{0};
    std::atomic<std::uint64_t> last_counter{0};
    std::atomic<std::uint64_t> worker_tag{0};
};
static MultiActorCarrier::Outcome dispatch(
    void* raw, MultiActorCarrier::Message message,
    MultiActorCarrier::Cause cause, std::uint64_t fuel) noexcept {
    auto& actor = *static_cast<ActorState*>(raw);
    auto& e = *actor.evidence;
    const auto tag = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::uint64_t unset = 0;
    (void)e.thread_tag.compare_exchange_strong(unset, tag);
    unset = 0;
    (void)actor.worker_tag.compare_exchange_strong(unset, tag);
    if (e.thread_tag.load() != tag || actor.worker_tag.load() != tag ||
            fuel != 32 || message.sequence != actor.next_message)
        e.errors.fetch_add(1);

    if (cause == MultiActorCarrier::Cause::initial) {
        if (actor.frame.state != 0) e.errors.fetch_add(1);
        if (actor.slow) oreslang_rt_slow_frame_init(&actor.frame);
        else oreslang_rt_fast_frame_init(&actor.frame);
    } else if (cause != MultiActorCarrier::Cause::cpu_resume ||
               actor.frame.state != 1) {
        e.errors.fetch_add(1);
    }
    if (actor.slow) e.a_dispatches.fetch_add(1);
    else if (e.a_dispatches.load() > 0 &&
             actor.evidence->b_completed.load() == 0 &&
             actor.frame.state == 1 &&
             actor.next_message == 1)
        e.progress_b_while_a_incomplete.fetch_add(1);

    const auto before = actor.frame.count;
    const int status = actor.slow
        ? oreslang_rt_slow_frame_step(&actor.frame, fuel)
        : oreslang_rt_fast_frame_step(&actor.frame, fuel);
    const auto expected = actor.slow ? 1000000000ULL : 1000ULL;
    if (actor.frame.count < before || actor.frame.count > before + fuel ||
            actor.frame.count > expected)
        e.errors.fetch_add(1);
    actor.last_counter.store(actor.frame.count);
    if (status == 1 && actor.frame.state == 1)
        return MultiActorCarrier::Outcome::cooperate;
    if (status == 0 && actor.frame.state == 2) {
        const auto result = actor.slow
            ? oreslang_rt_slow_frame_result(&actor.frame)
            : oreslang_rt_fast_frame_result(&actor.frame);
        if (result != static_cast<std::int64_t>(expected)) e.errors.fetch_add(1);
        ++actor.next_message;
        actor.completed.fetch_add(1);
        if (!actor.slow) {
            e.b_completed.fetch_add(1);
            // Reinitialization is allowed only after a complete turn.
            actor.frame = Frame{};
        }
        return MultiActorCarrier::Outcome::complete;
    }
    e.errors.fetch_add(1);
    return MultiActorCarrier::Outcome::complete;
}
static void cancel(void* raw, MultiActorCarrier::Message,
                   bool had_active) noexcept {
    auto& actor = *static_cast<ActorState*>(raw);
    const auto tag = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    if (tag != actor.evidence->thread_tag.load())
        actor.evidence->errors.fetch_add(1);
    if (had_active) {
        if (actor.frame.state != 1) actor.evidence->errors.fetch_add(1);
        if (actor.slow) oreslang_rt_slow_frame_cancel(&actor.frame);
        else oreslang_rt_fast_frame_cancel(&actor.frame);
        actor.cancellations.fetch_add(1);
    }
}
template<class Pred>
static bool until(Pred pred, std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}
static void test_bounded_fairness_cancellation_and_lifetime() {
    SharedEvidence evidence;
    ActorState a; a.evidence = &evidence; a.slow = true;
    ActorState b; b.evidence = &evidence;
    MultiActorCarrier::Producer expired;
    {
        MultiActorCarrier carrier(32);
        const auto aId = carrier.register_actor(&a, dispatch, cancel);
        const auto bId = carrier.register_actor(&b, dispatch, cancel);
        bool duplicate_root = false;
        try { (void)carrier.register_actor(&b, dispatch, cancel); }
        catch (const std::invalid_argument&) { duplicate_root = true; }
        REQUIRE(duplicate_root);
        ActorState unused[6];
        for (auto& st : unused) {
            st.evidence = &evidence;
            (void)carrier.register_actor(&st, dispatch, cancel);
        }
        bool overflow = false;
        try { (void)carrier.register_actor(&b, dispatch, cancel); }
        catch (const std::overflow_error&) { overflow = true; }
        REQUIRE(overflow);
        expired = carrier.producer();
        REQUIRE(!expired.try_send(aId, {1, 0})); // admission before start
        carrier.start();
        REQUIRE(!expired.try_send({aId.slot, aId.generation + 100}, {1, 0}));
        REQUIRE(!expired.try_send({0, aId.generation}, {1, 0}));
        REQUIRE(expired.try_send(aId, {1, 0}));
        REQUIRE(until([&] { return evidence.a_dispatches.load() > 0; }, 2s));
        for (std::uint64_t i = 1; i <= 64; ++i)
            REQUIRE(until([&] {
                return expired.try_send(bId, {i, static_cast<std::int64_t>(i)});
            }, 2s));
        REQUIRE(until([&] { return b.completed.load() == 64; }, 10s));
        REQUIRE(evidence.progress_b_while_a_incomplete.load() > 0);
        REQUIRE(a.completed.load() == 0);
        REQUIRE(a.last_counter.load() > 0);
        REQUIRE(evidence.errors.load() == 0);
        REQUIRE(expired.request_cancel(aId));
        REQUIRE(!expired.request_cancel(aId));
        REQUIRE(!expired.try_send(aId, {2, 0}));
        REQUIRE(until([&] { return carrier.actor_stats(aId).retired; }, 2s));
        REQUIRE(a.cancellations.load() == 1);
        REQUIRE(a.completed.load() == 0);
        REQUIRE(!expired.try_send(aId, {2, 0}));
        const auto pre = carrier.stats();
        REQUIRE(pre.accepted == 65 && pre.completed == 64);
        REQUIRE(pre.cancelled_active == 1);
        REQUIRE(pre.cooperations > 100);
        REQUIRE(pre.worker_thread_tag != 0);
        REQUIRE(pre.worker_thread_tag == a.worker_tag.load());
        REQUIRE(pre.worker_thread_tag == b.worker_tag.load());
        REQUIRE(carrier.actor_stats(bId).completed == 64);
        REQUIRE(carrier.actor_stats(bId).cooperations >= 64);
        carrier.join();
        const auto s = carrier.stats();
        REQUIRE(s.abandoned == 1);
        REQUIRE(s.protocol_failures == 0);
        REQUIRE(carrier.stopped());
        REQUIRE(a.frame.state == 3);
        REQUIRE(b.frame.state == 0); // terminal frame was reset after completion
        REQUIRE(evidence.errors.load() == 0);
        REQUIRE(!expired.try_send(bId, {65, 0}));
    }
    // Weak producer does not dereference freed carrier or state.
    REQUIRE(!expired.try_send({1, 1}, {999, 0}));
    REQUIRE(!expired.request_cancel({1, 1}));
}
struct BusyHost {
    std::atomic<int> initials{0}, resumes{0}, cancels{0}, thread_errors{0};
    std::atomic<std::uint64_t> tag{0};
};
static MultiActorCarrier::Outcome always_cooperate(
        void* ptr, MultiActorCarrier::Message,
        MultiActorCarrier::Cause cause, std::uint64_t fuel) noexcept {
    auto& s = *static_cast<BusyHost*>(ptr);
    const auto tag = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::uint64_t unset = 0;
    (void)s.tag.compare_exchange_strong(unset, tag);
    if (s.tag.load() != tag || fuel != 32) s.thread_errors.fetch_add(1);
    if (cause == MultiActorCarrier::Cause::initial)
        s.initials.fetch_add(1);
    else if (cause == MultiActorCarrier::Cause::cpu_resume)
        s.resumes.fetch_add(1);
    else s.thread_errors.fetch_add(1);
    return MultiActorCarrier::Outcome::cooperate;
}
static void busy_cancel(void* ptr, MultiActorCarrier::Message,
                        bool had_active) noexcept {
    auto& s = *static_cast<BusyHost*>(ptr);
    const auto tag = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    if (tag != s.tag.load() || !had_active)
        s.thread_errors.fetch_add(1);
    s.cancels.fetch_add(1);
}
static void test_saturation_and_concurrent_cancel() {
    BusyHost busy;
    MultiActorCarrier carrier(32);
    const auto ticket = carrier.register_actor(&busy, always_cooperate, busy_cancel);
    auto producer = carrier.producer();
    carrier.start();
    REQUIRE(producer.try_send(ticket, {1, 0}));
    REQUIRE(until([&] { return busy.initials.load() == 1; }, 2s));
    // Until the active turn completes, NONE of these 64 later messages may
    // reenter the actor. A saturated mailbox cannot block cancellation.
    for (std::uint64_t n = 2; n <= 65; ++n)
        REQUIRE(producer.try_send(ticket, {n, 0}));
    REQUIRE(!producer.try_send(ticket, {66, 0}));
    REQUIRE(busy.initials.load() == 1);
    REQUIRE(until([&] { return busy.resumes.load() >= 32; }, 2s));

    // Multiple producers racing with cancellation may only publish through
    // the bounded mutex gate; successful admissions remain accounted once.
    std::atomic<bool> release{false};
    std::atomic<int> successes{0};
    std::thread producers[4];
    for (auto& thread : producers) {
        thread = std::thread([&] {
            while (!release.load()) std::this_thread::yield();
            for (int i = 0; i != 100; ++i)
                if (producer.try_send(ticket, {1000 + static_cast<std::uint64_t>(i), 0}))
                    successes.fetch_add(1);
        });
    }
    release.store(true);
    REQUIRE(producer.request_cancel(ticket));
    for (auto& thread : producers) thread.join();
    REQUIRE(!producer.try_send(ticket, {999, 0}));
    REQUIRE(until([&] { return carrier.actor_stats(ticket).retired; }, 2s));
    REQUIRE(busy.cancels.load() == 1);
    REQUIRE(busy.thread_errors.load() == 0);
    REQUIRE(busy.initials.load() == 1);
    REQUIRE(successes.load() == 0);
    carrier.join();
    const auto stats = carrier.stats();
    REQUIRE(stats.accepted == 65);
    REQUIRE(stats.completed == 0 && stats.abandoned == 65);
    REQUIRE(stats.cancelled_active == 1 && stats.protocol_failures == 0);
    REQUIRE(stats.worker_thread_tag == busy.tag.load());
}
static void test_bad_fuel_and_start_constraints() {
    for (std::uint64_t n : {0ULL, 513ULL}) {
        bool invalid = false;
        try { MultiActorCarrier carrier(n); }
        catch (const std::invalid_argument&) { invalid = true; }
        REQUIRE(invalid);
    }
    MultiActorCarrier carrier;
    bool invalid = false;
    try { (void)carrier.register_actor(nullptr, dispatch, cancel); }
    catch (const std::invalid_argument&) { invalid = true; }
    REQUIRE(invalid);
    carrier.start();
    invalid = false;
    SharedEvidence e;
    ActorState state; state.evidence = &e;
    try { (void)carrier.register_actor(&state, dispatch, cancel); }
    catch (const std::logic_error&) { invalid = true; }
    REQUIRE(invalid);
    carrier.join();
}
int main() {
    test_bad_fuel_and_start_constraints();
    test_saturation_and_concurrent_cancel();
    test_bounded_fairness_cancellation_and_lifetime();
    std::puts("TWO_COMPILED_ACTORS_ONE_REAL_CARRIER_PASS");
    return 0;
}
