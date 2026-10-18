#pragma once

#include "ActorCheckpointEnvelopeV1.hpp"
#include "ActorHotUpgradeGate.hpp"
#include "VersionedActorCell.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * HOST-ONLY deterministic native transaction proof, NOT a compiled Oreslang
 * actor, supervisor, mailbox router, or source-generated AOT upgrade.
 *
 * The caller is responsible for fencing ingress and every external resource
 * before invoking upgrade. The gate refuses publication while turn pins live.
 * No native callback/guest code is invoked while the router mutex is held.
 */
class NativeCounterUpgradeProof final {
public:
    using Image = ActorHotUpgradeGate::Image;
    using Sha256 = ActorCheckpointEnvelopeV1::Sha256;
    using Behavior = std::int64_t (*)(std::int64_t, std::int64_t);
    using Restore = std::int64_t (*)(std::int64_t);

private:
    struct Version {
        Version(std::int64_t value, Behavior code)
            : state(value), apply(code) {
            if (!apply) throw std::invalid_argument("null counter behavior");
        }
        ::oreslang::VersionedActorCell state;
        Behavior apply;
    };

    mutable std::mutex mutex_;
    ActorHotUpgradeGate gate_;
    std::shared_ptr<Version> active_;
    const ActorCheckpointEnvelopeV1::ActorId actor_uuid_;
    Sha256 sha256_;

    static std::vector<std::uint8_t> scalar(std::int64_t value) {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "int64 ABI mismatch");
        std::memcpy(&bits, &value, sizeof(bits));
        std::vector<std::uint8_t> bytes(8);
        for (int i = 7; i >= 0; --i) {
            bytes[7 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
        }
        return bytes;
    }

    static std::int64_t read_scalar(const std::vector<std::uint8_t>& bytes) {
        if (bytes.size() != 8)
            throw std::invalid_argument("counter checkpoint requires one int64");
        std::uint64_t bits = 0;
        for (std::uint8_t byte : bytes) bits = (bits << 8) | byte;
        std::int64_t result = 0;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    static ActorCheckpointEnvelopeV1::Digest parse_hex(const std::string& value) {
        if (value.size() != 64)
            throw std::invalid_argument("invalid code/schema digest length");
        ActorCheckpointEnvelopeV1::Digest out{};
        for (std::size_t i = 0; i < 32; ++i) {
            const auto nibble = [](char c) -> std::uint8_t {
                if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
                if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
                throw std::invalid_argument("noncanonical digest character");
            };
            out[i] = static_cast<std::uint8_t>((nibble(value[i * 2]) << 4)
                                                    | nibble(value[i * 2 + 1]));
        }
        return out;
    }

public:
    NativeCounterUpgradeProof(std::uint64_t logical_id,
                              ActorCheckpointEnvelopeV1::ActorId actor_uuid,
                              Image initial,
                              std::int64_t initial_value,
                              Behavior first,
                              Sha256 sha256)
        : gate_(logical_id, std::move(initial)),
          active_(std::make_shared<Version>(initial_value, first)),
          actor_uuid_(actor_uuid),
          sha256_(sha256) {
        if (!sha256_) throw std::invalid_argument("trusted SHA-256 required");
    }
    NativeCounterUpgradeProof(const NativeCounterUpgradeProof&) = delete;
    NativeCounterUpgradeProof& operator=(const NativeCounterUpgradeProof&) = delete;

    std::uint64_t epoch() const { return gate_.epoch(); }
    Image image() const { return gate_.image(); }

    std::int64_t value() const {
        std::shared_ptr<Version> version;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            version = active_;
        }
        return version->state.snapshot().value();
    }

    void deliver(std::int64_t message) {
        auto pin = gate_.pin();
        std::shared_ptr<Version> version;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            version = active_;
        }
        auto previous = version->state.snapshot();
        const auto updated = version->apply(previous.value(), message);
        if (version->state.try_commit(previous, updated)
                != ::oreslang::VersionedActorCell::Commit::Committed)
            throw std::logic_error("counter state changed during pinned actor turn");
    }

    /**
     * Nonblocking prepare/restore/publish. A false result means old work is
     * still pinned; the original generation remains fully usable.
     * Rollback before publication is strict and idempotent.
     */
    bool upgrade(const Image& replacement, Behavior next, Restore restore) {
        if (!next || !restore) throw std::invalid_argument("null replacement/restorer");
        auto candidate = gate_.prepare(replacement);
        bool published = false;
        try {
            if (gate_.pins() != 0) {
                gate_.abort(candidate);
                return false;
            }

            std::shared_ptr<Version> previous;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                previous = active_;
            }
            auto state = previous->state.snapshot();
            const auto before = gate_.image();
            ActorCheckpointEnvelopeV1::Header header{
                actor_uuid_, gate_.epoch(), parse_hex(before.code_sha256),
                parse_hex(before.state_sha256),
                parse_hex(before.message_abi_sha256),
                parse_hex(before.capability_sha256)};
            const auto frame = ActorCheckpointEnvelopeV1::encode(
                {header, scalar(state.value())}, sha256_);
            auto verified = ActorCheckpointEnvelopeV1::decode(frame, header, sha256_);
            const std::int64_t restored = restore(read_scalar(verified.state_bytes));
            auto next_version = std::make_shared<Version>(restored, next);

            // Pin admission remains frozen by ActorHotUpgradeGate::prepare.
            // Hold router mutex only across the no-throw code pointer swap.
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!gate_.publish(candidate)) {
                    gate_.abort(candidate);
                    return false;
                }
                published = true;
                active_.swap(next_version);
            }
            previous->state.retire();
            return true;
        } catch (...) {
            if (!published && gate_.frozen()) gate_.abort(candidate);
            throw;
        }
    }
};

} // namespace oreslang::runtime
