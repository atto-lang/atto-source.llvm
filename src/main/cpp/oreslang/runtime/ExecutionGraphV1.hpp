#pragma once

// Trusted host-side typed CPU/GPU graph substrate v1. NOT guest AST lowering,
// a device driver, or a GPU-on-CPU fallback. All GPU/transfer graphs reject
// atomically before CPU execution or scheduling.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace oreslang::execution_v1 {

enum class Domain { Cpu, Gpu };
enum class Kind { Unit, HostI64, DeviceI64 };
enum class Op { Unit, ConstantI64, AddI64, UploadI64, GpuAddI64, DownloadI64 };
using Value = std::variant<std::monostate, std::int64_t>;

struct Node {
    std::size_t id;
    Domain domain;
    Op operation;
    Kind result;
    std::vector<std::size_t> inputs;
    std::int64_t constant = 0;
};

class Builder;
class Ref {
    friend class Builder;
    std::shared_ptr<const char> owner_;
    std::size_t index_;
    Kind kind_;
    Ref(std::shared_ptr<const char> owner, std::size_t index, Kind kind)
        : owner_(std::move(owner)), index_(index), kind_(kind) {}
public:
    Kind kind() const noexcept { return kind_; }
    std::size_t index() const noexcept { return index_; }
};

struct Cancellation final : std::runtime_error {
    Cancellation() : std::runtime_error("CPU execution graph cancelled") {}
};

class CpuFuture {
    struct State {
        std::mutex mu;
        std::atomic<bool> cancelled{false};
        bool settled = false;
        std::promise<Value> promise;
        std::shared_future<Value> value = promise.get_future().share();

        void complete(Value v) {
            std::lock_guard<std::mutex> hold(mu);
            if (settled) return;
            settled = true;
            promise.set_value(std::move(v));
        }
        void fail(std::exception_ptr ex) {
            std::lock_guard<std::mutex> hold(mu);
            if (settled) return;
            settled = true;
            promise.set_exception(std::move(ex));
        }
        bool cancel() {
            std::lock_guard<std::mutex> hold(mu);
            if (settled) return false;
            settled = true;
            cancelled.store(true, std::memory_order_release);
            promise.set_exception(std::make_exception_ptr(Cancellation()));
            return true;
        }
    };
    friend class Plan;
    std::shared_ptr<State> state_;
    explicit CpuFuture(std::shared_ptr<State> state): state_(std::move(state)) {}
public:
    bool cancel() { return state_->cancel(); }
    Value get() const { return state_->value.get(); }
    bool ready() const {
        return state_->value.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }
};

class Plan {
    std::vector<Node> nodes_;
    std::size_t output_;
    Kind type_;

    static std::int64_t checked_add(std::int64_t a, std::int64_t b) {
        if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
            (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) {
            throw std::overflow_error("CPU i64 arithmetic overflow");
        }
        return a + b;
    }

    Value evaluate(const std::atomic<bool>& cancelled) const {
        std::vector<Value> values(nodes_.size());
        for (const Node& node : nodes_) {
            if (cancelled.load(std::memory_order_acquire)) throw Cancellation();
            switch (node.operation) {
                case Op::Unit:
                    values[node.id] = std::monostate{};
                    break;
                case Op::ConstantI64:
                    values[node.id] = node.constant;
                    break;
                case Op::AddI64:
                    values[node.id] = checked_add(
                        std::get<std::int64_t>(values.at(node.inputs.at(0))),
                        std::get<std::int64_t>(values.at(node.inputs.at(1))));
                    break;
                case Op::UploadI64: case Op::GpuAddI64: case Op::DownloadI64:
                    throw std::logic_error("unadmitted device operation");
            }
        }
        if (cancelled.load(std::memory_order_acquire)) throw Cancellation();
        return values.at(output_);
    }
    void admit_cpu() const {
        if (!cpu_only())
            throw std::runtime_error("GPU execution/transfer provider unavailable; no implicit CPU fallback");
    }
public:
    Plan(std::vector<Node> nodes, std::size_t output, Kind type)
        : nodes_(std::move(nodes)), output_(output), type_(type) {
        if (nodes_.empty() || nodes_.size() > 4096 || output_ >= nodes_.size())
            throw std::invalid_argument("invalid execution graph plan");
    }
    const std::vector<Node>& nodes() const noexcept { return nodes_; }
    Kind result_type() const noexcept { return type_; }
    bool cpu_only() const noexcept {
        for (const Node& n : nodes_) {
            if (n.domain != Domain::Cpu || n.operation == Op::DownloadI64) return false;
        }
        return true;
    }
    /** Host-only join, never call from a native actor carrier. */
    Value execute_cpu() const {
        admit_cpu();
        const std::atomic<bool> alive{false};
        return evaluate(alive);
    }
    /**
     * Caller supplies a trusted, bounded CPU executor. The scheduler must
     * execute or reject the task; no internal thread or unbounded queue.
     * GPU preflight is synchronous and failure settles the Future without
     * queuing any host task. Cancellation is cooperative between CPU steps.
     */
    CpuFuture submit_cpu(const std::function<void(std::function<void()>)>& schedule) const {
        auto state = std::make_shared<CpuFuture::State>();
        CpuFuture exposed(state);
        try {
            admit_cpu();
            if (!schedule) throw std::invalid_argument("missing trusted CPU executor");
            Plan copy = *this;
            schedule([state, copy = std::move(copy)] {
                if (state->cancelled.load(std::memory_order_acquire)) return;
                try {
                    state->complete(copy.evaluate(state->cancelled));
                } catch (...) {
                    state->fail(std::current_exception());
                }
            });
        } catch (...) {
            state->fail(std::current_exception());
        }
        return exposed;
    }
};

class Builder {
    std::shared_ptr<const char> owner_ = std::make_shared<char>(0);
    std::vector<Node> nodes_;
    bool sealed_ = false;

    void validate(const Ref& ref, Kind expected) const {
        if (ref.owner_ != owner_ || ref.index_ >= nodes_.size() || ref.kind_ != expected)
            throw std::invalid_argument("invalid domain, type or graph ownership");
    }
    Ref append(Domain domain, Op op, Kind result,
               std::int64_t constant, std::vector<std::size_t> deps) {
        if (sealed_) throw std::logic_error("execution graph builder sealed");
        if (nodes_.size() >= 4096) throw std::length_error("execution graph node budget exceeded");
        for (auto dep : deps)
            if (dep >= nodes_.size()) throw std::invalid_argument("forward graph dependency");
        const auto id = nodes_.size();
        nodes_.push_back(Node{id, domain, op, result, std::move(deps), constant});
        return Ref(owner_, id, result);
    }
public:
    // Builders are not cloneable authority; refs cannot transfer ownership.
    Builder() = default;
    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;
    Ref unit() { return append(Domain::Cpu, Op::Unit, Kind::Unit, 0, {}); }
    Ref cpu_i64(std::int64_t constant) {
        return append(Domain::Cpu, Op::ConstantI64, Kind::HostI64, constant, {});
    }
    Ref cpu_add(const Ref& a, const Ref& b) {
        validate(a, Kind::HostI64); validate(b, Kind::HostI64);
        return append(Domain::Cpu, Op::AddI64, Kind::HostI64, 0, {a.index_, b.index_});
    }
    Ref upload_i64(const Ref& host) {
        validate(host, Kind::HostI64);
        return append(Domain::Gpu, Op::UploadI64, Kind::DeviceI64, 0, {host.index_});
    }
    Ref gpu_add_i64(const Ref& a, const Ref& b) {
        validate(a, Kind::DeviceI64); validate(b, Kind::DeviceI64);
        return append(Domain::Gpu, Op::GpuAddI64, Kind::DeviceI64, 0, {a.index_, b.index_});
    }
    Ref download_i64(const Ref& device) {
        validate(device, Kind::DeviceI64);
        return append(Domain::Cpu, Op::DownloadI64, Kind::HostI64, 0, {device.index_});
    }
    Plan build(const Ref& output) {
        if (sealed_) throw std::logic_error("execution graph builder sealed");
        if (output.owner_ != owner_ || output.index_ >= nodes_.size())
            throw std::invalid_argument("execution graph output belongs to another builder");
        sealed_ = true;
        return Plan(nodes_, output.index_, output.kind_);
    }
};

} // namespace oreslang::execution_v1
