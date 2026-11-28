#include "oreslang/frontend/NativeScalarObjectAdmission.hpp"
#include <fstream>
#include <iterator>
#include <type_traits>
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
        if(argc!=4 && argc!=10)throw std::runtime_error("expected module release expected-release [source-pin object-pin target profile object-file mode]");
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
        const auto symbol=stable_scalar_link_symbol(verified,slot,module,module,"value");
        if(argc==4){std::cout<<symbol<<'\n';return 0;}
        const std::string mode=argv[9];
        NativeScalarObjectPin object_pin{slot,module,module,"value",argv[4],argv[5],
            symbol,argv[6],argv[7]};
        if(mode=="symbol")object_pin.physical_symbol+="wrong";
        if(mode=="source")object_pin.expected_source_sha256=std::string(64,'0');
        if(mode=="target")object_pin.target_triple="aarch64-linux-gnu";
        if(mode=="profile")object_pin.optimization_profile="O3";
        std::ifstream input(argv[8],std::ios::binary|std::ios::ate);
        if(!input || input.tellg()<=0 || input.tellg()>16*1024*1024)
            throw std::runtime_error("invalid fixture object size");
        const auto n=static_cast<std::size_t>(input.tellg());
        input.seekg(0);
        std::vector<std::uint8_t> candidate(n);
        if(!input.read(reinterpret_cast<char*>(candidate.data()),n))
            throw std::runtime_error("fixture read failed");
        static_assert(!std::is_default_constructible_v<VerifiedNativeScalarObject>);
        const auto admitted=admit_native_scalar_object(verified,object_pin,argv[4],
            "x86_64-pc-linux-gnu",argv[7],candidate);
        candidate[0]^=1;
        if(sha256(admitted.bytes())!=admitted.object_digest())
            throw std::runtime_error("mutable object admission snapshot");
        // Publish the admitted snapshot, never reopen the candidate path.
        std::cout.write(reinterpret_cast<const char*>(admitted.bytes().data()),admitted.bytes().size());
        if(!std::cout)throw std::runtime_error("fixture output failed");
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';return 65;
    }
}
