// Host-only regression: copying a local value must not revive an actor or
// task scope already revoked by cancellation, STOP, or owner retirement.
// Custom C++ aggregates here are *not* compiler-authorized guest values.
#include "oreslang/runtime/LocalContextDomainsV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

using namespace oreslang::runtime;
namespace {
constexpr ExecutionStampV1 parent_id{9, 18, 71, 3, 1, 101, 1};
constexpr ExecutionStampV1 child_id {9, 18, 71, 3, 1, 102, 1};

template<class Scope>
struct CloseOnCopy {
    std::shared_ptr<Scope> scope;
    explicit CloseOnCopy(std::shared_ptr<Scope> s) : scope(std::move(s)) {}
    CloseOnCopy(CloseOnCopy&&) noexcept = default;
    CloseOnCopy(const CloseOnCopy& other) : scope(other.scope) {
        // get_copy() must drop its mutex BEFORE calling host copy code.
        assert(scope->close());
    }
};

struct PauseOnCopy {
    std::shared_ptr<std::atomic<bool>> entered;
    std::shared_ptr<std::atomic<bool>> release;
    PauseOnCopy(std::shared_ptr<std::atomic<bool>> a,
                std::shared_ptr<std::atomic<bool>> b)
        : entered(std::move(a)), release(std::move(b)) {}
    PauseOnCopy(PauseOnCopy&&) noexcept = default;
    PauseOnCopy(const PauseOnCopy& other)
        : entered(other.entered), release(other.release) {
        entered->store(true, std::memory_order_release);
        while (!release->load(std::memory_order_acquire))
            std::this_thread::yield();
    }
};

void actor_reentrant_close_denies_snapshot() {
    auto scope = ActorLocalScopeV1::open(parent_id);
    assert(scope);
    ActorLocalKeyV1<CloseOnCopy<ActorLocalScopeV1>> key;
    assert(scope->set(parent_id, key, CloseOnCopy(scope)));
    const auto returned = scope->get_copy(parent_id, key);
    assert(!returned); // closed DURING copy, not before get_copy()
    assert(!scope->is_open());
    assert(!scope->get_copy(parent_id, key));
    assert(!scope->set(parent_id, key, CloseOnCopy(scope)));
    assert(!scope->close());
}

void task_parent_cancel_during_child_copy_denied() {
    auto parent = TaskLocalScopeV1::open(parent_id);
    auto child = TaskLocalScopeV1::open(child_id);
    assert(parent && child);
    TaskLocalKeyV1<CloseOnCopy<TaskLocalScopeV1>> key;
    assert(parent->set(parent_id, key, CloseOnCopy(parent)));
    // The explicit child-copy path invokes the parent's value copy. The
    // value closes its own parent, so it must never install into the child.
    assert(!parent->copy_to_child(parent_id, *child, child_id, key));
    assert(!child->get_copy(child_id, key));
    assert(!parent->is_open());
    assert(child->is_open());
    assert(child->close());
}

void concurrent_close_during_copy_denied() {
    auto entered = std::make_shared<std::atomic<bool>>(false);
    auto release = std::make_shared<std::atomic<bool>>(false);
    for (int i = 0; i < 24; ++i) {
        auto scope = TaskLocalScopeV1::open(parent_id);
        assert(scope);
        TaskLocalKeyV1<PauseOnCopy> key;
        assert(scope->set(parent_id, key, PauseOnCopy(entered, release)));
        entered->store(false, std::memory_order_release);
        release->store(false, std::memory_order_release);
        std::atomic<bool> returned_value{true};
        std::thread reader([&] {
            returned_value.store(scope->get_copy(parent_id, key).has_value(),
                                 std::memory_order_release);
        });
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(8);
        while (!entered->load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        assert(entered->load(std::memory_order_acquire));
        // The copy constructor is paused outside the mutex. Close now,
        // before permitting that copy to finish.
        assert(scope->close());
        release->store(true, std::memory_order_release);
        reader.join();
        assert(!returned_value.load(std::memory_order_acquire));
        assert(!scope->get_copy(parent_id, key));
    }
}

void stable_snapshot_still_works() {
    auto scope = TaskLocalScopeV1::open(parent_id);
    TaskLocalKeyV1<int> key;
    assert(scope && scope->set(parent_id, key, 42));
    assert(scope->get_copy(parent_id, key) == 42);
    const ExecutionStampV1 stale{9, 18, 71, 3, 1, 101, 2};
    assert(!scope->get_copy(stale, key));
    assert(scope->close());
    assert(!scope->get_copy(parent_id, key));
}
}
int main() {
    actor_reentrant_close_denies_snapshot();
    task_parent_cancel_during_child_copy_denied();
    concurrent_close_during_copy_denied();
    stable_snapshot_still_works();
    std::puts("LOCAL_CONTEXT_READ_REVOCATION_HOST_PASS");
}
