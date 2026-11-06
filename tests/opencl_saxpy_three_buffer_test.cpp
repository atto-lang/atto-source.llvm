#include "oreslang/runtime/AttoSaxpyProvider.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace oreslang::opencl;
void check(bool predicate, const char* message) {
    if (!predicate) throw std::runtime_error(message);
}
template<class F> void reject(F&& f) {
    try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid SAXPY action was admitted");
}

const std::string kSource = R"atto(
compute fnc saxpy(x: DeviceRead<f32>, y: DeviceRead<f32>,
    out: DeviceWrite<f32>, alpha: f32, n: u64) device {
  const i = gpu.global_id.x;
  if i < n { out[i] = alpha * x[i] + y[i]; }
}
)atto";

class FakeGpu final : public Api, public SaxpyHardware {
public:
    struct Buffer { std::vector<unsigned char> bytes; };
    struct Event { EventStatus status; };
    struct Program { std::string source; };

    Api& base() noexcept override { return *this; }
    const std::string& physical_device_name() const noexcept override {
        return device_;
    }

    void validate_artifact(const oreslang::dop::OpenClArtifact& artifact) const override {
        check(artifact.artifact_abi_version == 1
              && artifact.kernel_name == "atto_saxpy_f32"
              && artifact.required_address_bits == 64
              && artifact.required_float32 && artifact.required_little_endian
              && artifact.required_bytes != 0 && artifact.global_rows != 0
              && artifact.global_fields == 1, "typed SAXPY artifact");
    }

    Handle prepare(const oreslang::dop::OpenClArtifact& artifact) override {
        ++prepared_;
        return new Program{artifact.device_source};
    }
    void release_program(Handle handle) override {
        delete static_cast<Program*>(handle);
        ++program_releases_;
    }
    Handle allocate(std::uint64_t bytes) override {
        if (fail_allocate_ && ++allocation_attempts_ == 3)
            throw std::runtime_error("injected third allocation failure");
        ++allocations_;
        return new Buffer{std::vector<unsigned char>(static_cast<std::size_t>(bytes), 0)};
    }
    void release_buffer(Handle handle) override {
        if (fail_release_once_) {
            fail_release_once_ = false;
            throw std::runtime_error("injected physical release failure");
        }
        delete static_cast<Buffer*>(handle);
        ++buffer_releases_;
    }
    Handle enqueue_upload(Handle destination, const unsigned char* bytes,
                          std::size_t size) override {
        auto& storage = static_cast<Buffer*>(destination)->bytes;
        check(storage.size() == size, "upload length");
        std::memcpy(storage.data(), bytes, size);
        ++uploads_;
        return event();
    }
    Handle enqueue_kernel(Handle, Handle, Handle, std::uint64_t,
                          std::uint32_t) override {
        throw std::logic_error("legacy two-buffer kernel ABI is forbidden");
    }
    Handle enqueue_saxpy(Handle program, Handle x, Handle y, Handle output,
                         float alpha, std::uint64_t rows) override {
        if (fail_launch_) {
            throw std::runtime_error("SAXPY enqueue outcome unknown");
        }
        check(static_cast<Program*>(program)->source.find("atto_saxpy_f32")
                  != std::string::npos, "source-compiled kernel");
        auto& xb = static_cast<Buffer*>(x)->bytes;
        auto& yb = static_cast<Buffer*>(y)->bytes;
        auto& ob = static_cast<Buffer*>(output)->bytes;
        check(x != y && x != output && y != output && xb.size() == rows * 4
              && xb.size() == yb.size() && xb.size() == ob.size(),
              "independent three-buffer geometry");
        for (std::uint64_t i = 0; i < rows; ++i) {
            float a = 0, b = 0;
            std::memcpy(&a, xb.data() + i * 4, 4);
            std::memcpy(&b, yb.data() + i * 4, 4);
            volatile float product = alpha * a;
            const float result = product + b;
            std::memcpy(ob.data() + i * 4, &result, 4);
        }
        ++kernels_;
        return event();
    }
    Handle enqueue_download(Handle source, unsigned char* out,
                            std::size_t length) override {
        const auto& bytes = static_cast<Buffer*>(source)->bytes;
        check(bytes.size() == length, "download length");
        std::memcpy(out, bytes.data(), length);
        ++downloads_;
        return event();
    }
    EventStatus poll_event(Handle handle) override {
        return static_cast<Event*>(handle)->status;
    }
    void release_event(Handle handle) override {
        delete static_cast<Event*>(handle);
        ++event_releases_;
    }
    bool prove_device_loss_quiescence() override {
        ++quiescence_checks_;
        return allow_quiescence_;
    }

    void complete_current() { if (last_) last_->status = EventStatus::Complete; }
    void fail_current() { if (last_) last_->status = EventStatus::Failed; }

    bool pending_ = false;
    bool fail_launch_ = false;
    bool fail_allocate_ = false;
    bool fail_release_once_ = false;
    bool allow_quiescence_ = false;
    int prepared_ = 0, program_releases_ = 0;
    int allocations_ = 0, allocation_attempts_ = 0, buffer_releases_ = 0;
    int uploads_ = 0, kernels_ = 0, downloads_ = 0;
    int event_releases_ = 0, quiescence_checks_ = 0;
private:
    Handle event() {
        auto* result = new Event{pending_ ? EventStatus::Pending
                                         : EventStatus::Complete};
        last_ = result;
        return result;
    }
    std::string device_ = "trusted-test-gpu";
    Event* last_ = nullptr; // test-only; never dereferenced after event release
};

SaxpyProvider provider(FakeGpu& api, std::uint64_t bytes = 4096) {
    return SaxpyProvider(api, "tenant-test", api.physical_device_name(), 7,
                         {bytes, bytes, bytes, bytes / 4});
}
void run_to_terminal(SaxpyProvider& p, SaxpyProvider::Operation& op) {
    for (int step = 0; step < 12; ++step) {
        const auto state = p.poll(op);
        if (state == SaxpyProvider::State::Succeeded ||
            state == SaxpyProvider::State::Cancelled ||
            state == SaxpyProvider::State::Failed)
            return;
    }
    throw std::runtime_error("SAXPY did not reach terminal event");
}
}

int main() {
    using S = SaxpyProvider::State;
    // Real source-derived opencl artifact through a three-buffer mock.
    for (const std::size_t n : {1u, 2u, 8u, 128u}) {
        FakeGpu api;
        auto p = provider(api);
        std::vector<float> x(n), y(n);
        for (std::size_t i = 0; i < n; ++i) {
            x[i] = static_cast<float>(i) + 0.25f;
            y[i] = static_cast<float>(i * 2) - 0.5f;
        }
        auto op = p.prepare(kSource, x, y, 1.5f);
        reject([&] { (void)op->result(); });
        reject([&] { (void)p.prepare(kSource, x, y, 1.5f); });
        run_to_terminal(p, *op);
        check(op->state() == S::Succeeded && !p.busy(), "successful GPU event");
        check(api.allocations_ == 3 && api.buffer_releases_ == 3,
              "three owned physical buffers released");
        check(api.prepared_ == 1 && api.program_releases_ == 1,
              "one program lease released");
        check(api.uploads_ == 2 && api.kernels_ == 1 && api.downloads_ == 1
              && api.event_releases_ == 4, "authentic event sequence");
        auto actual = op->result();
        for (std::size_t i = 0; i < n; ++i) {
            volatile float scaled = x[i] * 1.5f;
            const float expected = scaled + y[i];
            std::uint32_t a = 0, b = 0;
            std::memcpy(&a, &actual[i], sizeof(a));
            std::memcpy(&b, &expected, sizeof(b));
            check(a == b, "independent float32 oracle mismatch");
        }
    }

    // Cancellation cannot free buffers on an incomplete GPU event.
    {
        FakeGpu api;
        api.pending_ = true;
        auto p = provider(api);
        auto op = p.prepare(kSource, {1.0f}, {2.0f}, 3.0f);
        check(p.poll(*op) == S::UploadX, "upload starts");
        p.request_cancel(*op);
        check(p.poll(*op) == S::UploadX, "pending event remains pinned");
        check(p.busy() && api.buffer_releases_ == 0,
              "no physical release before event");
        reject([&] { (void)op->result(); });
        api.complete_current();
        check(p.poll(*op) == S::Cancelled, "event completion permits cancel");
        check(!p.busy() && api.buffer_releases_ == 3 &&
              api.uploads_ == 1 && api.kernels_ == 0,
              "cancelled upload never executes a kernel");
    }

    // An error/unknown enqueue outcome is never a quiescence witness.
    {
        FakeGpu api;
        api.fail_launch_ = true;
        auto p = provider(api);
        auto op = p.prepare(kSource, {1.0f}, {2.0f}, 3.0f);
        check(p.poll(*op) == S::UploadX, "first upload");
        check(p.poll(*op) == S::UploadY, "second upload");
        reject([&] { (void)p.poll(*op); });
        check(op->state() == S::Quarantined && p.busy() &&
              api.buffer_releases_ == 0, "unknown submission stays pinned");
        reject([&] { p.recover_after_quiescence(*op); });
        check(api.buffer_releases_ == 0, "no speculative recovery");
        api.allow_quiescence_ = true;
        p.recover_after_quiescence(*op);
        check(op->state() == S::Failed && api.buffer_releases_ == 3 &&
              !p.busy(), "out-of-band queue proof permits retirement");
    }

    // Driver event failure is not physical quiescence.
    {
        FakeGpu api;
        api.pending_ = true;
        auto p = provider(api);
        auto op = p.prepare(kSource, {1.0f}, {2.0f}, 3.0f);
        check(p.poll(*op) == S::UploadX, "first upload");
        api.fail_current();
        check(p.poll(*op) == S::Quarantined, "driver failure quarantines");
        check(api.event_releases_ == 0 && api.buffer_releases_ == 0,
              "failed event retained");
        api.allow_quiescence_ = true;
        p.recover_after_quiescence(*op);
        check(op->state() == S::Failed && api.event_releases_ == 1 &&
              api.buffer_releases_ == 3, "recover after trusted queue finish");
    }

    // Release error is retryable; do not unlock quota early.
    {
        FakeGpu api;
        auto p = provider(api);
        auto op = p.prepare(kSource, {1.0f}, {2.0f}, 3.0f);
        api.fail_release_once_ = true;
        reject([&] { run_to_terminal(p, *op); });
        check(op->state() == S::ReleasePending && p.busy(),
              "failed physical release retains operation");
        reject([&] { (void)op->result(); });
        p.retry_release();
        check(op->state() == S::Succeeded && !p.busy() &&
              api.buffer_releases_ == 3, "release retry succeeds");
    }

    // Pre-enqueue partial allocation must release all successfully allocated
    // resources and must never record a physical GPU event.
    {
        FakeGpu api;
        api.fail_allocate_ = true;
        auto p = provider(api);
        reject([&] { (void)p.prepare(kSource, {1.0f}, {2.0f}, 3.0f); });
        check(!p.busy() && api.allocations_ == 2 &&
              api.buffer_releases_ == 2 && api.program_releases_ == 1 &&
              api.event_releases_ == 0, "pre-enqueue rollback");
    }

    // Quota, identity, alias, zero, overflow and unsupported source closed.
    {
        FakeGpu api;
        reject([&] {
            SaxpyProvider invalid(api, "tenant", "other-device", 7,
                                  {100, 100, 100, 25});
        });
        reject([&] {
            SaxpyProvider invalid(api, "tenant", api.physical_device_name(),
                                  0, {100, 100, 100, 25});
        });
        auto p = provider(api, 24);
        reject([&] { (void)p.prepare(kSource, {}, {}, 1.5f); });
        reject([&] { (void)p.prepare(kSource, {1}, {1, 2}, 1.5f); });
        reject([&] { (void)p.prepare(kSource, {1}, {2},
                                   std::numeric_limits<float>::infinity()); });
        reject([&] { (void)p.prepare(kSource, {1, 2, 3}, {1, 2, 3}, 1.5f); });
        reject([&] {
            (void)p.prepare(kSource + "fnc host() { print(1); }", {1}, {1}, 1.5f);
        });
        check(!p.busy() && api.allocations_ == 0, "rejected input no device allocation");
    }
    std::cout << "atto-saxpy-three-buffer-host-state-machine-passed\n";
    return 0;
}
