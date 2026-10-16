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
// +/ and pkg: namespace suffixes are canonical across host platforms.
// Dot, parent and empty components must never acquire authority via cleanup.
inline void validate_package_suffix(std::string_view suffix) {
    if (suffix.empty()) throw std::invalid_argument("empty rooted import suffix");
    std::size_t start = 0;
    while (true) {
        const auto slash = suffix.find('/', start);
        const auto part = suffix.substr(start, slash == std::string_view::npos
            ? suffix.size() - start : slash - start);
        if (part.empty() || part == "." || part == "..")
            throw std::invalid_argument("noncanonical rooted import component");
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
}
inline Import classify(std::string_view raw) {
    if (raw.empty() || raw.size()>4096) throw std::invalid_argument("invalid import length");
    // The Java resolver and shared import contract require forward slashes
    // for explicit package-root and dependency namespace spellings.
    if ((starts(raw, "+") || starts(raw, "pkg:")) &&
        raw.find('\\') != std::string_view::npos)
        throw std::invalid_argument("rooted import requires forward-slash separators");
    std::string s(raw);
    for (char &c:s) {
        auto b=static_cast<unsigned char>(c);
        if (b<32 || b==127) throw std::invalid_argument("control byte in import");
        if (c=='\\') c='/';
    }
    if (starts(s,"java:")) return {Kind::HostJava,s,{},{}};
    // A Windows drive path must not be treated as a bare relative filename
    // when the compiler is hosted on POSIX (and vice versa). Drive-relative
    // C:foo and NTFS alternate-data-stream suffixes are deliberately denied.
    const auto is_drive = s.size() >= 2 &&
        ((s[0]>='A'&&s[0]<='Z')||(s[0]>='a'&&s[0]<='z')) && s[1]==':';
    if (is_drive && s.size() >= 3 && s[2]=='/')
        return {Kind::Absolute,s,{},{}};
    if (starts(s,"//")) return {Kind::Absolute,s,{},{}};
    if (starts(s,"+/")) {
        if (s.find(':', 2) != std::string::npos)
            throw std::invalid_argument("colon in rooted import suffix");
        validate_package_suffix(std::string_view(s).substr(2));
        return {Kind::Root,s,{},s.substr(2)};
    }
    if (starts(s,"pkg:")) {
        auto slash=s.find('/',4);
        if (slash==std::string::npos || slash==4 || slash+1==s.size() ||
            s[slash+1]=='/' || !valid_alias(std::string_view(s).substr(4,slash-4)))
            throw std::invalid_argument("expected pkg:<alias>/<source>");
        if (s.find(':', slash + 1) != std::string::npos)
            throw std::invalid_argument("colon in package import suffix");
        validate_package_suffix(std::string_view(s).substr(slash + 1));
        return {Kind::Package,s,s.substr(4,slash-4),s.substr(slash+1)};
    }
    if (s[0]=='@' || s[0]=='+') throw std::invalid_argument("invalid project-root import prefix");
    if (s.find(':') != std::string::npos)
        throw std::invalid_argument("unsupported colon or drive-relative import path");
    if (fs::path(s).is_absolute()) return {Kind::Absolute,s,{},{}};
    if (s=="."||s==".."||starts(s,"./")||starts(s,"../")) return {Kind::Relative,s,{},{}};
    return {Kind::Bare,s,{},{}};
}
inline void reject_control_bytes(std::string_view input, std::string_view label) {
    for (unsigned char byte : input)
        if (byte < 32 || byte == 127)
            throw std::invalid_argument(std::string("control byte in ") + std::string(label));
}
inline fs::path normalized_absolute(const fs::path &path) {
    reject_control_bytes(path.string(), "filesystem path");
    auto result = fs::absolute(path).lexically_normal();
    // std::filesystem retains a trailing empty component on some platforms.
    if (!result.has_filename()) result = result.parent_path();
    return result;
}
inline bool beneath(const fs::path &root, const fs::path &candidate) {
    // Callers may use this public helper directly. Normalize both operands
    // here rather than assuming rooted_path already normalized them.
    auto normalized_root = normalized_absolute(root);
    auto normalized_candidate = normalized_absolute(candidate);
    auto c=normalized_candidate.begin();
    for(auto r=normalized_root.begin();r!=normalized_root.end();++r,++c)
        if(c==normalized_candidate.end()||*r!=*c) return false;
    return true;
}
inline fs::path rooted_path(const fs::path &root, std::string_view suffix) {
    if (suffix.empty()||suffix.front()=='/') throw std::invalid_argument("invalid rooted import");
    if (suffix.size()>4096) throw std::invalid_argument("rooted import too long");
    reject_control_bytes(suffix,"rooted import");
    std::string normalized(suffix);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.find(':') != std::string::npos)
        throw std::invalid_argument("colon in rooted import suffix");
    fs::path p(normalized);
    if(p.is_absolute()) throw std::invalid_argument("absolute rooted import");
    auto anchor=normalized_absolute(root);
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
    auto normalized=normalized_absolute(p);
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
