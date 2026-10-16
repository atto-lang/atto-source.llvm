#pragma once
// Host-only LLVM import policy primitives; not compiler/linker parity.
// Lexical normalization does not sandbox symlink or concurrent filesystem writes.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang::imports {
namespace fs = std::filesystem;

enum class Kind { Root, Package, Relative, Bare, Absolute, HostJava };
struct Import { Kind kind; std::string path, alias, suffix; };
inline bool starts(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(0,p.size()) == p;
}
inline bool valid_alias(std::string_view s) {
    if (s.empty() || !((s[0]>='A'&&s[0]<='Z')||(s[0]>='a'&&s[0]<='z'))) return false;
    for (unsigned char c : s)
        if (!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'))
            return false;
    return true;
}
inline Import classify(std::string_view raw) {
    if (raw.empty() || raw.size()>4096) throw std::invalid_argument("invalid import length");
    std::string s(raw);
    for (char &c:s) {
        auto b=static_cast<unsigned char>(c);
        if (b<32 || b==127) throw std::invalid_argument("control byte in import");
        if (c=='\\') c='/';
    }
    if (starts(s,"java:")) return {Kind::HostJava,s,{},{}};
    if (starts(s,"@/")) {
        if (s.size()==2 || s[2]=='/') throw std::invalid_argument("invalid @/ path");
        return {Kind::Root,s,{},s.substr(2)};
    }
    if (starts(s,"pkg:")) {
        auto slash=s.find('/',4);
        if (slash==std::string::npos || slash==4 || slash+1==s.size() ||
            s[slash+1]=='/' || !valid_alias(std::string_view(s).substr(4,slash-4)))
            throw std::invalid_argument("expected pkg:<alias>/<source>");
        return {Kind::Package,s,s.substr(4,slash-4),s.substr(slash+1)};
    }
    if (s[0]=='@') throw std::invalid_argument("invalid @ prefix");
    if (fs::path(s).is_absolute()) return {Kind::Absolute,s,{},{}};
    if (s=="."||s==".."||starts(s,"./")||starts(s,"../")) return {Kind::Relative,s,{},{}};
    return {Kind::Bare,s,{},{}};
}
inline bool beneath(const fs::path &root, const fs::path &candidate) {
    auto c=candidate.begin();
    for(auto r=root.begin();r!=root.end();++r,++c)
        if(c==candidate.end()||*r!=*c) return false;
    return true;
}
inline fs::path rooted_path(const fs::path &root, std::string_view suffix) {
    if (suffix.empty()||suffix.front()=='/') throw std::invalid_argument("invalid rooted import");
    fs::path p(suffix);
    if(p.is_absolute()) throw std::invalid_argument("absolute rooted import");
    auto anchor=fs::absolute(root).lexically_normal();
    auto candidate=(anchor/p).lexically_normal();
    if(!beneath(anchor,candidate)) throw std::invalid_argument("root escape");
    return candidate;
}
inline std::string trim(std::string_view s) {
    size_t l=0,r=s.size();
    while(l<r&&std::isspace(static_cast<unsigned char>(s[l]))) ++l;
    while(r>l&&std::isspace(static_cast<unsigned char>(s[r-1]))) --r;
    return std::string(s.substr(l,r-l));
}
inline void append(std::vector<fs::path>& roots, const fs::path& p) {
    auto normalized=fs::absolute(p).lexically_normal();
    if (!normalized.has_filename()) normalized=normalized.parent_path();
    if(std::find(roots.begin(),roots.end(),normalized)==roots.end()) roots.push_back(normalized);
}
inline void append_list(std::vector<fs::path>& roots, std::string_view s, char sep, const fs::path& cwd) {
    if(sep=='\0') throw std::invalid_argument("NUL path-list separator");
    size_t start=0;
    while(true){
        auto end=s.find(sep,start);
        auto item=trim(s.substr(start,end==std::string_view::npos?end:end-start));
        if(!item.empty()){fs::path p(item);append(roots,p.is_absolute()?p:cwd/p);}
        if(end==std::string_view::npos) break;
        start=end+1;
    }
}
// Omitted source.roots defaults to [.]; explicitly present [] stays empty.
// Preserve source roots, import paths, ORES_PATHS, ORESLANG_PATH precedence.
inline std::vector<fs::path> search_roots(
    const fs::path& project, const std::optional<std::vector<std::string>>& source,
    const std::vector<std::string>& imports, std::string_view ores_paths,
    std::string_view legacy_paths, char legacy_sep, const fs::path& cwd) {
    std::vector<fs::path> roots;
    const auto add_project=[&](const std::string& s){
        if(trim(s).empty()) throw std::invalid_argument("blank manifest root");
        fs::path p(s);append(roots,p.is_absolute()?p:project/p);
    };
    if(!source) add_project(".");
    else for(const auto& s:*source) add_project(s);
    for(const auto& s:imports) add_project(s);
    append_list(roots,ores_paths,':',cwd);
    append_list(roots,legacy_paths,legacy_sep,cwd);
    return roots;
}
} // namespace oreslang::imports
