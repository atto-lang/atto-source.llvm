#pragma once
#include "oreslang/runtime/ExceptionPolicy.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace oreslang::runtime {

// Host-only control-plane types. No C++ exception_ptr, pointers or guest
// function objects are ever retained in the actor-to-supervisor envelope.
enum class ActorErrorCode : std::uint16_t {
    ActorStopped = 1001,
    ActorMailboxFull = 1002,
    ActorMailboxClosed = 1003,
    ActorSendRejected = 1004,
    ActorTransport = 1005,
    ActorIsolation = 1006,
    ActorCapabilityRevoked = 1007,
    ActorSpawn = 1008,
    ActorSupervision = 1009,
    ActorRestartLimit = 1010,
    ActorDeadlineExceeded = 1011,
    ActorCancelled = 1012,
    ActorLeaseExpired = 1013,
    ActorFencing = 1014,
    ActorGroupClosed = 1015,
    ActorGroupPermit = 1016,
    ActorRemoteUnavailable = 1017,
    ActorSerialization = 1018,
    ActorHeapExhausted = 1019,
    ActorRuntimeInvariant = 1020,
};

struct ActorErrorDefinition {
    ActorErrorCode code;
    const char* wire_name;
    bool recoverable;
};

constexpr ActorErrorDefinition actor_error_definition(ActorErrorCode code) {
    switch (code) {
        case ActorErrorCode::ActorStopped: return {code, "stopped", true};
        case ActorErrorCode::ActorMailboxFull: return {code, "mailbox_full", true};
        case ActorErrorCode::ActorMailboxClosed: return {code, "mailbox_closed", true};
        case ActorErrorCode::ActorSendRejected: return {code, "send_rejected", true};
        case ActorErrorCode::ActorTransport: return {code, "transport", true};
        case ActorErrorCode::ActorIsolation: return {code, "isolation", false};
        case ActorErrorCode::ActorCapabilityRevoked: return {code, "capability_revoked", false};
        case ActorErrorCode::ActorSpawn: return {code, "spawn", true};
        case ActorErrorCode::ActorSupervision: return {code, "supervision", false};
        case ActorErrorCode::ActorRestartLimit: return {code, "restart_limit", false};
        case ActorErrorCode::ActorDeadlineExceeded: return {code, "deadline_exceeded", false};
        case ActorErrorCode::ActorCancelled: return {code, "cancelled", false};
        case ActorErrorCode::ActorLeaseExpired: return {code, "lease_expired", false};
        case ActorErrorCode::ActorFencing: return {code, "fencing", false};
        case ActorErrorCode::ActorGroupClosed: return {code, "group_closed", true};
        case ActorErrorCode::ActorGroupPermit: return {code, "group_permit", false};
        case ActorErrorCode::ActorRemoteUnavailable: return {code, "remote_unavailable", true};
        case ActorErrorCode::ActorSerialization: return {code, "serialization", true};
        case ActorErrorCode::ActorHeapExhausted: return {code, "heap_exhausted", false};
        case ActorErrorCode::ActorRuntimeInvariant: return {code, "runtime_invariant", false};
    }
    // Invalid enum casts can come from FFI; fail closed.
    throw std::invalid_argument("unknown native actor error code");
}

constexpr bool actor_error_code_known(std::uint16_t code) noexcept {
    return code >= 1001 && code <= 1020;
}

inline std::string checked_actor_identity(std::string_view identity) {
    if (identity.size() > 128) throw std::invalid_argument("actor identity too long");
    for (unsigned char ch : identity) {
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
                || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'
                || ch == '.' || ch == ':')) {
            throw std::invalid_argument("invalid actor identity");
        }
    }
    return std::string(identity);
}

class ActorErrorEnvelope {
public:
    static ActorErrorEnvelope make(ActorErrorCode code,
            std::string_view diagnostic,
            std::string_view actor_id,
            std::string_view supervisor_id) {
        (void)actor_error_definition(code); // validate enum before transfer
        return ActorErrorEnvelope(code, bounded_control_diagnostic(diagnostic),
                checked_actor_identity(actor_id),
                checked_actor_identity(supervisor_id));
    }

    ActorErrorCode code() const noexcept { return code_; }
    const std::string& diagnostic() const noexcept { return diagnostic_; }
    const std::string& actor_id() const noexcept { return actor_id_; }
    const std::string& supervisor_id() const noexcept { return supervisor_id_; }
    bool recoverable() const { return actor_error_definition(code_).recoverable; }

private:
    ActorErrorEnvelope(ActorErrorCode code, std::string diagnostic,
            std::string actor_id, std::string supervisor_id)
        : code_(code), diagnostic_(std::move(diagnostic)),
          actor_id_(std::move(actor_id)), supervisor_id_(std::move(supervisor_id)) {}

    ActorErrorCode code_;
    std::string diagnostic_;
    std::string actor_id_;
    std::string supervisor_id_;
};

} // namespace oreslang::runtime
