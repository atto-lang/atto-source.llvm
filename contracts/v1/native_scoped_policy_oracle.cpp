// Host policy oracle for the canonical 45 authorization vectors ONLY.
// Not an AttoLang guest executable or an acceptable guest-runtime report.
#include "scoped_permission_policy.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
using namespace oreslang::permissions;

static Permission parse_kind(std::string_view kind) {
    if (kind == "read") return Permission::Read;
    if (kind == "write") return Permission::Write;
    if (kind == "net") return Permission::Net;
    if (kind == "env") return Permission::Env;
    if (kind == "run") return Permission::Run;
    if (kind == "sys") return Permission::Sys;
    if (kind == "ffi") return Permission::Ffi;
    if (kind == "import") return Permission::Import;
    throw std::invalid_argument("unknown permission kind");
}
static unsigned parse_count(std::string_view s) {
    unsigned n = 0;
    const auto [last, err] = std::from_chars(s.data(), s.data()+s.size(), n);
    if (err != std::errc{} || last != s.data()+s.size() || n > 512 || s.empty())
        throw std::invalid_argument("invalid vector field count");
    return n;
}
int main(int argc, char** argv) {
    try {
        if (argc < 6) throw std::invalid_argument("incomplete host-policy vector");
        const Permission kind = parse_kind(argv[1]);
        const std::string resource(argv[2]);
        const std::string_view coarse(argv[3]);
        if (coarse != "0" && coarse != "1")
            throw std::invalid_argument("invalid coarse ceiling field");
        std::size_t i = 4;
        const unsigned allowed = parse_count(argv[i++]);
        if (static_cast<std::size_t>(argc) < i + allowed + 1)
            throw std::invalid_argument("incomplete allow vector");
        std::vector<ScopeRule> rules;
        for (unsigned n=0; n<allowed; ++n)
            rules.push_back({kind, argv[i++], false});
        const unsigned denied = parse_count(argv[i++]);
        if (static_cast<std::size_t>(argc) != i + denied || rules.size() + denied > 512)
            throw std::invalid_argument("incomplete deny vector");
        for (unsigned n=0; n<denied; ++n)
            rules.push_back({kind, argv[i++], true});
        const auto policy = ScopedPolicy::from_host(
            coarse == "1" ? std::vector<Permission>{kind} : std::vector<Permission>{},
            std::move(rules));
        std::cout << (policy.allows(kind, resource) ? "allow\n" : "deny\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HOST_POLICY_VECTOR_REJECTED: " << error.what() << '\n';
        return 2;
    }
}
