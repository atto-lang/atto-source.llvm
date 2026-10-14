#include "oreslang/runtime/NativeCoreError.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace oreslang::runtime;

static void check(bool pass, const char* why) {
    if (!pass) { std::cerr << why << "\n"; std::exit(EXIT_FAILURE); }
}
int main() {
    check(core_error_catalog.size() == 99, "catalog count");
    check(core_error_schema_version == 1, "schema version");
    for (const auto& entry : core_error_catalog) {
        check(find_core_error_by_name(entry.name) == &entry, "name lookup identity");
        check(find_core_error_by_code(entry.code) == &entry, "code lookup identity");
        check(core_error_is_a(entry, entry), "reflexive nominal relationship");
        check(core_error_is_a(entry, core_error_catalog[0]), "subtype of Error");
        if (entry.parent_code >= 0)
            check(core_error_is_a(entry, *find_core_error_by_code(entry.parent_code)),
                  "immediate nominal parent");
    }
    check(!find_core_error_by_code(-1), "negative code rejected");
    check(!find_core_error_by_code(999999), "unknown code rejected");
    check(!find_core_error_by_name("ActorPanicError"), "unknown class rejected");
    const auto& file = *find_core_error_by_name("FileNotFoundError");
    const auto& io = *find_core_error_by_name("IOError");
    const auto& net = *find_core_error_by_name("NetworkError");
    const auto& fs = *find_core_error_by_name("FileSystemError");
    const auto& actor = *find_core_error_by_name("ActorError");
    const auto& ask = *find_core_error_by_name("ActorAskTimeoutError");
    check(core_error_is_a(file, fs) && core_error_is_a(file, io), "filesystem inheritance");
    check(!core_error_is_a(file, net) && !core_error_is_a(io, file), "sibling/parent exclusion");
    check(core_error_is_a(ask, actor) && !core_error_is_a(ask, net), "actor inheritance");
    check(actor.abstract_type && actor.actor_api_error, "abstract ActorError interface");
    check(core_error_implements(*find_core_error_by_name("ActorError"),
                                CoreErrorInterface::ActorApiError),
          "abstract ActorError must implement ActorApiError");
    check(core_error_implements(ask, CoreErrorInterface::ActorApiError),
          "actor leaf inherits ActorApiError");
    check(core_error_implements(*find_core_error_by_name("MailboxFullError"),
                                CoreErrorInterface::ActorApiError),
          "state leaf direct ActorApiError membership");
    check(core_error_implements(*find_core_error_by_name("ProcessNotFoundError"),
                                CoreErrorInterface::ActorApiError),
          "process lookup direct ActorApiError membership");
    check(!core_error_implements(io, CoreErrorInterface::ActorApiError),
          "unrelated IOError not ActorApiError");
    check(!core_error_implements(ask, CoreErrorInterface::RetryAdvisory),
          "actor error must not imply retry");
    check(core_error_implements(ask, CoreErrorInterface::CodedError),
          "actor leaf inherits CodedError");
    check(core_error_implements(ask, CoreErrorInterface::Serializable),
          "actor leaf inherits Serializable");
    check(!core_error_implements(ask, static_cast<CoreErrorInterface>(0xff)),
          "unknown interface tag rejected");

    const CoreErrorDescriptor forged{"FileNotFoundError", 3001, 3090, false, false, false};
    check(!is_canonical_core_error_descriptor(forged), "value-spoofed descriptor accepted");
    check(!core_error_is_a(forged, io), "forged descriptor satisfied ancestry");
    check(!core_error_implements(forged, CoreErrorInterface::ActorApiError),
          "forged descriptor got inherited interface membership");
    try { (void)NativeCoreError::make_trusted(forged, "x"); check(false, "forged value accepted"); }
    catch (const std::invalid_argument&) {}
    try { (void)NativeCoreError::make_trusted(io, "x"); check(false, "abstract constructed"); }
    catch (const std::invalid_argument&) {}

    const auto concrete = NativeCoreError::make_trusted(file, "missing");
    check(concrete.code() == 3001 && concrete.diagnostic() == "missing", "native value payload");
    check(concrete.is_a(io) && !concrete.is_a(net), "native value ancestry");
    const auto actor_value = NativeCoreError::make_trusted(ask, "request failure");
    check(actor_value.is_actor_api_error() && !actor_value.has_retry_advisory(),
          "actor failure API interface inheritance");
    check(!concrete.is_actor_api_error(), "filesystem type acquired actor marker");

    std::size_t advisories = 0;
    for (const auto& entry : core_error_catalog) if (entry.retry_advisory) ++advisories;
    check(advisories == 5, "advisory set drift");
    const auto reset = NativeCoreError::make_trusted(
        *find_core_error_by_name("ConnectionResetError"), "retry advisory");
    check(reset.has_retry_advisory(), "advisory missing");
    check(!concrete.has_retry_advisory(), "non-advisory became advisory");

    const std::string invalid = std::string("bad:") + "\xed\xa0\x80";
    const auto normalized = NativeCoreError::make_trusted(file, invalid);
    check(normalized.diagnostic() == "bad:???", "UTF8 normalization");
    check(NativeCoreError::make_trusted(file, std::string(2048, 'x'))
               .diagnostic().size() == max_control_diagnostic_bytes, "bounded diagnostic");
    return EXIT_SUCCESS;
}
