#include "oreslang/runtime/RequestLocalSchedulerTurnV1.hpp"

#include <atomic>
#include <unordered_set>
#include <set>
#include <list>
#include <forward_list>
#include <deque>
#include <any>
#include <unordered_map>
#include <variant>
#include <tuple>
#include <optional>
#include <map>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <initializer_list>
#include <thread>
#include <utility>
#include <vector>

using namespace oreslang::runtime;

// RequestLocal uses the SAME host owned-value policy as logical/physical
// local storage. Borrow-only C++ views are negative type-admission fixtures.
static_assert(local_value_allowed_v1<std::string>);
static_assert(!local_value_allowed_v1<int*>);
static_assert(!local_value_allowed_v1<std::reference_wrapper<int>>);
static_assert(!local_value_allowed_v1<std::string_view>);
static_assert(!local_value_allowed_v1<std::initializer_list<int>>);
static_assert(local_value_allowed_v1<std::optional<int>>);
static_assert(local_value_allowed_v1<std::vector<std::string>>);
static_assert(local_value_allowed_v1<std::tuple<int, std::string>>);
static_assert(local_value_allowed_v1<std::map<int, std::string>>);
static_assert(!local_value_allowed_v1<std::optional<std::string_view>>);
static_assert(!local_value_allowed_v1<std::optional<int*>>);
static_assert(!local_value_allowed_v1<std::pair<int, std::reference_wrapper<int>>>);
static_assert(!local_value_allowed_v1<std::tuple<int, std::string_view>>);
static_assert(!local_value_allowed_v1<std::array<int*, 3>>);
static_assert(!local_value_allowed_v1<std::vector<std::optional<std::string_view>>>);
static_assert(!local_value_allowed_v1<std::variant<int, std::string_view>>);
static_assert(!local_value_allowed_v1<std::map<int, std::string_view>>);
static_assert(!local_value_allowed_v1<std::unordered_map<int, std::string_view>>);
static_assert(!local_value_allowed_v1<std::shared_ptr<int>>);
static_assert(!local_value_allowed_v1<std::weak_ptr<int>>);
static_assert(!local_value_allowed_v1<std::function<void()>>);
static_assert(!local_value_allowed_v1<std::any>);
static_assert(!local_value_allowed_v1<std::optional<std::shared_ptr<int>>>);
static_assert(!local_value_allowed_v1<std::vector<std::function<void()>>>);
static_assert(!local_value_allowed_v1<std::deque<int*>>);
static_assert(!local_value_allowed_v1<std::list<int*>>);
static_assert(!local_value_allowed_v1<std::forward_list<int*>>);
static_assert(!local_value_allowed_v1<std::set<int*>>);
static_assert(!local_value_allowed_v1<std::unordered_set<int*>>);


#if __cplusplus >= 202002L
static_assert(!local_value_allowed_v1<std::span<int>>);
static_assert(!local_value_allowed_v1<std::ranges::ref_view<std::vector<int>>>);
#endif

namespace {
using Context = RequestContextLocalV1;
thread_local int test_native_tls_marker = 0;

// HOST-ONLY reentrant copy probe. This opaque C++ pointer carrier is not
// allowed in guest AttoLang typed locals without a compiler-owned descriptor.
struct CloseScopeDuringCopy {
    RequestContextLocalV1* scope;
    explicit CloseScopeDuringCopy(RequestContextLocalV1* s) : scope(s) {}
    CloseScopeDuringCopy(CloseScopeDuringCopy&&) noexcept = default;
    CloseScopeDuringCopy(const CloseScopeDuringCopy& other) : scope(other.scope) {
        assert(scope->close());
    }
};
struct Scenario {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef actor{};
    RequestLocalKeyV1<std::string> trace;
    RequestLocalKeyV1<int> counter;
    TaskCancellation::Ptr left_cancel{TaskCancellation::root()};
    TaskCancellation::Ptr right_cancel{TaskCancellation::root()};
    Context::Ptr left, right;
    std::mutex mutex;
    std::condition_variable cv;
    int turns = 0;
    bool correct = true;
};

void on_actor_message(void* userdata, std::size_t actor_index,
                      std::uint64_t payload) {
    auto& s = *static_cast<Scenario*>(userdata);
    assert(actor_index == 0);
    assert(ores_native_actor_scheduler_is_current_carrier(s.scheduler));
    assert(ores_native_actor_scheduler_is_current_actor(
        s.scheduler, s.actor));
    assert(s.left->belongs_to(s.actor) && s.right->belongs_to(s.actor));
    // Each HTTP request is attached to one scheduler-issued DATA mail. A
    // later message on the same physical actor never inherits that request.
    std::uint64_t mail = 0;
    assert(ores_native_actor_scheduler_current_data_mail_sequence(
        s.scheduler, s.actor, &mail));
    if (payload == 1) assert(s.left->bind_original_mail_once(mail));
    if (payload == 2) assert(s.right->bind_original_mail_once(mail));
    assert(request_local_current_actor_turn_v1(
        s.scheduler, s.actor, *s.left) == (payload == 1));
    assert(request_local_current_actor_turn_v1(
        s.scheduler, s.actor, *s.right) == (payload == 2));
    OresNativeActorRef other_generation = s.actor;
    ++other_generation.generation;
    assert(!request_local_current_actor_turn_v1(
        s.scheduler, other_generation, *s.left));
    // Two overlapping requests have the SAME actor, but different local
    // storage. A physical TLS marker is intentionally shared across turns.
    if (payload == 1) {
        assert(test_native_tls_marker == 0);
        test_native_tls_marker = 41;
        assert(request_local_set_on_actor_turn_v1(
            s.scheduler, s.actor, *s.left, s.trace,
            std::string("trace-left")));
        assert(s.left->set(s.counter, 1));

        // A copy constructor can close its own source scope. The legacy
        // actor adapter must revalidate after copy and return no snapshot.
        auto copy_cancel = TaskCancellation::root();
        auto copy_scope = Context::open(
            {11, 22, 503, 1, s.actor}, copy_cancel);
        assert(copy_scope);
        assert(copy_scope->bind_original_mail_once(mail));
        RequestLocalKeyV1<CloseScopeDuringCopy> copied;
        assert(request_local_set_on_actor_turn_v1(
            s.scheduler, s.actor, *copy_scope, copied,
            CloseScopeDuringCopy(copy_scope.get())));
        assert(!request_local_get_on_actor_turn_v1(
            s.scheduler, s.actor, *copy_scope, copied));
        assert(!copy_scope->active());
        copy_scope.reset();
        copy_cancel->retire();
    } else if (payload == 2) {
        assert(test_native_tls_marker == 41);
        assert(!s.right->get_copy(s.trace));
        assert(request_local_set_on_actor_turn_v1(
            s.scheduler, s.actor, *s.right, s.trace,
            std::string("trace-right")));
        assert(s.right->set(s.counter, 2));
    } else if (payload == 3) {
        assert(test_native_tls_marker == 41);
        // Both previous request-local values exist as owned host state,
        // but neither may be observed through the guest-turn adapter.
        assert(!request_local_get_on_actor_turn_v1(
            s.scheduler, s.actor, *s.left, s.trace));
        assert(!request_local_get_on_actor_turn_v1(
            s.scheduler, s.actor, *s.right, s.trace));
        assert(s.left->get_copy(s.trace) == std::string("trace-left"));
        assert(s.left->get_copy(s.counter) == 1);
        assert(s.right->get_copy(s.trace) == std::string("trace-right"));
    } else if (payload == 4) {
        assert(s.right->get_copy(s.trace) == std::string("trace-right"));
        assert(s.right->get_copy(s.counter) == 2);
        assert(s.left->get_copy(s.trace) == std::string("trace-left"));
    } else {
        std::terminate();
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        assert(++s.turns == static_cast<int>(payload));
    }
    s.cv.notify_one();
}

void deliver(Scenario& s, int turn) {
    assert(ores_native_actor_scheduler_try_send(
        s.scheduler, s.actor, static_cast<std::uint64_t>(turn))
        == ORES_NATIVE_SEND_ACCEPTED);
    std::unique_lock<std::mutex> lock(s.mutex);
    assert(s.cv.wait_for(lock, std::chrono::seconds(8),
                         [&] { return s.turns >= turn; }));
}

void test_same_actor_two_simultaneous_requests() {
    Scenario s;
    s.scheduler = ores_native_actor_scheduler_open(
        1, 4, 1, on_actor_message, &s);
    assert(s.scheduler);
    s.actor = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    assert(s.actor.scheduler_domain);

    const RequestIdentityV1 a{11, 22, 501, 1, s.actor};
    const RequestIdentityV1 b{11, 22, 502, 1, s.actor};
    s.left = Context::open(a, s.left_cancel);
    s.right = Context::open(b, s.right_cancel);
    assert(s.left && s.right);
    assert(s.left->identity().request != s.right->identity().request);
    assert(s.left->identity().generation == 1);
    assert(!s.left->belongs_to({s.actor.scheduler_domain,
                                s.actor.actor_slot,
                                s.actor.generation + 1}));
    // Possessing a logical request pointer does not grant host-side
    // scheduler authority to invoke the guest adapter from another thread.
    assert(!request_local_current_actor_turn_v1(
        s.scheduler, s.actor, *s.left));
    assert(!request_local_set_on_actor_turn_v1(
        s.scheduler, s.actor, *s.left, s.counter, 123));
    assert(!request_local_get_on_actor_turn_v1(
        s.scheduler, s.actor, *s.left, s.trace));

    deliver(s, 1);
    deliver(s, 2);
    deliver(s, 3);
    deliver(s, 4);

    // Cancelling a single request must not cancel the shared actor or the
    // other request's context.
    assert(s.left_cancel->cancel("http-request-disconnected"));
    assert(!s.left->active());
    assert(!s.left->get_copy(s.trace));
    assert(!s.left->set(s.trace, std::string("stale")));
    assert(s.right->active());
    assert(s.right->get_copy(s.trace) == std::string("trace-right"));

    assert(ores_native_actor_scheduler_stop(s.scheduler, s.actor));
    assert(ores_native_actor_scheduler_wait_done(s.scheduler, s.actor));
    ores_native_actor_scheduler_retire(s.scheduler);
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(ores_native_actor_scheduler_try_destroy(s.scheduler));
    assert(s.right->close());
    assert(!s.right->close());
    assert(!s.right->active());
    s.left_cancel->retire();
    s.right_cancel->retire();
}

void test_context_moves_between_physical_threads() {
    auto authority = TaskCancellation::root();
    const OresNativeActorRef actor{7, 1, 9};
    auto context = Context::open({1, 2, 3, 4, actor}, authority);
    assert(context);
    RequestLocalKeyV1<int> key;
    RequestLocalKeyV1<std::string> label;
    std::atomic<int> phase{0};
    std::thread worker_a([&] {
        test_native_tls_marker = 17;
        assert(context->set(key, 100));
        assert(context->set(label, std::string("scope-is-logical")));
        phase.store(1, std::memory_order_release);
    });
    worker_a.join();
    std::thread worker_b([&] {
        // A new physical thread starts with independent TLS, but reuses the
        // SAME explicit logical request handle (as a resumed Task would).
        assert(test_native_tls_marker == 0);
        assert(phase.load(std::memory_order_acquire) == 1);
        assert(context->get_copy(key) == 100);
        assert(context->get_copy(label) ==
               std::string("scope-is-logical"));
        test_native_tls_marker = 93;
        assert(context->set(key, 101));
    });
    worker_b.join();
    assert(test_native_tls_marker == 0);  // independent host thread TLS
    assert(context->get_copy(key) == 101);
    assert(authority->cancel("request-end"));
    assert(!context->get_copy(key));
    assert(!context->set(key, 102));
    authority->retire();
}

void test_capacity_and_replay_fail_closed() {
    auto authority = TaskCancellation::root();
    assert(!Context::open({}, authority));
    assert(!Context::open({1, 2, 3, 4, {5, 1, 9}}, {}));
    auto scope = Context::open({1, 2, 3, 4, {5, 1, 9}}, authority);
    assert(scope);
    std::vector<RequestLocalKeyV1<int>> keys;
    for (int index = 0; index < 33; ++index) keys.emplace_back();
    for (int index = 0; index < 32; ++index)
        assert(scope->set(keys[static_cast<std::size_t>(index)], index));
    assert(!scope->set(keys[32], 999));
    assert(scope->get_copy(keys[31]) == 31);
    assert(scope->set(keys[0], 123)); // replacement does not use extra slot
    assert(scope->get_copy(keys[0]) == 123);
    // Explicit erase must restore capacity without running value destructor
    // under the scope lock and without permitting double retirement.
    assert(scope->erase(keys[1]));
    assert(!scope->erase(keys[1]));
    assert(!scope->get_copy(keys[1]));
    assert(scope->set(keys[32], 999));
    assert(scope->get_copy(keys[32]) == 999);
    assert(scope->erase(keys[32]));
    assert(!scope->erase(keys[32]));
    assert(scope->close());
    assert(!scope->get_copy(keys[0]));
    assert(!scope->set(keys[0], 2));
    assert(!scope->erase(keys[0]));
    authority->retire();
}

// A copy of an already admitted value is user-defined code. Copying under
// the request lock previously deadlocked if it closed/cancelled this scope.
struct ReentrantRequestCopy {
    std::function<void()> on_copy;
    explicit ReentrantRequestCopy(std::function<void()> callback)
        : on_copy(std::move(callback)) {}
    ReentrantRequestCopy(ReentrantRequestCopy&&) noexcept = default;
    ReentrantRequestCopy(const ReentrantRequestCopy& other)
        : on_copy(other.on_copy) {
        if (on_copy) on_copy();
    }
};

void test_request_copy_reentrancy_and_pin_lifetime() {
    for (int iteration = 0; iteration < 48; ++iteration) {
        auto cancellation = TaskCancellation::root();
        auto scope = Context::open(
            {42, 5, static_cast<std::uint64_t>(iteration + 1), 1,
             {84, 1, 1}}, cancellation);
        assert(scope);
        RequestLocalKeyV1<ReentrantRequestCopy> key;
        std::atomic<int> callback_count{0};
        std::weak_ptr<Context> weak = scope;

        // Alternate synchronous closure and cancellation from T's copy.
        // The source cell must remain pinned after the store is emptied.
        assert(scope->set(key, ReentrantRequestCopy([weak, cancellation,
                                                      &callback_count,
                                                      iteration] {
            ++callback_count;
            if ((iteration & 1) == 0) {
                auto live = weak.lock();
                assert(live);
                assert(live->close());
            } else {
                assert(cancellation->cancel("reentrant-copy"));
            }
        })));
        auto snapshot = scope->get_copy(key);
        assert(snapshot);
        assert(callback_count.load() == 1);
        assert(!scope->active());
        assert(!scope->get_copy(key));
        assert(!scope->set(key, ReentrantRequestCopy([] {})));
        snapshot.reset();
        scope.reset();
        cancellation->retire();
    }
}

// Replacing a cell invokes its destructor after dropping the request lock.
// Reentrant cleanup must see a fully published replacement.
struct ReentrantRequestDrop {
    std::function<void()> on_drop;
    explicit ReentrantRequestDrop(std::function<void()> callback)
        : on_drop(std::move(callback)) {}
    ReentrantRequestDrop(const ReentrantRequestDrop&) = default;
    // A user-defined destructor suppresses the implicit move constructor.
    // Without an explicit move, set(T) copies the callback twice and runs it
    // on temporary/parameter teardown before the stored cell is replaced.
    // A moved-from std::function is only "valid but unspecified", so clear
    // it explicitly to disarm the temporary.
    ReentrantRequestDrop(ReentrantRequestDrop&& other) noexcept
        : on_drop(std::exchange(other.on_drop, nullptr)) {}
    ~ReentrantRequestDrop() { if (on_drop) on_drop(); }
};

void test_request_reentrant_drop_after_replacement() {
    auto cancellation = TaskCancellation::root();
    auto scope = Context::open({11, 12, 13, 1, {14, 0, 1}},
                               cancellation);
    assert(scope);
    RequestLocalKeyV1<ReentrantRequestDrop> key;
    std::atomic<int> callbacks{0};
    std::weak_ptr<Context> weak = scope;
    assert(scope->set(key, ReentrantRequestDrop([weak, &callbacks] {
        ++callbacks;
        auto live = weak.lock();
        assert(live && live->active());
    })));
    assert(scope->set(key, ReentrantRequestDrop([] {})));
    assert(callbacks.load() == 1);
    // Erase runs destructor AFTER removing the key and releasing the lock.
    // A reentrant destructor may install a new cell; outer erase must not
    // discard it or deadlock on the same RequestLocal mutex.
    assert(scope->set(key, ReentrantRequestDrop([weak, &key, &callbacks] {
        ++callbacks;
        auto live = weak.lock();
        assert(live);
        assert(live->set(key, ReentrantRequestDrop([] {})));
    })));
    assert(scope->erase(key));
    assert(callbacks.load() == 2);
    assert(scope->erase(key));
    assert(!scope->erase(key));
    assert(!scope->get_copy(key));
    assert(scope->close());
    scope.reset();
    cancellation->retire();
}

void test_cancel_and_set_race() {
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto authority = TaskCancellation::root();
        auto scope = Context::open(
            {1, 1, static_cast<std::uint64_t>(iteration + 1), 1,
             {1, 1, 1}}, authority);
        assert(scope);
        RequestLocalKeyV1<int> key;
        std::atomic<bool> begin{false};
        std::thread writer([&] {
            while (!begin.load(std::memory_order_acquire))
                std::this_thread::yield(); // test worker, not carrier
            for (int n = 0; n != 200; ++n) {
                if (!scope->set(key, n)) break;
                if ((n % 3) == 0) (void)scope->erase(key);
                (void)scope->get_copy(key);
            }
        });
        begin.store(true, std::memory_order_release);
        assert(authority->cancel("cancel-concurrently"));
        writer.join();
        assert(!scope->active());
        assert(!scope->get_copy(key));
        assert(!scope->set(key, 999));
        authority->retire();
    }
}
} // namespace

int main() {
    test_same_actor_two_simultaneous_requests();
    test_context_moves_between_physical_threads();
    test_capacity_and_replay_fail_closed();
    test_request_copy_reentrancy_and_pin_lifetime();
    test_request_reentrant_drop_after_replacement();
    test_cancel_and_set_race();
    std::puts("REQUEST_CONTEXT_LOCAL_HOST_ONLY_PASS");
}
