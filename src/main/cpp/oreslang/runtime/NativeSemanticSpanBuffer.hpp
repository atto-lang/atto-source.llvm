#pragma once

#include "oreslang/runtime/NativeTraceParentV00.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace oreslang::runtime {

/**
 * Caller-owned fixed-capacity staging for immutable, sampled native spans.
 * C++17, no allocation, clock read, I/O, JIT, lock, guest object or OTLP
 * exporter. This is not linked to generated Atto code until LLVM probe ABI
 * and actor lifecycle lowering are implemented.
 */
struct NativeCompletedSpan final {
    NativeTraceParentV00 context{};
    std::array<std::uint8_t, 8> parent_span_id{};
    std::uint16_t operation_id = 0;
    std::uint8_t kind = 0; // OTLP INTERNAL=1, SERVER=2, CLIENT=3...
    std::uint64_t start_unix_ns = 0;
    std::uint64_t end_unix_ns = 0;
    std::uint16_t http_status = 0;
    bool failed = false;
};

struct NativeSemanticSpanSlot final {
    NativeCompletedSpan value{};
    std::atomic<std::uint8_t> published{0};
    NativeSemanticSpanSlot() noexcept = default;
    NativeSemanticSpanSlot(const NativeSemanticSpanSlot&) = delete;
    NativeSemanticSpanSlot& operator=(const NativeSemanticSpanSlot&) = delete;
};

class NativeSemanticSpanBuffer final {
public:
    NativeSemanticSpanBuffer(
            NativeSemanticSpanSlot* slots,
            std::uint32_t capacity) noexcept
        : slots_(slots), capacity_(capacity) {}

    NativeSemanticSpanBuffer(const NativeSemanticSpanBuffer&) = delete;
    NativeSemanticSpanBuffer& operator=(const NativeSemanticSpanBuffer&) = delete;

    [[nodiscard]] bool record(
            const NativeCompletedSpan& span,
            bool tracing_enabled) noexcept {
        // No clock, CAS, event allocation or pointer dereference when off.
        if (!tracing_enabled || !span.context.sampled()) return false;
        if (slots_ == nullptr || capacity_ == 0 || capacity_ > 65536
                || trace_all_zero(span.context.trace_id)
                || trace_all_zero(span.context.span_id)
                || span.parent_span_id == span.context.span_id
                || span.operation_id == 0
                || span.kind == 0 || span.kind > 5
                || span.start_unix_ns == 0
                || span.end_unix_ns < span.start_unix_ns
                || (span.http_status != 0 && span.http_status < 100)
                || span.http_status > 599) {
            return false;
        }

        std::uint32_t position = cursor_.load(std::memory_order_acquire);
        for (;;) {
            if (position >= capacity_) {
                increment_dropped();
                return false;
            }
            if (cursor_.compare_exchange_weak(
                    position, position + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                // Exactly one publisher owns this slot. A reader may observe
                // only published=1; reservation alone is not a record.
                slots_[position].value = span;
                slots_[position].published.store(
                    1, std::memory_order_release);
                return true;
            }
        }
    }

    [[nodiscard]] bool read(
            std::uint32_t index,
            NativeCompletedSpan& out) const noexcept {
        if (slots_ == nullptr || index >= capacity_
                || index >= cursor_.load(std::memory_order_acquire)
                || slots_[index].published.load(std::memory_order_acquire) != 1) {
            return false;
        }
        out = slots_[index].value;
        return true;
    }

    [[nodiscard]] std::uint32_t reserved_count() const noexcept {
        const auto count = cursor_.load(std::memory_order_acquire);
        return count > capacity_ ? capacity_ : count;
    }

    [[nodiscard]] std::uint32_t dropped_count() const noexcept {
        return dropped_.load(std::memory_order_acquire);
    }

private:
    void increment_dropped() noexcept {
        auto count = dropped_.load(std::memory_order_relaxed);
        while (count != UINT32_MAX && !dropped_.compare_exchange_weak(
                count, count + 1, std::memory_order_relaxed,
                std::memory_order_relaxed)) { }
    }

    NativeSemanticSpanSlot* const slots_;
    const std::uint32_t capacity_;
    std::atomic<std::uint32_t> cursor_{0};
    std::atomic<std::uint32_t> dropped_{0};
};

} // namespace oreslang::runtime
