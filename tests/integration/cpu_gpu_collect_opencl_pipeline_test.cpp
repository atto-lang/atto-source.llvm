#include "oreslang/runtime/ActorGroupLifecycle.hpp" // authoritative NativeActorGroup on current main
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/PipelineSourceLowering.hpp"
#include "oreslang/runtime/BoundedActorPort.hpp" // current-main CONTROL/port PR #97
#include "oreslang/runtime/GpuActorControlBridge.hpp"
#include "oreslang/runtime/OpenClF32Device.hpp"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG // Release run must exercise every pipeline check
#endif
#include <cassert>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace oreslang;
using Group = runtime::NativeActorGroup;
using Bridge = gpu::GpuActorControlBridge<Group>;

struct CpuBatch {
    std::vector<std::uint8_t> bytes;
    explicit CpuBatch(std::vector<std::uint8_t> b) : bytes(std::move(b)) {}
    CpuBatch(CpuBatch&&) = default;
    CpuBatch& operator=(CpuBatch&&) = default;
    CpuBatch(const CpuBatch&) = delete;
    CpuBatch& operator=(const CpuBatch&) = delete;
};
struct GpuBatch {
    std::vector<std::uint8_t> bytes;
    explicit GpuBatch(std::vector<std::uint8_t> b) : bytes(std::move(b)) {}
    GpuBatch(GpuBatch&&) = default;
    GpuBatch& operator=(GpuBatch&&) = default;
    GpuBatch(const GpuBatch&) = delete;
    GpuBatch& operator=(const GpuBatch&) = delete;
};

using CpuDelivery = runtime::BoundedActorPort<CpuBatch>::Delivery;

struct RealOpenClProvider : gpu::GpuDispatchCoordinator::Provider {
    std::shared_ptr<std::optional<CpuDelivery>> owned_input;
    std::vector<std::uint8_t> device_result;
    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> device_event{false};

    explicit RealOpenClProvider(std::shared_ptr<std::optional<CpuDelivery>> input)
        : owned_input(std::move(input)) {}
    ~RealOpenClProvider() override {
        if (worker.joinable()) worker.join();
    }

    void launch(const dop::OpenClArtifact& kernel,
                const gpu::Identity&, gpu::Ticket,
                gpu::GpuDispatchCoordinator::Completion completion) override {
        if (!owned_input || !owned_input->has_value())
            throw std::runtime_error("GPU worker has no owned CPU input frame");
        worker = std::thread([this, kernel, completion = std::move(completion)]() mutable {
            try {
                gpu::OpenClDeviceSelector selected;
                selected.required_type = CL_DEVICE_TYPE_CPU; // EXPLICIT test device
                auto result = gpu::execute_opencl_f32(
                    kernel, owned_input->value().value().bytes, selected);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    device_result = std::move(result.output);
                }
                device_event.store(true);
                completion(nullptr);
            } catch (...) {
                completion(std::current_exception());
            }
        });
    }
    void release(const gpu::Identity&, std::uint64_t,
                 gpu::GpuDispatchCoordinator::Completion completion) override {
        // The concrete executor has already completed all OpenCL commands,
        // readback and object release before invoking the device event.
        if (!device_event.load()) {
            completion(std::make_exception_ptr(std::runtime_error("device did not quiesce")));
            return;
        }
        assert(owned_input->has_value());
        assert(owned_input->value().acknowledge_released());
        owned_input->reset();
        completion(nullptr);
    }
};

int main(int argc, char** argv) {
    if (argc != 2) throw std::invalid_argument("expected source .ores pipeline path");
    std::ifstream source_file(argv[1]);
    if (!source_file) throw std::runtime_error("cannot read pipeline source");
    const std::string source((std::istreambuf_iterator<char>(source_file)),
                             std::istreambuf_iterator<char>());
    dop::Plan layout(dop::Layout::AoSoA, 7, 3, 4, 16, 4);
    const auto checked = compile_pipeline_source(
        source, layout, {64, 4096, 100, true});
    if (checked.entry != "do_work"
        || checked.stages[0].name != "CpuWorker"
        || checked.stages[1].name != "GpuWorker"
        || checked.stages[2].name != "CollectResultsWorker")
        throw std::runtime_error("invalid source-derived actor stage identity");
    const auto& kernel = checked.kernel;
    const auto bytes = static_cast<std::size_t>(kernel.required_bytes);
    auto group = std::make_shared<Group>(1004, 3);
    auto cpu = group->adopt(1004, 1, 1, [] {});
    auto gpu_actor = group->adopt(1004, 2, 1, [] {});
    auto collect = group->adopt(1004, 3, 1, [] {});
    assert(group->mark_ready(cpu));
    assert(group->mark_ready(gpu_actor));
    assert(group->mark_ready(collect));
    cpu.get_ready().get();
    gpu_actor.get_ready().get();
    collect.get_ready().get();

    runtime::BoundedActorPort<CpuBatch> to_gpu(1, bytes);
    runtime::BoundedActorPort<GpuBatch> to_collect(1, bytes);
    std::atomic<int> cpu_turns{0}, gpu_turns{0}, collected{0};
    assert(group->run_data_turn(cpu, [&] {
        std::vector<std::uint8_t> input(bytes, 0);
        for (std::uint64_t row = 0; row < layout.rows(); ++row)
            for (std::uint32_t field = 0; field < layout.fields(); ++field) {
                float value = static_cast<float>(row * layout.fields() + field) / 2.0f;
                const auto off = static_cast<std::size_t>(layout.byte_offset(row, field));
                std::memcpy(input.data() + off, &value, sizeof(value));
            }
        assert(to_gpu.offer(CpuBatch(std::move(input)), bytes));
        ++cpu_turns;
    }) == Group::DataTurn::executed);
    assert(cpu_turns == 1 && to_gpu.used_frames() == 1);
    CpuBatch rejected(std::vector<std::uint8_t>(bytes, 0));
    assert(!to_gpu.offer(std::move(rejected), bytes)); // bounded frame/bytes
    assert(rejected.bytes.size() == bytes); // rejection retains ownership

    auto cpu_input = std::make_shared<std::optional<CpuDelivery>>();
    auto driver = std::make_shared<RealOpenClProvider>(cpu_input);
    gpu::GpuDispatchCoordinator coordinator(driver, {1, bytes * 2});
    std::shared_ptr<Bridge::Receipt> route;
    std::optional<gpu::GpuDispatchCoordinator::Submission> submission;
    const gpu::Identity identity{"tenant", "OpenCL-CPU-explicit", 17};
    assert(group->run_port_turn(gpu_actor, to_gpu, [&](auto& delivery) {
        *cpu_input = std::move(delivery);
        assert(cpu_input->has_value());
        assert(to_gpu.used_frames() == 1); // held while GPU device uses input
        submission.emplace(coordinator.submit(kernel, identity));
        route = Bridge::attach(*submission, group, gpu_actor, [&](std::exception_ptr failure) {
            if (failure) std::rethrow_exception(failure);
            std::vector<std::uint8_t> result;
            {
                std::lock_guard<std::mutex> lock(driver->mutex);
                result = driver->device_result;
            }
            assert(to_collect.offer(GpuBatch(std::move(result)), bytes));
            ++gpu_turns;
        });
    }) == Group::DataTurn::executed);
    assert(submission.has_value());
    submission->get_ready().get();
    submission->get_done().get(); // real OpenCL event and physical release
    if (driver->worker.joinable()) driver->worker.join();
    assert(to_gpu.used_frames() == 0);
    assert(gpu_turns == 0 && collected == 0); // no callback executes actor code
    assert(route->route.load() == Bridge::Route::posted);
    assert(group->run_control_turn(gpu_actor) == Group::ControlTurn::executed);
    assert(gpu_turns == 1 && to_collect.used_frames() == 1 && collected == 0);
    // The collector processes ordinary DATA. GPU completion CONTROL only
    // publishes the bounded output; no CONTROL callback runs collector code.
    assert(group->run_port_turn(collect, to_collect, [&](auto& final) {
                for (std::uint64_t row = 0; row < layout.rows(); ++row)
                    for (std::uint32_t field = 0; field < layout.fields(); ++field) {
                        float actual = 0;
                        const auto off = static_cast<std::size_t>(layout.byte_offset(row, field));
                        std::memcpy(&actual, final.value().bytes.data() + off, sizeof(actual));
                        const float expected = static_cast<float>(
                            row * layout.fields() + field) + 1.0f;
                        assert(actual == expected);
                    }
                assert(final.acknowledge_released());
                ++collected;

    }) == Group::DataTurn::executed);
    assert(collected == 1 && to_collect.used_frames() == 0);
    assert(group->finalized(cpu));
    assert(group->finalized(gpu_actor));
    assert(group->finalized(collect));
    cpu.get_done().get();
    gpu_actor.get_done().get();
    collect.get_done().get();
    to_gpu.close();
    to_collect.close();
    coordinator.close();
    group->close();
}
