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
    require(file.allows(Permission::Read,"/workspace/data/file.txt"),true,"path child");
    require(file.allows(Permission::Read,"/workspace/database/file.txt"),false,"path prefix");
    require(file.allows(Permission::Read,"/workspace/data "),false,"whitespace");
    require(file.allows(Permission::Read,"/workspace/data/../secret"),false,"dot traversal");
    require(file.allows(Permission::Write,"/workspace/data/file"),false,"no write ceiling");
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
    auto sub = make(Permission::Net,{scope(Permission::Net,"*.example.com:443")});
    require(sub.allows(Permission::Net,"a.example.com:443"),true,"wildcard subdomain");
    require(sub.allows(Permission::Net,"example.com:443"),false,"no apex");
    require(sub.allows(Permission::Net,"badexample.com:443"),false,"boundary");
    auto bounded = ScopedPolicy::from_host({}, {scope(Permission::Net,"*")});
    require(bounded.allows(Permission::Net,"api.example.com:443"),false,"coarse ceiling");
    auto denied = make(Permission::Net,{scope(Permission::Net,"*"),scope(Permission::Net,"api.example.com:443",true)});
    require(denied.allows(Permission::Net,"api.example.com:443"),false,"net explicit deny");
    require(denied.allows(Permission::Net,"api.example.com"),false,"portless deny");
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
