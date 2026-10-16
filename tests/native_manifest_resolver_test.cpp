#include "oreslang/imports/ManifestSourceResolver.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace oreslang::imports;
void require(bool test,const char* why){if(!test)throw std::runtime_error(why);}
template<class Func>void rejects(Func f,const char* why){try{f();}catch(const std::invalid_argument&){return;}throw std::runtime_error(why);}
int main(){
    auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    auto tmp=fs::temp_directory_path()/("ores-native-manifest-test-"+std::to_string(stamp));
    fs::create_directories(tmp);
    try{
        const auto app=tmp/"app", nested=app/"packages/child", shared=tmp/"ambient";
        fs::create_directories(app/"src/.hidden");
        fs::create_directories(nested/"src");
        fs::create_directories(app/"zed_modules/acme/math/src");
        fs::create_directories(shared);
        const auto write=[](const fs::path& path,std::string_view text){std::ofstream out(path);out<<text;out.close();};
        write(app/".ores.pkg.toml", R"(schema_version = "1"
[package]
name = "app"
[source]
roots = ["src"]
import_paths = ["packages"]
[entrypoints]
main = "src/main.ores"
[dependencies]
math = { zed = "acme/math" }
)");
        write(app/"src/main.ores","main");
        write(app/"src/util.ores","util");
        write(app/"src/javaonly.java","foreign native unit must reject");
        write(app/"src/other.c","unverified foreign source must reject");
        write(app/"src/.hidden/code.ores","hidden");
        write(app/"zed_modules/acme/math/src/add.ores","add");
        write(nested/".ores.pkg.toml","schema_version=\"1\"\n[source]\nroots=[\"src\"]\n");
        write(nested/"src/leaf.ores","leaf");
        write(shared/"util.ores","ambient");
        auto config=read_project(app/"src/main.ores");
        require(config.package_manifest,"not package manifest");
        require(config.root==app,"bad package root");
        require(config.source_roots.size()==1,"source roots not loaded");
        require(config.dependencies.size()==1,"dependency not loaded");
        require(config.main_entry==app/"src/main.ores","entrypoint mismatch");
        SourceIdentityRegistry identities;
        require(identities.register_source(config,app/"src/main.ores")==
                fs::canonical(app/"src/main.ores"),"canonical source identity");
        require(identities.register_source(config,app/"src/main.ores")==
                fs::canonical(app/"src/main.ores"),"repeat physical source identity");
        require(resolve_source(config,app/"src/main.ores","@/src/util")==fs::canonical(app/"src/util.ores"),"@ root");
        require(resolve_source(config,app/"src/main.ores","pkg:math/src/add")==fs::canonical(app/"zed_modules/acme/math/src/add.ores"),"pkg alias");
        write(app/"zed_modules/acme/math/.zpkg.toml", "[package]\norg=\"acme\"\nname=\"wrong\"\n");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","pkg:math/src/add");},"mismatched dependency metadata");
        write(app/"zed_modules/acme/math/.zpkg.toml", "[package]\norg=\"acme\"\nname=\"math\"\n");
        require(resolve_source(config,app/"src/main.ores","pkg:math/src/add").has_value(),"matching zed metadata rejected");
        require(resolve_source(config,app/"src/main.ores",".hidden/code")==fs::canonical(app/"src/.hidden/code.ores"),"bare hidden");
        require(resolve_source(config,app/"src/main.ores","util",shared.string())==fs::canonical(app/"src/util.ores"),"project precedence");
        require(resolve_source(config,app/"src/main.ores","leaf",shared.string())==std::nullopt,"undeclared bare file");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","@/../../other");},"root escape");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","pkg:unknown/foo");},"unknown alias");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","java:java.util.List");},
                "unsupported native Java host import");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","javaonly");},
                "implicit Java import must not be considered native source");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","javaonly.java");},
                "explicit Java import must not be considered native source");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","other.c");},
                "foreign C input must not be returned as Oreslang source");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","./javaonly.java");},
                "importer-relative Java source admission");
        auto child=read_project(nested/"src/leaf.ores");
        require(child.root==nested,"nearest nested package");
        require(child.dependencies.empty(),"inherited consumer aliases");
        rejects([&]{(void)identities.register_source(child,app/"src/main.ores");},
                "same canonical source in conflicting package context");
        rejects([&]{(void)find_manifest(app/"src/main.ores",shared);},
                "outside discovery boundary must not silently select boundary");
        rejects([&]{(void)read_project(app/"src/main.ores",shared);},
                "outside project boundary must fail closed");
        require(read_project(shared).root==shared,"manifestless directory root");
        require(read_project(shared/"util.ores").root==shared,"manifestless file root");
        auto installed=app/"zed_modules/acme/math";
        auto bounded=read_project(app/"zed_modules/acme/math/src/add.ores",installed);
        require(!bounded.package_manifest,"consumer manifest escaped into package");
        rejects([&]{(void)resolve_source(bounded,app/"zed_modules/acme/math/src/add.ores","@/src/util");},"consumer root authority");
        write(app/"src/.oreslangc.cfg.toml","schema_version=\"1\"\n");
        require(read_project(app/"src/main.ores").manifest==app/".ores.pkg.toml","package precedence");
        write(app/".ores.pkg.toml","schema_version=\"1\"\n[source]\nroots=[]\n");
        auto empty=read_project(app/"src/main.ores");
        require(empty.source_roots.empty(),"explicit roots=[]");
        require(!resolve_source(empty,app/"src/main.ores","util"),"empty roots leaked root");
        write(app/".ores.pkg.toml","schema_version=\"1\"\n[entrypoints]\nmain=\"../escape\"\n");
        rejects([&]{(void)read_project(app/"src/main.ores");},"entrypoint traversal");
        write(app/".ores.pkg.toml","schema_version=\"1\"\n[source]\nroots=[\"src\"]\nroots=[\"lib\"]\n");
        rejects([&]{(void)read_project(app/"src/main.ores");},"duplicate key");
        write(app/".ores.pkg.toml","schema_version=\"1\"\n[dependencies]\nfoo=\"../../evil\"\n");
        rejects([&]{(void)read_project(app/"src/main.ores");},"dependency traversal");
        write(app/".ores.pkg.toml","schema_version=\"1\"\n[dependencies]\nfoo=\"-evil/name\"\n");
        rejects([&]{(void)read_project(app/"src/main.ores");},"invalid dependency identifier start");
        write(app/".ores.pkg.toml","schema_version=\"1\"\n[source]\nroots=[\"src\"]\n");
        write(app/".zpkg.toml","[install]\ndir=\"../outside\"\n");
        rejects([&]{(void)read_project(app/"src/main.ores");},"install traversal");
        write(app/".zpkg.toml","[install]\ndir=\"zed_modules\"\n");
        require(read_project(app/"src/main.ores").package_manifest,"valid install setup");
        fs::remove_all(tmp);
        std::cout << "native manifest discovery and resolution: PASS\n";
        return 0;
    }catch(...){fs::remove_all(tmp);throw;}
}
