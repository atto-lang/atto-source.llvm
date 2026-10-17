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
        std::error_code symlink_error;
        fs::create_symlink("javaonly.java",app/"src/java_alias.ores",symlink_error);
        write(app/"src/other.c","unverified foreign source must reject");
        write(app/"src/.hidden/code.ores","hidden");
        write(app/"zed_modules/acme/math/src/add.ores","add");
        write(nested/".ores.pkg.toml","schema_version=\"1\"\n[source]\nroots=[\"src\"]\n");
        write(nested/"src/leaf.ores","leaf");
        write(shared/"util.ores","ambient");
        // In the shared project-root contract +/ is relative to [project].root,
        // including when the package manifest is physically above that root.
        const auto configured=tmp/"configured";
        fs::create_directories(configured/"app/src");
        write(configured/".ores.pkg.toml",
              "schema_version=\"1\"\n[project]\nroot=\"app\"\n[source]\nroots=[\"src\"]\n");
        write(configured/"app/src/main.ores","main");
        write(configured/"app/src/helper.ores","helper");
        write(configured/"outside.ores","wrong owner");
        auto configured_project=read_project(configured/"app/src/main.ores");
        require(configured_project.root==configured/"app",
                "package project.root did not move +/ anchor");
        require(resolve_source(configured_project,configured/"app/src/main.ores",
                               "+/src/helper")==
                fs::canonical(configured/"app/src/helper.ores"),
                "configured root import did not resolve source");
        require(!resolve_source(configured_project,configured/"app/src/main.ores",
                                "+/outside"),"root import escaped configured project root");
        write(configured/".ores.pkg.toml",
              "schema_version=\"1\"\n[project]\nroot=\"../ambient\"\n");
        rejects([&]{(void)read_project(configured/"app/src/main.ores");},
                "package project.root traversal admitted");
        write(configured/".ores.pkg.toml",
              "schema_version=\"1\"\n[project]\nroot=\"app/../ambient\"\n");
        rejects([&]{(void)read_project(configured/"app/src/main.ores");},
                "package project.root dot normalization admitted");
        fs::create_directories(configured/"app/child");
        write(configured/"app/child/.ores.pkg.toml","schema_version=\"1\"\n");
        write(configured/".ores.pkg.toml",
              "schema_version=\"1\"\n[project]\nroot=\"app/child\"\n");
        rejects([&]{(void)read_project(configured/"app/src/main.ores");},
                "package project.root crossed nested package authority");
        write(configured/".ores.pkg.toml",
              "schema_version=\"1\"\n[project]\nroot=\"app\"\n");
        // Existing but malformed modern marker must shadow a valid legacy
        // manifest in the same directory. Falling back changes +/ ownership.
        const auto priority=tmp/"manifest-priority";
        fs::create_directories(priority/"src");
        write(priority/"src/main.ores","main");
        write(priority/".oreslangc.cfg.toml","schema_version=\"1\"\n");
        require(read_project(priority/"src/main.ores").manifest ==
                priority/".oreslangc.cfg.toml", "valid legacy marker ignored");
        fs::create_directory(priority/".ores.pkg.toml");
        rejects([&]{(void)read_project(priority/"src/main.ores");},
                "directory modern marker fell through to legacy");
        fs::remove(priority/".ores.pkg.toml");
        std::error_code malformed_link_error;
        fs::create_symlink(priority/"missing-modern.toml",
                           priority/".ores.pkg.toml",malformed_link_error);
        if(!malformed_link_error)
            rejects([&]{(void)read_project(priority/"src/main.ores");},
                    "dangling modern symlink fell through to legacy");
        fs::remove(priority/".ores.pkg.toml");
        fs::create_symlink(priority/".oreslangc.cfg.toml",
                           priority/".ores.pkg.toml",malformed_link_error);
        if(!malformed_link_error)
            rejects([&]{(void)read_project(priority/"src/main.ores");},
                    "intra-directory modern symlink admitted as package manifest");
        fs::remove(priority/".ores.pkg.toml");
        fs::remove(priority/".oreslangc.cfg.toml");
        fs::create_directory(priority/".oreslangc.cfg.toml");
        rejects([&]{(void)read_project(priority/"src/main.ores");},
                "malformed legacy manifest silently became manifestless");

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
        auto altered_context=config;
        altered_context.boundary=app/"src";
        rejects([&]{(void)identities.register_source(altered_context,app/"src/main.ores");},
                "same physical source with different discovery boundary");

        // Symlinks to valid .ores files outside declared roots must not
        // launder package, rooted or bare-search import authority.
        fs::create_directories(tmp/"outside");
        fs::create_directories(app/"packages");
        write(tmp/"outside/valid.ores","untrusted");
        std::error_code external_root_error,external_pkg_error,external_bare_error,internal_error;
        fs::create_symlink(tmp/"outside/valid.ores",app/"src/external.ores",external_root_error);
        fs::create_symlink(tmp/"outside/valid.ores",
                           app/"zed_modules/acme/math/src/external.ores",external_pkg_error);
        fs::create_directory_symlink(tmp/"outside",app/"packages/outside",external_bare_error);
        fs::create_symlink(app/"src/util.ores",app/"src/internal.ores",internal_error);
        if(!external_root_error){
            rejects([&]{(void)resolve_source(config,app/"src/main.ores","+/src/external");},
                    "rooted physical symlink escape");
            rejects([&]{(void)resolve_source(config,app/"src/main.ores","external");},
                    "bare-root physical symlink escape");
        }
        if(!external_pkg_error)
            rejects([&]{(void)resolve_source(config,app/"src/main.ores","pkg:math/src/external");},
                    "package physical symlink escape");
        if(!external_root_error)
            rejects([&]{(void)resolve_source(config,app/"src/main.ores","./external.ores");},
                    "package-relative physical symlink escape");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","../../outside/valid.ores");},
                "package-relative parent traversal escape");
        if(!external_bare_error)
            rejects([&]{(void)resolve_source(config,app/"src/main.ores","outside/valid");},
                    "import_paths physical directory symlink escape");
        if(!internal_error)
            require(resolve_source(config,app/"src/main.ores","+/src/internal")==
                    fs::canonical(app/"src/util.ores"),"in-root symlink rejected");
        require(resolve_source(config,app/"src/main.ores","+/src/util")==fs::canonical(app/"src/util.ores"),"+ root");
        // The search root may be *exactly* the nested package directory.
        // Checking only descendants of that root would skip its own marker.
        std::error_code nested_alias_error;
        fs::create_symlink(app/"src/util.ores",nested/"parent-alias.ores",nested_alias_error);
        auto configured_child_root=config;
        configured_child_root.import_paths={nested};
        rejects([&]{(void)resolve_source(configured_child_root,app/"src/main.ores",
                                         "src/leaf");},
                "configured bare root at nested package acquired child authority");
        if(!nested_alias_error)
            rejects([&]{(void)resolve_source(configured_child_root,app/"src/main.ores",
                                             "parent-alias");},
                    "configured child search root symlink laundered parent source");

        // Parent project authority must not tunnel into a nested package
        // through relative, bare import-path or symlink-alias spellings.
        rejects([&]{(void)resolve_source(config,app/"src/main.ores",
                                        "../packages/child/src/leaf");},
                "relative import crossed independently declared child package");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores",
                                        "child/src/leaf");},
                "bare import crossed independently declared child package");
        if(!nested_alias_error){
            rejects([&]{(void)resolve_source(config,app/"src/main.ores",
                                            "../packages/child/parent-alias");},
                    "relative child symlink alias bypassed package boundary");
            rejects([&]{(void)resolve_source(config,app/"src/main.ores",
                                            "child/parent-alias");},
                    "bare child symlink alias bypassed package boundary");
        }

        rejects([&]{(void)resolve_source(config,app/"src/main.ores","+/packages/child/src/leaf");},
                "parent +/ crossed direct child package marker");
        // Canonical physical identity by itself misses this: a symlink
        // spelled through the child package points to a parent-owned file.
        // Lexical traversal must retain the child's package boundary.
        if(!nested_alias_error)
            rejects([&]{(void)resolve_source(config,app/"src/main.ores","+/packages/child/parent-alias");},
                    "child package symlink laundered parent +/ authority");
        require(resolve_source(config,app/"src/main.ores","pkg:math/src/add")==fs::canonical(app/"zed_modules/acme/math/src/add.ores"),"pkg alias");
        write(app/"zed_modules/acme/math/.zpkg.toml", "[package]\norg=\"acme\"\nname=\"wrong\"\n");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","pkg:math/src/add");},"mismatched dependency metadata");
        write(app/"zed_modules/acme/math/.zpkg.toml", "[package]\norg=\"acme\"\nname=\"math\"\n");
        require(resolve_source(config,app/"src/main.ores","pkg:math/src/add").has_value(),"matching zed metadata rejected");
        require(resolve_source(config,app/"src/main.ores",".hidden/code")==fs::canonical(app/"src/.hidden/code.ores"),"bare hidden");
        require(resolve_source(config,app/"src/main.ores","util",shared.string())==fs::canonical(app/"src/util.ores"),"project precedence");
        require(resolve_source(config,app/"src/main.ores","leaf",shared.string())==std::nullopt,"undeclared bare file");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","+/src/util.ores:stream");},
                "rooted alternate data stream must reject");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","pkg:math/src/add.ores:stream");},
                "package alternate data stream must reject");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","C:relative.ores");},
                "drive-relative import must reject");
        if(!fs::path("C:/foreign/source.ores").is_absolute())
            rejects([&]{(void)resolve_source(config,app/"src/main.ores","C:/foreign/source.ores");},
                    "cross-host drive path must never become a relative source");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","+/../../other");},"root escape");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","pkg:unknown/foo");},"unknown alias");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","java:java.util.List");},
                "unsupported native Java host import");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","javaonly");},
                "implicit Java import must not be considered native source");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","javaonly.java");},
                "explicit Java import must not be considered native source");
        if(!symlink_error)
            rejects([&]{(void)resolve_source(config,app/"src/main.ores","java_alias.ores");},
                    "ores extension must not launder foreign symlink target");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","other.c");},
                "foreign C input must not be returned as Oreslang source");
        rejects([&]{(void)resolve_source(config,app/"src/main.ores","./javaonly.java");},
                "importer-relative Java source admission");
        auto child=read_project(nested/"src/leaf.ores");
        require(child.root==nested,"nearest nested package");
        require(child.dependencies.empty(),"inherited consumer aliases");
        rejects([&]{(void)identities.register_source(child,app/"src/main.ores");},
                "same canonical source in conflicting package context");
        rejects([&]{(void)identities.register_source(config,app/"src/javaonly.java");},
                "foreign source registered in native identity graph");
        rejects([&]{(void)find_manifest(app/"src/main.ores",shared);},
                "outside discovery boundary must not silently select boundary");
        rejects([&]{(void)read_project(app/"src/main.ores",shared);},
                "outside project boundary must fail closed");
        require(read_project(shared).root==shared,"manifestless directory root");
        require(read_project(shared/"util.ores").root==shared,"manifestless file root");
        auto installed=app/"zed_modules/acme/math";
        auto bounded=read_project(app/"zed_modules/acme/math/src/add.ores",installed);
        require(!bounded.package_manifest,"consumer manifest escaped into package");
        rejects([&]{(void)resolve_source(bounded,app/"zed_modules/acme/math/src/add.ores","+/src/util");},"consumer root authority");
        write(app/"src/.oreslangc.cfg.toml","schema_version=\"1\"\n");
        require(read_project(app/"src/main.ores").manifest==app/".ores.pkg.toml","package precedence");
        write(app/".ores.pkg.toml","schema_version=\"1\"\n[source]\nroots=[]\n");
        auto empty=read_project(app/"src/main.ores");
        require(empty.source_roots.empty(),"explicit roots=[]");
        require(!resolve_source(empty,app/"src/main.ores","util"),"empty roots leaked root");
        write(app/".ores.pkg.toml","schema_version=\"1\"\n[entrypoints]\nmain=\"../escape\"\n");
        rejects([&]{(void)read_project(app/"src/main.ores");},"entrypoint traversal");
        write(app/".ores.pkg.toml","schema_version=\"1\"\n[package]\nname=7\n");
        rejects([&]{(void)read_project(app/"src/main.ores");},
                "known TOML metadata key with wrong type admitted");
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
        // Entrypoint paths and manifest metadata must also be checked against
        // physical targets when they exist, not only against lexical ancestry.
        write(app/".ores.pkg.toml",
              "schema_version=\"1\"\n[entrypoints]\nmain=\"src/external.ores\"\n");
        if(!external_root_error)
            rejects([&]{(void)read_project(app/"src/main.ores");},
                    "package entrypoint physical symlink escape");
        // A generated entrypoint need not exist yet, but a directory
        // symlink in its ancestor chain must not rebase it outside the root.
        std::error_code redirected_main_error, dangling_main_error;
        fs::create_directory_symlink(tmp/"outside",app/"src"/"generated-outside",
                                     redirected_main_error);
        if(!redirected_main_error) {
            write(app/".ores.pkg.toml",
                  "schema_version=\"1\"\n[entrypoints]\nmain=\"src/generated-outside/not-yet.ores\"\n");
            rejects([&]{(void)read_project(app/"src/main.ores");},
                    "missing entrypoint under outside symlink was accepted");
        }
        fs::create_directory_symlink(tmp/"outside"/"absent",
                                     app/"src"/"dangling-generated",dangling_main_error);
        if(!dangling_main_error) {
            write(app/".ores.pkg.toml",
                  "schema_version=\"1\"\n[entrypoints]\nmain=\"src/dangling-generated/not-yet.ores\"\n");
            rejects([&]{(void)read_project(app/"src/main.ores");},
                    "dangling entrypoint ancestor was accepted");
        }
        write(app/".ores.pkg.toml",
              "schema_version=\"1\"\n[entrypoints]\nmain=\"src/not-yet-created/main.ores\"\n");
        require(read_project(app/"src/main.ores").main_entry ==
                app/"src/not-yet-created/main.ores",
                "legitimate generated in-root entrypoint denied");
        write(tmp/"outside/evil.toml","schema_version=\"1\"\n");
        fs::remove(app/".ores.pkg.toml");
        std::error_code manifest_symlink_error;
        fs::create_symlink(tmp/"outside/evil.toml",app/".ores.pkg.toml",manifest_symlink_error);
        if(!manifest_symlink_error)
            rejects([&]{(void)read_project(app/"src/main.ores");},
                    "package manifest symlink escaped discovery root");

        fs::remove_all(tmp);
        std::cout << "native manifest discovery and resolution: PASS\n";
        return 0;
    }catch(...){fs::remove_all(tmp);throw;}
}
