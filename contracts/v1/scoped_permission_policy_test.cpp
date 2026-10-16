#include "scoped_permission_policy.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace oreslang::permissions;
static int checks = 0;
static void require(bool actual, bool expected, const char* label) {
    ++checks;
    if (actual != expected) throw std::runtime_error(label);
}
int main() {
    const ScopedPolicy none;
    for (auto p : {Permission::Read,Permission::Write,Permission::Net,Permission::Env,
                   Permission::Run,Permission::Sys,Permission::Ffi,Permission::Import})
        require(none.allows(p,"/workspace/test"), false, "default-deny");
    const auto scope = [](Permission p, std::string s, bool deny=false){return ScopeRule{p,std::move(s),deny};};
    const auto make = [&](Permission p,std::vector<ScopeRule> rules){return ScopedPolicy::from_host({p},std::move(rules));};
    auto file = make(Permission::Read,{scope(Permission::Read,"/workspace/data")});
    auto root = make(Permission::Read,{scope(Permission::Read,"/")});
    require(root.allows(Permission::Read,"/workspace/data"),true,"explicit root scope");
    require(root.allows(Permission::Read,"relative/path"),false,"no relative paths");
    require(file.allows(Permission::Read,"/workspace/data/file.txt"),true,"path child");
    require(file.allows(Permission::Read,"/workspace/database/file.txt"),false,"path prefix");
    require(file.allows(Permission::Read,"/workspace/data "),false,"whitespace");
    require(file.allows(Permission::Read,"/workspace/data/../secret"),false,"dot traversal");
    require(file.allows(Permission::Write,"/workspace/data/file"),false,"no write ceiling");
    require(file.allows(Permission::Read,"/workspace/" + std::string(5000, 'x')),
            false, "oversized path resource rejected");
    bool rejected_oversized_path = false;
    try { (void)make(Permission::Read,{scope(Permission::Read,"/" + std::string(5000, 'x'))}); }
    catch (const std::invalid_argument&) { rejected_oversized_path = true; }
    require(rejected_oversized_path,true,"oversized path scope rejected");
    bool rejected_oversized_rules = false;
    try { (void)make(Permission::Env,std::vector<ScopeRule>(513,scope(Permission::Env,"KEY"))); }
    catch (const std::invalid_argument&) { rejected_oversized_rules = true; }
    require(rejected_oversized_rules,true,"oversized policy rule vector rejected");
    bool rejected_oversized_token = false;
    try { (void)make(Permission::Env,{scope(Permission::Env,std::string(1025, 'a'))}); }
    catch (const std::invalid_argument&) { rejected_oversized_token = true; }
    require(rejected_oversized_token,true,"oversized token rejected");
    auto env = make(Permission::Env,{scope(Permission::Env,"*"),scope(Permission::Env,"API_KEY",true)});
    require(env.allows(Permission::Env,"API_KEY"),false,"deny precedence");
    require(env.allows(Permission::Env,"API_KEY "),true,"preserve literal env resource");
    require(env.allows(Permission::Env,"OTHER"),true,"wildcard env");
    auto run = make(Permission::Run,{scope(Permission::Run,"git*")});
    require(run.allows(Permission::Run,"git-status"),true,"token prefix");
    require(run.allows(Permission::Run,"curl"),false,"token prefix negative");
    auto host = make(Permission::Net,{scope(Permission::Net,"api.example.com:443")});
    require(host.allows(Permission::Net,"API.EXAMPLE.COM.:0443"),true,"DNS canonical");
    require(host.allows(Permission::Net,"api.example.com:444"),false,"port separation");
    require(host.allows(Permission::Net,"api.example.com:0"),false,"invalid port");
    require(host.allows(Permission::Net,"127.1:443"),false,"numeric alias");
    require(host.allows(Permission::Net,"alice@api.example.com:443"),false,"url credentials");
    auto numeric_alias = make(Permission::Net,{scope(Permission::Net,"*"),
        scope(Permission::Net,"127.0.0.1",true)});
    for (const char* host_alias : {"127.0x0.0.1:443", "1.2.3.0x01:443",
                                   "0x7f.0.0.1:443", "127.0.0.01:443",
                                   "127.1:443"}) {
        require(numeric_alias.allows(Permission::Net,host_alias),false,
                "numeric IPv4 alias under broad allow");
    }
    require(numeric_alias.allows(Permission::Net,"127.0.0.1:443"),false,
            "canonical denied IPv4");
    require(numeric_alias.allows(Permission::Net,"api.example.com:443"),true,
            "ordinary DNS permitted under wildcard");
    // Differential-style, self-contained numeric-address regression matrix:
    // broad allow-net must reject every noncanonical numeric/hex spelling.
    // This is a policy test, not an actual socket/connect-time DNS guard.
    auto numeric_wildcard = make(Permission::Net,{scope(Permission::Net,"*")});
    const std::vector<std::string> numeric_parts = {
        "0","00","01","1","127","0x0","0x1","0x7f",
        "0X7F","0xFF","077","255","256"
    };
    const auto canonical_octet = [](const std::string& s) {
        return s == "0" || s == "1" || s == "127" || s == "255";
    };
    for (const auto& first : numeric_parts)
        for (const auto& second : numeric_parts)
            for (const auto& third : numeric_parts) {
                const std::string endpoint =
                    first + "." + second + "." + third + ".1:443";
                require(numeric_wildcard.allows(Permission::Net,endpoint),
                        canonical_octet(first) && canonical_octet(second) &&
                            canonical_octet(third),
                        "noncanonical numeric address matrix");
            }
    require(host.allows(Permission::Net,std::string(321, 'a')),false,
            "oversized endpoint rejected before allocating parser state");
    // Canonical IPv6 string equivalence and deny precedence; no DNS needed.
#if defined(__unix__) || defined(__APPLE__)
    auto ipv6 = make(Permission::Net, {
        scope(Permission::Net,"[0:0:0:0:0:0:0:1]:443")});
    require(ipv6.allows(Permission::Net,"[::1]:443"),true,"IPv6 compression canonical");
    require(ipv6.allows(Permission::Net,"[::1]:444"),false,"IPv6 port isolation");
    auto ipv6deny = make(Permission::Net, {
        scope(Permission::Net,"*"),scope(Permission::Net,"[::1]:443",true)});
    require(ipv6deny.allows(Permission::Net,"[0:0:0:0:0:0:0:1]:443"),false,
            "IPv6 expanded spelling cannot evade deny");
    require(ipv6deny.allows(Permission::Net,"[::1]"),false,
            "IPv6 portless request cannot evade port-specific deny");
    for (const char* bad : {"::1:443", "[::1", "[::1]:", "[::1]:0",
                            "[::1]:65536", "[::1]x:443", "[::1]:-1",
                            "[fe80::1%eth0]:443", "[::ffff:127.0.0.1]:443",
                            "[::192.0.2.1]:443", "[::ffff:c000:201]:443"}) {
        require(ipv6deny.allows(Permission::Net,bad),false,"reject unsafe IPv6 syntax");
    }
#endif
    auto sub = make(Permission::Net,{scope(Permission::Net,"*.example.com:443")});
    require(sub.allows(Permission::Net,"a.example.com:443"),true,"wildcard subdomain");
    require(sub.allows(Permission::Net,"example.com:443"),false,"no apex");
    require(sub.allows(Permission::Net,"badexample.com:443"),false,"boundary");
    auto bounded = ScopedPolicy::from_host({}, {scope(Permission::Net,"*")});
    require(bounded.allows(Permission::Net,"api.example.com:443"),false,"coarse ceiling");
    auto denied = make(Permission::Net,{scope(Permission::Net,"*"),scope(Permission::Net,"api.example.com:443",true)});
    require(denied.allows(Permission::Net,"api.example.com:443"),false,"net explicit deny");
    require(denied.allows(Permission::Net,"api.example.com"),false,"portless deny");
    const auto named = make(Permission::Net, {
        scope(Permission::Net,"api.example.com:443"),
        scope(Permission::Net,"127.0.0.1",true)
    });
    require(named.allows(Permission::Net,"api.example.com:443"),true,
            "host allowed before resolution");
    require(named.allows_network_connection("api.example.com:443","127.0.0.1:443"),
            false,"resolved denied loopback");
    require(named.allows_network_connection("api.example.com:443","203.0.113.7:443"),
            true,"selected unblocked canonical IPv4");
    require(named.allows_network_connection("api.example.com:443","203.0.113.7:444"),
            false,"selected port must match requested port");
    require(named.allows_network_connection("api.example.com:443","evil.example:443"),
            false,"only numeric IP as resolved identity");
    require(named.allows_network_connection("api.example.com:443","127.1:443"),
            false,"no abbreviated numeric alias");
    require(named.allows_network_connection("api.example.com:443","[::1]:443"),
            false,"IPv6 unavailable, fail closed");
    require(named.allows_network_connection("api.example.com","203.0.113.7:443"),
            false,"pre-connect authorization requires explicit port");
    require(named.allows_network_connection("other.example.com:443","203.0.113.7:443"),
            false,"host permission still required");
    auto allowed_literal = make(Permission::Net, {
        scope(Permission::Net,"203.0.113.7:443")});
    require(allowed_literal.allows_network_connection("203.0.113.7:443", "203.0.113.7:443"),
            true,"numeric request bound to exact selected address");
    require(allowed_literal.allows_network_connection("203.0.113.7:443", "198.51.100.8:443"),
            false,"numeric request cannot authorize different selected IP");
    auto broad_net = make(Permission::Net,{scope(Permission::Net,"*")});
    require(broad_net.allows_network_connection("203.0.113.7:443", "198.51.100.8:443"),
            false,"even wildcard grant cannot excuse numeric address mismatch");
    require(broad_net.allows_network_connection("[::1]:443", "203.0.113.7:443"),
            false,"IPv6 request cannot pass IPv4-only selected address check");
    auto allDeny = make(Permission::Net,{scope(Permission::Net,"*"),scope(Permission::Net,"*",true)});
    require(allDeny.allows(Permission::Net,"other.example.org:443"),false,"deny all");
    for (const char* bad : {"/workspace/../secret", "/workspace//secret", "relative/path"}) {
        bool rejected = false;
        try { (void)make(Permission::Read,{scope(Permission::Read,bad)}); }
        catch (const std::invalid_argument&) {rejected = true;}
        require(rejected,true,"scope invalid");
    }
    bool invalid = false;
    try { (void)ScopedPolicy::from_host({static_cast<Permission>(99)},{}); }
    catch (const std::invalid_argument&) {invalid = true;}
    require(invalid,true,"invalid enum");
    std::cout << "NATIVE_SCOPED_PERMISSION_POLICY_SUBSTRATE_PASS checks=" << checks << "\n";
}
