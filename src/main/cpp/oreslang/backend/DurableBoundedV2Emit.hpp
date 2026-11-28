#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <set>

namespace oreslang {
// Trusted compiler manifest -> freestanding C bridge, not ordinary LLVM IR
// actor lowering. No user text is inserted into generated code.
struct BoundedFieldV2 {std::string name;unsigned tag;};
inline std::string emit_bounded_v2_bridge(std::string_view actor,const std::vector<BoundedFieldV2>& fields){
    auto deny=[](){throw std::invalid_argument("E_DURABLE_BOUNDED_SCHEMA_V2");};
    auto name=[](std::string_view n){auto alpha=[](unsigned char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_';};if(n.empty()||n.size()>128||!alpha(n.front()))return false;for(unsigned char c:n)if(!alpha(c)&&!(c>='0'&&c<='9'))return false;return true;};
    if(!name(actor)||fields.size()>256)deny();
    std::uint64_t h=UINT64_C(0xcbf29ce484222325);
    auto byte=[&](unsigned char b){h=(h^b)*UINT64_C(0x100000001b3);};
    auto sized=[&](std::string_view n){byte(static_cast<unsigned char>(n.size()));byte(0);byte(0);byte(0);for(unsigned char c:n)byte(c);};
    for(unsigned char c:std::string_view("atto.actor.state.schema.v2"))byte(c);
    byte(0);sized(actor);std::set<std::string> names;std::string tags;
    for(const auto& f:fields){if(!name(f.name)||f.tag<1||f.tag>4||!names.insert(f.name).second)deny();sized(f.name);byte(static_cast<unsigned char>(f.tag));tags+=std::to_string(f.tag)+",";}
    return "#define ATTO_BOUNDED_SCHEMA "+std::to_string(h)+"ULL\n#define ATTO_BOUNDED_TAGS "+tags+"\n#include \"DurableBoundedV2Bridge.h\"\n";
}
} // namespace oreslang
