#include "oreslang/runtime/GpuDispatchCoordinator.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>

template<class F> void expect_failure(F fn) {
    try { fn(); } catch (const std::exception&) { return; }
    throw std::runtime_error("GPU bridge should have rejected operation");
}
static void check(bool ok) {
    if (!ok) throw std::runtime_error("native GPU dispatch coordinator broken");
}
using namespace oreslang;

struct DeferredProvider final : gpu::GpuDispatchCoordinator::Provider {
    gpu::GpuDispatchCoordinator::Completion event_callback;
    gpu::GpuDispatchCoordinator::Completion release_callback;
    int launched = 0, released = 0;
    void launch(const dop::OpenClArtifact&, const gpu::Identity&, gpu::Ticket,
                gpu::GpuDispatchCoordinator::Completion done) override {
        ++launched;
        event_callback = std::move(done);
    }
    void release(const gpu::Identity&, std::uint64_t,
                 gpu::GpuDispatchCoordinator::Completion done) override {
        ++released;
        release_callback = std::move(done);
    }
};

static dop::OpenClArtifact kernel() {
    dop::Plan plan(dop::Layout::AoSoA, 7, 3, 4, 16, 4);
    return dop::lower_opencl(
        dop::Kernel(plan, dop::Element::F32, dop::Op::Scale, 2.0f, 0.0f),
        {64, 4096, 100, true});
}

int main() {
    const gpu::Identity id{"tenant", "device", 17};
    auto p = std::make_shared<DeferredProvider>();
    const auto reservation = kernel().required_bytes * 2;
    gpu::GpuDispatchCoordinator service(p, {1, reservation});
    auto job = service.submit(kernel(), id);
    job.get_ready().get();
    check(service.in_flight_frames() == 1 && service.reserved_bytes() == reservation);
    expect_failure([&] { (void)service.submit(kernel(), id); });
    expect_failure([&] { service.close(); });
    p->event_callback(nullptr);
    check(p->released == 1 && service.reserved_bytes() == reservation);
    check(job.get_done().wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
    p->release_callback(nullptr);
    job.get_done().get();
    check(service.reserved_bytes() == 0 && service.in_flight_frames() == 0);
    service.close();

    auto cancelled_provider = std::make_shared<DeferredProvider>();
    gpu::GpuDispatchCoordinator cancelled(cancelled_provider, {1, 4096});
    auto cancelled_job = cancelled.submit(kernel(), id);
    check(cancelled_job.cancel());
    check(!cancelled_job.cancel());
    expect_failure([&] { (void)cancelled.submit(kernel(), id); });
    cancelled_provider->event_callback(nullptr);
    check(cancelled.reserved_bytes() > 0);
    cancelled_provider->release_callback(nullptr);
    expect_failure([&] { cancelled_job.get_done().get(); });
    check(cancelled.reserved_bytes() == 0);
    cancelled.close();

    auto failed_provider = std::make_shared<DeferredProvider>();
    gpu::GpuDispatchCoordinator failed(failed_provider, {1, 4096});
    auto failed_job = failed.submit(kernel(), id);
    failed_provider->event_callback(nullptr);
    failed_provider->release_callback(
        std::make_exception_ptr(std::runtime_error("device not released")));
    expect_failure([&] { failed_job.get_done().get(); });
    check(failed.reserved_bytes() > 0);
    expect_failure([&] { failed.close(); });

    std::cout << "native GPU bounded async dispatch and quiescent release PASS\n";
}
