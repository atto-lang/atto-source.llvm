#pragma once

#include "oreslang/frontend/AttoGpuSaxpyV1.hpp"
#include "oreslang/runtime/OpenClProvider.hpp"

#include <cmath>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::opencl {

// Separate trusted three-buffer ABI. The legacy two-buffer DOP Api remains
// unchanged; implementations cannot claim SAXPY by accepting its old kernel
// entrypoint. Only SystemApi or a trusted test implementation supplies this.
class SaxpyHardware {
public:
    virtual ~SaxpyHardware() = default;
    virtual Api& base() noexcept = 0;
    virtual const std::string& physical_device_name() const noexcept = 0;
    virtual Api::Handle enqueue_saxpy(Api::Handle program, Api::Handle x,
            Api::Handle y, Api::Handle output, float alpha,
            std::uint64_t rows) = 0;
};

class SaxpyProvider final {
public:
    enum class State {
        Reserved, UploadX, UploadY, Kernel, Download,
        ReleasePending, Succeeded, Cancelled, Failed, Quarantined
    };

    struct Limits {
        std::uint64_t resident_bytes;
        std::uint64_t pinned_host_bytes;
        std::uint64_t maximum_allocation_bytes;
        std::uint64_t maximum_rows;
    };

    class Operation final {
    public:
        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;
        State state() const noexcept { return state_.load(); }
        std::uint64_t rows() const noexcept { return rows_; }
        std::uint64_t bytes_per_buffer() const noexcept { return bytes_; }
        std::vector<float> result() const {
            if (state_ != State::Succeeded)
                throw std::logic_error("SAXPY result not proven complete");
            std::vector<float> values(static_cast<std::size_t>(rows_));
            std::memcpy(values.data(), output_bytes_.data(), output_bytes_.size());
            return values;
        }

    private:
        friend class SaxpyProvider;
        Operation(std::uint64_t rows, std::uint64_t bytes,
                  float alpha, std::vector<unsigned char> x,
                  std::vector<unsigned char> y)
            : rows_(rows), bytes_(bytes), alpha_(alpha),
              x_bytes_(std::move(x)), y_bytes_(std::move(y)),
              output_bytes_(static_cast<std::size_t>(bytes), 0) {}

        const std::uint64_t rows_;
        const std::uint64_t bytes_;
        const float alpha_;
        std::vector<unsigned char> x_bytes_, y_bytes_, output_bytes_;
        Api::Handle program_ = nullptr;
        Api::Handle x_device_ = nullptr, y_device_ = nullptr, out_device_ = nullptr;
        Api::Handle event_ = nullptr;
        std::atomic<State> state_{State::Reserved};
        State release_result_ = State::Failed;
        bool cancel_requested_ = false;
    };

    SaxpyProvider(SaxpyHardware& hardware, std::string tenant,
                  std::string selected_device, std::uint64_t queue_generation,
                  Limits limits)
        : hardware_(hardware), tenant_(std::move(tenant)),
          device_(std::move(selected_device)),
          queue_generation_(queue_generation), limits_(limits) {
        if (tenant_.empty() || device_.empty() || queue_generation_ == 0 ||
            device_ != hardware_.physical_device_name())
            throw std::invalid_argument("SAXPY provider physical identity mismatch");
        if (!limits_.resident_bytes || !limits_.pinned_host_bytes ||
            !limits_.maximum_allocation_bytes || !limits_.maximum_rows)
            throw std::invalid_argument("SAXPY provider limits missing");
        const std::uint32_t byte_order = 1;
        if (*reinterpret_cast<const unsigned char*>(&byte_order) != 1 ||
            sizeof(float) != 4 || !std::numeric_limits<float>::is_iec559)
            throw std::runtime_error("SAXPY requires little-endian IEEE binary32");
    }

    // Must be invoked from trusted host CONTROL, not an actor carrier:
    // clBuildProgram and allocation may block. No device command is enqueued
    // here; polling submits one nonblocking command at a time.
    std::shared_ptr<Operation> prepare(
            std::string_view atto_source,
            const std::vector<float>& x, const std::vector<float>& y,
            float alpha) {
        std::lock_guard<std::mutex> guard(lock_);
        if (active_)
            throw std::logic_error("SAXPY provider queue is already reserved");
        if (x.empty() || x.size() != y.size() ||
            !std::isfinite(alpha) || x.size() > limits_.maximum_rows)
            throw std::invalid_argument("SAXPY invalid input lengths or coefficient");
        const auto rows = static_cast<std::uint64_t>(x.size());
        const auto bytes = checked_mul(rows, 4);
        const auto quota = checked_mul(bytes, 3);
        if (bytes > limits_.maximum_allocation_bytes ||
            bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            quota > limits_.resident_bytes || quota > limits_.pinned_host_bytes)
            throw std::invalid_argument("SAXPY buffer or queue quota exceeded");

        const auto parsed = gpu_source_v1::compile_saxpy(atto_source);
        const dop::OpenClArtifact artifact{
            parsed.kernel_name, parsed.device_source, bytes, rows, 1,
            parsed.abi_version, parsed.required_address_bits,
            parsed.requires_f32, true};
        hardware_.base().validate_artifact(artifact);

        std::vector<unsigned char> x_bytes(static_cast<std::size_t>(bytes));
        std::vector<unsigned char> y_bytes(static_cast<std::size_t>(bytes));
        std::memcpy(x_bytes.data(), x.data(), x_bytes.size());
        std::memcpy(y_bytes.data(), y.data(), y_bytes.size());

        auto op = std::shared_ptr<Operation>(new Operation(
            rows, bytes, alpha, std::move(x_bytes), std::move(y_bytes)));
        active_ = op;
        try {
            op->program_ = hardware_.base().prepare(artifact);
            if (!op->program_)
                throw std::runtime_error("SAXPY physical program missing");
            op->x_device_ = require_allocation(bytes);
            op->y_device_ = require_allocation(bytes);
            op->out_device_ = require_allocation(bytes);
        } catch (...) {
            // No enqueue was attempted. Partial physical release failures
            // retain active_ and all not-yet-released handles for a retry.
            op->state_ = State::ReleasePending;
            try { release_resources(*op); } catch (...) {}
            throw;
        }
        return op;
    }

    // Poll is nonblocking with respect to device completion. Each hardware
    // event is retained until it reports Complete. Failed/device-lost event
    // status does not authenticate quiescence.
    State poll(Operation& op) {
        std::lock_guard<std::mutex> guard(lock_);
        verify_owner(op);
        if (terminal(op.state_)) return op.state_;
        if (op.state_ == State::Quarantined || op.state_ == State::ReleasePending)
            return op.state_;
        if (op.state_ == State::Reserved && op.cancel_requested_) {
            finish(op, State::Cancelled);
            return op.state_;
        }
        if (op.event_) {
            EventStatus status;
            try {
                status = hardware_.base().poll_event(op.event_);
            } catch (...) {
                op.state_ = State::Quarantined;
                throw;
            }
            if (status == EventStatus::Pending) return op.state_;
            if (status != EventStatus::Complete) {
                op.state_ = State::Quarantined;
                return op.state_;
            }
            // A failed clReleaseEvent can be retried while the physical
            // event stays owned; never discard a still-live event handle.
            hardware_.base().release_event(op.event_);
            op.event_ = nullptr;
            if (op.cancel_requested_) {
                finish(op, State::Cancelled);
                return op.state_;
            }
            if (op.state_ == State::Download) {
                finish(op, State::Succeeded);
                return op.state_;
            }
        }
        try {
            Api::Handle event = nullptr;
            State next = State::Reserved;
            switch (op.state_) {
            case State::Reserved:
                event = hardware_.base().enqueue_upload(
                    op.x_device_, op.x_bytes_.data(), op.x_bytes_.size());
                next = State::UploadX;
                break;
            case State::UploadX:
                event = hardware_.base().enqueue_upload(
                    op.y_device_, op.y_bytes_.data(), op.y_bytes_.size());
                next = State::UploadY;
                break;
            case State::UploadY:
                event = hardware_.enqueue_saxpy(
                    op.program_, op.x_device_, op.y_device_, op.out_device_,
                    op.alpha_, op.rows_);
                next = State::Kernel;
                break;
            case State::Kernel:
                event = hardware_.base().enqueue_download(
                    op.out_device_, op.output_bytes_.data(),
                    op.output_bytes_.size());
                next = State::Download;
                break;
            default:
                throw std::logic_error("SAXPY invalid submission transition");
            }
            // The API must return a durable native event. If a command was
            // accepted without an event, no host cleanup can be authorized.
            if (!event)
                throw std::runtime_error("SAXPY enqueue lacks native event");
            op.event_ = event;
            op.state_ = next;
        } catch (...) {
            // An enqueue may have reached hardware before it threw.
            // Only out-of-band trusted queue quiescence may release memory.
            op.state_ = State::Quarantined;
            throw;
        }
        return op.state_;
    }

    void request_cancel(Operation& op) {
        std::lock_guard<std::mutex> guard(lock_);
        verify_owner(op);
        if (!terminal(op.state_)) op.cancel_requested_ = true;
    }

    // Explicit blocking CONTROL/shutdown recovery ONLY. Never invoke on an
    // actor carrier. Device loss and unknown enqueue outcomes remain pinned
    // until the trusted backend certifies the queue cannot access buffers.
    void recover_after_quiescence(Operation& op) {
        std::lock_guard<std::mutex> guard(lock_);
        verify_owner(op);
        if (op.state_ != State::Quarantined)
            throw std::logic_error("SAXPY recovery requires quarantined operation");
        if (!hardware_.base().prove_device_loss_quiescence())
            throw std::logic_error("SAXPY device queue not proven quiescent");
        if (op.event_) {
            hardware_.base().release_event(op.event_);
            op.event_ = nullptr;
        }
        finish(op, op.cancel_requested_ ? State::Cancelled : State::Failed);
    }

    // Retry partial release without ever reusing a quarantined buffer.
    void retry_release() {
        std::lock_guard<std::mutex> guard(lock_);
        if (!active_ || active_->state_ != State::ReleasePending)
            throw std::logic_error("SAXPY has no retryable resource release");
        release_resources(*active_);
    }

    bool busy() const {
        std::lock_guard<std::mutex> guard(lock_);
        return static_cast<bool>(active_);
    }

private:
    static std::uint64_t checked_mul(std::uint64_t a, std::uint64_t b) {
        if (b != 0 && a > std::numeric_limits<std::uint64_t>::max() / b)
            throw std::overflow_error("SAXPY size/quota overflow");
        return a * b;
    }
    static bool terminal(State s) noexcept {
        return s == State::Succeeded || s == State::Cancelled || s == State::Failed;
    }
    Api::Handle require_allocation(std::uint64_t bytes) {
        auto result = hardware_.base().allocate(bytes);
        if (!result) throw std::runtime_error("SAXPY provider returned null allocation");
        return result;
    }
    void verify_owner(const Operation& op) const {
        if (!active_ || active_.get() != &op) {
            if (terminal(op.state_)) return;
            throw std::logic_error("foreign or retired SAXPY operation");
        }
    }
    void finish(Operation& op, State result) {
        op.release_result_ = result;
        op.state_ = State::ReleasePending;
        release_resources(op);
    }
    void release_resources(Operation& op) {
        if (op.event_)
            throw std::logic_error("SAXPY event still owns physical resources");
        // Null each handle only after the device driver confirms release.
        auto& api = hardware_.base();
        if (op.out_device_) {
            api.release_buffer(op.out_device_); op.out_device_ = nullptr;
        }
        if (op.y_device_) {
            api.release_buffer(op.y_device_); op.y_device_ = nullptr;
        }
        if (op.x_device_) {
            api.release_buffer(op.x_device_); op.x_device_ = nullptr;
        }
        if (op.program_) {
            api.release_program(op.program_); op.program_ = nullptr;
        }
        op.state_ = op.release_result_;
        active_.reset();
    }

    SaxpyHardware& hardware_;
    const std::string tenant_, device_;
    const std::uint64_t queue_generation_;
    const Limits limits_;
    mutable std::mutex lock_;
    std::shared_ptr<Operation> active_;
};

} // namespace oreslang::opencl
