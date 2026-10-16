#pragma once
// Stage B: bounded native file/manifest resolver. Not a multi-file linker.
// Strict TOML v1 subset: unsupported syntax must fail, never be reinterpreted.
#include "oreslang/imports/ImportPathPolicy.hpp"
#include <fstream>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>

namespace oreslang::imports {
struct SourceConfig {
    fs::path root;
    fs::path manifest;
    fs::path boundary;
    bool package_manifest = false;
    std::vector<fs::path> source_roots;
    std::vector<fs::path> import_paths;
    std::map<std::string, fs::path> dependencies;
    std::optional<fs::path> main_entry;
};

inline std::string read_bounded_text(const fs::path& source) {
    std::error_code ec;
    const auto size=fs::file_size(source,ec);
    if (ec || size > 256 * 1024) throw std::invalid_argument("unreadable or oversized import manifest");
    std::ifstream file(source,std::ios::binary);
    if(!file) throw std::invalid_argument("cannot open import manifest");
    std::string text(static_cast<std::size_t>(size),'\0');
    if(!text.empty() && !file.read(text.data(),static_cast<std::streamsize>(size)))
        throw std::invalid_argument("truncated import manifest");
    if (file.peek() != std::char_traits<char>::eof())
        throw std::invalid_argument("import manifest changed while reading");
    return text;
}
inline std::string without_toml_comment(std::string_view line) {
    bool quoted=false, escaped=false;
    std::string out;
    for(char c:line){
        if(escaped){out.push_back(c);escaped=false;continue;}
        if(quoted && c=='\\'){out.push_back(c);escaped=true;continue;}
        if(c=='"'){quoted=!quoted;out.push_back(c);continue;}
        if(c=='#' && !quoted)break;
        out.push_back(c);
    }
    if(quoted)throw std::invalid_argument("unterminated TOML string");
    return trim(out);
}
inline std::string toml_string(std::string_view value) {
    if(value.size()<2 || value.front()!='"' || value.back()!='"')
        throw std::invalid_argument("expected basic TOML string");
    std::string out;
    for(std::size_t i=1;i+1<value.size();++i){
        char c=value[i];
        if(c=='\\'){
            if(++i+1>=value.size()) throw std::invalid_argument("invalid TOML escape");
            char escaped=value[i];
            if(escaped=='"'||escaped=='\\')c=escaped;
            else throw std::invalid_argument("unsupported TOML escape");
        }else if(c=='"') throw std::invalid_argument("unescaped TOML quote");
        out.push_back(c);
    }
    reject_control_bytes(out,"TOML string");
    return out;
}
inline std::vector<std::string> toml_string_array(std::string_view raw) {
    auto val=trim(raw);
    if(val.size()<2||val.front()!='['||val.back()!=']')
        throw std::invalid_argument("expected TOML array");
    std::vector<std::string> result;
    std::size_t p=1;
    while(p<val.size()-1){
        while(p<val.size()-1 && std::isspace(static_cast<unsigned char>(val[p]))) ++p;
        if(p==val.size()-1)break;
        if(val[p]!='"')throw std::invalid_argument("array needs quoted strings");
        auto start=p++;
        bool escape=false;
        while(p<val.size()-1){
            if(escape){escape=false;++p;continue;}
            if(val[p]=='\\'){escape=true;++p;continue;}
            if(val[p++]=='"')break;
        }
        if(p>=val.size()||val[p-1]!='"')throw std::invalid_argument("unterminated array item");
        result.push_back(toml_string(std::string_view(val).substr(start,p-start)));
        if(result.size()>128)throw std::invalid_argument("too many manifest search roots");
        while(p<val.size()-1 && std::isspace(static_cast<unsigned char>(val[p])))++p;
        if(p==val.size()-1)break;
        if(val[p++]!=',')throw std::invalid_argument("invalid array delimiter");
    }
    return result;
}
inline std::string dependency_identity(std::string_view raw) {
    const std::string v=trim(raw);
    std::string id;
    if(!v.empty() && v.front()=='"')id=toml_string(v);
    else if(v.size()>2 && v.front()=='{' && v.back()=='}'){
        const auto interior=trim(std::string_view(v).substr(1,v.size()-2));
        auto eq=interior.find('=');
        if(eq==std::string::npos || trim(std::string_view(interior).substr(0,eq))!="zed")
            throw std::invalid_argument("dependency requires zed identity");
        id=toml_string(trim(std::string_view(interior).substr(eq+1)));
    }else throw std::invalid_argument("unsupported dependency declaration");
    auto slash=id.find('/');
    if(slash==std::string::npos || slash==0 || slash==id.size()-1 || id.find('/',slash+1)!=std::string::npos)
        throw std::invalid_argument("expected zed org/name");
    for(auto section : {id.substr(0,slash),id.substr(slash+1)}){
        if(section=="."||section==".."||section.find("..")!=std::string::npos)
            throw std::invalid_argument("invalid zed identifier");
        if (!((section.front()>='A'&&section.front()<='Z') ||
              (section.front()>='a'&&section.front()<='z') ||
              (section.front()>='0'&&section.front()<='9')))
            throw std::invalid_argument("invalid zed identifier start");
        for(unsigned char c:section)
            if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||
                 (c>='0'&&c<='9')||c=='_'||c=='.'||c=='-'))
                throw std::invalid_argument("invalid zed identifier");
    }
    return id;
}

// A deliberate, bounded subset of TOML, not a full TOML implementation.
// Unknown syntax is rejected; source graphs must not be built from partial data.
inline std::map<std::string,std::string> restricted_manifest_fields(const fs::path& manifest) {
    std::map<std::string,std::string> fields;
    std::set<std::string> seen_tables;
    std::istringstream input(read_bounded_text(manifest));
    std::string line,section;
    std::size_t count=0;
    while(std::getline(input,line)){
        if(++count>8192||line.size()>4096)throw std::invalid_argument("manifest line limit exceeded");
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        reject_control_bytes(line,"TOML line");
        const std::string stripped=without_toml_comment(line);
        if(stripped.empty())continue;
        if(stripped.front()=='['){
            if(stripped.size()<3||stripped.back()!=']'||stripped[1]=='[')
                throw std::invalid_argument("unsupported TOML table syntax");
            section=stripped.substr(1,stripped.size()-2);
            if(section!="package"&&section!="source"&&section!="dependencies"&&
               section!="entrypoints"&&section!="project"&&section!="install")
                throw std::invalid_argument("unsupported TOML table: "+section);
            if(!seen_tables.insert(section).second)
                throw std::invalid_argument("duplicate TOML table");
            continue;
        }
        const auto eq=stripped.find('=');
        if(eq==std::string::npos)throw std::invalid_argument("expected TOML key = value");
        std::string key=trim(std::string_view(stripped).substr(0,eq));
        if(key.empty()||key.size()>128)throw std::invalid_argument("invalid TOML key");
        for(unsigned char c:key)
            if(!std::isalnum(c)&&c!='_'&&c!='-')throw std::invalid_argument("unsupported TOML key");
        const std::string name=section.empty()?key:section+"."+key;
        const std::string value=trim(std::string_view(stripped).substr(eq+1));
        if(value.empty()||!fields.emplace(name,value).second)
            throw std::invalid_argument("missing or duplicate TOML value: "+name);
    }
    return fields;
}

inline std::optional<fs::path> find_manifest(const fs::path& start,
                                              const fs::path& boundary={}) {
    const fs::path physical_boundary=boundary.empty()?fs::path{}:fs::canonical(boundary);
    auto point=fs::canonical(start);
    auto cursor=fs::is_directory(point)?point:point.parent_path();
    // A mismatched caller-supplied boundary is a programming or authority
    // error, not an instruction to discover the boundary's manifest instead.
    if(!physical_boundary.empty()&&!beneath(physical_boundary,cursor))
        throw std::invalid_argument("source outside package discovery boundary");
    std::optional<fs::path> legacy;
    while(!cursor.empty() && (physical_boundary.empty()||beneath(physical_boundary,cursor))){
        auto pkg=cursor/".ores.pkg.toml", old=cursor/".oreslangc.cfg.toml";
        if(fs::is_regular_file(pkg))return pkg;
        if(!legacy && fs::is_regular_file(old))legacy=old;
        auto parent=cursor.parent_path();
        if(parent==cursor)break;
        cursor=parent;
    }
    return legacy;
}
inline SourceConfig read_project(const fs::path& source,
                                 const fs::path& boundary={}) {
    SourceConfig result;
    result.boundary=boundary.empty()?fs::path{}:fs::canonical(boundary);
    const auto found=find_manifest(source,boundary);
    if(!found){
        const auto physical=fs::canonical(source);
        result.root=result.boundary.empty() ?
            (fs::is_directory(physical)?physical:physical.parent_path()) : result.boundary;
        result.source_roots.push_back(result.root);
        return result;
    }
    result.manifest=*found;
    result.package_manifest=result.manifest.filename()==".ores.pkg.toml";
    result.root=result.manifest.parent_path();
    const auto fields=restricted_manifest_fields(result.manifest);
    const auto one=[&](const std::string& name)->std::optional<std::string>{
        auto it=fields.find(name);
        if(it==fields.end())return std::nullopt;
        return toml_string(it->second);
    };
    const auto schema=one("schema_version");
    if(!schema||*schema!="1")throw std::invalid_argument("requires schema_version = \"1\"");
    // Do not silently accept arbitrary types for known-but-unused metadata.
    // Even metadata not yet needed by the linker is part of admission.
    for(const auto& name:{"package.name","package.version","project.name","project.version"})
        (void)one(name);
    if(auto explicit_root=one("project.root")){
        if(result.package_manifest && *explicit_root!=".")
            throw std::invalid_argument("package project.root cannot override manifest root");
        if(!result.package_manifest){
            auto p=fs::path(*explicit_root);
            result.root=normalized_absolute(p.is_absolute()?p:result.root/p);
        }
    }
    if(!result.boundary.empty()&&!beneath(result.boundary,result.root))
        throw std::invalid_argument("nested project escapes package discovery boundary");
    const auto add_path=[&](std::vector<fs::path>& out,const std::string& entry){
        if(entry.empty())throw std::invalid_argument("blank manifest source path");
        auto path=fs::path(entry);
        append(out,path.is_absolute()?path:result.root/path);
    };
    auto roots=fields.find("source.roots");
    if(roots==fields.end())add_path(result.source_roots,".");
    else for(const auto& p:toml_string_array(roots->second))add_path(result.source_roots,p);
    if(auto imports=fields.find("source.import_paths");imports!=fields.end())
        for(const auto& p:toml_string_array(imports->second))add_path(result.import_paths,p);
    if(auto main=one("entrypoints.main")){
        auto path=fs::path(*main);
        if(result.package_manifest && (main->empty()||path.is_absolute()))
            throw std::invalid_argument("package main must be project-relative");
        auto entry=normalized_absolute(path.is_absolute()?path:result.root/path);
        if(result.package_manifest&&!beneath(result.root,entry))
            throw std::invalid_argument("package main escapes project root");
        result.main_entry=entry;
    }
    if(result.package_manifest){
        std::string install_dir="zed_modules";
        auto zed=result.root/".zpkg.toml";
        if(fs::is_regular_file(zed)){
            // Installer metadata may have additional tables. For now admit only
            // the restricted subset; reject rather than silently misparse it.
            auto z=restricted_manifest_fields(zed);
            if(auto it=z.find("install.dir");it!=z.end())install_dir=toml_string(it->second);
        }
        auto relative=fs::path(install_dir);
        if(install_dir.empty()||relative.is_absolute())
            throw std::invalid_argument("zed install.dir must be project-relative");
        const auto install_root=normalized_absolute(result.root/relative);
        if(!beneath(result.root,install_root))throw std::invalid_argument("zed install.dir escapes project");
        for(const auto& [key,value]:fields){
            if(!starts(key,"dependencies."))continue;
            const auto alias=key.substr(std::string("dependencies.").size());
            if(!valid_alias(alias))throw std::invalid_argument("invalid dependency alias");
            const auto dependency=dependency_identity(value);
            result.dependencies.emplace(alias,normalized_absolute(install_root/fs::path(dependency)));
        }
    }
    // Unknown manifest fields are unsupported, not ignored.
    for(const auto& [key,value]:fields){
        (void)value;
        if(key=="schema_version"||key=="package.name"||key=="package.version"||
           key=="project.name"||key=="project.version"||key=="project.root"||
           key=="entrypoints.main"||key=="source.roots"||key=="source.import_paths"||
           (result.package_manifest&&starts(key,"dependencies.")))continue;
        throw std::invalid_argument("unsupported project manifest field: "+key);
    }
    return result;
}
inline std::optional<fs::path> existing_source(const fs::path& candidate){
    std::vector<fs::path> candidates={candidate};
    if(candidate.extension().empty()){
        candidates.push_back(fs::path(candidate.string()+".ores"));
        // Never mistake discoverable Java sources for compilable LLVM units.
        candidates.push_back(fs::path(candidate.string()+".java"));
    }
    for(const auto& path:candidates){
        if(!fs::is_regular_file(path))continue;
        const auto physical=fs::canonical(path);
        if(path.extension()!=".ores" || physical.extension()!=".ores")
            throw std::invalid_argument("native imports support only .ores sources");
        return physical;
    }
    return std::nullopt;
}
inline void verify_installed_package(const fs::path& directory) {
    auto metadata=directory/".zpkg.toml";
    if(!fs::is_regular_file(metadata))return;
    const auto fields=restricted_manifest_fields(metadata);
    auto org=fields.find("package.org"),name=fields.find("package.name");
    if(org==fields.end()||name==fields.end())
        throw std::invalid_argument("installed package requires org and name metadata");
    const std::string identity=toml_string(org->second)+"/"+toml_string(name->second);
    const std::string expected=directory.parent_path().filename().string()+"/"+
                               directory.filename().string();
    if(identity!=expected)
        throw std::invalid_argument("installed zed package identity mismatch");
}

// A single physical file may not be silently imported under two package
// contexts. The future graph linker must register every resolved source.
struct SourceIdentityRegistry {
    std::map<fs::path,fs::path> contexts;
    fs::path register_source(const SourceConfig& config,const fs::path& source){
        auto physical=fs::canonical(source);
        if(physical.extension()!=".ores")
            throw std::invalid_argument("native source graph requires .ores files");
        auto project=fs::canonical(config.root);
        const auto [it,inserted]=contexts.emplace(physical,project);
        if(!inserted && it->second!=project)
            throw std::invalid_argument("same source resolved under conflicting package contexts");
        return physical;
    }
};
inline std::optional<fs::path> resolve_source(const SourceConfig& config,
                                              const fs::path& importer,
                                              std::string_view raw,
                                              std::string_view ores_paths={},
                                              std::string_view legacy_paths={},
                                              char legacy_separator=':',
                                              const fs::path& cwd=fs::current_path()){
    auto input=classify(raw);
    switch(input.kind){
        case Kind::HostJava:
            throw std::invalid_argument("native Java host imports require unsupported backend lowering");
        case Kind::Root:
            if(!config.package_manifest)throw std::invalid_argument("@/ requires .ores.pkg.toml");
            return existing_source(rooted_path(config.root,input.suffix));
        case Kind::Package:{
            auto it=config.dependencies.find(input.alias);
            if(it==config.dependencies.end())throw std::invalid_argument("undeclared dependency alias");
            verify_installed_package(it->second);
            return existing_source(rooted_path(it->second,input.suffix));
        }
        case Kind::Absolute:return existing_source(input.path);
        case Kind::Relative:return existing_source(normalized_absolute(importer.parent_path()/input.path));
        case Kind::Bare:{
            std::vector<fs::path> roots;
            for(const auto& p:config.source_roots)append(roots,p);
            for(const auto& p:config.import_paths)append(roots,p);
            append_list(roots,ores_paths,':',cwd);
            append_list(roots,legacy_paths,legacy_separator,cwd);
            for(const auto& root:roots){
                auto path=rooted_path(root,input.path);
                if(auto match=existing_source(path))return match;
            }
            return std::nullopt;
        }
    }
    throw std::logic_error("unreachable native import classification");
}
} // namespace oreslang::imports
