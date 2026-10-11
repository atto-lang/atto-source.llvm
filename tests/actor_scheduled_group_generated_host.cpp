#include "oreslang/runtime/NativeActorScheduledGroup.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using oreslang::runtime::ActorExclusiveScheduler;
using oreslang::runtime::NativeActorFieldEntrypoints;
using oreslang::runtime::NativeActorScheduledGroup;
using oreslang::runtime::NativeActorScheduledGroup;

extern "C" {
OresActorI64Fields* ores_actor_Counter_init(OresActorI64Heap*, std::uint64_t, std::uint64_t);
void ores_actor_Counter_drop(OresActorI64Heap*, std::uint64_t, std::uint64_t,
                              OresActorI64Fields*);
std::uint64_t ores_actor_Counter_layout_hash();
std::uint32_t ores_actor_Counter_layout_version();
std::int64_t ores_actor_Counter_add(OresActorI64Heap*, std::uint64_t, std::uint64_t,
                                     OresActorI64Fields*, std::int64_t);
}

static void check(bool condition, const char* error) {
    if (!condition) throw std::runtime_error(error);
}

class CarrierPool final {
public:
    CarrierPool() {
        for (int n = 0; n < 4; ++n) {
            carriers_.emplace_back([this] {
                for (;;) {
                    ActorExclusiveScheduler::Task task;
                    {
                        std::unique_lock<std::mutex> guard(mutex_);
                        changed_.wait(guard, [&] { return closing_ || !queue_.empty(); });
                        if (closing_ && queue_.empty()) return;
                        task = std::move(queue_.front());
                        queue_.pop_front();
                        ++active_;
                    }
                    task();
                    {
                        std::lock_guard<std::mutex> guard(mutex_);
                        --active_;
                        if (queue_.empty() && active_ == 0) changed_.notify_all();
                    }
                }
            });
        }
    }
    ~CarrierPool() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            closing_ = true;
        }
        changed_.notify_all();
        for (auto& thread : carriers_) thread.join();
    }
    void submit(ActorExclusiveScheduler::Task task) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closing_) throw std::logic_error("carrier pool closed");
            queue_.push_back(std::move(task));
        }
        changed_.notify_one();
    }
    void drain() {
        std::unique_lock<std::mutex> guard(mutex_);
        if (!changed_.wait_for(guard, 5s,
                              [&] { return queue_.empty() && active_ == 0; }))
            throw std::runtime_error("native carrier pool failed to drain");
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<ActorExclusiveScheduler::Task> queue_;
    std::vector<std::thread> carriers_;
    int active_ = 0;
    bool closing_ = false;
};

static std::int64_t slow_add(OresActorI64Heap* h, std::uint64_t d,
                              std::uint64_t e, OresActorI64Fields* f,
                              std::int64_t arg) {
    std::this_thread::sleep_for(30ms);
    return ores_actor_Counter_add(h, d, e, f, arg);
}

int main() {
    try {
        CarrierPool carriers;
        auto scheduled = NativeActorScheduledGroup::create(
            9123, 3, 2, [&](auto task) { carriers.submit(std::move(task)); });
        const NativeActorFieldEntrypoints descriptor{
            &ores_actor_Counter_init, &ores_actor_Counter_drop,
            &ores_actor_Counter_layout_hash, &ores_actor_Counter_layout_version,
            2, UINT64_C(0xb2d57409773a17ed)
        };
        std::mutex mutex;
        std::condition_variable changed;
        int ready_count = 0, result_count = 0, done_count = 0;
        int cancelled = 0;
        std::vector<std::int64_t> answers;
        std::vector<std::string> done_errors;
        std::atomic<int> active_parent{0}, overlap{0};

        auto on_parent = [&] {
            if (active_parent.fetch_add(1) != 0) ++overlap;
            std::this_thread::sleep_for(1ms);
            active_parent.fetch_sub(1);
        };
        auto ready = [&](const auto& result) {
            on_parent();
            if (!result.ready) ++overlap;
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++ready_count;
            }
            changed.notify_all();
        };
        auto result = [&](std::optional<std::int64_t> value,
                          const std::string& error) {
            on_parent();
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (value) answers.push_back(*value);
                else if (!error.empty()) ++cancelled;
                else ++overlap;
                ++result_count;
            }
            changed.notify_all();
        };
        auto done = [&](const std::string& error) {
            on_parent();
            {
                std::lock_guard<std::mutex> lock(mutex);
                done_errors.push_back(error);
                ++done_count;
            }
            changed.notify_all();
        };
        auto until = [&](auto predicate) {
            std::unique_lock<std::mutex> lock(mutex);
            if (!changed.wait_for(lock, 5s, predicate))
                throw std::runtime_error("multi-actor parent CONTROL callback timed out");
        };

        auto a = scheduled->spawn(101, 1, 8, descriptor);
        auto b = scheduled->spawn(102, 1, 8, descriptor);
        scheduled->on_ready(a, ready);
        scheduled->on_ready(b, ready);
        scheduled->on_done(a, done);
        scheduled->on_done(b, done);
        until([&] { return ready_count == 2; });
        check(a->ref.get_ready().wait_for(0ms) == std::future_status::ready,
              "actor A readiness missing");
        check(b->ref.get_ready().wait_for(0ms) == std::future_status::ready,
              "actor B readiness missing");

        // Distinct actors have separate heaps yet all parent callbacks must
        // be serialized by the ONE supervisor CONTROL logical lane.
        check(scheduled->invoke_i64(a, &ores_actor_Counter_add, 20, result),
              "actor A first call rejected");
        check(scheduled->invoke_i64(b, &ores_actor_Counter_add, 30, result),
              "actor B first call rejected");
        until([&] { return result_count == 2; });
        check(scheduled->invoke_i64(a, &ores_actor_Counter_add, 3, result),
              "actor A second call rejected");
        check(scheduled->invoke_i64(b, &ores_actor_Counter_add, 2, result),
              "actor B second call rejected");
        until([&] { return result_count == 4; });
        std::sort(answers.begin(), answers.end());
        check(answers == std::vector<std::int64_t>({32, 35, 42, 44}),
              "two actor heaps did not persist and isolate their fields");
        scheduled->stop(a);
        scheduled->stop(b);
        until([&] { return done_count == 2; });
        check(done_errors[0].empty() && done_errors[1].empty(),
              "normal termination did not settle done successfully");
        check(a->heap_owner->physically_finalized()
              && b->heap_owner->physically_finalized(),
              "normal done completed before native physical cleanup");

        // A busy child reserves one lane only. Its queued DATA is cancelled
        // and each callback is settled once on parent CONTROL, never inline.
        auto c = scheduled->spawn(103, 1, 8, descriptor);
        scheduled->on_ready(c, ready);
        scheduled->on_done(c, done);
        until([&] { return ready_count == 3; });
        int accepted = 0;
        for (int n = 0; n < 4; ++n) {
            if (scheduled->invoke_i64(c, &slow_add, n, result)) ++accepted;
        }
        check(accepted >= 1 && accepted <= 3,
              "bounded DATA mailbox admission was not enforced");
        scheduled->cancel(c);
        until([&] { return result_count == 8 && done_count == 3; });
        check(!done_errors[2].empty(), "cancelled actor resolved successful done");
        check(c->heap_owner->physically_finalized(),
              "cancelled heap not physically reclaimed");
        check(cancelled >= 1, "cancelled queued/overflow call lost notification");
        check(overlap == 0, "parent callbacks overlapped or reported invalid result");
        check(scheduled->finalized_count() == 3,
              "parent CONTROL failed to acknowledge all three native heaps");
        carriers.drain();
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "multi-actor scheduler host: %s\n", ex.what());
        return 87;
    }
}
