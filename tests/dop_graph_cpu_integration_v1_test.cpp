#include "oreslang/runtime/DopGraphCpuIntegrationV1.hpp"
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace oreslang::dop;
namespace graph = oreslang::execution_v1;

static unsigned checks = 0;
static void require(bool ok, const char* reason) {
    ++checks;
    if (!ok) throw std::runtime_error(reason);
}
template <class Fn> static void reject(Fn&& action, const char* reason) {
    ++checks;
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error(reason);
}

static graph::Plan answer() {
    graph::Builder builder;
    auto a = builder.cpu_i64(20);
    auto b = builder.cpu_i64(22);
    return builder.build(builder.cpu_add(a, b));
}

int main() {
    for (auto kind : {Layout::AoS, Layout::SoA, Layout::AoSoA}) {
        const Plan layout(kind, 7, 3, 4, 16, kind == Layout::AoSoA ? 4 : 1);
        const Kernel kernel(layout, Element::F32, Op::ScaleBias, 2, 1);
        auto input = std::make_shared<HostBuffer>(layout);
        auto output = std::make_shared<HostBuffer>(layout);
        for (std::uint64_t row = 0; row < 7; ++row)
            for (std::uint32_t field = 0; field < 3; ++field)
                input->write(row, field, static_cast<float>(row * 3 + field));
        const auto sourceRoot = input->region_root();
        const auto outputRoot = output->region_root();
        const RegionAccess source(sourceRoot, 1, 2, 3, RegionMode::Read);
        const RegionAccess destination(outputRoot, 1, 2, 3, RegionMode::Write);
        std::function<void()> queued;
        auto scheduler = [&](std::function<void()> f) { queued = std::move(f); };

        auto cancelled = submit_owned_cpu_graph(kernel, input, output,
            source, destination, answer(), scheduler);
        require(!cancelled.ready() && static_cast<bool>(queued), "missing pending job");
        require(cancelled.cancel(), "preclaim cancellation must settle");
        queued();
        reject([&] { (void) cancelled.get(); }, "cancelled job published result");
        require(output->read(2, 1) == 0, "cancelled job wrote destination");

        auto successful = submit_owned_cpu_graph(kernel, input, output,
            source, destination, answer(), scheduler);
        require(!successful.ready(), "job ran before dispatch");
        queued();
        queued(); // Erroneous executor duplicate must not repeat kernel work.
        require(successful.get() == 42, "typed i64 graph result mismatch");
        require(!successful.cancel(), "completed future accepted cancellation");
        for (std::uint64_t row = 0; row < 7; ++row)
            for (std::uint32_t field = 0; field < 3; ++field) {
                float expected = row >= 2 && row < 5 && field == 1
                    ? static_cast<float>(2 * (3 * row + 1) + 1) : 0.0f;
                require(output->read(row, field) == expected,
                    "owned f32 row/field execution incorrect");
            }

        graph::Builder gpu;
        const auto host = gpu.cpu_i64(5);
        const auto device = gpu.upload_i64(host);
        queued = nullptr;
        auto forbidden = submit_owned_cpu_graph(kernel, input, output,
            source, destination, gpu.build(gpu.download_i64(device)), scheduler);
        require(!queued, "unsupported GPU graph reached executor");
        reject([&] { (void) forbidden.get(); }, "GPU graph admitted CPU fallback");
        reject([&] { (void) submit_owned_cpu_graph(
            kernel, input, output,
            RegionAccess(RegionRoot::trusted_create(7, 3), 1, 2, 3, RegionMode::Read),
            destination, answer(), scheduler).get();
        }, "forged planner root was accepted");

        // Physical borrow failure must settle the future and preserve output.
        {
            auto loan = output->borrow_read();
            auto borrowed = submit_owned_cpu_graph(kernel, input, output,
                source, destination, answer(), scheduler);
            queued();
            reject([&] { (void) borrowed.get(); },
                   "exclusive output admission bypassed a read loan");
            require(loan.read(2, 1) == 15, "failed admission mutated destination");
        }

        // Scheduler can enqueue and then reject; the late delivery MUST NOT run.
        queued = nullptr;
        auto rejected = submit_owned_cpu_graph(kernel, input, output,
            source, destination, answer(),
            [&](std::function<void()> f) {
                queued = std::move(f);
                throw std::runtime_error("enqueued and rejected");
            });
        reject([&] { (void) rejected.get(); }, "scheduler rejection was not propagated");
        require(static_cast<bool>(queued), "rejected scheduler did not retain task");
        output->write(2, 1, 99);
        queued();
        require(output->read(2, 1) == 99, "late rejected task mutated buffer");

        // Match the Java/.NET/Forge one-shot scheduling stress intent:
        // two simultaneous deliveries cannot re-run the claimed native kernel.
        for (int trial = 0; trial < 128; ++trial) {
            queued = nullptr;
            auto raced = submit_owned_cpu_graph(kernel, input, output,
                source, destination, answer(), scheduler);
            require(static_cast<bool>(queued), "missing race task");
            std::thread first([&] { queued(); });
            std::thread second([&] { queued(); });
            first.join();
            second.join();
            require(raced.get() == 42, "duplicate native delivery lost result");
        }
        // The worker-claim/cancel race must have a terminal outcome. Once the
        // kernel starts, cancellation is allowed to suppress result publication
        // but must not pretend to undo already committed destination bytes.
        for (int trial = 0; trial < 64; ++trial) {
            queued = nullptr;
            auto raced = submit_owned_cpu_graph(kernel, input, output,
                source, destination, answer(), scheduler);
            std::thread worker([&] { queued(); });
            (void) raced.cancel();
            worker.join();
            require(raced.ready(), "native claim/cancel race remained pending");
            try {
                require(raced.get() == 42, "unexpected completed race result");
            } catch (const graph::Cancellation&) {
                require(raced.ready(), "cancelled task should be terminal");
            }
        }

        // C++ queued work must retain native owners, not dangling references.
        queued = nullptr;
        auto retained = submit_owned_cpu_graph(kernel, input, output,
            source, destination, answer(), scheduler);
        std::weak_ptr<HostBuffer> weakInput = input;
        std::weak_ptr<HostBuffer> weakOutput = output;
        input.reset();
        output.reset();
        require(!weakInput.expired() && !weakOutput.expired(),
                "queued closure released native owners prematurely");
        queued();
        require(retained.get() == 42, "queued job failed after caller released owners");
        queued = nullptr;
        require(weakInput.expired() && weakOutput.expired(),
                "completed closure unexpectedly retained native owners");
    }
    std::cout << "PASS " << checks
              << " native owned-F32 + typed-I64 host graph assertions\n";
}
