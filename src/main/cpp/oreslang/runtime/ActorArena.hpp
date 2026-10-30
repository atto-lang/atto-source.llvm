#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace oreslang::runtime {

// One dedicated native allocation per actor incarnation. This bounds actor
// memory ownership but is NOT address-space, page-table, or OS isolation.
// No native pointer is returned to guest code or to untrusted callers.
class ActorArena final {
public:
    static constexpr std::uint32_t kScalarSlots = 16;
    static constexpr std::size_t kScalarPrefix =
        sizeof(std::int64_t) * kScalarSlots;

    ActorArena(std::uint64_t generation, std::uint32_t capacity)
        : generation_(generation), capacity_(capacity),
          storage_(capacity >= kScalarPrefix && generation != 0
                       ? std::make_unique<std::uint8_t[]>(capacity)
                       : throw std::invalid_argument("invalid actor arena size or generation")),
          next_(kScalarPrefix) {}
    ActorArena(const ActorArena&) = delete;
    ActorArena& operator=(const ActorArena&) = delete;
    ActorArena(ActorArena&&) = delete;
    ActorArena& operator=(ActorArena&&) = delete;
    ~ActorArena() noexcept { scrub(); }

    std::uint64_t generation() const noexcept { return generation_; }
    std::uint32_t capacity() const noexcept { return capacity_; }
    std::size_t used_bytes() const noexcept { return next_; }

    // Checked bump allocation; zero-byte requests, overflow and bad alignment
    // are rejected before consuming a single byte. Offset zero is reserved.
    std::uint32_t allocate(std::uint32_t size, std::uint32_t alignment = 1) {
        if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
            throw std::invalid_argument("actor arena allocation shape invalid");
        const std::size_t pad = (alignment - (next_ % alignment)) % alignment;
        if (next_ > capacity_ || pad > capacity_ - next_ ||
            size > capacity_ - next_ - pad)
            throw std::bad_alloc();
        const auto offset = next_ + pad;
        next_ = offset + size;
        return static_cast<std::uint32_t>(offset);
    }

    void write_byte(std::uint32_t offset, std::uint32_t size,
                    std::uint32_t index, std::uint8_t value) {
        check_range(offset, size, index);
        storage_[static_cast<std::size_t>(offset) + index] = value;
    }
    std::uint8_t read_byte(std::uint32_t offset, std::uint32_t size,
                           std::uint32_t index) const {
        check_range(offset, size, index);
        return storage_[static_cast<std::size_t>(offset) + index];
    }
    std::int64_t load_scalar(std::uint32_t slot) const {
        if (slot >= kScalarSlots) throw std::out_of_range("scalar slot invalid");
        std::int64_t value;
        std::memcpy(&value, storage_.get() + sizeof(value) * slot, sizeof(value));
        return value;
    }
    void store_scalar(std::uint32_t slot, std::int64_t value) {
        if (slot >= kScalarSlots) throw std::out_of_range("scalar slot invalid");
        std::memcpy(storage_.get() + sizeof(value) * slot, &value, sizeof(value));
    }

private:
    void check_range(std::uint32_t offset, std::uint32_t size,
                     std::uint32_t index) const {
        if (offset < kScalarPrefix || size == 0 || index >= size ||
            offset > next_ || size > next_ - offset)
            throw std::out_of_range("actor arena allocation outside live range");
    }
    void scrub() noexcept {
        if (!storage_) return;
        volatile std::uint8_t* bytes = storage_.get();
        for (std::size_t i = 0; i < capacity_; ++i) bytes[i] = 0;
    }
    const std::uint64_t generation_;
    const std::uint32_t capacity_;
    std::unique_ptr<std::uint8_t[]> storage_;
    std::size_t next_;
};

} // namespace oreslang::runtime
