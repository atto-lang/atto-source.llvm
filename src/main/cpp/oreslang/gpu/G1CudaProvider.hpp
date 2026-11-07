#pragma once
#include "oreslang/runtime/GpuBufferFence.hpp"
#include "oreslang/runtime/GpuLaunchGeometry.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oreslang::gpu::g2 {

using Handle = std::uint64_t;
enum class EventResult { Pending, Complete, Failed };

// Driver functions operate on one explicitly selected device and one stream.
// The real implementation MUST derive limits from CUDA, not guest source.
class Driver {
public:
    virtual ~Driver() = default;
    virtual const std::string& device_identity() const = 0;
    virtual const Limits& device_limits() const = 0;
    virtual Handle load_ptx(std::string_view ptx) = 0;
    virtual Handle kernel(Handle module, const char* entry) = 0;
    virtual Handle allocate(std::size_t bytes) = 0;
    virtual void upload(Handle dst, const float* src, std::size_t bytes) = 0;
    virtual void launch(Handle fn, std::uint32_t grid, std::uint32_t group,
                        float scale, Handle x, Handle y, Handle out, std::uint64_t n) = 0;
    virtual void download(float* dst, Handle src, std::size_t bytes) = 0;
    virtual Handle record_event() = 0;
    virtual EventResult event_result(Handle event) = 0;
    // True ONLY if the physical stream is known quiescent, including prior errors.
    virtual bool stream_quiescent() = 0;
    virtual void destroy_event(Handle event) = 0;
    virtual void free_buffer(Handle allocation) = 0;
    virtual void unload(Handle module) = 0;
};

enum class State {
    Ready, Submitted, CancelRequested, Quarantined,
    Completed, Cancelled, Failed, ReleaseFailed, Closed
};

// G2 host-side proof harness only: not a guest-owned Future, scheduler API,
// multitenant GPU broker, or generally authorized CUDA execution service.
// Driver and its CUDA context must outlive this single-flight operation.
class SaxpyOperation final {
public:
    SaxpyOperation(Driver& driver, std::string tenant, std::uint64_t generation,
                   std::uint64_t max_bytes)
        : driver_(driver), tenant_(std::move(tenant)), generation_(generation),
          max_bytes_(max_bytes) {
        if (tenant_.empty() || driver_.device_identity().empty() ||
            generation_ == 0 || generation_ > std::numeric_limits<std::uint64_t>::max() - 2 ||
            max_bytes_ == 0)
            throw std::invalid_argument("GPU-G2-001: invalid tenant, device or generation");
    }
    SaxpyOperation(const SaxpyOperation&) = delete;
    SaxpyOperation& operator=(const SaxpyOperation&) = delete;
    ~SaxpyOperation() {
        // Cannot release in-flight host staging or handles without device proof.
        // A production scheduler must retain operations until they settle.
        if (module_ || event_ || allocations_[0] || allocations_[1] || allocations_[2])
            std::terminate();
    }

    State state() const noexcept { return state_; }
    bool has_live_device_handles() const noexcept {
        return module_ || event_ || allocations_[0] || allocations_[1] || allocations_[2];
    }
    const std::vector<float>& output() const {
        if (state_ != State::Completed)
            throw std::logic_error("GPU-G2-003: output is not authenticated complete");
        return output_;
    }

    void submit(std::string_view ptx, float scale,
                const std::vector<float>& x, const std::vector<float>& y,
                std::uint32_t group = 256) {
        if (state_ != State::Ready) throw std::logic_error("GPU-G2-003: single-flight operation");
        if (ptx.empty() || ptx.size() > 512 * 1024 || ptx.find('\0') != ptx.npos ||
            ptx.find(".entry saxpy") == ptx.npos)
            throw std::invalid_argument("GPU-G2-001: invalid or untrusted PTX artifact");
        if (x.size() != y.size() || x.size() > max_bytes_ / sizeof(float) ||
            x.size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument("GPU-G2-001: invalid or oversized GPU vectors");
        const auto shape = validate_logical_launch(
            Dim3(static_cast<std::int64_t>(x.size()), 1, 1), Dim3(group, 1, 1),
            0, driver_.device_limits());
        if (shape.physical.grid.y != 1 || shape.physical.grid.z != 1 ||
            shape.physical.block.y != 1 || shape.physical.block.z != 1 ||
            shape.physical.grid.x > std::numeric_limits<std::uint32_t>::max() ||
            shape.physical.block.x > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("GPU-G2-001: unsupported CUDA geometry");
        if (x.empty()) {
            state_ = State::Completed; // no device submission, no zero-byte allocations
            return;
        }
        const std::size_t bytes = x.size() * sizeof(float);
        input_x_ = x; input_y_ = y; output_.assign(x.size(), 0.0f);
        identities_ = {{{tenant_, driver_.device_identity(), generation_},
                        {tenant_, driver_.device_identity(), generation_ + 1},
                        {tenant_, driver_.device_identity(), generation_ + 2}}};
        try {
            module_ = require_handle(driver_.load_ptx(ptx));
            const auto fn = require_handle(driver_.kernel(module_, "saxpy"));
            for (std::size_t i = 0; i < 3; ++i) {
                allocations_[i] = require_handle(driver_.allocate(bytes));
                fences_[i] = std::make_unique<BufferFence>(identities_[i], bytes);
                tickets_[i].emplace(fences_[i]->begin_command(identities_[i]));
            }
            // Mark every loan submitted before the first queue command. A driver
            // may enqueue and then raise an error; never roll back as unsubmitted.
            for (std::size_t i = 0; i < 3; ++i)
                fences_[i]->mark_submitted(*tickets_[i]);
            submitted_to_driver_ = true;
            driver_.upload(allocations_[0], input_x_.data(), bytes);
            driver_.upload(allocations_[1], input_y_.data(), bytes);
            driver_.launch(fn, static_cast<std::uint32_t>(shape.physical.grid.x), group,
                           scale, allocations_[0], allocations_[1], allocations_[2], x.size());
            driver_.download(output_.data(), allocations_[2], bytes);
            event_ = require_handle(driver_.record_event());
            state_ = State::Submitted;
        } catch (...) {
            if (submitted_to_driver_) {
                state_ = State::Quarantined;
                poison_all();
                // No driver frees here: a command may still access every buffer.
            } else {
                state_ = State::Failed;
                abort_reserved();
                try { release_resources(); }
                catch (...) { state_ = State::ReleaseFailed; }
            }
            throw;
        }
    }

    void request_cancel() {
        if (state_ != State::Submitted)
            throw std::logic_error("GPU-G2-003: only submitted work can cancel");
        poison_all();
        state_ = State::CancelRequested;
    }

    // Returns false until a device event or stream query proves quiescence.
    // No carrier thread waits on CUDA; caller drives this from a scheduler.
    bool poll() {
        if (state_ == State::Completed || state_ == State::Cancelled ||
            state_ == State::Failed || state_ == State::Closed) return true;
        if (state_ != State::Submitted && state_ != State::CancelRequested &&
            state_ != State::Quarantined && state_ != State::ReleaseFailed)
            throw std::logic_error("GPU-G2-003: operation is not submitted");
        if (state_ == State::ReleaseFailed) {
            release_resources();
            state_ = terminal_;
            return true;
        }
        EventResult event = EventResult::Failed;
        if (event_) {
            try { event = driver_.event_result(event_); }
            catch (...) { state_ = State::Quarantined; poison_all(); return false; }
            if (event == EventResult::Pending) return false;
        }
        const bool normal_success = state_ == State::Submitted &&
                                    event_ && event == EventResult::Complete;
        // An event failure or failed enqueue never proves quiescence by itself.
        if (!normal_success) {
            bool idle = false;
            try { idle = driver_.stream_quiescent(); }
            catch (...) { return false; }
            if (!idle) return false;
        }
        if (normal_success) {
            for (std::size_t i = 0; i < 3; ++i)
                fences_[i]->complete(*tickets_[i], true);
            terminal_ = State::Completed;
        } else {
            poison_all();
            for (std::size_t i = 0; i < 3; ++i)
                fences_[i]->acknowledge_cancelled_device_event(*tickets_[i], identities_[i]);
            terminal_ = (state_ == State::CancelRequested) ? State::Cancelled : State::Failed;
        }
        try { release_resources(); state_ = terminal_; }
        catch (...) { state_ = State::ReleaseFailed; throw; }
        return true;
    }

    void close() {
        if (has_live_device_handles())
            throw std::logic_error("GPU-G2-003: cannot close before device retirement");
        if (state_ != State::Completed && state_ != State::Cancelled &&
            state_ != State::Failed)
            throw std::logic_error("GPU-G2-003: operation has not reached terminal state");
        state_ = State::Closed;
    }

private:
    static Handle require_handle(Handle handle) {
        if (!handle) throw std::runtime_error("GPU-G2-002: driver returned null handle");
        return handle;
    }
    void abort_reserved() {
        for (std::size_t i = 0; i < 3; ++i)
            if (tickets_[i]) fences_[i]->abort_unsubmitted(*tickets_[i]);
    }
    void poison_all() {
        if (poisoned_) return;
        for (std::size_t i = 0; i < 3; ++i)
            if (tickets_[i]) fences_[i]->cancel_pending(*tickets_[i]);
        poisoned_ = true;
    }
    void release_resources() {
        // All calls may fail; zero each handle only *after* confirmed release,
        // so retry cannot prematurely declare resource retirement.
        if (event_) { driver_.destroy_event(event_); event_ = 0; }
        for (auto& h : allocations_)
            if (h) { driver_.free_buffer(h); h = 0; }
        if (module_) { driver_.unload(module_); module_ = 0; }
        for (std::size_t i = 0; i < 3; ++i)
            if (fences_[i] && fences_[i]->status() != ::oreslang::gpu::Status::Closed) {
                fences_[i]->retire_after_backend_release(identities_[i]);
                fences_[i]->close_after_retirement();
            }
    }
    Driver& driver_;
    std::string tenant_;
    std::uint64_t generation_;
    std::uint64_t max_bytes_;
    State state_ = State::Ready;
    State terminal_ = State::Failed;
    Handle module_ = 0, event_ = 0;
    std::array<Handle, 3> allocations_{};
    std::array<Identity, 3> identities_{};
    std::array<std::unique_ptr<BufferFence>, 3> fences_{};
    std::array<std::optional<Ticket>, 3> tickets_{};
    std::vector<float> input_x_, input_y_, output_;
    bool submitted_to_driver_ = false, poisoned_ = false;
};

} // namespace oreslang::gpu::g2
