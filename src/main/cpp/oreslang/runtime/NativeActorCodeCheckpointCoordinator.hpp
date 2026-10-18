#pragma once

#include "oreslang/runtime/ActorCheckpointEnvelopeV1.hpp"
#include "oreslang/runtime/NativeActorHotUpgradeGate.hpp"
#include "oreslang/runtime/VersionedActorCell.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * HOST-ONLY composition proof: ONE NativeActorHotUpgradeGate owns admission,
 * code-generation lifetime, and publication. The immutable code image owns its
 * checkpoint manifest, so a newly admitted turn cannot observe an old manifest
 * after a generation switch. The separately owned actor state cell is never
 * replaced by an implicit code-only upgrade.
 *
 * Not the LLVM source actor scheduler, signed loader, or schema migration API.
 * Behavior pointers here are trusted, statically compiled C++ test callbacks.
 * A production scheduler must pin every resumed await/FFI/async continuation.
 */
class NativeActorCodeCheckpointCoordinator final {
public:
    using Wire = ActorCheckpointEnvelopeV1;
    using Behavior = std::int64_t (*)(std::int64_t, std::int64_t);
    using Restore = std::int64_t (*)(std::int64_t);
    struct Image final {
        Behavior invoke = nullptr;
        Wire::Header manifest{};
    };
    using Gate = NativeActorHotUpgradeGate<Image>;
    using Shape = Gate::Shape;

private:
    ActorHeapIdentity identity_;
    const Wire::ActorId actor_id_;
    const Shape shape_;
    Gate gate_;
    ::oreslang::VersionedActorCell state_;
    Wire::Sha256 sha256_;

    static std::vector<std::uint8_t> scalar(std::int64_t value) {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "int64 ABI mismatch");
        std::memcpy(&bits, &value, sizeof(bits));
        std::vector<std::uint8_t> result(8);
        for (int index = 7; index >= 0; --index)
            result[7 - index] = static_cast<std::uint8_t>(bits >> (8 * index));
        return result;
    }
    static std::int64_t decode_scalar(const std::vector<std::uint8_t>& value) {
        if (value.size() != 8)
            throw std::invalid_argument("code-only checkpoint must contain exactly one int64");
        std::uint64_t bits = 0;
        for (std::uint8_t byte : value) bits = (bits << 8) | byte;
        std::int64_t out = 0;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }
    static void compatible(const Wire::Header& old,
                           const Wire::Header& next) {
        if (next.actor_id != old.actor_id
            || next.incarnation != old.incarnation
            || next.state_schema_digest != old.state_schema_digest
            || next.message_abi_digest != old.message_abi_digest
            || next.capability_digest != old.capability_digest)
            throw std::invalid_argument("code-only upgrade changes actor/schema/wire/capabilities");
        if (next.code_digest == old.code_digest)
            throw std::invalid_argument("code-only upgrade must change code image digest");
    }
    struct StagedImage final {
        // callback declared before image so teardown precedes reclamation.
        std::function<void()> reclaim;
        Image image;
        explicit StagedImage(Image candidate, std::function<void()> cleanup)
            : reclaim(std::move(cleanup)), image(std::move(candidate)) {}
        StagedImage(const StagedImage&) = delete;
        StagedImage& operator=(const StagedImage&) = delete;
        ~StagedImage() noexcept {
            try { if (reclaim) reclaim(); } catch (...) {}
        }
    };

public:
    NativeActorCodeCheckpointCoordinator(
            ActorHeapIdentity identity, Wire::ActorId actor_id, Shape shape,
            Image initial, std::int64_t initial_state,
            Wire::Sha256 sha256, std::function<void()> reclaim)
        : identity_(identity), actor_id_(actor_id), shape_(std::move(shape)),
          gate_(identity, shape_, std::move(initial), std::move(reclaim)),
          state_(initial_state), sha256_(sha256) {
        if (!sha256_ || actor_id_ != gate_.pin(identity_).code().manifest.actor_id)
            throw std::invalid_argument("invalid checkpoint SHA-256 provider or actor identity");
    }

    NativeActorCodeCheckpointCoordinator(
            const NativeActorCodeCheckpointCoordinator&) = delete;
    NativeActorCodeCheckpointCoordinator& operator=(
            const NativeActorCodeCheckpointCoordinator&) = delete;

    std::int64_t value() const {
        return state_.snapshot().value();
    }
    Gate::Descriptor current() {
        auto lease = gate_.pin(identity_);
        return lease.descriptor();
    }
    Wire::Header manifest() {
        auto lease = gate_.pin(identity_);
        return lease.code().manifest;
    }
    std::vector<std::uint8_t> snapshot_frame() {
        auto lease = gate_.pin(identity_);
        return Wire::encode({lease.code().manifest,
                             scalar(state_.snapshot().value())}, sha256_);
    }

    void deliver(std::int64_t delta) {
        // The lease pins both the exact executable generation and its manifest.
        auto active = gate_.pin(identity_);
        auto snapshot = state_.snapshot();
        if (!active.code().invoke)
            throw std::logic_error("null compiled actor behavior");
        const auto next = active.code().invoke(snapshot.value(), delta);
        if (state_.try_commit(snapshot, next)
                != ::oreslang::VersionedActorCell::Commit::Committed)
            throw std::logic_error("actor DATA delivery lost single-turn state CAS");
    }

    /**
     * A false result is a busy old turn; the staged candidate is reclaimed
     * exactly once and the old DATA admission resumes. This method does not
     * persist a replacement state arena: exact-schema code-only upgrade only.
     * All callbacks run without a supervisor/CONTROL mutex held.
     */
    bool try_upgrade(std::uint64_t expected_revision, Image candidate,
                     Restore restore, std::function<void()> reclaim) {
        StagedImage staged(std::move(candidate), std::move(reclaim));
        if (!staged.image.invoke || !restore)
            throw std::invalid_argument("null code image or state restore callback");
        auto old_lease = gate_.pin(identity_);
        const auto old_manifest = old_lease.code().manifest;
        if (old_lease.descriptor().revision != expected_revision)
            throw std::logic_error("stale code generation");
        old_lease.close();

        // Only the existing code gate is authoritative for admission/freeze.
        auto ticket = gate_.freeze(identity_, expected_revision);
        if (!gate_.quiesced(ticket)) return false;
        compatible(old_manifest, staged.image.manifest);

        const auto before = state_.snapshot().value();
        auto checkpoint = Wire::encode({old_manifest, scalar(before)}, sha256_);
        auto decoded = Wire::decode(checkpoint, old_manifest, sha256_);
        const auto recovered = restore(decode_scalar(decoded.state_bytes));
        if (recovered != before)
            throw std::logic_error("code-only restore changed the owned actor state");

        (void)gate_.promote(ticket, shape_, std::move(staged.image),
                            std::move(staged.reclaim));
        return true;
    }

    void close() noexcept { gate_.close(); }
};

}  // namespace oreslang::runtime
