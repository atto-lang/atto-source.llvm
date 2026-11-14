#pragma once

// Native v1 .oresi PUBLIC INTERFACE admission, interoperable with Java's
// ExportInterfaceCodec. This is metadata preflight, NOT native object linking,
// executable guest imports or authentication of an untrusted compiler.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang::package_interface {

struct NamespacePin {
    std::string name;
    std::string sha256;
};
struct DependencyPin {
    std::string alias;
    std::string package_instance_id;
    std::string compiler_version;
    std::string target_abi;
    std::string feature_policy;
    std::vector<NamespacePin> namespaces;
};
struct Export {
    std::string module;
    std::uint8_t kind = 0;
    std::string name;
    std::string signature;
};
class Verified {
    // Admission creates a frozen snapshot. Consumers cannot modify the
    // namespace export table after SHA/profile verification.
    const std::string alias_;
    const std::string package_instance_id_;
    const std::map<std::string, std::vector<Export>> namespaces_;
    // Only the validated admission function may construct this proof object.
    friend Verified admit(const DependencyPin&,
        const std::map<std::string, std::vector<std::uint8_t>>&);
    Verified(std::string alias, std::string package_instance_id,
             std::map<std::string, std::vector<Export>> namespaces)
        : alias_(std::move(alias)),
          package_instance_id_(std::move(package_instance_id)),
          namespaces_(std::move(namespaces)) {}
public:
    const std::string& alias() const noexcept { return alias_; }
    const std::string& package_instance_id() const noexcept {
        return package_instance_id_;
    }
    std::size_t namespace_count() const noexcept { return namespaces_.size(); }
    const std::vector<Export>& namespace_exports(const std::string& name) const {
        auto it = namespaces_.find(name);
        if (it == namespaces_.end())
            throw std::runtime_error("namespace not exported by locked package");
        return it->second;
    }
    std::vector<Export> module_exports(const std::string& ns, const std::string& module) const {
        std::vector<Export> exports;
        bool public_module = false;
        for (const auto& entry : namespace_exports(ns)) {
            if (entry.module != module) continue;
            if (entry.kind == 0 && entry.name == module) public_module = true;
            exports.push_back(entry);
        }
        if (!public_module)
            throw std::runtime_error("module not exported by locked package");
        return exports;
    }
};

namespace detail {
inline void reject(const char* text) { throw std::runtime_error(text); }
inline std::uint32_t rotate(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

// No dependency on OpenSSL or an LLVM runtime: this can be verified with
// GCC and Clang in a host-only admission gate. The 16 MiB input bound
// limits the padded temporary buffer and keeps bit-length arithmetic safe.
inline std::string sha256(const std::vector<std::uint8_t>& bytes) {
    static constexpr std::array<std::uint32_t,64> k{{
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    }};
    if (bytes.size() > 16u * 1024u * 1024u) reject("oversized .oresi");
    std::vector<std::uint8_t> padded = bytes;
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8u;
    padded.push_back(0x80u);
    while (padded.size() % 64u != 56u) padded.push_back(0);
    for (int i=7; i>=0; --i) padded.push_back(static_cast<std::uint8_t>(bits >> (i*8)));
    std::array<std::uint32_t,8> h{{
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    }};
    for (std::size_t offset=0; offset<padded.size(); offset+=64) {
        std::array<std::uint32_t,64> w{};
        for (std::size_t i=0;i<16;++i) {
            const auto n=offset+4*i;
            w[i]=(std::uint32_t(padded[n])<<24)|
                 (std::uint32_t(padded[n+1])<<16)|
                 (std::uint32_t(padded[n+2])<<8)|std::uint32_t(padded[n+3]);
        }
        for (std::size_t i=16;i<64;++i) {
            const auto x=w[i-15],y=w[i-2];
            const auto s0=rotate(x,7)^rotate(x,18)^(x>>3);
            const auto s1=rotate(y,17)^rotate(y,19)^(y>>10);
            w[i]=w[i-16]+s0+w[i-7]+s1;
        }
        auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
        for (std::size_t i=0;i<64;++i) {
            const auto s1=rotate(e,6)^rotate(e,11)^rotate(e,25);
            const auto ch=(e&f)^(~e&g);
            const auto t1=z+s1+ch+k[i]+w[i];
            const auto s0=rotate(a,2)^rotate(a,13)^rotate(a,22);
            const auto maj=(a&b)^(a&c)^(b&c);
            const auto t2=s0+maj;
            z=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;
        h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
    }
    static constexpr char hex[]="0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const auto value : h) {
        for (int shift=28;shift>=0;shift-=4) out.push_back(hex[(value>>shift)&0xfu]);
    }
    return out;
}

inline bool ident(const std::string& x) {
    if (x.empty() || x.size()>128) return false;
    if (!(x[0]>='A'&&x[0]<='Z')&&!(x[0]>='a'&&x[0]<='z')&&x[0]!='_') return false;
    for (std::size_t i=1;i<x.size();++i)
        if (!(x[i]>='A'&&x[i]<='Z')&&!(x[i]>='a'&&x[i]<='z')&&
            !(x[i]>='0'&&x[i]<='9')&&x[i]!='_') return false;
    return true;
}
// Dependency aliases have a stricter, lowercase-only contract than exported
// namespace identifiers. Match Java's [a-z][a-z0-9_]{0,127} exactly.
inline bool reserved_name(const std::string& x) {
    return x=="constructor"||x=="prototype"||x=="__proto__"||x=="__root__";
}
inline bool alias(const std::string& x) {
    if (x.empty() || x.size()>128u || x[0]<'a' || x[0]>'z' ||
        reserved_name(x)) return false;
    for (std::size_t i=1;i<x.size();++i) {
        const char ch=x[i];
        if (!((ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='_')) return false;
    }
    return true;
}
inline bool sha_hex(const std::string& x) {
    if (x.size()!=64) return false;
    for (const char c : x) if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
    return true;
}
inline bool package_instance(const std::string& x) {
    if (x.empty() || x.size()>256 || x.find("//")!=std::string::npos ||
        x.find("/../")!=std::string::npos || x.find("/./")!=std::string::npos ||
        (x.size()>=3 && x.compare(x.size()-3,3,"/..")==0) ||
        (x.size()>=2 && x.compare(x.size()-2,2,"/.")==0)) return false;
    if (!((x[0]>='A'&&x[0]<='Z')||(x[0]>='a'&&x[0]<='z')||(x[0]>='0'&&x[0]<='9')))return false;
    for (char c : x)
        if (!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
              c=='_'||c=='.'||c=='/'||c=='@'||c=='#'||c==':'||c=='+'||c=='-'))
            return false;
    return true;
}
inline bool utf8(std::string_view s) {
    for (std::size_t i=0;i<s.size();) {
        const auto first=static_cast<unsigned char>(s[i]);
        if (first<=0x7f) {++i;continue;}
        int len=0;std::uint32_t cp=0,min=0;
        if (first>=0xc2&&first<=0xdf) {len=2;cp=first&0x1fu;min=0x80;}
        else if (first>=0xe0&&first<=0xef) {len=3;cp=first&0x0fu;min=0x800;}
        else if (first>=0xf0&&first<=0xf4) {len=4;cp=first&0x07u;min=0x10000;}
        else return false;
        if (i+std::size_t(len)>s.size()) return false;
        for (int j=1;j<len;++j) {
            const auto next=static_cast<unsigned char>(s[i+j]);
            if ((next&0xc0u)!=0x80u) return false;
            cp=(cp<<6u)|(next&0x3fu);
        }
        if (cp<min||cp>0x10ffffu||(cp>=0xd800u&&cp<=0xdfffu))return false;
        i+=std::size_t(len);
    }
    return true;
}
// Must match Java PinnedPackageInterface.safePublicName. This is an
// admission policy for external export identities, not a full Atto lexer.
inline bool safe_public_name(const std::string& value) {
    if(value.empty()) return false;
    for(std::size_t i=0;i<value.size();) {
        const auto ch=static_cast<unsigned char>(value[i]);
        std::uint32_t cp=ch;
        std::size_t length=1;
        if(ch>=0xf0u) { cp=ch&0x07u; length=4; }
        else if(ch>=0xe0u) { cp=ch&0x0fu; length=3; }
        else if(ch>=0xc0u) { cp=ch&0x1fu; length=2; }
        for(std::size_t j=1;j<length;++j)
            cp=(cp<<6u)|(static_cast<unsigned char>(value[i+j])&0x3fu);
        if(cp=='/'||cp=='\\'||cp<=0x1fu||
           (cp>=0x7fu&&cp<=0x9fu)||
           cp==0x00adu||cp==0x061cu||cp==0x180eu||
           (cp>=0x200bu&&cp<=0x200fu)||
           (cp>=0x2028u&&cp<=0x202eu)||
           (cp>=0x2060u&&cp<=0x206fu)||
           cp==0xfeffu||(cp>=0xe0020u&&cp<=0xe007fu))
            return false;
        i+=length;
    }
    return true;
}

struct Reader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t at=0;
    std::uint8_t u8(){
        if(at>=bytes.size())reject("truncated .oresi");
        return bytes[at++];
    }
    std::uint32_t u32(){
        if(bytes.size()-at<4)reject("truncated .oresi");
        std::uint32_t x=0;
        for(int i=0;i<4;++i)x=(x<<8u)|u8();
        return x;
    }
    std::string str(){
        const auto n=u32();
        if(n>65536u||n>bytes.size()-at)reject("bad .oresi length");
        std::string s(bytes.begin()+static_cast<std::ptrdiff_t>(at),
                      bytes.begin()+static_cast<std::ptrdiff_t>(at+n));
        at+=n;
        if(!utf8(s))reject("invalid .oresi UTF-8");
        return s;
    }
};
inline bool utf8_less(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),
        [](char x,char y){return static_cast<unsigned char>(x)<static_cast<unsigned char>(y);});
}
inline bool entry_less(const Export& a,const Export& b) {
    if(a.module!=b.module)return utf8_less(a.module,b.module);
    if(a.kind!=b.kind)return a.kind<b.kind;
    if(a.name!=b.name)return utf8_less(a.name,b.name);
    return utf8_less(a.signature,b.signature);
}
inline bool same_symbol(const Export& a,const Export& b) {
    return a.module==b.module&&a.kind==b.kind&&a.name==b.name;
}
struct Decoded {
    std::string ns,compiler,target,policy;
    std::vector<Export> exports;
};
inline Decoded decode(const std::vector<std::uint8_t>& bytes) {
    static constexpr std::array<std::uint8_t,7> magic{{'O','R','E','S','I',0,1}};
    if(bytes.size()<27u||bytes.size()>16u*1024u*1024u)
        reject("invalid .oresi size");
    if (!std::equal(magic.begin(),magic.end(),bytes.begin()))
        reject("unsupported .oresi format");
    Reader r{bytes};
    for(std::size_t i=0;i<magic.size();++i)(void)r.u8();
    Decoded out;
    out.ns=r.str();out.compiler=r.str();out.target=r.str();out.policy=r.str();
    const auto count=r.u32();
    if(count>100000u)reject("oversized .oresi symbol table");
    // Every accepted symbol uses at least 15 bytes (two nonempty names).
    // Reject hostile count fields before reserving attacker-sized tables.
    if(count>(bytes.size()-r.at)/15u)
        reject("impossible .oresi export count");
    out.exports.reserve(count);
    for(std::uint32_t i=0;i<count;++i){
        Export entry;
        entry.module=r.str();entry.kind=r.u8();entry.name=r.str();entry.signature=r.str();
        if(entry.kind>5u||!safe_public_name(entry.module)||
           !safe_public_name(entry.name))
            reject("invalid .oresi export");
        if(!out.exports.empty()){
            const auto& last=out.exports.back();
            if(!entry_less(last,entry))reject("noncanonical .oresi order or duplicate");
            if(entry.kind!=1u&&same_symbol(last,entry))
                reject("duplicate nonfunction export identity");
        }
        out.exports.push_back(std::move(entry));
    }
    if(r.at!=bytes.size())reject("trailing .oresi bytes");
    std::set<std::string> modules;
    for(const auto& entry:out.exports) {
        if(entry.kind==0u) {
            if(entry.name!=entry.module)reject("noncanonical .oresi module marker");
            modules.insert(entry.module);
        }
    }
    for(const auto& entry:out.exports)
        if(!modules.count(entry.module))
            reject("unverified .oresi containing module visibility");
    return out;
}
} // namespace detail

inline std::string sha256(const std::vector<std::uint8_t>& bytes) {
    return detail::sha256(bytes);
}

inline Verified admit(const DependencyPin& pin,
                      const std::map<std::string,std::vector<std::uint8_t>>& snapshots) {
    if(!detail::alias(pin.alias)||!detail::package_instance(pin.package_instance_id)||
       pin.compiler_version.empty()||pin.target_abi.empty()||pin.feature_policy.empty()||
       pin.namespaces.empty()||pin.namespaces.size()>4096u||
       snapshots.size()!=pin.namespaces.size())
        detail::reject("invalid locked .oresi package identity/profile");
    // A closed lock may list 4096 interfaces. Cap the aggregate byte budget
    // before hashing or decoding any one attacker-controlled envelope.
    constexpr std::size_t max_envelope_bytes = 16u*1024u*1024u;
    constexpr std::size_t max_package_bytes = 64u*1024u*1024u;
    std::size_t total_bytes = 0;
    // Snapshot the complete bounded input before hashing/decoding. A const
    // reference alone does not freeze caller-owned byte vectors.
    // As with Java byte[].clone(), concurrent unsynchronized mutation DURING
    // the copy remains a caller-side data race; the driver must own its inputs.
    std::map<std::string, std::vector<std::uint8_t>> owned_snapshots;
    for (const auto& [name, bytes] : snapshots) {
        if (bytes.size()>max_envelope_bytes ||
            bytes.size()>max_package_bytes-total_bytes)
            detail::reject("package interface byte budget exceeded");
        total_bytes+=bytes.size();
        owned_snapshots.emplace(name, bytes);
    }
    std::set<std::string> seen;
    std::map<std::string, std::vector<Export>> checked_namespaces;
    for(const auto& ns:pin.namespaces){
        if(!detail::ident(ns.name)||detail::reserved_name(ns.name)||
           !detail::sha_hex(ns.sha256)||!seen.insert(ns.name).second)
            detail::reject("invalid or duplicate .oresi namespace pin");
        const auto it=owned_snapshots.find(ns.name);
        if(it==owned_snapshots.end())detail::reject("missing locked .oresi namespace");
        const auto& bytes=it->second;
        if(detail::sha256(bytes)!=ns.sha256)detail::reject("locked .oresi digest mismatch");
        auto decoded=detail::decode(bytes);
        if(decoded.ns!=ns.name||decoded.compiler!=pin.compiler_version||
           decoded.target!=pin.target_abi||decoded.policy!=pin.feature_policy)
            detail::reject("locked .oresi ABI/profile mismatch");
        checked_namespaces.emplace(ns.name,std::move(decoded.exports));
    }
    return Verified{pin.alias,pin.package_instance_id,std::move(checked_namespaces)};
}

} // namespace oreslang::package_interface
