#pragma once
// Native host-side IPC receipt correlation, not a wire transport or an
// authenticator. Callers must independently verify every remote receipt.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oreslang::runtime {

enum class IpcReceiptStage {
    local_admitted,
    dispatch_committed,
    remote_accepted,
    handler_committed,
    handler_failed,
    rejected_before_remote,
    outcome_unknown
};

struct IpcReceiptEnvelope {
    std::string domain, sender, destination, session, payload_sha256;
    std::uint64_t sender_incarnation = 0;
    std::uint64_t destination_incarnation = 0;
    std::uint64_t route_epoch = 0;
    std::uint64_t capability_generation = 0;
    std::uint64_t message_sequence = 0;
    std::uint64_t frame_sequence = 0;

    bool operator==(const IpcReceiptEnvelope& other) const {
        return std::tie(domain, sender, destination, session, payload_sha256,
                        sender_incarnation, destination_incarnation, route_epoch,
                        capability_generation, message_sequence, frame_sequence)
            == std::tie(other.domain, other.sender, other.destination, other.session,
                        other.payload_sha256, other.sender_incarnation,
                        other.destination_incarnation, other.route_epoch,
                        other.capability_generation, other.message_sequence,
                        other.frame_sequence);
    }
};

inline bool ipc_receipt_token(const std::string& value) {
    if (value.empty() || value.size() > 96) return false;
    for (unsigned char ch : value) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-'))
            return false;
    }
    return true;
}

inline bool ipc_receipt_digest(const std::string& value) {
    if (value.size() != 64) return false;
    for (unsigned char ch : value)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    return true;
}

inline bool ipc_positive_java_long(std::uint64_t value) {
    return value != 0 &&
        value <= static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
}

inline void validate_ipc_receipt_envelope(const IpcReceiptEnvelope& message) {
    if (!ipc_receipt_token(message.domain) ||
        !ipc_receipt_token(message.sender) ||
        !ipc_receipt_token(message.destination) ||
        !ipc_receipt_token(message.session) ||
        !ipc_receipt_digest(message.payload_sha256) ||
        !ipc_positive_java_long(message.sender_incarnation) ||
        !ipc_positive_java_long(message.destination_incarnation) ||
        !ipc_positive_java_long(message.route_epoch) ||
        !ipc_positive_java_long(message.capability_generation) ||
        !ipc_positive_java_long(message.message_sequence) ||
        !ipc_positive_java_long(message.frame_sequence))
        throw std::invalid_argument("noncanonical IPC receipt identity");
}

inline bool same_ipc_transport_epoch(
        const IpcReceiptEnvelope& a, const IpcReceiptEnvelope& b) {
    return std::tie(a.domain, a.sender, a.destination, a.session,
                    a.sender_incarnation, a.destination_incarnation,
                    a.route_epoch, a.capability_generation)
        == std::tie(b.domain, b.sender, b.destination, b.session,
                    b.sender_incarnation, b.destination_incarnation,
                    b.route_epoch, b.capability_generation);
}

class NativeIpcReceiptTable final {
public:
    explicit NativeIpcReceiptTable(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0 || capacity > 65536)
            throw std::invalid_argument("invalid bounded IPC correlation capacity");
    }

    NativeIpcReceiptTable(const NativeIpcReceiptTable&) = delete;
    NativeIpcReceiptTable& operator=(const NativeIpcReceiptTable&) = delete;

    void admit(const IpcReceiptEnvelope& message) {
        validate_ipc_receipt_envelope(message);
        std::lock_guard<std::mutex> guard(mutex_);
        const auto stream = std::make_tuple(message.domain, message.sender,
            message.sender_incarnation, message.destination,
            message.destination_incarnation);
        if (has_stream_ && stream != stream_)
            throw std::invalid_argument("cross-stream or stale actor incarnation");
        if (message.message_sequence <= last_sequence_)
            throw std::invalid_argument("duplicate/reordered message sequence");
        if (message.frame_sequence <= last_frame_sequence_)
            throw std::invalid_argument("duplicate/reordered frame sequence");
        if (message.route_epoch < last_route_epoch_ ||
            message.capability_generation < last_capability_generation_)
            throw std::invalid_argument("route or capability generation rollback");
        if (pending_.size() >= capacity_)
            throw std::overflow_error("bounded IPC correlation capacity exhausted");
        // Install stream identity first while has_stream_ is still false.
        // A throwing copy/allocation cannot leave a live correlation entry
        // without its stream identity or charge a transport credit.
        if (!has_stream_) stream_ = stream;
        // Commit the monotonic indices only after successful insertion.
        const bool inserted = pending_.emplace(message.message_sequence,
            Entry{message, IpcReceiptStage::local_admitted}).second;
        if (!inserted) throw std::invalid_argument("duplicate sequence");
        has_stream_ = true;
        last_sequence_ = message.message_sequence;
        last_frame_sequence_ = message.frame_sequence;
        last_route_epoch_ = message.route_epoch;
        last_capability_generation_ = message.capability_generation;
        ++transport_credits_;
    }

    /** Trusted LOCAL publication fence. No physical write may begin before
     *  this succeeds; failure after commitment is indeterminate, never a
     *  definitive never-sent result.
     */
    void commit_dispatch(const IpcReceiptEnvelope& message) {
        validate_ipc_receipt_envelope(message);
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = pending_.find(message.message_sequence);
        if (iter == pending_.end() || !(iter->second.envelope == message) ||
            iter->second.stage != IpcReceiptStage::local_admitted)
            throw std::invalid_argument("invalid or duplicate IPC dispatch commitment");
        iter->second.stage = IpcReceiptStage::dispatch_committed;
    }

    /** Trusted LOCAL rejection before dispatch commitment. */
    void reject_before_remote(const IpcReceiptEnvelope& message) {
        validate_ipc_receipt_envelope(message);
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = pending_.find(message.message_sequence);
        if (iter == pending_.end() || !(iter->second.envelope == message) ||
            iter->second.stage != IpcReceiptStage::local_admitted)
            throw std::invalid_argument("invalid local pre-remote IPC rejection");
        pending_.erase(iter);
        --transport_credits_;
    }

    // IMPORTANT: This function DOES NOT authenticate receipts. The trusted
    // transport adapter must verify peer, capability and frame integrity first.
    // Host-local verdicts are never accepted from a peer-authenticated frame.
    void accept_verified(const IpcReceiptEnvelope& message, IpcReceiptStage next) {
        validate_ipc_receipt_envelope(message);
        if (next == IpcReceiptStage::outcome_unknown ||
            next == IpcReceiptStage::rejected_before_remote)
            throw std::invalid_argument(
                "local transport decision cannot be asserted by peer receipt");
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = pending_.find(message.message_sequence);
        if (iter == pending_.end() || !(iter->second.envelope == message))
            throw std::invalid_argument("forged/stale/unmatched receipt");
        const auto previous = iter->second.stage;
        if (previous == IpcReceiptStage::dispatch_committed &&
            next == IpcReceiptStage::remote_accepted) {
            iter->second.stage = next;
            --transport_credits_;
            return;
        }
        if (previous == IpcReceiptStage::remote_accepted &&
            (next == IpcReceiptStage::handler_committed ||
             next == IpcReceiptStage::handler_failed)) {
            pending_.erase(iter);
            return;
        }
        throw std::invalid_argument("illegal or duplicate IPC receipt transition");
    }

    // Correlation identities handed to the local transport supervisor on
    // teardown. This is a notification of ambiguity, NEVER replay authority.
    struct LostCorrelation {
        IpcReceiptEnvelope envelope;
        IpcReceiptStage last_observed;
    };

    /** Atomically retire correlations bound to one complete locally trusted
     *  transport epoch. Textual session names are reusable and are therefore
     *  insufficient loss authority. Message/frame sequence and digest identify
     *  individual messages, not the transport epoch.
     *
     *  All copies/allocations complete BEFORE any state removal, so bad_alloc
     *  cannot strand a subset of unreported correlations.
     */
    std::vector<LostCorrelation> retire_transport_lost(
            const IpcReceiptEnvelope& trusted_epoch) {
        validate_ipc_receipt_envelope(trusted_epoch);
        std::lock_guard<std::mutex> guard(mutex_);
        std::vector<LostCorrelation> retired;
        std::size_t matching = 0;
        for (const auto& [sequence, entry] : pending_) {
            (void)sequence;
            matching += same_ipc_transport_epoch(entry.envelope, trusted_epoch) ? 1u : 0u;
        }
        retired.reserve(matching);
        for (const auto& [sequence, entry] : pending_) {
            (void)sequence;
            if (same_ipc_transport_epoch(entry.envelope, trusted_epoch))
                retired.push_back({entry.envelope, entry.stage});
        }
        // No throwing operations after this point while the mutex is held.
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (!same_ipc_transport_epoch(it->second.envelope, trusted_epoch)) {
                ++it;
                continue;
            }
            if (it->second.stage == IpcReceiptStage::local_admitted ||
                it->second.stage == IpcReceiptStage::dispatch_committed)
                --transport_credits_;
            it = pending_.erase(it);
        }
        return retired;
    }

    std::size_t mark_transport_lost(const IpcReceiptEnvelope& trusted_epoch) {
        return retire_transport_lost(trusted_epoch).size();
    }

    std::pair<std::size_t, std::size_t> counters() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return {pending_.size(), transport_credits_};
    }

    bool invariant() const {
        std::lock_guard<std::mutex> guard(mutex_);
        if (transport_credits_ > pending_.size() || pending_.size() > capacity_)
            return false;
        std::size_t waiting = 0;
        for (const auto& item : pending_)
            waiting += (item.second.stage == IpcReceiptStage::local_admitted ||
                        item.second.stage == IpcReceiptStage::dispatch_committed) ? 1 : 0;
        return waiting == transport_credits_;
    }

private:
    struct Entry { IpcReceiptEnvelope envelope; IpcReceiptStage stage; };
    using Stream = std::tuple<std::string, std::string, std::uint64_t,
                              std::string, std::uint64_t>;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    bool has_stream_ = false;
    Stream stream_;
    std::uint64_t last_sequence_ = 0, last_frame_sequence_ = 0, last_route_epoch_ = 0;
    std::uint64_t last_capability_generation_ = 0;
    std::size_t transport_credits_ = 0;
    std::map<std::uint64_t, Entry> pending_;
};
}  // namespace oreslang::runtime
