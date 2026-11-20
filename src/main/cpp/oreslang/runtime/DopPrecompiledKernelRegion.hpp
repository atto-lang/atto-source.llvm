#pragma once

#include "oreslang/runtime/DopCheckedRegion.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace oreslang::dop {

/**
 * Native host-only reference for a sealed AOT / governed-hybrid kernel table.
 * The selected function is machine code fixed at link time: no JIT, dynamic
 * library load, interpreter, executable-memory changes, or tracing collector.
 * A caller must already have exclusive actor execution authority. This type
 * owns its CheckedRegion, so there is no dangling external store pointer.
 */
class DopPrecompiledKernelRegion final {
public:
    using RowHandle = CheckedRegion::RowHandle;

    class Ticket final {
        friend class DopPrecompiledKernelRegion;
        std::uint64_t issuer_;
        std::uint64_t generation_;
        std::uint64_t region_epoch_;
        Layout layout_;
        constexpr Ticket(std::uint64_t issuer, std::uint64_t generation,
                         std::uint64_t region_epoch, Layout layout) noexcept
            : issuer_(issuer), generation_(generation),
              region_epoch_(region_epoch), layout_(layout) {}
    public:
        Ticket(const Ticket&) = default;
        Ticket& operator=(const Ticket&) = default;
    };

    explicit DopPrecompiledKernelRegion(Plan plan, bool sealed = true)
        : region_(plan), issuer_(next_issuer()), sealed_(sealed),
          read_kernel_(read_for(plan.layout())),
          write_kernel_(write_for(plan.layout())) {
        if (plan.scalar_bytes() != sizeof(std::uint32_t))
            throw std::invalid_argument("precompiled u32 kernel requires 4-byte fields");
    }

    DopPrecompiledKernelRegion(const DopPrecompiledKernelRegion&) = delete;
    DopPrecompiledKernelRegion& operator=(const DopPrecompiledKernelRegion&) = delete;
    DopPrecompiledKernelRegion(DopPrecompiledKernelRegion&&) = delete;
    DopPrecompiledKernelRegion& operator=(DopPrecompiledKernelRegion&&) = delete;

    [[nodiscard]] Layout layout() const noexcept { return region_.layout(); }
    [[nodiscard]] bool sealed() const noexcept { return sealed_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t rows() const noexcept { return region_.rows(); }
    [[nodiscard]] std::uint32_t fields() const noexcept { return region_.fields(); }
    [[nodiscard]] std::size_t storage_bytes() const noexcept {
        return region_.storage_bytes();
    }
    [[nodiscard]] Ticket ticket() const noexcept {
        return Ticket{issuer_, generation_, region_.epoch(), region_.layout()};
    }
    [[nodiscard]] RowHandle row(std::uint64_t index) const { return region_.row(index); }

    [[nodiscard]] std::uint32_t read(Ticket permit, RowHandle handle,
                                     std::uint32_t field) const {
        check(permit);
        return read_kernel_(region_, handle, field);
    }
    void write(Ticket permit, RowHandle handle, std::uint32_t field,
               std::uint32_t value) {
        check(permit);
        write_kernel_(region_, handle, field, value);
    }

    // Structural changes are an explicit trusted operation, never a reaction
    // to a telemetry hint. CheckedRegion copies before publishing a new epoch.
    // Failure preserves *all* old tickets and values; success retires them.
    // No runtime code generation or heap work is performed by read/write.
    void relayout(Plan next) {
        if (sealed_) throw std::logic_error("sealed AOT layout cannot change");
        if (next.scalar_bytes() != sizeof(std::uint32_t))
            throw std::invalid_argument("precompiled u32 kernel requires 4-byte fields");
        if (generation_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("precompiled dispatch generation exhausted");
        const auto new_read = read_for(next.layout());
        const auto new_write = write_for(next.layout());
        region_.relayout(std::move(next)); // can throw; previous state remains valid
        read_kernel_ = new_read;
        write_kernel_ = new_write;
        ++generation_;
    }

private:
    using ReadKernel = std::uint32_t (*)(const CheckedRegion&, RowHandle,
                                         std::uint32_t);
    using WriteKernel = void (*)(CheckedRegion&, RowHandle, std::uint32_t,
                                  std::uint32_t);

    template<Layout L>
    static std::uint32_t read_known(const CheckedRegion& store, RowHandle row,
                                    std::uint32_t field) {
        if (store.layout() != L) throw std::logic_error("AOT read kernel/layout mismatch");
        return store.read<std::uint32_t>(row, field);
    }
    template<Layout L>
    static void write_known(CheckedRegion& store, RowHandle row,
                            std::uint32_t field, std::uint32_t value) {
        if (store.layout() != L) throw std::logic_error("AOT write kernel/layout mismatch");
        store.write<std::uint32_t>(row, field, value);
    }
    static ReadKernel read_for(Layout layout) {
        switch (layout) {
        case Layout::AoS: return &read_known<Layout::AoS>;
        case Layout::SoA: return &read_known<Layout::SoA>;
        case Layout::AoSoA: return &read_known<Layout::AoSoA>;
        }
        throw std::invalid_argument("unsupported AOT layout");
    }
    static WriteKernel write_for(Layout layout) {
        switch (layout) {
        case Layout::AoS: return &write_known<Layout::AoS>;
        case Layout::SoA: return &write_known<Layout::SoA>;
        case Layout::AoSoA: return &write_known<Layout::AoSoA>;
        }
        throw std::invalid_argument("unsupported AOT layout");
    }
    static std::uint64_t next_issuer() {
        static std::atomic<std::uint64_t> next{1};
        auto value = next.load(std::memory_order_relaxed);
        while (true) {
            if (value == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("precompiled dispatch issuer exhausted");
            if (next.compare_exchange_weak(value, value + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                return value;
        }
    }
    void check(const Ticket& permit) const {
        if (permit.issuer_ != issuer_ || permit.generation_ != generation_ ||
            permit.region_epoch_ != region_.epoch() ||
            permit.layout_ != region_.layout())
            throw std::logic_error("stale or foreign precompiled dispatch ticket");
    }

    CheckedRegion region_;
    const std::uint64_t issuer_;
    const bool sealed_;
    std::uint64_t generation_ = 1;
    ReadKernel read_kernel_;
    WriteKernel write_kernel_;
};

} // namespace oreslang::dop
