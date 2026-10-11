#include "oreslang/runtime/ActorExclusiveScheduler.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::ActorExclusiveScheduler;
using namespace std::chrono_literals;

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class TwoCarrierPool {
public:
    TwoCarrierPool() {
        for (int i = 0; i < 2; ++i) {
            carriers_.emplace_back([this] {
                while (true) {
                    ActorExclusiveScheduler::Task task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cv_.wait(lock, [&] { return shutdown_ || !pending_.empty(); });
                        if (shutdown_ && pending_.empty()) return;
                        task = std::move(pending_.front());
                        pending_.pop_front();
                    }
                    task();
                }
            });
        }
    }

    ~TwoCarrierPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
        for (auto& t : carriers_) t.join();
    }

    void enqueue(ActorExclusiveScheduler::Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_) throw std::logic_error("executor shut down");
            pending_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_ = false;
    std::deque<ActorExclusiveScheduler::Task> pending_;
    std::vector<std::thread> carriers_;
};

int main() {
    TwoCarrierPool carriers;
    std::atomic<int> active{0}, maximum{0}, completed{0}, errors{0};
    auto executor = [&](ActorExclusiveScheduler::Task task) { carriers.enqueue(std::move(task)); };
    auto lane = std::make_shared<ActorExclusiveScheduler>(2, executor, [&] { ++errors; });
    auto parent = std::make_shared<ActorExclusiveScheduler>(1, executor);
    std::promise<void> started, unblock, first_finished, control_finished;
    auto release = unblock.get_future().share();
    auto initial = started.get_future();
    auto done = first_finished.get_future();
    auto ctl_done = control_finished.get_future();

    check(lane->post_data([&] {
        int n = active.fetch_add(1) + 1;
        maximum.store(std::max(maximum.load(), n));
        started.set_value();
        release.wait(); // A deliberately uncooperative handler occupies ONE carrier.
        active.fetch_sub(1);
        ++completed;
        first_finished.set_value();
    }), "initial data admission failed");
    check(initial.wait_for(5s) == std::future_status::ready, "first actor handler never began");
    check(lane->post_data([&] {
        int n = active.fetch_add(1) + 1;
        maximum.store(std::max(maximum.load(), n));
        active.fetch_sub(1);
        ++completed;
    }), "first bounded mailbox entry failed");
    check(lane->post_data([&] { ++completed; }), "second bounded entry failed");
    check(!lane->post_data([&] { ++completed; }), "saturated mailbox admitted third data");

    // The parent has its own exclusive CONTROL lane; the data backlog must
    // not impede its reserved event, even while a different actor is busy.
    check(parent->post_control([&] { control_finished.set_value(); }),
          "parent CONTROL was rejected by independent data backlog");
    check(ctl_done.wait_for(5s) == std::future_status::ready,
          "parent CONTROL waited behind a busy child carrier");
    check(lane->post_control([&] { ++completed; }),
          "reserved child CONTROL slot unavailable at data capacity");
    check(!lane->post_control([&] { ++completed; }),
          "one-slot reserved CONTROL double-admitted");

    release.wait_for(0ms);
    unblock.set_value();
    check(done.wait_for(5s) == std::future_status::ready,
          "blocked actor did not release its lane");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (completed.load() != 4 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check(completed == 4, "queued actor work or CONTROL was lost");
    check(maximum == 1, "one logical actor executed on two carriers at once");
    check(errors == 0, "user handler unexpectedly failed");
    lane->close();
    check(!lane->post_data([] {}), "closed lane accepted new user data");
    check(lane->finalize(), "quiescent actor scheduler refused terminal fence");
    check(!lane->post_control([] {}), "physically finalized lane accepted CONTROL");
    check(parent->finalize(), "idle parent scheduler refused retirement");

    // User exceptions cannot strand the logical lane; host receives failure
    // signal to enqueue supervisor CONTROL, and the next job runs.
    auto crashing = std::make_shared<ActorExclusiveScheduler>(2, executor, [&] { ++errors; });
    std::promise<void> survived;
    auto survivor = survived.get_future();
    check(crashing->post_data([] { throw std::runtime_error("actor handler panic"); }),
          "failed to enqueue synthetic panic");
    check(crashing->post_data([&] { survived.set_value(); }),
          "failed to enqueue recovery turn");
    check(survivor.wait_for(5s) == std::future_status::ready,
          "exception blocked all later carrier work");
    check(errors == 1, "panic was not delivered to CONTROL callback");
    crashing->close();
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (!crashing->finalize() && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
    check(crashing->finalize(), "panic scheduler never quiesced");
    return 0;
}
