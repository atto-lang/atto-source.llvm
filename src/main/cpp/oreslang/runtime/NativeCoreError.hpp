// Generated from Java std/error v1 error-catalog.json; parity-checked by CTest.
// Host-only native value metadata, NOT LLVM source-class admission or catch lowering.
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <stdexcept>

#include "oreslang/runtime/ExceptionPolicy.hpp"

namespace oreslang::runtime {

struct CoreErrorDescriptor {
    std::string_view name;
    int code;
    int parent_code;  // -1 only for Error
    bool abstract_type;
    bool retry_advisory;   // explicitly declared nominal interface, not policy
    bool actor_api_error;  // explicitly declared nominal interface
};

inline constexpr int core_error_schema_version = 1;
inline constexpr std::array<CoreErrorDescriptor, 99> core_error_catalog{{
    {"Error", 0, -1, false, false, false},
    {"ArgumentError", 1000, 0, true, false, false},
    {"InvalidArgumentError", 1001, 1000, false, false, false},
    {"MissingArgumentError", 1002, 1000, false, false, false},
    {"UnexpectedArgumentError", 1003, 1000, false, false, false},
    {"ArityError", 1004, 1000, false, false, false},
    {"TypeMismatchError", 1005, 1000, false, false, false},
    {"CastError", 1006, 1000, false, false, false},
    {"RangeError", 1007, 1000, false, false, false},
    {"IndexError", 1008, 1000, false, false, false},
    {"StateError", 2000, 0, true, false, false},
    {"InvalidStateError", 2001, 2000, false, false, false},
    {"AlreadyInitializedError", 2002, 2000, false, false, false},
    {"NotInitializedError", 2003, 2000, false, false, false},
    {"ClosedError", 2004, 2000, false, false, false},
    {"ReadOnlyError", 2005, 2000, false, false, false},
    {"ImmutableError", 2006, 2000, false, false, false},
    {"ConcurrentModificationError", 2007, 2000, false, false, false},
    {"StaleHandleError", 2008, 2000, false, false, false},
    {"IOError", 3000, 0, true, false, false},
    {"FileSystemError", 3090, 3000, true, false, false},
    {"FileNotFoundError", 3001, 3090, false, false, false},
    {"FileAlreadyExistsError", 3002, 3090, false, false, false},
    {"NotDirectoryError", 3003, 3090, false, false, false},
    {"IsDirectoryError", 3004, 3090, false, false, false},
    {"DirectoryNotEmptyError", 3005, 3090, false, false, false},
    {"ReadOnlyFileSystemError", 3006, 3090, false, false, false},
    {"EndOfFileError", 3007, 3000, false, false, false},
    {"BrokenPipeError", 3008, 3000, false, false, false},
    {"NetworkError", 4000, 3000, true, false, false},
    {"DnsResolutionError", 4001, 4000, false, false, false},
    {"ConnectionRefusedError", 4002, 4000, false, false, false},
    {"ConnectionResetError", 4003, 4000, false, true, false},
    {"ConnectionAbortedError", 4004, 4000, false, false, false},
    {"HostUnreachableError", 4005, 4000, false, false, false},
    {"NetworkUnreachableError", 4006, 4000, false, false, false},
    {"TlsHandshakeError", 4007, 4000, false, false, false},
    {"ProtocolError", 4008, 4000, false, false, false},
    {"TimingError", 5000, 0, true, false, false},
    {"TimeoutError", 5001, 5000, false, true, false},
    {"DeadlineExceededError", 5002, 5000, false, false, false},
    {"WouldBlockError", 5003, 5000, false, true, false},
    {"RetryLimitExceededError", 5004, 5000, false, false, false},
    {"BackpressureError", 5005, 5000, false, true, false},
    {"RateLimitedError", 5006, 5000, false, true, false},
    {"TimerConfigurationError", 5007, 5000, false, false, false},
    {"ClockError", 5008, 5000, false, false, false},
    {"DataError", 6000, 0, true, false, false},
    {"ParseError", 6001, 6000, false, false, false},
    {"SyntaxError", 6002, 6000, false, false, false},
    {"EncodingError", 6003, 6000, false, false, false},
    {"DecodingError", 6004, 6000, false, false, false},
    {"ValidationError", 6005, 6000, false, false, false},
    {"SerializationError", 6006, 6000, false, false, false},
    {"DeserializationError", 6007, 6000, false, false, false},
    {"SchemaMismatchError", 6008, 6000, false, false, false},
    {"SecurityError", 7000, 0, true, false, false},
    {"AccessDeniedError", 7001, 7000, false, false, false},
    {"AuthenticationError", 7002, 7000, false, false, false},
    {"AuthorizationError", 7003, 7000, false, false, false},
    {"CapabilityDeniedError", 7004, 7000, false, false, false},
    {"SignatureVerificationError", 7005, 7000, false, false, false},
    {"IntegrityError", 7006, 7000, false, false, false},
    {"SandboxViolationError", 7007, 7000, false, false, false},
    {"CertificateError", 7008, 7000, false, false, false},
    {"OperationError", 8000, 0, true, false, false},
    {"ArithmeticError", 8001, 8000, false, false, false},
    {"DivisionByZeroError", 8002, 8000, false, false, false},
    {"IntegerOverflowError", 8003, 8000, false, false, false},
    {"NumericConversionError", 8004, 8000, false, false, false},
    {"UnsupportedOperationError", 8005, 8000, false, false, false},
    {"VersionMismatchError", 8006, 8000, false, false, false},
    {"FeatureUnavailableError", 8007, 8000, false, false, false},
    {"DependencyFailureError", 8008, 8000, false, false, false},
    {"KeyNotFoundError", 6009, 6000, false, false, false},
    {"DuplicateKeyError", 6010, 6000, false, false, false},
    {"InvalidFormatError", 6011, 6000, false, false, false},
    {"EmptyCollectionError", 2009, 2000, false, false, false},
    {"IteratorExhaustedError", 2010, 2000, false, false, false},
    {"DeadlockDetectedError", 2011, 2000, false, false, false},
    {"ProcessNotFoundError", 2012, 2000, false, false, true},
    {"MailboxFullError", 2013, 2000, false, false, true},
    {"NotImplementedError", 8009, 8000, false, false, false},
    {"ResourceExhaustedError", 8010, 8000, false, false, false},
    {"ActorError", 9000, 0, true, false, true},
    {"ActorSpawnRejectedError", 9001, 9000, false, false, false},
    {"ActorInitializationError", 9002, 9000, false, false, false},
    {"ActorTerminatedError", 9003, 9000, false, false, false},
    {"ActorMailboxClosedError", 9004, 9000, false, false, false},
    {"ActorMessageRejectedError", 9005, 9000, false, false, false},
    {"ActorProtocolMismatchError", 9006, 9000, false, false, false},
    {"ActorAskTimeoutError", 9007, 9000, false, false, false},
    {"ActorReplyUnavailableError", 9008, 9000, false, false, false},
    {"ActorReferenceExpiredError", 9009, 9000, false, false, false},
    {"ActorNodeUnreachableError", 9010, 9000, false, false, false},
    {"ActorNameConflictError", 9011, 9000, false, false, false},
    {"ActorSpawnQuotaExceededError", 9012, 9000, false, false, false},
    {"ActorSupervisorUnavailableError", 9013, 9000, false, false, false},
    {"ActorOutputClosedError", 9014, 9000, false, false, false},
}};

constexpr const CoreErrorDescriptor* find_core_error_by_code(int code) noexcept {
    for (const auto& descriptor : core_error_catalog) {
        if (descriptor.code == code) return &descriptor;
    }
    return nullptr;
}

constexpr const CoreErrorDescriptor* find_core_error_by_name(
        std::string_view name) noexcept {
    for (const auto& descriptor : core_error_catalog) {
        if (descriptor.name == name) return &descriptor;
    }
    return nullptr;
}

constexpr bool is_canonical_core_error_descriptor(
        const CoreErrorDescriptor& candidate) noexcept {
    for (const auto& descriptor : core_error_catalog) {
        // Comparing the address of the trusted table entry, not its guest-
        // controllable name/code. A matching forged value is rejected.
        if (&descriptor == &candidate) return true;
    }
    return false;
}

constexpr bool core_error_is_a(const CoreErrorDescriptor& type,
                               const CoreErrorDescriptor& ancestor) noexcept {
    if (!is_canonical_core_error_descriptor(type)
            || !is_canonical_core_error_descriptor(ancestor)) return false;
    const CoreErrorDescriptor* cursor = &type;
    // A validated finite acyclic table; bound traversal defensively anyway.
    for (std::size_t depth = 0; cursor && depth < core_error_catalog.size(); ++depth) {
        if (cursor == &ancestor) return true;
        cursor = find_core_error_by_code(cursor->parent_code);
    }
    return false;
}

constexpr bool validate_core_error_catalog() noexcept {
    if (core_error_catalog.size() != 99
            || core_error_catalog[0].name != "Error"
            || core_error_catalog[0].code != 0
            || core_error_catalog[0].parent_code != -1
            || core_error_catalog[0].abstract_type) return false;
    for (std::size_t i = 0; i < core_error_catalog.size(); ++i) {
        const auto& e = core_error_catalog[i];
        if (e.name.empty() || e.code < 0 || (i > 0 && e.parent_code < 0))
            return false;
        bool parent_seen = i == 0;
        for (std::size_t j = 0; j < i; ++j) {
            const auto& previous = core_error_catalog[j];
            if (previous.code == e.code || previous.name == e.name) return false;
            if (previous.code == e.parent_code) parent_seen = true;
        }
        if (!parent_seen) return false;
        if (e.abstract_type && (e.retry_advisory || e.actor_api_error))
            return false;
    }
    return true;
}
static_assert(validate_core_error_catalog(), "invalid std/error v1 native catalog");

// A host-native value with owned diagnostics, but no reference to mutable
// guest objects or Throwable/exception_ptr. Not a source-level Oreslang Error.
class NativeCoreError final {
public:
    // The caller MUST have linked an authenticated nominal Oreslang type to a
    // table entry. A code or name received from a guest is not that proof.
    static NativeCoreError make_trusted(const CoreErrorDescriptor& descriptor,
                                        std::string_view diagnostic) {
        if (!is_canonical_core_error_descriptor(descriptor))
            throw std::invalid_argument("noncanonical std/error descriptor");
        if (descriptor.abstract_type)
            throw std::invalid_argument("abstract std/error type");
        return NativeCoreError(&descriptor, bounded_control_diagnostic(diagnostic));
    }

    const CoreErrorDescriptor& type() const noexcept { return *descriptor_; }
    int code() const noexcept { return descriptor_->code; }
    const std::string& diagnostic() const noexcept { return diagnostic_; }
    bool is_a(const CoreErrorDescriptor& ancestor) const noexcept {
        return core_error_is_a(*descriptor_, ancestor);
    }
    // Classification only: never schedule an automatic retry from this flag.
    bool has_retry_advisory() const noexcept { return descriptor_->retry_advisory; }

private:
    NativeCoreError(const CoreErrorDescriptor* descriptor, std::string diagnostic)
        : descriptor_(descriptor), diagnostic_(std::move(diagnostic)) {}
    const CoreErrorDescriptor* descriptor_;
    std::string diagnostic_;
};

} // namespace oreslang::runtime
