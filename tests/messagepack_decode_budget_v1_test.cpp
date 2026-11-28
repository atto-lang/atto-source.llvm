#include "oreslang/runtime/MessagePackWireV2.hpp"
#include <iostream>
#include <stdexcept>
using namespace oreslang::wire;
template<class F> void rejected(F f) {
    try { f(); } catch(const std::invalid_argument&) { return; }
    throw std::runtime_error("expected budget rejection");
}
int main() {
    const std::string id = "budget.v1";
    const auto shape = Shape::list(Shape::list(Shape::string()));
    const Value msg(Value::Items{Value(Value::Items{Value("abcd"),Value("efgh")}),
                                Value(Value::Items{Value("ijkl"),Value("mnop")})});
    const auto bytes = encode(id,shape,msg);
    if (!(decode(id,shape,bytes)==msg)) throw std::runtime_error("legacy roundtrip");
    DecodeLimits l;
    l.wire=bytes.size();
    if (!(decode(id,shape,bytes,l)==msg)) throw std::runtime_error("exact wire cap");
    --l.wire; rejected([&]{decode(id,shape,bytes,l);});
    l=DecodeLimits{}; l.elements=5;
    rejected([&]{decode(id,shape,bytes,l);});
    l.elements=6;
    if (!(decode(id,shape,bytes,l)==msg)) throw std::runtime_error("cumulative elements");
    l=DecodeLimits{}; l.expanded=sizeof(Value)*6+id.size()+16;
    if (!(decode(id,shape,bytes,l)==msg)) throw std::runtime_error("expanded boundary");
    --l.expanded; rejected([&]{decode(id,shape,bytes,l);});
    l=DecodeLimits{}; l.depth=1; rejected([&]{decode(id,shape,bytes,l);});
    l=DecodeLimits{}; l.work=1; rejected([&]{decode(id,shape,bytes,l);});
    l=DecodeLimits{}; l.expanded=0; rejected([&]{decode(id,shape,bytes,l);});
    l=DecodeLimits{}; l.wire=kMaxBytes+1; rejected([&]{decode(id,shape,bytes,l);});
    std::cout<<"MESSAGEPACK_DECODE_BUDGET_V1_HOST_PASS\n";
}
