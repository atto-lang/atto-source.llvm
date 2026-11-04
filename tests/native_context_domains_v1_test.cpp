#include "oreslang/runtime/LocalContextDomainsV1.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::runtime;

namespace {
constexpr ExecutionStampV1 actor_a{9, 18, 71, 3, 1, 101, 1};
constexpr ExecutionStampV1 task_b{9, 18, 71, 3, 1, 102, 1};
constexpr ExecutionStampV1 other_actor{9, 18, 71, 4, 1, 101, 1};
constexpr ExecutionStampV1 reincarnated{9, 18, 71, 3, 2, 101, 1};
constexpr ExecutionStampV1 reused_task{9, 18, 71, 3, 1, 101, 2};

void test_isolation_and_migration() {
    auto actor = ActorLocalScopeV1::open(actor_a);
    auto ta = TaskLocalScopeV1::open(actor_a);
    auto tb = TaskLocalScopeV1::open(task_b);
    assert(actor && ta && tb);
    ActorLocalKeyV1<std::string> actor_key;
    TaskLocalKeyV1<std::string> task_key;
    assert(actor->set(actor_a, actor_key, std::string("actor-config")));
    assert(ta->set(actor_a, task_key, std::string("span-a")));
    assert(tb->set(task_b, task_key, std::string("span-b")));
    assert(!ta->get_copy(task_b, task_key));
    assert(!actor->get_copy(other_actor, actor_key));
    assert(!actor->get_copy(reincarnated, actor_key));
    assert(!ta->get_copy(reused_task, task_key));
    assert(!ta->set(other_actor, task_key, std::string("injection")));
    std::thread carrier_a([&] {
        assert(ta->get_copy(actor_a, task_key) == std::string("span-a"));
        assert(actor->get_copy(actor_a, actor_key) == std::string("actor-config"));
    });
    carrier_a.join();
    std::thread carrier_b([&] {
        assert(ta->get_copy(actor_a, task_key) == std::string("span-a"));
        assert(actor->get_copy(task_b, actor_key) == std::string("actor-config"));
        assert(tb->get_copy(task_b, task_key) == std::string("span-b"));
    });
    carrier_b.join();
    assert(ta->copy_to_child(actor_a, *tb, task_b, task_key));
    assert(tb->get_copy(task_b, task_key) == std::string("span-a"));
    assert(ta->set(actor_a, task_key, std::string("new-parent")));
    assert(tb->get_copy(task_b, task_key) == std::string("span-a"));
    assert(!ta->copy_to_child(actor_a, *ta, actor_a, task_key));
    assert(actor->close());
    assert(!actor->close());
    assert(!actor->get_copy(actor_a, actor_key));
    assert(!actor->set(actor_a, actor_key, std::string("stale")));
    assert(ta->close());
    assert(tb->close());
}

struct DropProbe {
    std::shared_ptr<std::atomic<int>> drops;
    explicit DropProbe(std::shared_ptr<std::atomic<int>> c) : drops(std::move(c)) {}
    DropProbe(const DropProbe&) = default;
    DropProbe(DropProbe&& other) noexcept : drops(std::move(other.drops)) {}
    DropProbe& operator=(const DropProbe&) = default;
    ~DropProbe() { if (drops) ++*drops; }
};

void test_exact_drop_and_physical_tls() {
    auto drops = std::make_shared<std::atomic<int>>(0);
    ThreadLocalKeyV1<DropProbe> key;
    std::thread worker([&] {
        assert(key.set(DropProbe{drops}));
        assert(key.set(DropProbe{drops}));
        assert(*drops == 1); // overwritten value retired exactly once
    });
    worker.join();
    assert(*drops == 2); // second value destroyed on thread exit
    assert(!key.get_copy());
    ThreadLocalKeyV1<int> num;
    assert(num.set(15));
    std::thread other([&] {
        assert(!num.get_copy());
        assert(num.set(99));
        assert(num.get_copy() == 99);
    });
    other.join();
    assert(num.get_copy() == 15);
    assert(num.erase());
    assert(!num.get_copy());
}

struct ReentrantCopy {
    std::shared_ptr<ActorLocalScopeV1> scope;
    explicit ReentrantCopy(std::shared_ptr<ActorLocalScopeV1> value)
        : scope(std::move(value)) {}
    ReentrantCopy(ReentrantCopy&&) noexcept = default;
    ReentrantCopy(const ReentrantCopy& other) : scope(other.scope) {
        // A read that copied under the scope mutex would deadlock here.
        assert(scope->close());
    }
};

struct ThreadReentrantCopy {
    std::function<void()> on_copy;
    explicit ThreadReentrantCopy(std::function<void()> callback)
        : on_copy(std::move(callback)) {}
    ThreadReentrantCopy(ThreadReentrantCopy&&) noexcept = default;
    ThreadReentrantCopy(const ThreadReentrantCopy& other)
        : on_copy(other.on_copy) { on_copy(); }
};

void test_reentrant_copy_safe_retirement() {
    auto actor = ActorLocalScopeV1::open(actor_a);
    ActorLocalKeyV1<ReentrantCopy> actor_key;
    assert(actor->set(actor_a, actor_key, ReentrantCopy(actor)));
    auto copied = actor->get_copy(actor_a, actor_key);
    assert(copied);
    assert(!actor->is_open());
    assert(!actor->get_copy(actor_a, actor_key));

    ThreadLocalKeyV1<ThreadReentrantCopy> tls_key;
    assert(tls_key.set(ThreadReentrantCopy([&] { (void)tls_key.erase(); })));
    auto tls_snapshot = tls_key.get_copy();
    assert(tls_snapshot);
    assert(!tls_key.get_copy());
}

void test_bounds_and_close_race() {
    assert(!TaskLocalScopeV1::open({}));
    assert(!ActorLocalScopeV1::open({}));
    auto scope = TaskLocalScopeV1::open(actor_a);
    std::vector<TaskLocalKeyV1<int>> keys;
    for (int n = 0; n != 33; ++n) keys.emplace_back();
    for (int n = 0; n != 32; ++n)
        assert(scope->set(actor_a, keys[n], n));
    assert(!scope->set(actor_a, keys[32], 500));
    assert(scope->set(actor_a, keys[0], 800));
    assert(scope->get_copy(actor_a, keys[0]) == 800);
    assert(scope->erase(actor_a, keys[1]));
    assert(scope->set(actor_a, keys[32], 500));
    scope->close();
    assert(!scope->get_copy(actor_a, keys[32]));
    for (int repeat = 0; repeat < 200; ++repeat) {
        auto race = TaskLocalScopeV1::open(actor_a);
        TaskLocalKeyV1<int> k;
        std::atomic<bool> go{false};
        std::thread writer([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int n = 0; n != 400; ++n) {
                if (!race->set(actor_a, k, n)) break;
                (void)race->get_copy(actor_a, k);
            }
        });
        go.store(true, std::memory_order_release);
        assert(race->close());
        writer.join();
        assert(!race->is_open());
        assert(!race->set(actor_a, k, 111));
        assert(!race->get_copy(actor_a, k));
    }
}
} // namespace

int main() {
    test_isolation_and_migration();
    test_exact_drop_and_physical_tls();
    test_reentrant_copy_safe_retirement();
    test_bounds_and_close_race();
    std::puts("LOCAL_CONTEXT_DOMAINS_V1_HOST_PASS");
}
