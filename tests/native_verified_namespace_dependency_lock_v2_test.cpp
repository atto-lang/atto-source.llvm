#include "oreslang/frontend/VerifiedNamespaceDependencyLockV2.hpp"
#include <openssl/sha.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
using namespace oreslang::frontend;
static_assert(!std::is_default_constructible<VerifiedNamespaceDependencyLockV2>::value,
              "unverified caller must not construct dependency pins");
static std::string sha(std::string_view v) {
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(v.data()),v.size(),d);
    const char* hex="0123456789abcdef";std::string result;
    for(unsigned char c:d){result+=hex[c>>4];result+=hex[c&15];}return result;
}
template<class F>void denied(F&& fn) {
    try {fn();} catch(const std::invalid_argument&) {return;}
    throw std::runtime_error("bad dependency lock accepted");
}
int main(){
    const std::string a="v2:"+std::string(64,'a')+":"+std::string(64,'b')+":app";
    const std::string b="v2:"+std::string(64,'c')+":"+std::string(64,'d')+":checkout";
    const std::set<std::string> owners{a,b};
    const std::string entry="[[dependency]]\nimporter = \""+a+"\"\nalias = \"checkout\"\ntarget = \""+b+"\"\n";
    const std::string good="schema_version = \"1\"\n"+entry;
    const auto auth=[&](const std::string& txt){return verify_namespace_dependency_lock_v2(txt,sha(txt),owners,sha);};
    int cases=0;
    if(auth(good).alias_pins().at(a).at("checkout") != b)
        throw std::runtime_error("lock target mismatch");
    ++cases;
    denied([&]{verify_namespace_dependency_lock_v2(good,sha("different"),owners,sha);});++cases;
    denied([&]{verify_namespace_dependency_lock_v2(good,sha(good),{},sha);});++cases;
    denied([&]{verify_namespace_dependency_lock_v2(good,sha(good),owners,NamespaceLockHashV2{});});++cases;
    denied([&]{auth(good+entry);});++cases;
    denied([&]{auth("schema_version = \"1\"\n"+entry+"alias = \"second\"\n");});++cases;
    denied([&]{auth("schema_version = \"1\"\n"+entry+"unknown = \"x\"\n");});++cases;
    denied([&]{auth("schema_version = \"2\"\n"+entry);});++cases;
    denied([&]{auth(entry);});++cases;
    denied([&]{auth("schema_version = \"1\"\n");});++cases;
    denied([&]{auth("schema_version = \"1\"\n[[dependency]]\nimporter = \""+a+"\"\nalias = \"checkout\"\n");});++cases;
    denied([&]{auth("schema_version = \"1\"\n[[dependency]]\nimporter = \""+a+"\"\nalias = \"checkout\"\ntarget = \""+a+"\"\n");});++cases;
    denied([&]{auth("schema_version = \"1\"\n[[dependency]]\nimporter = \""+a+"\"\nalias = \"+/\"\ntarget = \""+b+"\"\n");});++cases;
    denied([&]{auth("schema_version = \"1\"\n[[dependency]]\nimporter = \""+a+"\"\nalias = \"checkout\"\ntarget = \""+b+"\" # comment\n");});++cases;
    denied([&]{auth("schema_version = \"1\"\n"+entry+"[unexpected]\n");});++cases;
    denied([&]{auth(std::string(65537,'x'));});++cases;
    // Additional cross-package pin hardening: a syntactically valid but
    // physically unadmitted target cannot appear in the verified alias map.
    const std::string unlisted="v2:"+std::string(64,'e')+":"+std::string(64,'f')+":other";
    denied([&]{auth(good+"[[dependency]]\nimporter = \""+a+"\"\nalias = \"backup\"\ntarget = \""+unlisted+"\"\n");});++cases;
    // Neither can an unadmitted importer acquire authority over a valid target.
    denied([&]{auth(good+"[[dependency]]\nimporter = \""+unlisted+"\"\nalias = \"backup\"\ntarget = \""+b+"\"\n");});++cases;
    // Aliases must be canonical identifiers, not numeric or reserved path forms.
    denied([&]{auth("schema_version = \"1\"\n[[dependency]]\nimporter = \""+a+"\"\nalias = \"1checkout\"\ntarget = \""+b+"\"\n");});++cases;
    // Repeated schema assignments after a dependency entry are not ignored.
    denied([&]{auth(good+"schema_version = \"1\"\n");});++cases;
    // A digest-shaped owner with uppercase hex is not a canonical package ID.
    const std::string wrong_case="v2:"+std::string(64,'A')+":"+std::string(64,'b')+":app";
    denied([&]{verify_namespace_dependency_lock_v2(good,sha(good),{a,b,wrong_case},sha);});++cases;
    // Two distinct aliases from the same verified importer may share one
    // exact target without weakening the physical package identity check.
    const auto two=auth(good+"[[dependency]]\nimporter = \""+a+"\"\nalias = \"backup\"\ntarget = \""+b+"\"\n");
    if(two.alias_pins().at(a).size()!=2 || two.alias_pins().at(a).at("backup")!=b)
        throw std::runtime_error("distinct pinned aliases were not preserved");
    ++cases;
    std::cout<<"PASS independently pinned namespace dependency lock cases="<<cases<<"\n";
}
