#include "oreslang/runtime/SaxpyOpenClProvider.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
using namespace oreslang;
using namespace oreslang::opencl;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template <typename Callable> void reject(Callable&& callback) {
    try { callback(); } catch (const std::exception&) { return; }
    throw std::runtime_error("SAXPY expected rejection");
}

struct FakeApi final : SaxpyApi {
    struct Buffer { std::vector<unsigned char> bytes; };
    struct Event {
        enum class Kind { Upload, Kernel, Download } kind;
        Buffer* x = nullptr;
        Buffer* y = nullptr;
        Buffer* out = nullptr;
        const unsigned char* host_in = nullptr;
        unsigned char* host_out = nullptr;
        std::size_t bytes = 0;
        float alpha = 0;
        std::uint64_t elements = 0;
        bool completed = false;
    };

    std::unordered_set<Buffer*> live_buffers;
    std::unordered_set<Event*> live_events;
    std::uint32_t released_buffers = 0;
    std::uint32_t released_events = 0;
    std::uint32_t released_programs = 0;
    std::uint32_t enqueued = 0;
    bool auto_complete = true;
    bool physical_quiescent = false;
    bool fail_alloc_once = false;
    bool fail_release_event_once = false;
    bool throw_during_kernel_enqueue = false;
    bool return_null_upload_event = false;
    bool device_lost = false;

    std::string authenticated_device_name() const override {
        return "fake-gpu0";
    }
    std::uint64_t authenticated_max_allocation() const override {
        return 1024 * 1024;
    }
    void validate_artifact(const dop::OpenClArtifact&) const override {}
    Handle prepare(const dop::OpenClArtifact&) override {
        return new int(1);
    }
    Handle prepare_saxpy(const gpu_source_v1::Artifact&) override {
        return new int(2);
    }
    void release_program(Handle value) override {
        delete static_cast<int*>(value);
        ++released_programs;
    }
    Handle allocate(std::uint64_t bytes) override {
        if (fail_alloc_once) {
            fail_alloc_once = false;
            throw std::runtime_error("injected allocation failure");
        }
        auto* buffer = new Buffer{std::vector<unsigned char>(
            static_cast<std::size_t>(bytes), 0)};
        live_buffers.insert(buffer);
        return buffer;
    }
    void release_buffer(Handle handle) override {
        auto* buffer = static_cast<Buffer*>(handle);
        if (!live_buffers.erase(buffer))
            throw std::logic_error("double release or foreign physical buffer");
        delete buffer;
        ++released_buffers;
    }
    Handle enqueue_upload(Handle target, const unsigned char* host,
                          std::size_t bytes) override {
        ++enqueued;
        if (return_null_upload_event)
            return nullptr; // unknown durable enqueue: quarantine
        auto* ev = new Event;
        ev->kind = Event::Kind::Upload;
        ev->out = static_cast<Buffer*>(target);
        ev->host_in = host;
        ev->bytes = bytes;
        live_events.insert(ev);
        return ev;
    }
    Handle enqueue_kernel(Handle, Handle, Handle,
                          std::uint64_t, std::uint32_t) override {
        throw std::logic_error("legacy 2-buffer kernel may not run SAXPY");
    }
    Handle enqueue_saxpy_kernel(Handle, Handle x, Handle y,
                                Handle out, float alpha,
                                std::uint64_t n) override {
        ++enqueued;
        if (throw_during_kernel_enqueue)
            throw std::runtime_error("injected ambiguous kernel enqueue");
        auto* ev = new Event;
        ev->kind = Event::Kind::Kernel;
        ev->x = static_cast<Buffer*>(x);
        ev->y = static_cast<Buffer*>(y);
        ev->out = static_cast<Buffer*>(out);
        ev->alpha = alpha;
        ev->elements = n;
        live_events.insert(ev);
        return ev;
    }
    Handle enqueue_download(Handle source, unsigned char* host,
                            std::size_t bytes) override {
        ++enqueued;
        auto* ev = new Event;
        ev->kind = Event::Kind::Download;
        ev->out = static_cast<Buffer*>(source);
        ev->host_out = host;
        ev->bytes = bytes;
        live_events.insert(ev);
        return ev;
    }
    EventStatus poll_event(Handle handle) override {
        auto* ev = static_cast<Event*>(handle);
        if (!live_events.count(ev))
            throw std::logic_error("foreign native event");
        if (device_lost) return EventStatus::DeviceLost;
        if (!auto_complete) return EventStatus::Pending;
        if (!ev->completed) {
            switch (ev->kind) {
            case Event::Kind::Upload:
                if (ev->bytes > ev->out->bytes.size())
                    throw std::runtime_error("upload exceeds physical capacity");
                std::memcpy(ev->out->bytes.data(), ev->host_in, ev->bytes);
                break;
            case Event::Kind::Kernel:
                for (std::uint64_t i = 0; i < ev->elements; ++i) {
                    float x = 0, y = 0;
                    const auto offset = static_cast<std::size_t>(i * sizeof(float));
                    std::memcpy(&x, ev->x->bytes.data() + offset, sizeof(float));
                    std::memcpy(&y, ev->y->bytes.data() + offset, sizeof(float));
                    volatile float product = ev->alpha * x;
                    const float value = product + y;
                    std::memcpy(ev->out->bytes.data() + offset, &value, sizeof(float));
                }
                break;
            case Event::Kind::Download:
                if (ev->bytes > ev->out->bytes.size())
                    throw std::runtime_error("download exceeds physical capacity");
                std::memcpy(ev->host_out, ev->out->bytes.data(), ev->bytes);
                break;
            }
            ev->completed = true;
        }
        return EventStatus::Complete;
    }
    void release_event(Handle handle) override {
        if (fail_release_event_once) {
            fail_release_event_once = false;
            throw std::runtime_error("injected clReleaseEvent failure");
        }
        auto* ev = static_cast<Event*>(handle);
        if (!live_events.erase(ev))
            throw std::logic_error("double release of native event");
        delete ev;
        ++released_events;
    }
    bool prove_device_loss_quiescence() override {
        return physical_quiescent;
    }
    ~FakeApi() override {
        for (auto* ev : live_events) delete ev;
        for (auto* buffer : live_buffers) delete buffer;
    }
};

gpu_source_v1::Artifact saxpy() {
    return gpu_source_v1::compile_saxpy(
        "compute fnc saxpy(x: DeviceRead<f32>, y: DeviceRead<f32>, "
        "out: DeviceWrite<f32>, alpha: f32, n: u64) device { "
        "const i = gpu.global_id.x; if i < n { "
        "out[i] = alpha * x[i] + y[i]; } }");
}
SaxpyProvider make_provider(FakeApi& api,
                            std::uint64_t resident = 8192,
                            std::uint64_t staging = 8192) {
    return SaxpyProvider(api,
        {"tenant-a", api.authenticated_device_name(), 8, 11},
        {resident, staging, 1024});
}
void finish(SaxpyProvider& provider, SaxpyProvider::Operation& op) {
    for (int i = 0; i < 10; ++i) {
        const auto state = op.state();
        if (state == SaxpyProvider::State::Succeeded ||
            state == SaxpyProvider::State::Cancelled ||
            state == SaxpyProvider::State::Failed ||
            state == SaxpyProvider::State::ReleaseFailed ||
            state == SaxpyProvider::State::Quarantined) return;
        (void)provider.poll(op);
    }
    throw std::runtime_error("SAXPY fake driver never reached terminal state");
}
} // namespace

int main() {
    const auto artifact = saxpy();
    const std::vector<float> x{1.0f, 2.0f, 3.0f, 4.0f};
    const std::vector<float> y{5.0f, 6.0f, 7.0f, 8.0f};

    // Four distinct physical commands: upload X, upload Y, kernel, download.
    {
        FakeApi api;
        auto provider = make_provider(api);
        auto op = provider.begin(artifact, x, y, 2.0f);
        check(op->allocation_generation() == 1, "fresh allocation generation");
        check(provider.usage().resident_bytes == 48 &&
              provider.usage().host_staging_bytes == 48 &&
              provider.usage().queue_slots == 1, "three-buffer quota");
        reject([&] { (void)provider.begin(artifact, x, y, 2.0f); });
        finish(provider, *op);
        check(op->state() == SaxpyProvider::State::Succeeded, "physical-success state");
        check(op->result() == std::vector<float>({7, 10, 13, 16}), "SAXPY three-buffer output");
        check(provider.closed_for_release() && provider.usage().queue_slots == 0,
              "post-event quota release");
        check(api.enqueued == 4 && api.released_events == 4 &&
              api.released_buffers == 3 && api.released_programs == 1,
              "all native handles retired");
        reject([&] { (void)provider.poll(*op); });
        auto second = provider.begin(artifact, x, y, 3.0f);
        check(second->allocation_generation() == 2, "monotone generation");
        finish(provider, *second);
        check(second->state() == SaxpyProvider::State::Succeeded, "second generation terminal");
    }

    // Timeouts/cancellation cannot retire resources before real event.
    {
        FakeApi api;
        api.auto_complete = false;
        auto provider = make_provider(api);
        auto op = provider.begin(artifact, x, y, 2.0f);
        provider.request_cancel(*op);
        check(provider.poll(*op) == SaxpyProvider::State::UploadX,
              "pending event must not complete on timeout");
        check(provider.usage().resident_bytes == 48 &&
              api.released_buffers == 0 && api.released_events == 0,
              "cancellation holds physical resources");
        reject([&] { (void)op->result(); });
        api.auto_complete = true;
        check(provider.poll(*op) == SaxpyProvider::State::Cancelled,
              "cancelled only after native event completion");
        check(api.enqueued == 1 && provider.usage().queue_slots == 0,
              "cancel before Y upload avoids further effects");
    }

    // Device loss is quarantine, never quiescence. An authenticated queue
    // finish is required before memory release.
    {
        FakeApi api;
        api.device_lost = true;
        auto provider = make_provider(api);
        auto op = provider.begin(artifact, x, y, 2.0f);
        check(provider.poll(*op) == SaxpyProvider::State::Quarantined,
              "device lost must quarantine");
        reject([&] { provider.recover_quarantined(); });
        check(api.released_buffers == 0 && provider.usage().queue_slots == 1,
              "device loss retains all physical ownership");
        api.physical_quiescent = true;
        provider.recover_quarantined();
        check(op->state() == SaxpyProvider::State::Failed &&
              api.released_buffers == 3 && provider.closed_for_release(),
              "trusted quiescence enables physical retirement");
    }

    // Enqueue exceptions may have already dispatched hardware: fail closed.
    {
        FakeApi api;
        api.throw_during_kernel_enqueue = true;
        auto provider = make_provider(api);
        auto op = provider.begin(artifact, x, y, 2.0f);
        check(provider.poll(*op) == SaxpyProvider::State::UploadY,
              "first upload transition");
        check(provider.poll(*op) == SaxpyProvider::State::Quarantined,
              "ambiguous physical enqueue must quarantine");
        check(provider.usage().resident_bytes == 48 && api.released_buffers == 0,
              "kernel submit failure keeps all resources");
        reject([&] { (void)provider.begin(artifact, x, y, 2.0f); });
        api.physical_quiescent = true;
        provider.recover_quarantined();
        check(op->state() == SaxpyProvider::State::Failed, "recovered failed command");
    }

    // A pre-enqueue failure can clean up safely without any submitted work.
    {
        FakeApi api;
        api.fail_alloc_once = true;
        auto provider = make_provider(api);
        reject([&] { (void)provider.begin(artifact, x, y, 2.0f); });
        check(provider.closed_for_release() && provider.usage().queue_slots == 0
              && api.released_programs == 1 && api.enqueued == 0,
              "pre-enqueue rollback");
    }

    // Physical handle release failure retains full quotas for retry.
    {
        FakeApi api;
        api.fail_release_event_once = true;
        auto provider = make_provider(api);
        auto op = provider.begin(artifact, x, y, 2.0f);
        check(provider.poll(*op) == SaxpyProvider::State::ReleaseFailed,
              "event release is retryable");
        check(provider.usage().queue_slots == 1 && api.released_buffers == 0,
              "failed release retains bytes");
        provider.retry_release();
        check(op->state() == SaxpyProvider::State::Failed &&
              provider.closed_for_release() && api.released_buffers == 3,
              "retry safely retires exact physical handles");
    }

    // Forged/unsupported device sources, zeros, nonfinite values and
    // untrusted quotas reject before any physical submission.
    {
        FakeApi api;
        auto provider = make_provider(api);
        auto forged = artifact;
        forged.device_source += "\n__kernel void evil(){}";
        reject([&] { (void)provider.begin(forged, x, y, 1.0f); });
        reject([&] { (void)provider.begin(artifact, {}, y, 1.0f); });
        reject([&] { (void)provider.begin(artifact, x, {}, 1.0f); });
        reject([&] { (void)provider.begin(artifact, x, y,
                                        std::numeric_limits<float>::infinity()); });
        reject([&] { (void)provider.begin(artifact, {NAN}, {2.0f}, 1.0f); });
        check(provider.closed_for_release() && api.enqueued == 0,
              "no physical activity from invalid guest input");
        reject([&] { (void)SaxpyProvider(api, {"tenant-a", "foreign-device", 8, 11},
                                        {1024, 1024, 256}); });
        auto small = make_provider(api, 47, 48);
        reject([&] { (void)small.begin(artifact, x, y, 2.0f); });
        check(small.usage().queue_slots == 0, "quota rejection is atomic");
    }

    std::cout << "SAXPY three-buffer OpenCL provider host reference tests passed"
              << " (no hardware execution claimed)\n";
    return 0;
}
