#include "oreslang/frontend/Parser.hpp"
#include "oreslang/backend/NestedEnumV2.hpp"
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    const auto program=oreslang::parse_program(R"(
        define enum Bar as
            Message1 = 1
        end
        define enum Foo as
            Message1: Bar
        end
        define match pattern Messages as
            ...Foo
        end
    )");
    const auto wire=oreslang::nested_enum_v2::one_level_nullary(
        program,"Foo","Foo.Message1","Bar.Message1");
    const std::string expected=
        "9402d98f6f7265736c616e672e656e756d2e306162326236366364343839"
        "373130373332323563653264353938323735336137343830326636653261316437393861"
        "306632646364386665383233643232612e326434356430383134646535323635373039"
        "326261626636363331323437633761623136303164643035373935646432616534323833"
        "65303665383036373039c42071605bdfa3efdbcb5fe7aa4d4230f049269d4cd630b"
        "72c8c27f452c9b09ea42093a3466f6f0192a342617201";
    std::string got;
    static constexpr char symbols[]="0123456789abcdef";
    for(auto b:wire){ got.push_back(symbols[b>>4]);got.push_back(symbols[b&15]); }
    if(got!=expected) {
        std::cerr << "nested MessagePack golden mismatch: " << got << "\n";
        return 1;
    }
    const auto mustReject=[&](const std::string& d,const std::string& o,const std::string& i) {
        bool rejected=false;
        try { (void)oreslang::nested_enum_v2::one_level_nullary(program,d,o,i); }
        catch(const std::runtime_error&){ rejected=true; }
        assert(rejected);
    };
    mustReject("Foo","Foo.Message1","Foo.Message1");
    mustReject("Foo","Bar.Message1","Bar.Message1");
    mustReject("Missing","Foo.Message1","Bar.Message1");
    std::cout << "NESTED_ENUM_NATIVE_GOLDEN_PASS bytes=" << wire.size() << "\n";
    return 0;
}
