#pragma once
// Reference-only native IPC receipt/correlation model. Not a network transport.
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace oreslang::runtime {

struct IpcReceiptIdentity final {
    std::string tenant;
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

    bool operator==(const IpcReceiptIdentity&) const = default;
};

enum class IpcReceiptStage {
    local_admitted,
    remote_accepted,
    handler_committed,
    handler_failed,
    rejected_before_remote,
    unknown
};

class NativeIpcReceiptLedger final {
public:
    explicit NativeIpcReceiptLedger(std::size_t capacity) : capacity_(capacity) {
        if (!capacity_ || capacity_ > 65536)
            throw std::invalid_argument("invalid bounded IPC correlation capacity");
    }

    IpcReceiptStage admit(const IpcReceiptIdentity& id) {
        validate(id);
        auto stream = std::make_tuple(id.tenant, id.sender_actor,
            id.sender_incarnation, id.destination_actor, id.destination_incarnation);
        if (stream_.has_value() && stream != *stream_)
            throw std::invalid_argument("mixed IPC actor stream or incarnation");
        if (id.message_sequence <= last_sequence_)
            throw std::invalid_argument("reused or reordered IPC sequence");
        if (id.route_epoch < last_route_epoch_ ||
            id.capability_generation < last_capability_generation_)
            throw std::invalid_argument("stale IPC route/capability generation");
        if (live_.size() >= capacity_)
            throw std::overflow_error("bounded IPC correlation table full");
        // All fallible allocations occur before watermark/credit publication.
        const auto [it, inserted] = live_.emplace(id.message_sequence,
            Record{id, IpcReceiptStage::local_admitted});
        if (!inserted)
            throw std::logic_error("IPC sequence collision");
        stream_ = std::move(stream);
        last_sequence_ = id.message_sequence;
        last_route_epoch_ = id.route_epoch;
        last_capability_generation_ = id.capability_generation;
        ++transport_credits_;
        return it->second.stage;
    }

    IpcReceiptStage transition(const IpcReceiptIdentity& id,
                               IpcReceiptStage event, bool authenticated) {
        if (!authenticated) throw std::invalid_argument("unauthenticated IPC receipt");
        auto it = live_.find(id.message_sequence);
        if (it == live_.end() || !(it->second.id == id))
            throw std::invalid_argument("stale/mismatched IPC receipt identity");
        auto old = it->second.stage;
        if (old == IpcReceiptStage::local_admitted &&
            event == IpcReceiptStage::remote_accepted) {
            it->second.stage = event;
            --transport_credits_;
            return event;
        }
        const bool proven_no_remote = old == IpcReceiptStage::local_admitted &&
            event == IpcReceiptStage::rejected_before_remote;
        const bool settled_remote = old == IpcReceiptStage::remote_accepted &&
            (event == IpcReceiptStage::handler_committed ||
             event == IpcReceiptStage::handler_failed);
        const bool uncertain = (old == IpcReceiptStage::local_admitted ||
                                old == IpcReceiptStage::remote_accepted) &&
                               event == IpcReceiptStage::unknown;
        if (!(proven_no_remote || settled_remote || uncertain))
            throw std::invalid_argument("illegal or duplicate IPC receipt transition");
        if (old == IpcReceiptStage::local_admitted) --transport_credits_;
        live_.erase(it);
        return event;
    }

    std::size_t live_count() const noexcept { return live_.size(); }
    std::size_t transport_credits() const noexcept { return transport_credits_; }
    bool invariant() const noexcept {
        if (transport_credits_ > live_.size() || live_.size() > capacity_) return false;
        std::size_t admitted = 0;
        for (const auto& [sequence, record] : live_) {
            if (sequence != record.id.message_sequence) return false;
            if (record.stage == IpcReceiptStage::local_admitted) ++admitted;
            else if (record.stage != IpcReceiptStage::remote_accepted) return false;
        }
        return admitted == transport_credits_;
    }
    bool can_replay(const IpcReceiptIdentity&) const noexcept { return false; }

private:
    struct Record { IpcReceiptIdentity id; IpcReceiptStage stage; };
    using StreamId = std::tuple<std::string, std::string, std::uint64_t,
                                std::string, std::uint64_t>;
    static void validate(const IpcReceiptIdentity& id) {
        const auto valid_token = [](const std::string& str) {
            if (str.empty() || str.size() > 96) return false;
            for (unsigned char c : str) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                      c == '.' )) return false;
            }
            return true;
        };
        if (!valid_token(id.tenant) || !valid_token(id.sender_actor) ||
            !valid_token(id.destination_actor) ||
            !valid_token(id.transport_session))
            throw std::invalid_argument("invalid IPC actor identity");
        if (!id.sender_incarnation || !id.destination_incarnation ||
            !id.route_epoch || !id.message_sequence || !id.frame_sequence ||
            !id.capability_generation)
            throw std::invalid_argument("zero IPC incarnation or sequence");
        if (id.payload_sha256.size() != 64)
            throw std::invalid_argument("IPC payload hash size");
        for (unsigned char c : id.payload_sha256)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                throw std::invalid_argument("IPC payload digest not canonical");
    }

    const std::size_t capacity_;
    std::optional<StreamId> stream_;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t last_route_epoch_ = 0;
    std::uint64_t last_capability_generation_ = 0;
    std::size_t transport_credits_ = 0;
    std::map<std::uint64_t, Record> live_;
};
} // namespace oreslang::runtime
