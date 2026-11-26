#include "oreslang/frontend/NativeOresiV1Admission.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace oreslang::package_interface;

// Closed fixture metadata; the test driver owns the release pins.
// Not a production interface exporter or registry authentication mechanism.
static void str(std::vector<std::uint8_t>& bytes,const std::string& s) {
    auto n=static_cast<std::uint32_t>(s.size());
    for(int i=3;i>=0;--i) bytes.push_back(static_cast<std::uint8_t>(n>>(8*i)));
    bytes.insert(bytes.end(),s.begin(),s.end());
}
int main(int argc,char** argv) {
    try {
        if(argc!=4)throw std::runtime_error("expected module release expected-release");
        const std::string module=argv[1],release=argv[2],expected=argv[3];
        if((module!="cart"&&module!="tax")||(release!="1"&&release!="2")||
           (expected!="1"&&expected!="2"))
            throw std::runtime_error("invalid fixture arguments");
        const auto identity=[&](const std::string& v){return "fixture/"+module+"@"+v;};
        std::vector<std::uint8_t> bytes={'O','R','E','S','I',0,1};
        str(bytes,module);str(bytes,"atto-llvm-1");str(bytes,"llvm-aot-v1");
        str(bytes,"ownership-strict-v1");
        bytes.insert(bytes.end(),{0,0,0,2});
        str(bytes,module);bytes.push_back(0);str(bytes,module);str(bytes,"");
        str(bytes,module);bytes.push_back(1);str(bytes,"value");str(bytes,"():int");
        DependencyPin pin{module,identity(release),"atto-llvm-1","llvm-aot-v1",
                          "ownership-strict-v1",{{module,sha256(bytes)}}};
        const auto verified=admit(pin,{{module,bytes}});
        ScalarLogicalSlotPin slot{"fixture/app",module,identity(expected),
                                  "atto-llvm-1","ownership-strict-v1"};
        std::cout<<stable_scalar_link_symbol(verified,slot,module,module,"value")<<'\n';
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';return 65;
    }
}
