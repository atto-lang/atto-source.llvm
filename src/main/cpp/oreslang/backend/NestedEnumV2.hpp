#pragma once
#include "oreslang/frontend/Parser.hpp"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang::nested_enum_v2 {

inline constexpr std::size_t kMaxDescriptorBytes = 1048576;
inline void require_descriptor_budget(const std::string& descriptor) {
    if (descriptor.size() > kMaxDescriptorBytes)
        throw std::runtime_error("nested enum shape descriptor exceeds byte quota");
}


/** Compile-time emitter for the first finite, one-level nested-enum literal.
 * This is a reusable wire-lowering building block, NOT yet a guest runtime
 * path or a general encoder for arbitrary nested payloads.
 */
inline std::array<std::uint8_t,32> sha(std::string_view src) {
    return llvm::SHA256::hash(llvm::ArrayRef<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(src.data()), src.size()));
}
inline std::string hex(const std::array<std::uint8_t,32>& data) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(data.size()*2);
    for (auto b: data) {
        output.push_back(digits[b>>4]);
        output.push_back(digits[b&15]);
    }
    return output;
}
// v2 nominal ABI is ASCII-only. Java hashes nested shape descriptors
// as US-ASCII: silently accepting non-ASCII here would permit a peer to
// calculate a different schema identity for the same guest spelling.
inline void require_wire_name(const std::string& value) {
    if (value.empty() || value.size()>256)
        throw std::runtime_error("nested enum wire name must be an ASCII identifier");
    const auto first = value.front();
    if (!((first>='a'&&first<='z') || (first>='A'&&first<='Z') || first=='_'))
        throw std::runtime_error("nested enum wire name must be an ASCII identifier");
    for (const auto c: value) {
        if (!((c>='a'&&c<='z') || (c>='A'&&c<='Z')
              || (c>='0'&&c<='9') || c=='_'))
            throw std::runtime_error("nested enum wire name must be an ASCII identifier");
    }
}
inline const EnumDeclaration& owner(const Program& program, const std::string& name) {
    const auto iter=std::find_if(program.enums.begin(),program.enums.end(),
        [&](const EnumDeclaration& e){return e.name==name;});
    if(iter==program.enums.end()) throw std::runtime_error("unknown nested enum owner: "+name);
    return *iter;
}
inline const EnumCase& variant(const EnumDeclaration& e,const std::string& name) {
    const auto iter=std::find_if(e.cases.begin(),e.cases.end(),
        [&](const EnumCase& c){return c.name==name;});
    if(iter==e.cases.end()) throw std::runtime_error("unknown nested enum variant: "+name);
    return *iter;
}
inline void shape(std::string& out,const Program& p,const MessageShape& src,
                  std::vector<std::string>& active,unsigned depth=0) {
    if(depth>64) throw std::runtime_error("nested enum shape depth exceeds 64");
    require_descriptor_budget(out);
    switch(src.kind) {
        case MessageShape::Kind::Boolean: out+="B";return;
        case MessageShape::Kind::Integer: out+="I";return;
        case MessageShape::Kind::String: out+="S";return;
        case MessageShape::Kind::Box:
            throw std::runtime_error("Box<T> has no portable nested wire shape");
        case MessageShape::Kind::EnumRef: {
            require_wire_name(src.nominal_name);
            const auto& decl=owner(p,src.nominal_name);
            require_wire_name(decl.name);
            if(std::find(active.begin(),active.end(),decl.name)!=active.end())
                throw std::runtime_error("recursive nominal wire shape is unsupported");
            active.push_back(decl.name);
            out+="E("+std::to_string(decl.name.size())+":"+decl.name+")[";
            for(const auto& c: decl.cases) {
                require_wire_name(c.name);
                out+=std::to_string(c.tag)+":"+std::to_string(c.name.size())
                     +":"+c.name+"=";
                if(c.has_payload) shape(out,p,c.payload,active,depth+1);
                else out+="N";
                out+=";";
                require_descriptor_budget(out);
            }
            out+="]";
            active.pop_back();
            return;
        }
        default: break;
    }
    const char* prefix = src.kind==MessageShape::Kind::Tuple ? "T["
        : src.kind==MessageShape::Kind::FixedArray ? "FA["
        : src.kind==MessageShape::Kind::FixedList ? "FL["
        : src.kind==MessageShape::Kind::Array ? "A[" : "L[";
    out+=prefix;
    for(std::size_t i=0;i<src.elements.size();i++) {
        if(i) out+=",";
        shape(out,p,src.elements[i],active,depth+1);
    }
    out+="]";
}
inline void mp_string(std::vector<std::uint8_t>& result,const std::string& s) {
    if(s.size()<=31) result.push_back(static_cast<std::uint8_t>(0xa0|s.size()));
    else if(s.size()<=255) {
        result.push_back(0xd9);
        result.push_back(static_cast<std::uint8_t>(s.size()));
    } else if(s.size()<=65535) {
        result.push_back(0xda);
        result.push_back(static_cast<std::uint8_t>(s.size()>>8));
        result.push_back(static_cast<std::uint8_t>(s.size()));
    } else throw std::runtime_error("nested enum string exceeds quota");
    result.insert(result.end(),s.begin(),s.end());
}
inline void mp_tag(std::vector<std::uint8_t>& result,std::uint32_t n) {
    if(n<=127) result.push_back(static_cast<std::uint8_t>(n));
    else if(n<=255) {
        result.push_back(0xcc);result.push_back(static_cast<std::uint8_t>(n));
    } else if(n<=65535) {
        result.push_back(0xcd);result.push_back(static_cast<std::uint8_t>(n>>8));
        result.push_back(static_cast<std::uint8_t>(n));
    } else {
        result.push_back(0xce);
        for(int shift=24;shift>=0;shift-=8)
            result.push_back(static_cast<std::uint8_t>(n>>shift));
    }
}
inline std::vector<std::uint8_t> one_level_nullary(
    const Program& program, const std::string& domain,
    const std::string& outerName, const std::string& innerName) {
    // This entry point intentionally fails closed on other payload forms.
    // Full guest source values and variable payload lowering are separate.
    require_wire_name(domain);
    std::vector<std::string> allowed;
    for(const auto& e:program.enums) if(e.name==domain) {
        for(const auto& c:e.cases) allowed.push_back(e.name+"."+c.name);
    }
    for(const auto& m:program.tagged_patterns) if(m.name==domain) {
        allowed=m.alternatives;
    }
    for (const auto& qualified : allowed) {
        const auto dot=qualified.find('.');
        if(dot==std::string::npos)
            throw std::runtime_error("invalid closed enum case");
        require_wire_name(qualified.substr(0,dot));
        require_wire_name(qualified.substr(dot+1));
    }
    if(allowed.empty() || std::find(allowed.begin(),allowed.end(),outerName)==allowed.end())
        throw std::runtime_error("nested literal outside closed actor pattern");
    const auto dot=outerName.find('.');
    const auto dot2=innerName.find('.');
    if(dot==std::string::npos || dot2==std::string::npos)
        throw std::runtime_error("nested enum constructor requires qualified case name");
    const auto outerOwner=outerName.substr(0,dot);
    const auto& oc=variant(owner(program,outerOwner),outerName.substr(dot+1));
    const auto innerOwner=innerName.substr(0,dot2);
    require_wire_name(outerOwner);
    require_wire_name(outerName.substr(dot+1));
    require_wire_name(innerOwner);
    require_wire_name(innerName.substr(dot2+1));
    const auto& ic=variant(owner(program,innerOwner),innerName.substr(dot2+1));
    if(!oc.has_payload || oc.payload.kind!=MessageShape::Kind::EnumRef
       || oc.payload.nominal_name!=innerOwner || ic.has_payload)
        throw std::runtime_error("nested literal requires matching nullary enum child");

    std::sort(allowed.begin(),allowed.end());
    std::string fp="oreslang.tagged.domain.v1/"+domain+"/";
    for(const auto& name:allowed) fp+=std::to_string(name.size())+":"+name+";";
    const auto id=std::string("oreslang.enum.")+hex(sha(fp))+"."+
                  hex(sha(outerName+"#"+std::to_string(oc.tag)));
    std::string shapeText="oreslang.actor.shape.v2/T[S,I,";
    std::vector<std::string> active;
    shape(shapeText,program,oc.payload,active);
    shapeText+="]";
    require_descriptor_budget(shapeText);
    const auto digest=sha(shapeText);

    std::vector<std::uint8_t> output{0x94,0x02};
    mp_string(output,id);
    output.push_back(0xc4);output.push_back(32);
    output.insert(output.end(),digest.begin(),digest.end());
    output.push_back(0x93);mp_string(output,outerOwner);mp_tag(output,oc.tag);
    output.push_back(0x92);mp_string(output,innerOwner);mp_tag(output,ic.tag);
    if(output.size()>1048576) throw std::runtime_error("nested enum wire byte quota exceeded");
    return output;
}
}
