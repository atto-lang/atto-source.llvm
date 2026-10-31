#pragma once

#include "oreslang/runtime/DbWalOwnerRetirementGate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace oreslang::runtime {

// Host-only, fixed-capacity context-owned WAL shard authority registry.
// No allocator or GC; mutex operations are CONTROL-plane and NOT hard-RT.
// The caller MUST authenticate tenant membership, current actor incarnation,
// and the already-open regular file (fstat device/inode) BEFORE issue().
// Raw integer parameters alone do NOT grant access; there is no guest ABI.
template <std::size_t Capacity = 32>
class DbWalShardGrantRegistry final {
    static_assert(Capacity > 0 && Capacity <= 1024, "bounded WAL registry");
public:
    struct FileIdentity {
        std::uint64_t device;
        std::uint64_t inode;
        bool valid() const noexcept { return device != 0 || inode != 0; }
        bool operator==(const FileIdentity& other) const noexcept {
            return device == other.device && inode == other.inode;
        }
    };

    class Grant final {
        friend class DbWalShardGrantRegistry;
    public:
        Grant(const Grant&) = delete;
        Grant& operator=(const Grant&) = delete;
        Grant(Grant&& other) noexcept
            : issuer_(other.issuer_), index_(other.index_), epoch_(other.epoch_) {
            other.issuer_ = nullptr;
        }
        // Never overwrite a live authority and orphan its reserved slot.
        // Move construction for std::optional<Grant> is supported; assignment
        // is deliberately forbidden even if the destination looks empty.
        Grant& operator=(Grant&&) = delete;
        bool attached() const noexcept { return issuer_ != nullptr; }
    private:
        Grant(const DbWalShardGrantRegistry* issuer, std::size_t index,
                std::uint64_t epoch) noexcept
            : issuer_(issuer), index_(index), epoch_(epoch) {}
        const DbWalShardGrantRegistry* issuer_ = nullptr;
        std::size_t index_ = 0;
        std::uint64_t epoch_ = 0;
    };

    explicit DbWalShardGrantRegistry(std::uint64_t context_id)
        : context_id_(context_id) {
        if (!context_id) throw std::invalid_argument("WAL context required");
    }
    DbWalShardGrantRegistry(const DbWalShardGrantRegistry&) = delete;
    DbWalShardGrantRegistry& operator=(const DbWalShardGrantRegistry&) = delete;

    // One ACTIVE or DRAINING holder per logical shard and per physical file.
    // The bound gate MUST outlive the grant and registry slot.
    std::optional<Grant> issue(std::uint64_t context_id,
            std::uint32_t tenant_id, std::uint32_t shard_id,
            std::uint64_t actor_id, std::uint32_t generation,
            FileIdentity file, DbWalOwnerRetirementGate& physical_gate) {
        std::lock_guard<std::mutex> lock(mu_);
        if (sealed_ || context_id != context_id_ || tenant_id == 0
                || actor_id == 0 || generation == 0 || !file.valid()
                || !physical_gate.owns(actor_id, generation)
                || physical_gate.phase() != DbWalOwnerRetirementGate::Phase::open
                || physical_gate.active() || next_epoch_ == 0)
            return std::nullopt;
        std::size_t vacant = Capacity;
        for (std::size_t i = 0; i < Capacity; ++i) {
            const auto& slot = slots_[i];
            if (!slot.used) { if (vacant == Capacity) vacant = i; continue; }
            if ((slot.tenant == tenant_id && slot.shard == shard_id)
                    || slot.file == file) return std::nullopt;
        }
        if (vacant == Capacity) return std::nullopt;
        Slot& slot = slots_[vacant];
        slot = Slot{true, tenant_id, shard_id, actor_id, generation,
                file, next_epoch_, &physical_gate, Phase::active};
        const std::uint64_t issued = next_epoch_++;
        // Exhausting the epoch space permanently fails new issuance rather
        // than allowing reuse of a stale grant. Existing slots can retire.
        return Grant(this, vacant, issued);
    }

    bool can_write(const Grant& grant, std::uint64_t context_id,
            std::uint32_t tenant_id, std::uint32_t shard_id,
            std::uint64_t actor_id, std::uint32_t generation,
            FileIdentity file) const {
        std::lock_guard<std::mutex> lock(mu_);
        const Slot* s = lookup(grant);
        return s && !sealed_ && context_id == context_id_
            && s->phase == Phase::active && s->tenant == tenant_id
            && s->shard == shard_id && s->actor == actor_id
            && s->generation == generation && s->file == file
            && s->gate->owns(actor_id, generation)
            && s->gate->phase() == DbWalOwnerRetirementGate::Phase::open;
    }

    /**
     * AUTHORITATIVE admission for one physical write. A separate can_write()
     * check is only diagnostic; checking then admitting with a second call
     * would permit drain to win between those steps. Holding the registry
     * lock while taking the bound gate's active slot makes drain/write
     * ordering linearizable. No allocation, but NOT safe on hard-RT carriers.
     */
    bool begin_write(const Grant& grant, std::uint64_t context_id,
            std::uint32_t tenant_id, std::uint32_t shard_id,
            std::uint64_t actor_id, std::uint32_t generation,
            FileIdentity file) {
        std::lock_guard<std::mutex> lock(mu_);
        Slot* s = lookup(grant);
        if (!s || sealed_ || context_id != context_id_
                || s->phase != Phase::active
                || s->tenant != tenant_id || s->shard != shard_id
                || s->actor != actor_id || s->generation != generation
                || !(s->file == file)
                || !s->gate->owns(actor_id, generation))
            return false;
        return s->gate->admit(actor_id, generation);
    }

    /**
     * Only the trusted actual I/O worker / physical pin retirement may
     * release this slot. A cancelled logical Future is not that receipt.
     * A DRAINING owner can retire a previously admitted physical write.
     */
    bool physical_write_retired(const Grant& grant,
            std::uint64_t context_id, std::uint64_t actor_id,
            std::uint32_t generation) {
        std::lock_guard<std::mutex> lock(mu_);
        Slot* s = lookup(grant);
        if (!s || context_id != context_id_
                || s->actor != actor_id || s->generation != generation
                || !s->gate->owns(actor_id, generation)
                || !s->gate->active()) return false;
        s->gate->physical_write_retired(actor_id, generation);
        return true;
    }

    // Drain irrevocably seals writes but keeps the shard/file reserved.
    bool drain(const Grant& grant, std::uint64_t context_id,
            std::uint64_t actor_id, std::uint32_t generation) {
        std::lock_guard<std::mutex> lock(mu_);
        Slot* s = lookup(grant);
        if (!s || context_id != context_id_ || s->actor != actor_id
                || s->generation != generation || s->phase == Phase::closing)
            return false;
        s->phase = Phase::draining;
        return s->gate->request_drain(actor_id, generation);
    }

    bool begin_close(const Grant& grant, std::uint64_t context_id,
            std::uint64_t actor_id, std::uint32_t generation) {
        std::lock_guard<std::mutex> lock(mu_);
        Slot* s = lookup(grant);
        if (!s || context_id != context_id_ || s->actor != actor_id
                || s->generation != generation || s->phase != Phase::draining
                || !s->gate->begin_close(actor_id, generation))
            return false;
        s->phase = Phase::closing;
        return true;
    }

    // Called by trusted physical file/loan retirement code, not DATA.
    // A matching previously bound gate must report physical close and zero
    // active loans. The registry itself does not execute close()/fsync().
    bool retire_after_physical_close(const Grant& grant,
            const DbWalOwnerRetirementGate& bound_gate) {
        std::lock_guard<std::mutex> lock(mu_);
        Slot* s = lookup(grant);
        if (!s || s->phase != Phase::closing || s->gate != &bound_gate
                || bound_gate.phase() != DbWalOwnerRetirementGate::Phase::closed
                || bound_gate.active()) return false;
        *s = Slot{};
        return true;
    }

    void seal() {
        std::lock_guard<std::mutex> lock(mu_);
        sealed_ = true;
    }

    std::size_t outstanding() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t n = 0;
        for (const auto& s : slots_) if (s.used) ++n;
        return n;
    }

private:
    enum class Phase { active, draining, closing };
    struct Slot {
        bool used = false;
        std::uint32_t tenant = 0;
        std::uint32_t shard = 0;
        std::uint64_t actor = 0;
        std::uint32_t generation = 0;
        FileIdentity file{};
        std::uint64_t epoch = 0;
        DbWalOwnerRetirementGate* gate = nullptr;
        Phase phase = Phase::active;
    };

    Slot* lookup(const Grant& grant) {
        if (grant.issuer_ != this || grant.index_ >= Capacity) return nullptr;
        Slot& s = slots_[grant.index_];
        return s.used && s.epoch == grant.epoch_ ? &s : nullptr;
    }
    const Slot* lookup(const Grant& grant) const {
        if (grant.issuer_ != this || grant.index_ >= Capacity) return nullptr;
        const Slot& s = slots_[grant.index_];
        return s.used && s.epoch == grant.epoch_ ? &s : nullptr;
    }

    const std::uint64_t context_id_;
    mutable std::mutex mu_;
    std::array<Slot, Capacity> slots_{};
    std::uint64_t next_epoch_ = 1;
    bool sealed_ = false;
};

} // namespace oreslang::runtime
