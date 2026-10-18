#pragma once
// Native host-side IPC receipt correlation, not a wire transport or an
// authenticator. Callers must independently verify every remote receipt.
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace oreslang::runtime {

enum class IpcReceiptStage {
    local_admitted,
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

class NativeIpcReceiptTable final {
public:
    explicit NativeIpcReceiptTable(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0 || capacity > 65536)
            throw std::invalid_argument("invalid bounded IPC correlation capacity");
    }

    NativeIpcReceiptTable(const NativeIpcReceiptTable&) = delete;
    NativeIpcReceiptTable& operator=(const NativeIpcReceiptTable&) = delete;

    void admit(const IpcReceiptEnvelope& message) {
        validate(message);
        std::lock_guard<std::mutex> guard(mutex_);
        const auto stream = std::make_tuple(message.domain, message.sender,
            message.sender_incarnation, message.destination,
            message.destination_incarnation);
        if (has_stream_ && stream != stream_)
            throw std::invalid_argument("cross-stream or stale actor incarnation");
        if (message.message_sequence <= last_sequence_)
            throw std::invalid_argument("duplicate/reordered message sequence");
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
        last_route_epoch_ = message.route_epoch;
        last_capability_generation_ = message.capability_generation;
        ++transport_credits_;
    }

    // IMPORTANT: This function DOES NOT authenticate receipts. The trusted
    // transport adapter must verify peer, capability and frame integrity first.
    void accept_verified(const IpcReceiptEnvelope& message, IpcReceiptStage next) {
        validate(message);
        std::lock_guard<std::mutex> guard(mutex_);
        auto iter = pending_.find(message.message_sequence);
        if (iter == pending_.end() || !(iter->second.envelope == message))
            throw std::invalid_argument("forged/stale/unmatched receipt");
        const auto previous = iter->second.stage;
        if (previous == IpcReceiptStage::local_admitted) {
            if (next == IpcReceiptStage::remote_accepted) {
                iter->second.stage = next;
                --transport_credits_;
                return;
            }
            if (next == IpcReceiptStage::rejected_before_remote ||
                next == IpcReceiptStage::outcome_unknown) {
                --transport_credits_;
                pending_.erase(iter);
                return;
            }
        } else if (previous == IpcReceiptStage::remote_accepted) {
            if (next == IpcReceiptStage::handler_committed ||
                next == IpcReceiptStage::handler_failed ||
                next == IpcReceiptStage::outcome_unknown) {
                pending_.erase(iter);
                return;
            }
        }
        throw std::invalid_argument("illegal or duplicate IPC receipt transition");
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
            waiting += item.second.stage == IpcReceiptStage::local_admitted ? 1 : 0;
        return waiting == transport_credits_;
    }

private:
    static bool token(const std::string& value) {
        if (value.empty() || value.size() > 96) return false;
        for (unsigned char c : value) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
                return false;
        }
        return true;
    }
    static bool digest(const std::string& value) {
        if (value.size() != 64) return false;
        for (unsigned char c : value)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
        return true;
    }
    static void validate(const IpcReceiptEnvelope& message) {
        if (!token(message.domain) || !token(message.sender) ||
            !token(message.destination) || !token(message.session) ||
            !digest(message.payload_sha256) || !message.sender_incarnation ||
            !message.destination_incarnation || !message.route_epoch ||
            !message.capability_generation || !message.message_sequence ||
            !message.frame_sequence)
            throw std::invalid_argument("noncanonical IPC receipt identity");
    }

    struct Entry { IpcReceiptEnvelope envelope; IpcReceiptStage stage; };
    using Stream = std::tuple<std::string, std::string, std::uint64_t,
                              std::string, std::uint64_t>;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    bool has_stream_ = false;
    Stream stream_;
    std::uint64_t last_sequence_ = 0, last_route_epoch_ = 0;
    std::uint64_t last_capability_generation_ = 0;
    std::size_t transport_credits_ = 0;
    std::map<std::uint64_t, Entry> pending_;
};
}  // namespace oreslang::runtime
