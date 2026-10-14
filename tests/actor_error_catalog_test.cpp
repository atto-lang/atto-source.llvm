#include "oreslang/runtime/ActorErrorCatalog.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

using namespace oreslang::runtime;

static_assert(actor_error_code_known(1001));
static_assert(actor_error_code_known(1020));
static_assert(!actor_error_code_known(1000));
static_assert(!actor_error_code_known(1021));
static_assert(actor_error_definition(ActorErrorCode::ActorMailboxFull).recoverable);
static_assert(!actor_error_definition(ActorErrorCode::ActorCancelled).recoverable);
static_assert(!actor_error_definition(ActorErrorCode::ActorIsolation).recoverable);

int main() {
    for (std::uint16_t raw = 1001; raw <= 1020; ++raw) {
        const auto kind = static_cast<ActorErrorCode>(raw);
        const auto entry = actor_error_definition(kind);
        if (static_cast<std::uint16_t>(entry.code) != raw) return 1;
        if (entry.wire_name[0] == '\0') return 2;
    }
    const auto envelope = ActorErrorEnvelope::make(
            ActorErrorCode::ActorSendRejected, std::string(2000, 'x'),
            "actor.1", "supervisor:2");
    if (envelope.diagnostic().size() != 1024) return 3;
    if (envelope.actor_id() != "actor.1") return 4;
    if (envelope.supervisor_id() != "supervisor:2") return 5;
    if (!envelope.recoverable()) return 6;
    bool rejected = false;
    try {
        (void)ActorErrorEnvelope::make(ActorErrorCode::ActorStopped, "",
                "unsafe/id", "");
    } catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected) return 7;
    rejected = false;
    try {
        (void)actor_error_definition(static_cast<ActorErrorCode>(9999));
    } catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected) return 8;

    // Malformed UTF-8 must never pass through to actor CONTROL data.
    const auto invalid = ActorErrorEnvelope::make(
            ActorErrorCode::ActorTransport, std::string("\xFF\xFE", 2), "a", "b");
    if (invalid.diagnostic() != "??") return 9;
    const auto control = ActorErrorEnvelope::make(
            ActorErrorCode::ActorSendRejected, std::string("a\\x1b" "b\\xe2\\x80\\xae" "c"),
            "a", "b");
    if (control.diagnostic() != "a?b?c") return 10;
    const auto valid = ActorErrorEnvelope::make(
            ActorErrorCode::ActorTransport, std::string(1023, 'A') + "\\xc3\\xa9",
            "a", "b");
    if (valid.diagnostic() != std::string(1023, 'A')) return 11;
    const auto replacement = ActorErrorEnvelope::make(
            ActorErrorCode::ActorTransport, std::string("\\xe2\\x81\\xa7", 3),
            "a", "b");
    if (replacement.diagnostic() != "?") return 12;
}
