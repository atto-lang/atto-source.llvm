#pragma once

#include "oreslang/runtime/DopLayout.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace oreslang::dop {

// The executable CPU lowering and emitted OpenCL source share one typed IR.
// Device execution is a separate, explicit capability; no CPU fallback.
enum class Element { F32 };
enum class Op { Scale, Bias, ScaleBias };

struct Kernel {
    Plan layout;
    Element element;
    Op operation;
    float scale;
    float bias;
    Kernel(Plan l, Element e, Op o, float a, float b)
        : layout(l), element(e), operation(o), scale(a), bias(b) {
        if (element != Element::F32 || layout.scalar_bytes() != sizeof(float)
                || !std::isfinite(scale) || !std::isfinite(bias))
            throw std::invalid_argument("only finite homogeneous f32 DOP kernels are admitted");
    }
};

struct GpuTarget {
    std::uint32_t address_bits;
    std::uint64_t maximum_buffer_bytes;
    std::uint64_t maximum_rows;
    bool float32;
};

struct OpenClArtifact {
    std::string kernel_name;
    std::string device_source;
    std::uint64_t required_bytes;
    std::uint64_t global_rows;
    std::uint32_t global_fields;
};

class HostBuffer {
    struct State {
        Plan layout;
        std::vector<unsigned char> bytes;
        mutable std::mutex lock;
        std::size_t loans = 0;
        bool closed = false;
        static std::size_t checked_host_size(const Plan& layout) {
            if (layout.byte_length() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
                throw std::overflow_error("layout exceeds addressable host size");
            return static_cast<std::size_t>(layout.byte_length());
        }
        explicit State(Plan value)
            : layout(value), bytes(checked_host_size(value), 0) { }
    };
public:
    class ReadLoan {
    public:
        explicit ReadLoan(std::shared_ptr<State> state) : state_(std::move(state)) {}
        ReadLoan(const ReadLoan&) = delete;
        ReadLoan& operator=(const ReadLoan&) = delete;
        ReadLoan(ReadLoan&& other) noexcept : state_(std::move(other.state_)) {}
        ReadLoan& operator=(ReadLoan&&) = delete;
        ~ReadLoan() { release(); }
        float read(std::uint64_t row, std::uint32_t field) const {
            if (!state_) throw std::logic_error("stale read loan");
            std::lock_guard<std::mutex> guard(state_->lock);
            if (state_->closed) throw std::logic_error("closed read loan");
            const auto offset = state_->layout.byte_offset(row, field);
            float result = 0;
            std::memcpy(&result, state_->bytes.data() + static_cast<std::size_t>(offset), sizeof(result));
            return result;
        }
        void release() noexcept {
            if (!state_) return;
            auto held = std::move(state_);
            {
                std::lock_guard<std::mutex> guard(held->lock);
                --held->loans;
            } // The mutex must be unlocked before the last state reference dies.
        }
    private:
        std::shared_ptr<State> state_;
    };

    explicit HostBuffer(Plan layout) : state_(std::make_shared<State>(layout)) {}
    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;
    // Guard the owner pointer independently of State::lock: take() changes
    // the wrapper's shared_ptr, and the old check/use sequence raced with it.
    HostBuffer(HostBuffer&& other) noexcept {
        std::lock_guard<std::mutex> guard(other.owner_lock_);
        state_ = std::move(other.state_);
    }
    HostBuffer& operator=(HostBuffer&& other) {
        if (this == &other) return *this;
        std::scoped_lock guard(owner_lock_, other.owner_lock_);
        if (state_) {
            std::lock_guard<std::mutex> state_guard(state_->lock);
            if (!state_->closed)
                throw std::logic_error("cannot overwrite live DOP host buffer owner");
        }
        state_ = std::move(other.state_);
        return *this;
    }

    // Return the small immutable layout by VALUE; a reference could outlive a
    // concurrent take()/close() and dangle after a wrapper releases storage.
    Plan layout() const {
        std::lock_guard<std::mutex> guard(owner_lock_);
        active();
        return state_->layout;
    }
    bool identical_storage(const HostBuffer& other) const {
        if (this == &other) {
            std::lock_guard<std::mutex> guard(owner_lock_);
            return static_cast<bool>(state_);
        }
        std::scoped_lock guard(owner_lock_, other.owner_lock_);
        return state_ && state_ == other.state_;
    }

    HostBuffer take() {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->loans != 0) throw std::logic_error("cannot move borrowed buffer");
        HostBuffer replacement(std::move(state_));
        return replacement;
    }

    ReadLoan borrow_read() const {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->closed) throw std::logic_error("closed buffer");
        ++state_->loans;
        return ReadLoan(state_);
    }

    float read(std::uint64_t row, std::uint32_t field) const {
        auto loan = borrow_read();
        return loan.read(row, field);
    }

    void write(std::uint64_t row, std::uint32_t field, float value) {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->closed || state_->loans) throw std::logic_error("write to closed or borrowed buffer");
        const auto offset = state_->layout.byte_offset(row, field);
        std::memcpy(state_->bytes.data() + static_cast<std::size_t>(offset), &value, sizeof(value));
    }

    void close() {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        if (!state_) return; // relinquished ownership
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->loans != 0) throw std::logic_error("outstanding read loan");
        state_->closed = true;
    }

private:
    explicit HostBuffer(std::shared_ptr<State> state) : state_(std::move(state)) {}
    void active() const {
        // owner_lock_ is held by the caller before examining state_.
        if (!state_) throw std::logic_error("buffer moved or closed");
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->closed) throw std::logic_error("buffer moved or closed");
    }
    mutable std::mutex owner_lock_;
    std::shared_ptr<State> state_;
};

inline bool matching_layout(const Plan& left, const Plan& right) {
    return left.layout() == right.layout() && left.rows() == right.rows()
            && left.fields() == right.fields() && left.scalar_bytes() == right.scalar_bytes()
            && left.alignment() == right.alignment() && left.lanes() == right.lanes();
}

inline void execute_cpu(const Kernel& kernel, const HostBuffer& input, HostBuffer& output) {
    if (input.identical_storage(output)) throw std::invalid_argument("input/output alias forbidden");
    if (!matching_layout(kernel.layout, input.layout())
            || !matching_layout(kernel.layout, output.layout()))
        throw std::invalid_argument("kernel layout does not match owned buffers");
    auto loan = input.borrow_read();
    for (std::uint64_t row = 0; row < kernel.layout.rows(); ++row) {
        for (std::uint32_t field = 0; field < kernel.layout.fields(); ++field) {
            const auto v = loan.read(row, field);
            const auto result = kernel.operation == Op::Scale ? v * kernel.scale
                : kernel.operation == Op::Bias ? v + kernel.bias
                : (v * kernel.scale) + kernel.bias;
            output.write(row, field, result);
        }
    }
}

inline std::string cl_float(float value) {
    std::ostringstream result;
    // max_digits10 significant digits preserve every finite f32 constant,
    // including tiny subnormals that fixed decimal precision would round to zero.
    result << std::scientific << std::setprecision(std::numeric_limits<float>::max_digits10 - 1)
           << value << "f";
    return result.str();
}

inline OpenClArtifact lower_opencl(const Kernel& kernel, GpuTarget target) {
    const auto& p = kernel.layout;
    // Empty DOP shapes are valid CPU data but cannot allocate/launch GPU work.
    if (p.rows() == 0 || p.byte_length() == 0
            || target.address_bits != 64 || !target.float32
            || p.byte_length() > target.maximum_buffer_bytes
            || p.rows() > target.maximum_rows)
        throw std::invalid_argument("GPU target cannot admit kernel effects or buffer");
    std::string offset;
    switch (p.layout()) {
    case Layout::AoS:
        offset = "(row * " + std::to_string(align_up(checked_mul(p.fields(), 4), p.alignment()))
            + "UL + field * 4UL)";
        break;
    case Layout::SoA:
        offset = "(field * " + std::to_string(align_up(checked_mul(p.rows(), 4), p.alignment()))
            + "UL + row * 4UL)";
        break;
    case Layout::AoSoA:
        offset = "((row / " + std::to_string(p.lanes()) + "UL) * "
            + std::to_string(align_up(checked_mul(checked_mul(p.fields(), p.lanes()), 4), p.alignment()))
            + "UL + field * " + std::to_string(p.lanes() * 4) + "UL + (row % "
            + std::to_string(p.lanes()) + "UL) * 4UL)";
        break;
    }
    const auto rhs = kernel.operation == Op::Scale
        ? "(v * " + cl_float(kernel.scale) + ")"
        : kernel.operation == Op::Bias
        ? "(v + " + cl_float(kernel.bias) + ")"
        : "((v * " + cl_float(kernel.scale) + ") + " + cl_float(kernel.bias) + ")";
    const std::string name = "oreslang_dop_f32";
    std::string source = "#pragma OPENCL FP_CONTRACT OFF\n"
        "__kernel void " + name + "(__global const float* src, __global float* dst) {\n"
        "  const ulong row = (ulong)get_global_id(0);\n"
        "  const ulong field = (ulong)get_global_id(1);\n"
        "  if (row >= " + std::to_string(p.rows()) + "UL || field >= "
        + std::to_string(p.fields()) + "UL) return;\n"
        "  const ulong byteOffset = " + offset + ";\n"
        "  const ulong slot = byteOffset / 4UL;\n"
        "  const float v = src[slot];\n"
        "  dst[slot] = " + rhs + ";\n}\n";
    return OpenClArtifact{name, source, p.byte_length(), p.rows(), p.fields()};
}

} // namespace oreslang::dop
