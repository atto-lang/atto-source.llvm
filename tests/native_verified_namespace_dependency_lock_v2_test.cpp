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
    std::cout<<"PASS independently pinned namespace dependency lock cases="<<cases<<"\n";
}
