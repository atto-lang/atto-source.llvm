#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace oreslang::runtime {

// Trusted-host proof of a bounded ETS-like set. Not a guest ABI or hard-RT
// implementation: mutex admission may park, and construction allocates.
// Principals MUST be established from authenticated VM/actor context.
class NativeEtsScalarSet final {
public:
    struct Principal {
        std::uint64_t runtime;
        std::uint64_t isolate;
        std::uint64_t actor;
        std::uint64_t generation;
        bool valid() const noexcept {
            return runtime && isolate && actor && generation;
        }
        bool operator==(const Principal& rhs) const noexcept {
            return runtime == rhs.runtime && isolate == rhs.isolate
                && actor == rhs.actor && generation == rhs.generation;
        }
    };
    enum class Protection : std::uint8_t { private_table, protected_table, public_table };
    enum class Status : std::uint8_t {
        ok, absent, exists, full, denied, retired, invalid, overflow
    };
    struct ReadResult { Status status; std::uint64_t value; };
    struct Handle { std::uint64_t incarnation; };

    NativeEtsScalarSet(Principal owner, Protection protection, std::size_t capacity)
        : owner_(owner), protection_(protection), slots_(capacity),
          incarnation_(next_incarnation()) {
        if (!owner.valid() || capacity == 0 || capacity > 1048576)
            throw std::invalid_argument("invalid ETS owner or capacity");
        if (protection != Protection::private_table &&
            protection != Protection::protected_table &&
            protection != Protection::public_table)
            throw std::invalid_argument("invalid ETS protection");
    }

    NativeEtsScalarSet(const NativeEtsScalarSet&) = delete;
    NativeEtsScalarSet& operator=(const NativeEtsScalarSet&) = delete;
    Handle handle() const noexcept { return Handle{incarnation_}; }
    std::size_t capacity() const noexcept { return slots_.size(); }

    // The caller-provided Principal is a trusted runtime assertion, NOT a
    // security token. Never expose these methods to an untrusted guest directly.
    Status insert(Handle h, Principal caller, std::uint64_t key,
                  std::uint64_t value, bool only_if_absent = false) {
        std::lock_guard<std::mutex> lock(mu_);
        auto status = authorize(h, caller, true);
        if (status != Status::ok) return status;
        Slot* vacant = nullptr;
        for (auto& slot : slots_) {
            if (slot.occupied && slot.key == key) {
                if (only_if_absent) return Status::exists;
                slot.value = value;
                return Status::ok;
            }
            if (!slot.occupied && !vacant) vacant = &slot;
        }
        if (!vacant) return Status::full;
        *vacant = Slot{true, key, value};
        return Status::ok;
    }

    ReadResult lookup(Handle h, Principal caller, std::uint64_t key) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto status = authorize(h, caller, false);
        if (status != Status::ok) return {status, 0};
        for (const auto& slot : slots_)
            if (slot.occupied && slot.key == key) return {Status::ok, slot.value};
        return {Status::absent, 0};
    }

    Status erase(Handle h, Principal caller, std::uint64_t key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto status = authorize(h, caller, true);
        if (status != Status::ok) return status;
        for (auto& slot : slots_) {
            if (slot.occupied && slot.key == key) {
                slot.occupied = false;
                return Status::ok;
            }
        }
        return Status::absent;
    }

    // Bounded atomic per-key counter under the table lock. Prevent wraparound.
    ReadResult increment(Handle h, Principal caller, std::uint64_t key,
                         std::uint64_t delta) {
        std::lock_guard<std::mutex> lock(mu_);
        auto status = authorize(h, caller, true);
        if (status != Status::ok) return {status, 0};
        for (auto& slot : slots_) {
            if (slot.occupied && slot.key == key) {
                if (slot.value > std::numeric_limits<std::uint64_t>::max() - delta)
                    return {Status::overflow, 0};
                slot.value += delta;
                return {Status::ok, slot.value};
            }
        }
        return {Status::absent, 0};
    }

    // Called by the trusted lifetime owner after actor termination is known.
    // Not automatic: VM integration must guarantee this callback on every
    // retirement path before claiming owner-linked ETS cleanup.
    bool retire_owner(Principal owner) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!(owner == owner_) || !live_) return false;
        live_ = false;
        for (auto& slot : slots_) slot = Slot{};
        return true;
    }

private:
    struct Slot {
        bool occupied = false;
        std::uint64_t key = 0;
        std::uint64_t value = 0;
    };

    static std::uint64_t next_incarnation() {
        static std::atomic<std::uint64_t> sequence{1};
        std::uint64_t current = sequence.load(std::memory_order_relaxed);
        for (;;) {
            if (!current || current == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("ETS incarnation exhausted");
            if (sequence.compare_exchange_weak(current, current + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                return current;
        }
    }

    Status authorize(Handle h, Principal caller, bool write) const noexcept {
        if (!live_) return Status::retired;
        if (h.incarnation != incarnation_ || !caller.valid()) return Status::invalid;
        if (caller.runtime != owner_.runtime || caller.isolate != owner_.isolate)
            return Status::denied;
        if (caller == owner_) return Status::ok;
        if (protection_ == Protection::private_table) return Status::denied;
        if (write && protection_ == Protection::protected_table) return Status::denied;
        return Status::ok;
    }

    const Principal owner_;
    const Protection protection_;
    std::vector<Slot> slots_;
    const std::uint64_t incarnation_;
    mutable std::mutex mu_;
    bool live_ = true;
};

} // namespace oreslang::runtime
