#include "oreslang/frontend/NativeOresiV1Admission.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <map>

using namespace oreslang::package_interface;

static void require(bool condition,const char* message){
    if(!condition)throw std::runtime_error(message);
}
template<class F> static void fails(const char* expected,F&& f){
    try { f(); }
    catch(const std::exception& e){
        if(std::string(e.what()).find(expected)!=std::string::npos)return;
        throw std::runtime_error(std::string("unexpected rejection: ")+e.what());
    }
    throw std::runtime_error(std::string("unexpected admission: ")+expected);
}
static void u32(std::vector<std::uint8_t>& out,std::uint32_t n){
    for(int i=3;i>=0;--i)out.push_back(static_cast<std::uint8_t>(n>>(8*i)));
}
static void str(std::vector<std::uint8_t>& out,const std::string& value){
    u32(out,static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(),value.begin(),value.end());
}
static void entry(std::vector<std::uint8_t>& bytes,const std::string& module,
                  std::uint8_t kind,const std::string& name,
                  const std::string& signature){
    str(bytes,module);bytes.push_back(kind);str(bytes,name);str(bytes,signature);
}
// Exact Java ExportInterfaceCodec v1 envelope layout, NOT a native invented ABI.
static std::vector<std::uint8_t> fixture(const std::string& ns,
          const std::string& module,const std::string& target="java-aot-v1"){
    std::vector<std::uint8_t> bytes={'O','R','E','S','I',0,1};
    str(bytes,ns);
    str(bytes,"atto-java-1");
    str(bytes,target);
    str(bytes,"ownership-strict-v1");
    u32(bytes,2);
    entry(bytes,module,0,module,"");
    entry(bytes,module,1,"add","(int a,int b):int");
    return bytes;
}
static DependencyPin lock(const std::string& ns,
             const std::vector<std::uint8_t>& bytes){
    return {"math","acme/math@1#"+std::string(64,'a'),"atto-java-1",
        "java-aot-v1","ownership-strict-v1",{{ns,sha256(bytes)}}};
}
int main() {
    require(sha256({})==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA256 empty known-answer");
    require(sha256({'a','b','c'})==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA256 abc known-answer");

    const auto math=fixture("math","algebra");
    require(sha256(math)==
        "8db5681e1e41c389e2358f9e0cd5c46e78b5b33519ed4769056268a240847f3d",
        "Java .oresi v1 frozen wire digest drift");
    auto pin=lock("math",math);
    auto guest=admit(pin,{{"math",math}});
    require(guest.alias=="math","alias mismatch");
    require(guest.package_instance_id==pin.package_instance_id,"locked instance mismatch");
    require(guest.namespace_exports("math").size()==2,"export count mismatch");
    require(guest.module_exports("math","algebra").size()==2,"module binding mismatch");
    fails("module not exported",[&]{(void)guest.module_exports("math","hidden");});
    fails("namespace not exported",[&]{(void)guest.namespace_exports("ambient");});

    auto wrong=math;
    wrong.back()^=0x01;
    fails("digest mismatch",[&]{(void)admit(pin,{{"math",wrong}});});
    auto wrong_sha=pin;
    wrong_sha.namespaces.front().sha256=std::string(64,'f');
    fails("digest mismatch",[&]{(void)admit(wrong_sha,{{"math",math}});});

    auto foreign_target=fixture("math","algebra","llvm-aot-v1");
    auto foreign_pin=lock("math",foreign_target);
    fails("ABI/profile mismatch",[&]{(void)admit(foreign_pin,{{"math",foreign_target}});});

    auto wrong_ns=fixture("other","algebra");
    auto wrong_pin=lock("math",wrong_ns);
    fails("ABI/profile mismatch",[&]{(void)admit(wrong_pin,{{"math",wrong_ns}});});

    auto trailing=math;trailing.push_back(0);
    auto trailing_pin=lock("math",trailing);
    fails("trailing .oresi bytes",[&]{(void)admit(trailing_pin,{{"math",trailing}});});

    auto wrong_magic=math;wrong_magic[6]=2;
    auto magic_pin=lock("math",wrong_magic);
    fails("unsupported .oresi format",[&]{(void)admit(magic_pin,{{"math",wrong_magic}});});

    auto missing_module=std::vector<std::uint8_t>{'O','R','E','S','I',0,1};
    str(missing_module,"math");str(missing_module,"atto-java-1");
    str(missing_module,"java-aot-v1");str(missing_module,"ownership-strict-v1");
    u32(missing_module,1);entry(missing_module,"private_tools",1,"leak","()int");
    auto orphan_pin=lock("math",missing_module);
    fails("containing module visibility",[&]{
        (void)admit(orphan_pin,{{"math",missing_module}});
    });

    auto duplicate=std::vector<std::uint8_t>{'O','R','E','S','I',0,1};
    str(duplicate,"math");str(duplicate,"atto-java-1");
    str(duplicate,"java-aot-v1");str(duplicate,"ownership-strict-v1");
    u32(duplicate,2);
    entry(duplicate,"algebra",0,"algebra","");
    entry(duplicate,"algebra",0,"algebra","");
    auto duplicate_pin=lock("math",duplicate);
    fails("noncanonical .oresi order",[&]{(void)admit(duplicate_pin,{{"math",duplicate}});});

    auto malformed=math;malformed[11]=0xff; // first namespace UTF8 byte
    auto malformed_pin=lock("math",malformed);
    fails("invalid .oresi UTF-8",[&]{(void)admit(malformed_pin,{{"math",malformed}});});

    auto duplicate_ns=pin;
    duplicate_ns.namespaces.push_back(duplicate_ns.namespaces.front());
    fails("invalid or duplicate",[&]{(void)admit(duplicate_ns,
        {{"math",math},{"other",math}});});

    auto bad_instance=pin;bad_instance.package_instance_id="../escaped";
    fails("invalid locked",[&]{(void)admit(bad_instance,{{"math",math}});});

    auto legacy_pkg=pin;legacy_pkg.package_instance_id="foo@sha:"+std::string(64,'1');
    require(admit(legacy_pkg,{{"math",math}}).package_instance_id==
            legacy_pkg.package_instance_id,"sha-colon package ID rejected");
    auto second_pkg=pin;second_pkg.package_instance_id="acme/math@1#"+std::string(64,'b');
    const auto other=admit(second_pkg,{{"math",math}});
    require(other.package_instance_id!=guest.package_instance_id,
        "same name does not grant same nominal package instance");

    // A 2nd namespace must be in the exact pinned closed inventory.
    const auto net=fixture("network","tcp");
    auto two=pin;two.namespaces.push_back({"network",sha256(net)});
    const auto two_verified=admit(two,{{"math",math},{"network",net}});
    require(two_verified.namespaces.size()==2,"multi-namespace pin mismatch");
    fails("invalid locked",[&]{(void)admit(two,{{"math",math}});});

    std::cout<<"Native .oresi host preflight passed; no Atto guest or binary linker executed\n";
}
