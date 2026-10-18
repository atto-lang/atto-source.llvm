#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// Host-side correlation only. Authentication, I/O, durable deduplication and
// application commits must be provided by the actual proxy/worker transport.
enum class IpcReceiptStage {
    LocalAdmitted,
    RemoteAccepted,
    HandlerCommitted,
    HandlerFailed,
    RejectedBeforeAdmission,
    Unknown
};

struct IpcReceiptEnvelope final {
    std::string sender_domain;
    std::string sender_actor;
    std::uint64_t sender_incarnation;
    std::string destination_actor;
    std::uint64_t destination_incarnation;
    std::uint64_t route_epoch;
    std::string transport_session;
    std::uint64_t message_sequence;
    std::uint64_t frame_sequence;
    std::uint64_t capability_generation;
    std::string payload_sha256;

    bool operator==(const IpcReceiptEnvelope& b) const noexcept {
        return std::tie(sender_domain, sender_actor, sender_incarnation,
                        destination_actor, destination_incarnation, route_epoch,
                        transport_session, message_sequence, frame_sequence,
                        capability_generation, payload_sha256)
            == std::tie(b.sender_domain, b.sender_actor, b.sender_incarnation,
                        b.destination_actor, b.destination_incarnation, b.route_epoch,
                        b.transport_session, b.message_sequence, b.frame_sequence,
                        b.capability_generation, b.payload_sha256);
    }
};

/** Reference-correlated bounded admission for a single sender/destination incarnation.
 *
 * A transport must authenticate the complete envelope before calling advance().
 * In particular, true is not a substitute for a verified MAC/peer identity.
 * Local acceptance never means remote execution. UNKNOWN cannot be replayed.
 */
class NativeIpcReceiptLedger final {
public:
    explicit NativeIpcReceiptLedger(std::size_t capacity) : capacity_(capacity) {
        if (!capacity || capacity > 65536)
            throw std::invalid_argument("IPC capacity must be in [1,65536]");
    }
    NativeIpcReceiptLedger(const NativeIpcReceiptLedger&) = delete;
    NativeIpcReceiptLedger& operator=(const NativeIpcReceiptLedger&) = delete;

    static void validate(const IpcReceiptEnvelope& e) {
        for (const auto* value : {&e.sender_domain, &e.sender_actor,
                                  &e.destination_actor, &e.transport_session}) {
            if (value->empty() || value->size() > 96)
                throw std::invalid_argument("IPC token length outside bounds");
            for (unsigned char ch : *value) {
                if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
                    throw std::invalid_argument("IPC token is not canonical ASCII");
            }
        }
        constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        for (std::uint64_t n : {e.sender_incarnation, e.destination_incarnation,
                 e.route_epoch, e.message_sequence, e.frame_sequence,
                 e.capability_generation}) {
            if (n == 0 || n > limit)
                throw std::invalid_argument("IPC integer out of protocol bounds");
        }
        if (e.payload_sha256.size() != 64)
            throw std::invalid_argument("IPC SHA-256 must be exact lowercase hex");
        for (unsigned char ch : e.payload_sha256) {
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
                throw std::invalid_argument("IPC SHA-256 must be exact lowercase hex");
        }
    }

    void admit(const IpcReceiptEnvelope& e) {
        validate(e);
        std::lock_guard<std::mutex> lock(mutex_);
        if (stream_initialized_ &&
                (domain_ != e.sender_domain || sender_ != e.sender_actor ||
                 sender_incarnation_ != e.sender_incarnation ||
                 destination_ != e.destination_actor ||
                 destination_incarnation_ != e.destination_incarnation))
            throw std::logic_error("IPC cross-stream or stale incarnation");
        if (e.message_sequence <= last_sequence_)
            throw std::logic_error("IPC duplicate or reordered message sequence");
        if (e.route_epoch < last_route_ || e.capability_generation < last_capability_)
            throw std::logic_error("IPC route or capability generation rollback");
        if (live_.size() >= capacity_)
            throw std::logic_error("IPC bounded correlation exhausted");
        // Stage all allocating strings before touching state. Then only
        // noexcept swaps/counter writes remain after a successful insertion.
        if (!stream_initialized_) {
            std::string domain = e.sender_domain;
            std::string sender = e.sender_actor;
            std::string destination = e.destination_actor;
            live_.push_back(Entry{e, IpcReceiptStage::LocalAdmitted});
            domain_.swap(domain);
            sender_.swap(sender);
            destination_.swap(destination);
            sender_incarnation_ = e.sender_incarnation;
            destination_incarnation_ = e.destination_incarnation;
            stream_initialized_ = true;
        } else {
            live_.push_back(Entry{e, IpcReceiptStage::LocalAdmitted});
        }
        last_sequence_ = e.message_sequence;
        last_route_ = e.route_epoch;
        last_capability_ = e.capability_generation;
        ++transport_credits_;
    }

    void advance(const IpcReceiptEnvelope& e, IpcReceiptStage step, bool authenticated) {
        if (!authenticated) throw std::logic_error("IPC unauthenticated receipt");
        validate(e);
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(live_.begin(), live_.end(), [&](const Entry& v) {
            return v.envelope.message_sequence == e.message_sequence;
        });
        if (it == live_.end() || !(it->envelope == e))
            throw std::logic_error("IPC stale, forged, or duplicate receipt");
        const auto prior = it->stage;
        if (prior == IpcReceiptStage::LocalAdmitted) {
            if (step == IpcReceiptStage::RemoteAccepted) {
                it->stage = step;
                --transport_credits_;
                return;
            }
            if (step == IpcReceiptStage::RejectedBeforeAdmission ||
                step == IpcReceiptStage::Unknown) {
                --transport_credits_;
                live_.erase(it);
                return;
            }
        } else if (prior == IpcReceiptStage::RemoteAccepted) {
            if (step == IpcReceiptStage::HandlerCommitted ||
                step == IpcReceiptStage::HandlerFailed ||
                step == IpcReceiptStage::Unknown) {
                live_.erase(it);
                return;
            }
        }
        throw std::logic_error("IPC forbidden receipt transition");
    }

    // Transport disappearance is ambiguity, not a remote failure or replay.
    std::size_t mark_transport_lost() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto count = live_.size();
        live_.clear();
        transport_credits_ = 0;
        return count;
    }

    std::pair<std::size_t, std::size_t> budgets() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return {live_.size(), transport_credits_};
    }
    bool invariant() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (transport_credits_ > live_.size() || live_.size() > capacity_) return false;
        std::size_t expected = 0;
        for (const auto& entry : live_) {
            if (entry.stage == IpcReceiptStage::LocalAdmitted) ++expected;
            else if (entry.stage != IpcReceiptStage::RemoteAccepted) return false;
        }
        return expected == transport_credits_;
    }
    static constexpr bool can_replay_automatically() noexcept { return false; }

private:
    struct Entry { IpcReceiptEnvelope envelope; IpcReceiptStage stage; };
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<Entry> live_;
    bool stream_initialized_ = false;
    std::string domain_, sender_, destination_;
    std::uint64_t sender_incarnation_ = 0, destination_incarnation_ = 0;
    std::uint64_t last_sequence_ = 0, last_route_ = 0, last_capability_ = 0;
    std::size_t transport_credits_ = 0;
};

} // namespace oreslang::runtime
