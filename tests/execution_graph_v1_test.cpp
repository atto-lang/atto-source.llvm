#include "oreslang/runtime/ExecutionGraphV1.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::execution_v1;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <class F> void expect_fail(F&& fn, const char* required) {
    try { fn(); }
    catch (const std::exception& ex) {
        if (std::string(ex.what()).find(required) == std::string::npos)
            throw std::runtime_error(std::string("wrong diagnostic: ") + ex.what());
        return;
    }
    throw std::runtime_error("operation unexpectedly succeeded");
}

int main() {
    try {
        Builder empty;
        auto unit = empty.build(empty.unit());
        require(unit.cpu_only(), "empty CPU plan must be CPU only");
        require(std::holds_alternative<std::monostate>(unit.execute_cpu()), "Unit singleton");

        Builder cpu;
        const auto a = cpu.cpu_i64(20);
        const auto b = cpu.cpu_i64(22);
        auto sum = cpu.cpu_add(a, b);
        auto plan = cpu.build(sum);
        require(plan.result_type() == Kind::HostI64, "typed i64 output");
        require(std::get<std::int64_t>(plan.execute_cpu()) == 42, "CPU i64 execution");
        require(plan.nodes().size() == 3, "full DAG");

        std::vector<std::function<void()>> queue;
        auto future = plan.submit_cpu([&](std::function<void()> f) { queue.push_back(std::move(f)); });
        require(!future.ready() && queue.size() == 1, "one nonblocking CPU task");
        queue.front()();
        require(std::get<std::int64_t>(future.get()) == 42, "future completed after CPU work");
        require(!future.cancel(), "settled future must not cancel");

        queue.clear();
        auto cancelled = plan.submit_cpu([&](std::function<void()> f) { queue.push_back(std::move(f)); });
        require(cancelled.cancel(), "cancel pending task");
        queue.front()();
        expect_fail([&] { (void) cancelled.get(); }, "cancelled");

        Builder gpu;
        auto x = gpu.cpu_i64(2);
        auto y = gpu.cpu_i64(3);
        auto dx = gpu.upload_i64(x);
        auto dy = gpu.upload_i64(y);
        auto deviceResult = gpu.gpu_add_i64(dx, dy);
        auto hostResult = gpu.download_i64(deviceResult);
        auto gpuPlan = gpu.build(hostResult);
        require(!gpuPlan.cpu_only(), "GPU transfer requires device");
        require(gpuPlan.nodes().at(2).result == Kind::DeviceI64, "typed device result");
        expect_fail([&] { (void) gpuPlan.execute_cpu(); }, "no implicit CPU fallback");
        bool enqueued = false;
        auto failed = gpuPlan.submit_cpu([&](std::function<void()>) { enqueued = true; });
        require(!enqueued, "GPU rejection must precede any host execution");
        expect_fail([&] { (void) failed.get(); }, "no implicit CPU fallback");

        Builder one;
        Builder other;
        auto own = one.cpu_i64(1);
        auto foreign = other.cpu_i64(2);
        expect_fail([&] { (void) one.cpu_add(own, foreign); }, "graph ownership");
        expect_fail([&] { (void) one.upload_i64(foreign); }, "graph ownership");
        one.build(own);
        expect_fail([&] { (void) one.cpu_i64(4); }, "sealed");

        Builder overflow;
        auto biggest = overflow.cpu_i64(std::numeric_limits<std::int64_t>::max());
        auto plus = overflow.cpu_i64(1);
        auto over = overflow.build(overflow.cpu_add(biggest, plus));
        expect_fail([&] { (void) over.execute_cpu(); }, "overflow");

        Builder budget;
        for (int i = 0; i < 4096; ++i) budget.unit();
        expect_fail([&] { (void) budget.unit(); }, "node budget");

        auto executorRejected = plan.submit_cpu([](std::function<void()>) {
            throw std::runtime_error("trusted executor budget exceeded");
        });
        expect_fail([&] { (void) executorRejected.get(); }, "executor budget");

        auto inlineRejected = over.submit_cpu([](std::function<void()> task) {
            task();
        });
        expect_fail([&] { (void) inlineRejected.get(); }, "queued executor");

        auto inlineThenThrow = plan.submit_cpu([](std::function<void()> task) {
            task();
            throw std::runtime_error("inline then rejected");
        });
        expect_fail([&] { (void) inlineThenThrow.get(); }, "inline then rejected");

        // Admission failures can occur after the external scheduler retained
        // the closure. That rejected task must be inert if delivered later.
        std::function<void()> orphan;
        auto rejectedAfterQueue = plan.submit_cpu([&](std::function<void()> task) {
            orphan = std::move(task);
            throw std::runtime_error("executor rejected after enqueue");
        });
        require(static_cast<bool>(orphan), "missing late-rejected task");
        expect_fail([&] { (void) rejectedAfterQueue.get(); }, "rejected after enqueue");
        orphan();
        expect_fail([&] { (void) rejectedAfterQueue.get(); }, "rejected after enqueue");

        // Cancellation vs actual execution can race. Exactly one terminal state
        // must prevail. No unhandled exception or double promise settlement.
        for (int i = 0; i < 64; ++i) {
            std::function<void()> task;
            auto racing = plan.submit_cpu([&](std::function<void()> t) { task = std::move(t); });
            std::thread worker([task] { task(); });
            (void) racing.cancel();
            worker.join();
            try {
                require(std::get<std::int64_t>(racing.get()) == 42, "race result");
            } catch (const Cancellation&) {
                require(racing.ready(), "racing cancellation settled");
            }
        }
        // The executor is an external trust boundary: even if it mistakenly
        // dispatches the same one-shot task twice on competing threads, there
        // must be exactly one permitted evaluation/terminal publication.
        for (int i = 0; i < 128; ++i) {
            std::function<void()> duplicate;
            auto single = plan.submit_cpu(
                [&](std::function<void()> task) { duplicate = std::move(task); });
            require(static_cast<bool>(duplicate), "executor dropped duplicate test job");
            std::thread first(duplicate);
            std::thread second(duplicate);
            first.join();
            second.join();
            require(single.ready() &&
                    std::get<std::int64_t>(single.get()) == 42,
                    "duplicate executor dispatch broke one-shot result");
        }

        std::cout << "typed native CPU/GPU graph host contract: PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "typed native graph failure: " << ex.what() << '\n';
        return 1;
    }
}
