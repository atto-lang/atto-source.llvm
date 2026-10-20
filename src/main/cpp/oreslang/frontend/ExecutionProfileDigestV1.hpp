#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/SHA256.h"
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace oreslang::model::profile_v1 {

struct Profile {
    std::uint32_t version = 1;
    std::string backend_id;
    std::string backend_semantic_version;
    std::string compiler_semantic_version;
    std::string target_triple;
    std::string data_layout;
    std::string runtime_abi;
    std::string numeric_mode;
    bool deterministic = false;
    std::string allocation;
    bool freestanding = false;
    std::optional<std::string> device_abi;
    std::vector<std::string> numeric_features;
};

[[noreturn]] inline void fail(const char* code) { throw std::runtime_error(code); }

inline bool valid_utf8(const std::string& s) {
    const auto* p=reinterpret_cast<const unsigned char*>(s.data());
    std::size_t i=0;
    while(i<s.size()){
        unsigned c=p[i++];
        if(c<=0x7f)continue;
        unsigned need=0,lo=0x80,hi=0xbf;
        if(c>=0xc2&&c<=0xdf)need=1;
        else if(c==0xe0){need=2;lo=0xa0;}
        else if(c>=0xe1&&c<=0xec)need=2;
        else if(c==0xed){need=2;hi=0x9f;}
        else if(c>=0xee&&c<=0xef)need=2;
        else if(c==0xf0){need=3;lo=0x90;}
        else if(c>=0xf1&&c<=0xf3)need=3;
        else if(c==0xf4){need=3;hi=0x8f;}
        else return false;
        if(i+need>s.size()||p[i]<lo||p[i]>hi)return false;
        ++i;
        for(unsigned j=1;j<need;++j,++i)if(p[i]<0x80||p[i]>0xbf)return false;
    }
    return true;
}

inline int ordinal(const std::string& v, std::initializer_list<const char*> values) {
    int i=0;for(const char* x:values){if(v==x)return i;++i;}
    fail("MODEL_INVALID_EXECUTION_PROFILE");
}

inline bool feature_token(const std::string& s) {
    if(s.empty()||s.size()>64)return false;
    for(char c:s)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
        c=='_'||c=='.'||c=='+'||c=='-'))return false;
    return true;
}

struct Bytes {
    std::vector<std::uint8_t> data;
    void u8(std::uint8_t v){data.push_back(v);}
    void u32(std::uint32_t v){for(int shift=24;shift>=0;shift-=8)u8(static_cast<std::uint8_t>(v>>shift));}
    void raw(const std::string& s){data.insert(data.end(),s.begin(),s.end());}
    void string(const std::string& s){
        if(!valid_utf8(s)||s.size()>256)fail("MODEL_INVALID_EXECUTION_PROFILE");
        u32(static_cast<std::uint32_t>(s.size()));raw(s);
    }
};

inline std::vector<std::uint8_t> canonical_bytes(const Profile& p) {
    if(p.version!=1)fail("MODEL_INVALID_EXECUTION_PROFILE");
    for(std::size_t i=0;i<p.numeric_features.size();++i){
        const auto& f=p.numeric_features[i];
        if(!feature_token(f))fail("MODEL_INVALID_EXECUTION_PROFILE");
        if(i&&p.numeric_features[i-1]>=f)fail("MODEL_NONCANONICAL_EXECUTION_PROFILE");
    }
    Bytes out;
    const std::string prefix("ORESLANG-MODEL-PROFILE-V1\0",25);
    out.data.insert(out.data.end(),prefix.begin(),prefix.end());
    out.u32(1);
    out.string(p.backend_id);
    out.string(p.backend_semantic_version);
    out.string(p.compiler_semantic_version);
    out.string(p.target_triple);
    out.string(p.data_layout);
    out.string(p.runtime_abi);
    out.u8(static_cast<std::uint8_t>(ordinal(p.numeric_mode,{"strict","ieee","fast"})));
    out.u8(p.deterministic?1:0);
    out.u8(static_cast<std::uint8_t>(ordinal(p.allocation,{"preallocated","bounded","dynamic"})));
    out.u8(p.freestanding?1:0);
    out.u8(p.device_abi?1:0);if(p.device_abi)out.string(*p.device_abi);
    out.u32(static_cast<std::uint32_t>(p.numeric_features.size()));
    for(const auto& f:p.numeric_features)out.string(f);
    return out.data;
}

inline std::string digest(const Profile& p) {
    const auto bytes=canonical_bytes(p);
    const auto hash=llvm::SHA256::hash(llvm::ArrayRef<std::uint8_t>(bytes.data(),bytes.size()));
    static constexpr char hex[]="0123456789abcdef";
    std::string out;out.reserve(64);
    for(auto b:hash){out.push_back(hex[(b>>4)&15]);out.push_back(hex[b&15]);}
    return out;
}

} // namespace oreslang::model::profile_v1
