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
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using oreslang::runtime::ActorExclusiveScheduler;
using oreslang::runtime::NativeActorFieldEntrypoints;
using oreslang::runtime::NativeActorScheduledGroup;

extern "C" {
OresActorI64Fields* ores_actor_Counter_init(OresActorI64Heap*, std::uint64_t,
                                             std::uint64_t);
void ores_actor_Counter_drop(OresActorI64Heap*, std::uint64_t, std::uint64_t,
                              OresActorI64Fields*);
std::uint64_t ores_actor_Counter_layout_hash();
std::uint32_t ores_actor_Counter_layout_version();
std::uint64_t ores_actor_Counter_message_schema_hash();
std::int64_t ores_actor_Counter_receive(OresActorI64Heap*, std::uint64_t,
    std::uint64_t, OresActorI64Fields*, std::uint32_t, std::int64_t);
}
static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

class CarrierPool final {
public:
    CarrierPool() {
        for (int i = 0; i < 4; ++i) {
            carriers_.emplace_back([this] {
                for (;;) {
                    ActorExclusiveScheduler::Task task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        ready_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                        if (stop_ && queue_.empty()) return;
                        task = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    task();
                }
            });
        }
    }
    ~CarrierPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        ready_.notify_all();
        for (auto& t : carriers_) t.join();
    }
    void submit(ActorExclusiveScheduler::Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) throw std::runtime_error("pool already closed");
            queue_.push_back(std::move(task));
        }
        ready_.notify_one();
    }
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<ActorExclusiveScheduler::Task> queue_;
    std::vector<std::thread> carriers_;
    bool stop_ = false;
};

int main() {
    try {
        CarrierPool pool;
        auto group = NativeActorScheduledGroup::create(
            8271, 2, 3, [&](auto task) { pool.submit(std::move(task)); });
        const NativeActorFieldEntrypoints fields{
            &ores_actor_Counter_init, &ores_actor_Counter_drop,
            &ores_actor_Counter_layout_hash, &ores_actor_Counter_layout_version,
            2, ores_actor_Counter_layout_hash()
        };
        const auto hash = ores_actor_Counter_message_schema_hash();
        check(hash != 0, "generated union schema identity is missing");
        const NativeActorScheduledGroup::MailContract accepted{hash, 2};

        auto a = group->spawn(51, 1, 8, fields, accepted);
        auto b = group->spawn(52, 1, 8, fields, accepted);

        std::mutex mutex;
        std::condition_variable changed;
        std::vector<std::pair<int, std::int64_t>> replies;
        std::vector<std::string> errors;
        int ready = 0, done = 0;
        std::atomic<int> parent_inflight{0}, overlap{0};
        auto on_parent = [&] {
            if (parent_inflight.fetch_add(1) != 0) ++overlap;
            parent_inflight.fetch_sub(1);
        };
        auto when = [&](auto predicate) {
            std::unique_lock<std::mutex> lock(mutex);
            if (!changed.wait_for(lock, 5s, predicate))
                throw std::runtime_error("typed union supervisor CONTROL stalled");
        };
        auto ready_callback = [&](const auto& ticket) {
            on_parent();
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!ticket.ready) errors.emplace_back("actor failed readiness");
                ++ready;
            }
            changed.notify_all();
        };
        auto done_callback = [&](const std::string& error) {
            on_parent();
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!error.empty()) errors.push_back(error);
                ++done;
            }
            changed.notify_all();
        };
        group->on_ready(a, ready_callback);
        group->on_ready(b, ready_callback);
        group->on_done(a, done_callback);
        group->on_done(b, done_callback);
        when([&] { return ready == 2; });

        auto send = [&](int which, std::uint64_t schema,
                        std::uint32_t variant, std::int64_t amount) {
            auto child = which == 0 ? a : b;
            return group->send_tagged_i64(child, &ores_actor_Counter_receive,
                {schema, variant, amount},
                [&, which](std::optional<std::int64_t> result,
                           const std::string& error) {
                    on_parent();
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (result) replies.emplace_back(which, *result);
                        else errors.push_back(error);
                    }
                    changed.notify_all();
                });
        };
        check(send(0, hash, 0, 20), "actor A delta rejected");
        check(send(1, hash, 0, 30), "actor B delta rejected");
        when([&] { return replies.size() == 2; });
        check(send(0, hash, 0, 4), "second actor A delta rejected");
        check(send(1, hash, 1, 7), "actor B reset rejected");
        when([&] { return replies.size() == 4; });
        check(send(0, hash, 1, 0), "actor A reset rejected");
        check(send(1, hash, 0, 2), "actor B delta after reset rejected");
        when([&] { return replies.size() == 6; });

        // Internal tag ordinals, not nominal enum wire tag values:
        // both actors have isolated persistent native field records.
        // Cross-actor CONTROL callback order is nondeterministic, while
        // each actor's own turns must preserve causal message order.
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::vector<std::int64_t> from_a, from_b;
            for (const auto& entry : replies) {
                (entry.first == 0 ? from_a : from_b).push_back(entry.second);
            }
            check(from_a == std::vector<std::int64_t>({20, 24, 0}),
                  "actor A typed messages were not serialized");
            check(from_b == std::vector<std::int64_t>({30, 7, 9}),
                  "actor B typed messages were not serialized");
        }
        check(!send(0, hash ^ 1, 0, 999), "forged schema was admitted");
        check(!send(1, hash, 99, 999), "out-of-range case index was admitted");
        when([&] { return errors.size() == 2; });
        check(send(0, hash, 0, 3), "valid mail rejected after a forged envelope");
        when([&] { return replies.size() == 7; });
        {
            std::lock_guard<std::mutex> lock(mutex);
            check(replies.back() == std::make_pair(0, std::int64_t{3}),
                  "rejected message mutated a native actor heap");
        }

        group->stop(a);
        group->stop(b);
        when([&] { return done == 2; });
        check(a->heap_owner->physically_finalized()
              && b->heap_owner->physically_finalized(),
              "typed union get_done resolved ahead of physical heap retirement");
        check(group->finalized_count() == 2,
              "typed union supervisor CONTROL did not finalize both actors");
        check(overlap == 0, "parent CONTROL callbacks overlapped");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "typed union native execution: %s\n", ex.what());
        return 92;
    }
}
