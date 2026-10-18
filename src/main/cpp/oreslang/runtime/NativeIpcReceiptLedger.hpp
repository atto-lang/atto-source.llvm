#pragma once

// Bounded host-side IPC correlation substrate, NOT an IPC transport.
// No authentication, durable dedup, guest dispatch or exactly-once effects.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace oreslang::runtime {

enum class IpcReceiptStage {
    LocalAdmitted, RemoteAccepted, HandlerCommitted, HandlerFailed,
    RejectedBeforeRemote, OutcomeUnknown
};

struct IpcReceiptKey {
    std::string domain, sender, destination, session, sha256;
    std::uint64_t sender_incarnation, destination_incarnation;
    std::uint64_t route_epoch, message_sequence, frame_sequence, capability_generation;

    bool operator==(const IpcReceiptKey& rhs) const noexcept {
        return std::tie(domain, sender, destination, session, sha256,
                        sender_incarnation, destination_incarnation,
                        route_epoch, message_sequence, frame_sequence, capability_generation)
            == std::tie(rhs.domain, rhs.sender, rhs.destination, rhs.session, rhs.sha256,
                        rhs.sender_incarnation, rhs.destination_incarnation,
                        rhs.route_epoch, rhs.message_sequence, rhs.frame_sequence,
                        rhs.capability_generation);
    }
};

class NativeIpcReceiptLedger final {
public:
    explicit NativeIpcReceiptLedger(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0 || capacity_ > 65536)
            throw std::invalid_argument("IPC correlation capacity outside [1, 65536]");
    }
    NativeIpcReceiptLedger(const NativeIpcReceiptLedger&) = delete;
    NativeIpcReceiptLedger& operator=(const NativeIpcReceiptLedger&) = delete;

    static void validate(const IpcReceiptKey& key) {
        for (const std::string* part : {&key.domain, &key.sender, &key.destination, &key.session}) {
            if (part->empty() || part->size() > 96)
                throw std::invalid_argument("IPC identity token outside bounds");
            for (unsigned char ch : *part)
                if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
                    throw std::invalid_argument("IPC identity token not canonical ASCII");
        }
        constexpr std::uint64_t max = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
        for (std::uint64_t number : {key.sender_incarnation, key.destination_incarnation,
                key.route_epoch, key.message_sequence, key.frame_sequence,
                key.capability_generation}) {
            if (number == 0 || number > max)
                throw std::invalid_argument("IPC sequence/generation outside signed i64");
        }
        if (key.sha256.size() != 64)
            throw std::invalid_argument("IPC payload hash must contain 64 lowercase hex chars");
        for (unsigned char ch : key.sha256)
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
                throw std::invalid_argument("IPC payload hash is not canonical lowercase hex");
    }

    // Trusted adapters must bind authorization and bytes to the exact key.
    // Nothing in this class grants authority to an untrusted peer or guest.
    void admit(const IpcReceiptKey& key) {
        validate(key);
        std::lock_guard<std::mutex> lock(mutex_);
        StreamIdentity identity{key.domain, key.sender, key.destination,
                                      key.sender_incarnation, key.destination_incarnation};
        if (stream_initialized_ && !(identity == stream_))
            throw std::logic_error("IPC stream/actor incarnation mismatch");
        if (key.message_sequence <= last_sequence_)
            throw std::logic_error("duplicate or stale IPC message sequence");
        if (key.route_epoch < last_route_ || key.capability_generation < last_capability_)
            throw std::logic_error("IPC route/capability generation rollback");
        if (entries_.size() >= capacity_)
            throw std::logic_error("IPC correlation budget exhausted");
        // Perform potentially throwing allocations before publishing counters.
        entries_.emplace(key.message_sequence, Entry{key, IpcReceiptStage::LocalAdmitted});
        if (!stream_initialized_) {
            // StreamIdentity was fully constructed before the insertion.
            // Move/swap of normal allocator std::strings is noexcept in C++17.
            static_assert(std::is_nothrow_move_assignable<StreamIdentity>::value,
                          "IPC stream identity must publish without throwing");
            stream_ = std::move(identity);
            stream_initialized_ = true;
        }
        last_sequence_ = key.message_sequence;
        last_route_ = key.route_epoch;
        last_capability_ = key.capability_generation;
        ++credits_;
    }

    // Must ONLY be invoked by the transport AFTER independent authentication.
    // In particular there is no guest-supplied bool 'authenticated' bypass.
    void advance_verified(const IpcReceiptKey& key, IpcReceiptStage step) {
        validate(key);
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = entries_.find(key.message_sequence);
        if (found == entries_.end() || !(found->second.key == key))
            throw std::logic_error("stale or mismatched IPC receipt");
        const auto old = found->second.stage;
        if (old == IpcReceiptStage::LocalAdmitted) {
            if (step == IpcReceiptStage::RemoteAccepted) {
                found->second.stage = step;
                --credits_;
                return;
            }
            if (step == IpcReceiptStage::RejectedBeforeRemote ||
                step == IpcReceiptStage::OutcomeUnknown) {
                --credits_;
                entries_.erase(found);
                return;
            }
        } else if (old == IpcReceiptStage::RemoteAccepted) {
            if (step == IpcReceiptStage::HandlerCommitted ||
                step == IpcReceiptStage::HandlerFailed ||
                step == IpcReceiptStage::OutcomeUnknown) {
                entries_.erase(found);
                return;
            }
        }
        throw std::logic_error("illegal, reordered or duplicated IPC receipt");
    }

    std::size_t mark_transport_lost() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto count = entries_.size();
        entries_.clear();  // outcomes become UNKNOWN, not FAILED or replayable
        credits_ = 0;
        return count;
    }

    std::pair<std::size_t, std::size_t> budgets() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {entries_.size(), credits_};
    }

    bool invariant() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.size() > capacity_ || credits_ > entries_.size()) return false;
        std::size_t pending = 0;
        for (const auto& item : entries_) {
            if (item.second.stage == IpcReceiptStage::LocalAdmitted) ++pending;
            else if (item.second.stage != IpcReceiptStage::RemoteAccepted) return false;
        }
        return pending == credits_;
    }
    static constexpr bool may_replay_automatically() noexcept { return false; }

private:
    struct StreamIdentity {
        std::string domain, sender, destination;
        std::uint64_t sender_incarnation = 0, destination_incarnation = 0;
        bool operator==(const StreamIdentity& rhs) const noexcept {
            return std::tie(domain, sender, destination, sender_incarnation,
                            destination_incarnation)
                == std::tie(rhs.domain, rhs.sender, rhs.destination,
                            rhs.sender_incarnation, rhs.destination_incarnation);
        }
    };
    struct Entry { IpcReceiptKey key; IpcReceiptStage stage; };
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Entry> entries_;
    StreamIdentity stream_{};
    bool stream_initialized_ = false;
    std::uint64_t last_sequence_ = 0, last_route_ = 0, last_capability_ = 0;
    std::size_t credits_ = 0;
};

} // namespace oreslang::runtime
