#include "oreslang/runtime/ActorErrorCatalog.hpp"

#include <cassert>
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
        assert(static_cast<std::uint16_t>(entry.code) == raw);
        assert(entry.wire_name[0] != '\0');
    }
    const auto envelope = ActorErrorEnvelope::make(
            ActorErrorCode::ActorSendRejected, std::string(2000, 'x'),
            "actor.1", "supervisor:2");
    assert(envelope.diagnostic().size() == 1024);
    assert(envelope.actor_id() == "actor.1");
    assert(envelope.supervisor_id() == "supervisor:2");
    assert(envelope.recoverable());
    bool rejected = false;
    try {
        (void)ActorErrorEnvelope::make(ActorErrorCode::ActorStopped, "",
                "unsafe/id", "");
    } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try {
        (void)actor_error_definition(static_cast<ActorErrorCode>(9999));
    } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);

    // Malformed UTF-8 must never pass through to actor CONTROL data.
    const auto invalid = ActorErrorEnvelope::make(
            ActorErrorCode::ActorTransport, std::string("\xFF\xFE", 2), "a", "b");
    assert(invalid.diagnostic() == "??");
}
