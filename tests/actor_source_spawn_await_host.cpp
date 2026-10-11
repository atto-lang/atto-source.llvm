#include "oreslang/runtime/ActorExclusiveScheduler.hpp"
#include "oreslang/runtime/ActorGroupOwnedHeap.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::ActorExclusiveScheduler;
using oreslang::runtime::NativeActorFieldEntrypoints;
using oreslang::runtime::NativeActorHeapWorker;
using oreslang::runtime::NativeActorOwnedHeapGroup;
using namespace std::chrono_literals;

using GeneratedI64Method = std::int64_t (*)(
    OresActorI64Heap*, std::uint64_t, std::uint64_t,
    OresActorI64Fields*, std::int64_t);

extern "C" {
int ores_actor_Counter_program_step(void* context, std::uint32_t* state,
                                    std::int64_t* result);
}

class SharedNativeCarrierPool final {
public:
    SharedNativeCarrierPool() {
        for (int i = 0; i < 3; ++i) {
            threads_.emplace_back([this] {
                while (true) {
                    ActorExclusiveScheduler::Task task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        work_cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                        if (stopping_ && queue_.empty()) return;
                        task = std::move(queue_.front());
                        queue_.pop_front();
                        ++active_;
                    }
                    task();
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        --active_;
                        if (queue_.empty() && active_ == 0) idle_cv_.notify_all();
                    }
                }
            });
        }
    }
    ~SharedNativeCarrierPool() {
        drain();
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stopping_ = true;
        }
        work_cv_.notify_all();
        for (auto& thread : threads_) thread.join();
    }
    void execute(ActorExclusiveScheduler::Task task) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (stopping_) throw std::runtime_error("native carrier pool has shut down");
            queue_.push_back(std::move(task));
        }
        work_cv_.notify_one();
    }
    void drain() {
        std::unique_lock<std::mutex> guard(mutex_);
        idle_cv_.wait(guard, [&] { return queue_.empty() && active_ == 0; });
    }
private:
    std::mutex mutex_;
    std::condition_variable work_cv_, idle_cv_;
    std::deque<ActorExclusiveScheduler::Task> queue_;
    std::vector<std::thread> threads_;
    std::size_t active_ = 0;
    bool stopping_ = false;
};

struct HostContext final {
    SharedNativeCarrierPool carriers;
    NativeActorOwnedHeapGroup group{777, 1};
    std::shared_ptr<ActorExclusiveScheduler> parent_lane;
    std::shared_ptr<ActorExclusiveScheduler> actor_lane;
    std::shared_ptr<NativeActorHeapWorker> worker;
    std::mutex mutex;
    std::condition_variable waker;
    std::uint64_t wakes = 0;
    std::atomic<int> call_phase{0};
    std::atomic<std::int64_t> answer{0};
    std::atomic<int> ready_wakes{0};
    std::atomic<int> control_wakes{0};

    HostContext() {
        auto executor = [this](ActorExclusiveScheduler::Task task) {
            carriers.execute(std::move(task));
        };
        parent_lane = std::make_shared<ActorExclusiveScheduler>(4, executor);
        actor_lane = std::make_shared<ActorExclusiveScheduler>(4, executor,
            [this] {
                if (worker) worker->request_panic();
                pulse();
            });
    }
    ~HostContext() {
        group.close();
        carriers.drain();
    }
    void pulse() {
        {
            std::lock_guard<std::mutex> guard(mutex);
            ++wakes;
        }
        waker.notify_all();
    }
    std::uint64_t snapshot() {
        std::lock_guard<std::mutex> guard(mutex);
        return wakes;
    }
    bool await_pulse(std::uint64_t before) {
        std::unique_lock<std::mutex> guard(mutex);
        return waker.wait_for(guard, 4s, [&] { return wakes != before; });
    }
};

extern "C" int ores_actor_host_spawn(
        void* value,
        NativeActorFieldEntrypoints::Init init,
        NativeActorFieldEntrypoints::Drop drop,
        NativeActorFieldEntrypoints::LayoutHash hash,
        NativeActorFieldEntrypoints::LayoutVersion version,
        std::uint32_t field_count, std::uint64_t expected_hash) {
    auto& ctx = *static_cast<HostContext*>(value);
    if (ctx.worker) return -1;
    try {
        ctx.worker = ctx.group.spawn(8, 1, field_count + 4,
            NativeActorFieldEntrypoints{
                init, drop, hash, version, field_count, expected_hash
            },
            [&ctx] {
                // Enqueue-only. Never invoke parent user code or finalizers
                // on the terminating child's physical carrier.
                (void)ctx.parent_lane->post_control([&ctx] {
                    ++ctx.control_wakes;
                    (void)ctx.group.drain_control();
                    ctx.pulse();
                });
                ctx.pulse();
            });
        const auto ref = ctx.worker->ref();
        ref.when_ready([&ctx](const auto&) {
            ++ctx.ready_wakes;
            ctx.pulse(); // caller resumes from parent scheduler, never inline guest code
        });
        if (!ctx.actor_lane->post_data([&ctx] {
            (void)ctx.worker->initialize();
            ctx.pulse();
        })) return -1;
    } catch (...) { return -1; }
    return 1;
}

extern "C" int ores_actor_host_poll_ready(void* value) {
    auto& ctx = *static_cast<HostContext*>(value);
    if (!ctx.worker) return -1;
    auto future = ctx.worker->ref().get_ready();
    if (future.wait_for(0ms) != std::future_status::ready) return 0;
    try { future.get(); } catch (...) { return -1; }
    return 1;
}

extern "C" int ores_actor_host_call_i64(
        void* value, GeneratedI64Method method, std::int64_t argument) {
    auto& ctx = *static_cast<HostContext*>(value);
    if (!ctx.worker || !method) return -1;
    int expected = 0;
    if (!ctx.call_phase.compare_exchange_strong(expected, 1)) return -1;
    if (!ctx.actor_lane->post_data([&ctx, method, argument] {
        bool ok = ctx.worker->with_turn([&](auto* heap, auto turn, auto* fields) {
            if (std::getenv("ORES_ACTOR_TEST_HANDLER_PANIC"))
                throw std::runtime_error("controlled native actor handler panic");
            ctx.answer.store(method(heap, turn.domain, turn.epoch, fields, argument));
        });
        ctx.call_phase.store(ok ? 2 : -1);
        ctx.pulse();
    })) {
        ctx.call_phase.store(0);
        return 0;
    }
    return 1;
}

extern "C" int ores_actor_host_poll_i64(void* value, std::int64_t* result) {
    auto& ctx = *static_cast<HostContext*>(value);
    if (!result) return -1;
    const int status = ctx.call_phase.load();
    if (status == 2) {
        *result = ctx.answer.load();
        return 1;
    }
    return status < 0 ? -1 : 0;
}

extern "C" int ores_actor_host_stop(void* value) {
    auto& ctx = *static_cast<HostContext*>(value);
    if (!ctx.worker) return -1;
    ctx.worker->request_normal_stop();
    return 1;
}

extern "C" int ores_actor_host_cancel(void* value) {
    auto& ctx = *static_cast<HostContext*>(value);
    if (ctx.worker) ctx.worker->request_cancel();
    return 1;
}

extern "C" int ores_actor_host_poll_done(void* value) {
    auto& ctx = *static_cast<HostContext*>(value);
    if (!ctx.worker) return -1;
    const auto future = ctx.worker->ref().get_done();
    if (future.wait_for(0ms) != std::future_status::ready) return 0;
    try { future.get(); } catch (...) { return -1; }
    return 1;
}

int main() {
    try {
        HostContext ctx;
        std::uint32_t pc = 0;
        std::int64_t result = 0;
        bool saw_pending_await = false;
        for (int turn = 0; turn < 48; ++turn) {
            const auto old = pc;
            const auto snapshot = ctx.snapshot();
            const int disposition = ores_actor_Counter_program_step(&ctx, &pc, &result);
            if (disposition == 1) {
                if (pc != 6 || result != 32 || !ctx.worker
                    || !ctx.worker->physically_finalized()
                    || ctx.ready_wakes.load() == 0
                    || ctx.control_wakes.load() == 0)
                    throw std::runtime_error("source actor lifecycle/result contract violated");
                return 32;
            }
            if (disposition < 0) {
                if (!std::getenv("ORES_ACTOR_TEST_HANDLER_PANIC"))
                    throw std::runtime_error("compiled guest continuation failed");
                auto done = ctx.worker->ref().get_done();
                if (done.wait_for(4s) != std::future_status::ready
                    || !ctx.worker->physically_finalized())
                    throw std::runtime_error("panic never finalized persistent heap");
                bool propagated = false;
                try { done.get(); }
                catch (const std::runtime_error&) { propagated = true; }
                if (!propagated)
                    throw std::runtime_error("panic did not fail done observer");
                return 79; // Assert the compiler enqueued cancel through CONTROL.
            }
            if (pc == old) {
                saw_pending_await = true;
                if (!ctx.await_pulse(snapshot))
                    throw std::runtime_error("source await failed to resume");
            }
        }
        throw std::runtime_error(saw_pending_await
            ? "source actor continuation failed to terminate"
            : "source actor never suspended on its futures");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "source spawn/await test: %s\n", ex.what());
        return 79;
    }
}
