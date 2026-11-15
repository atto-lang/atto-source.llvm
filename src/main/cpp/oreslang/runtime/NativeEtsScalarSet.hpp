#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
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
        ok, absent, exists, full, denied, retired, invalid, overflow, conflict
    };
    struct ReadResult { Status status; std::uint64_t value; };
    struct VersionedRead { Status status; std::uint64_t value; std::uint64_t version; };
    struct Handle { std::uint64_t incarnation; };
    // An opaque trusted-runtime receipt for an owner-death handoff.
    // Numeric fields are identity/fencing, NOT guest authentication grants.
    struct HeirReceipt {
        Handle table;
        Principal deceased_owner;
        Principal successor;
        std::uint64_t table_epoch;
        std::uint64_t receipt_nonce;
    };

    NativeEtsScalarSet(Principal owner, Protection protection, std::size_t capacity)
        : owner_(owner), protection_(protection),
          slots_(checked_capacity(owner, protection, capacity)),
          incarnation_(next_incarnation()) {}

    NativeEtsScalarSet(const NativeEtsScalarSet&) = delete;
    NativeEtsScalarSet& operator=(const NativeEtsScalarSet&) = delete;
    Handle handle() const noexcept { return Handle{incarnation_}; }
    std::size_t capacity() const noexcept { return slots_.size(); }

    std::uint64_t table_epoch() const {
        std::lock_guard<std::mutex> lock(mu_);
        return table_epoch_;
    }

    // Trusted supervisor only: the actor's physical death and successor's
    // live registration must be independently authenticated by the OresVM.
    // The table is fenced immediately; neither old owner nor heir nor PUBLIC
    // peers may read/write until the single-use receipt is acknowledged.
    // No guest-supplied principal or nonce grants ownership.
    std::optional<HeirReceipt> retire_owner_with_heir(
            Principal deceased, Principal heir) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!live_ || handoff_ || !(deceased == owner_)
            || !heir.valid() || heir == deceased
            || heir.runtime != owner_.runtime || heir.isolate != owner_.isolate
            || table_epoch_ == std::numeric_limits<std::uint64_t>::max()
            || revoked_count_ == revoked_.size())
            return std::nullopt;
        for (std::size_t n = 0; n < revoked_count_; ++n)
            if (heir == revoked_[n]) return std::nullopt;
        // No user callback or memory allocation occurs while the lock is held.
        const HeirReceipt ticket{
            Handle{incarnation_}, deceased, heir,
            table_epoch_ + 1, next_incarnation()
        };
        handoff_ = ticket;
        retired_owner_confirmed_ = false;
        return ticket;
    }

    // The OresVM must call this only after independently authenticating
    // that the physical old actor's turn/Task/FFI leases are terminated and
    // its actor registry entry has retired. A caller-supplied Principal is
    // NOT that authentication. This flag prevents the host proof from
    // accidentally accepting an ACK while an old worker can still write.
    bool confirm_owner_retired(
            HeirReceipt receipt, Principal verified_deceased) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!live_ || !handoff_ || retired_owner_confirmed_
            || !(verified_deceased == handoff_->deceased_owner)
            || !receipt_matches(receipt, *handoff_))
            return false;
        retired_owner_confirmed_ = true;
        return true;
    }

    // Trusted CONTROL ACK: the owner is swapped only after the externally
    // authenticated successor is ready. No dual-writer interval exists.
    bool acknowledge_heir(HeirReceipt receipt, Principal verified_heir) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!live_ || !handoff_ || !retired_owner_confirmed_
            || !(verified_heir == handoff_->successor)
            || !receipt_matches(receipt, *handoff_))
            return false;
        revoked_[revoked_count_++] = owner_;
        owner_ = verified_heir;
        table_epoch_ = handoff_->table_epoch;
        handoff_.reset();
        retired_owner_confirmed_ = false;
        return true;
    }

    // A failed/expired handoff must close the table, not reopen deceased-owner
    // DATA or accidentally grant the heir a write lease.
    bool abandon_heir(HeirReceipt receipt) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!handoff_ || !receipt_matches(receipt, *handoff_)) return false;
        handoff_.reset();
        retired_owner_confirmed_ = false;
        live_ = false;
        for (auto& slot : slots_) slot = Slot{};
        return true;
    }

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
                if (!advance_version()) return Status::overflow;
                slot.value = value;
                slot.version = version_;
                return Status::ok;
            }
            if (!slot.occupied && !vacant) vacant = &slot;
        }
        if (!vacant) return Status::full;
        if (!advance_version()) return Status::overflow;
        *vacant = Slot{true, key, value, version_};
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
                if (!advance_version()) return Status::overflow;
                slot.occupied = false;
                slot.version = version_;
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
                if (!advance_version()) return {Status::overflow, 0};
                slot.value += delta;
                slot.version = version_;
                return {Status::ok, slot.value};
            }
        }
        return {Status::absent, 0};
    }

    // Each new version is unique for this table incarnation, even across
    // erase/reinsert and slot reuse. A stale value-only CAS cannot bypass ABA.
    VersionedRead lookup_versioned(Handle h, Principal caller,
                                   std::uint64_t key) const {
        std::lock_guard<std::mutex> lock(mu_);
        const auto status = authorize(h, caller, false);
        if (status != Status::ok) return {status, 0, 0};
        for (const auto& slot : slots_) {
            if (slot.occupied && slot.key == key)
                return {Status::ok, slot.value, slot.version};
        }
        return {Status::absent, 0, 0};
    }

    // Returns conflict instead of modifying data if either the old value or
    // immutable entry-version differs. An exhausted version counter fails
    // closed before writing.
    Status compare_exchange_versioned(Handle h, Principal caller,
                                      std::uint64_t key, std::uint64_t expected,
                                      std::uint64_t expected_version,
                                      std::uint64_t desired) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto status = authorize(h, caller, true);
        if (status != Status::ok) return status;
        if (!expected_version) return Status::invalid;
        for (auto& slot : slots_) {
            if (!slot.occupied || slot.key != key) continue;
            if (slot.value != expected || slot.version != expected_version)
                return Status::conflict;
            if (!advance_version()) return Status::overflow;
            slot.value = desired;
            slot.version = version_;
            return Status::ok;
        }
        return Status::absent;
    }

    // Called by the trusted lifetime owner after actor termination is known.
    // Not automatic: VM integration must guarantee this callback on every
    // retirement path before claiming owner-linked ETS cleanup.
    bool retire_owner(Principal owner) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!(owner == owner_) || !live_ || handoff_) return false;
        live_ = false;
        for (auto& slot : slots_) slot = Slot{};
        return true;
    }

private:
    static std::size_t checked_capacity(Principal owner, Protection protection,
                                        std::size_t capacity) {
        if (!owner.valid() || capacity == 0 || capacity > 1048576)
            throw std::invalid_argument("invalid ETS owner or capacity");
        if (protection != Protection::private_table &&
            protection != Protection::protected_table &&
            protection != Protection::public_table)
            throw std::invalid_argument("invalid ETS protection");
        return capacity;
    }

    static bool receipt_matches(
            const HeirReceipt& a, const HeirReceipt& b) noexcept {
        return a.table.incarnation == b.table.incarnation
            && a.deceased_owner == b.deceased_owner
            && a.successor == b.successor
            && a.table_epoch == b.table_epoch
            && a.receipt_nonce == b.receipt_nonce;
    }

    struct Slot {
        bool occupied = false;
        std::uint64_t key = 0;
        std::uint64_t value = 0;
        std::uint64_t version = 0;
    };

    bool advance_version() noexcept {
        if (version_ == std::numeric_limits<std::uint64_t>::max()) return false;
        ++version_;
        return true;
    }

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
        if (handoff_) return Status::denied;
        if (h.incarnation != incarnation_ || !caller.valid()) return Status::invalid;
        if (caller.runtime != owner_.runtime || caller.isolate != owner_.isolate)
            return Status::denied;
        for (std::size_t n = 0; n < revoked_count_; ++n)
            if (caller == revoked_[n]) return Status::denied;
        if (caller == owner_) return Status::ok;
        if (protection_ == Protection::private_table) return Status::denied;
        if (write && protection_ == Protection::protected_table) return Status::denied;
        return Status::ok;
    }

    Principal owner_;
    const Protection protection_;
    std::vector<Slot> slots_;
    const std::uint64_t incarnation_;
    mutable std::mutex mu_;
    bool live_ = true;
    std::uint64_t version_ = 0;
    std::uint64_t table_epoch_ = 1;
    std::optional<HeirReceipt> handoff_;
    bool retired_owner_confirmed_ = false;
    // Bounded revoked owner identities prevent a stale PUBLIC actor from
    // regaining access after a later heir receives the same table.
    // At capacity, further owner transfer fails closed until the runtime
    // supplies an authenticated generation registry/compaction protocol.
    std::array<Principal, 64> revoked_{};
    std::size_t revoked_count_ = 0;
};

} // namespace oreslang::runtime
