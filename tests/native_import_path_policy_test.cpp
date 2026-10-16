#include "oreslang/imports/ImportPathPolicy.hpp"
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace oreslang::imports;
static void check(bool ok, const char* label) {
    if(!ok) throw std::runtime_error(label);
}
static void reject(const std::function<void()>& f, const char* label) {
    try { f(); } catch(const std::invalid_argument&) { return; }
    throw std::runtime_error(label);
}
int main() {
  try {
    const std::vector<std::pair<std::string,Kind>> cases = {
        {"+/src/main",Kind::Root},{"pkg:math/lib/answer",Kind::Package},
        {"./foo",Kind::Relative},{"../foo",Kind::Relative},
        {".",Kind::Relative},{"..",Kind::Relative},
        {".hidden/util",Kind::Bare},{"..backup/util",Kind::Bare},
        {"lib/thing",Kind::Bare},{"/tmp/thing",Kind::Absolute},
        {"C:/Windows/System32",Kind::Absolute},
        {"C:\\Windows\\System32",Kind::Absolute},
        {"//fileserver/share/source.ores",Kind::Absolute},
        {"java:java.util.ArrayList",Kind::HostJava},
        {"pkg:other_2/mod",Kind::Package},{"pkg:a-b/mod",Kind::Package},
        {".\\hidden",Kind::Relative}
    };
    for(const auto& [input,kind]:cases)
        check(classify(input).kind==kind,"misclassified import");
    check(classify("pkg:math/lib").alias=="math","alias identity");
    check(classify("pkg:math/lib").suffix=="lib","package suffix");
    reject([&]{(void)classify("+/src\\util");},"rooted backslashes admitted");
    for(const std::string bad : {"","@","@/src","@alias/x","+","+src","+/","+//src","+/src//a","+/src/./a","+/src/../a","pkg:",
         "pkg:alias","pkg:/mod","pkg:alias/","pkg:_bad/mod",
         "pkg:bad.alias/mod","pkg:alias//file","pkg:alias/src/../a","pkg:alias/src/./a","pkg:alias/src/a/","file\nname",
         "C:relative.ores","D:source.ores","mod:stream",
         "./source.ores:stream","+/src/file.ores:alternate",
         "pkg:math/src/file.ores:alternate"})
         reject([&]{(void)classify(bad);},"invalid import accepted");
    reject([&]{(void)classify(std::string(4097,'x'));},"length bound");
    for (int byte = 0; byte < 32; ++byte)
        reject([&]{ (void)classify(std::string("safe") + static_cast<char>(byte) + "path"); },
               "ASCII import control byte accepted");
    reject([&]{ (void)classify(std::string("safe") + static_cast<char>(127) + "path"); },
           "DEL import byte accepted");
    check(classify(std::string(4096, 'a')).kind == Kind::Bare,
          "maximum allowed import length rejected");
    const fs::path root("/tmp/ores-root");
    check(rooted_path(root,"src/../lib/util")==root/"lib/util","normalized rooted source");
    check(rooted_path(root,"..hidden/a")==root/"..hidden/a","hidden relative component");
    check(rooted_path(root/"./","src/a")==root/"src/a","trailing root separator");
    reject([&]{(void)classify("pkg:math\\\\src\\\\a");},"pkg backslashes admitted");
    for(const std::string bad: {"../other/file","..\\\\other/file","a/../../../other","/etc/passwd","",
         "C:/Windows/System32","C:relative.ores","a/b.ores:stream",
         "nested\\folder:alternate"})
        reject([&]{(void)rooted_path(root,bad);},"root escape allowed");
    check(!beneath(root,"/tmp/ores-root/../outside"),"unnormalized ancestry escape");
    check(!beneath(root,"/tmp/ores-root/../../outside"),"multi-parent ancestry escape");
    check(beneath(root/"./","/tmp/ores-root/lib/../src"),"normalized ancestry");
    reject([&]{ (void)rooted_path(root,std::string_view("src\0outside",11)); },
           "embedded NUL rooted path");
    reject([&]{ (void)rooted_path(root,"src\nother"); },
           "line-break rooted path");
    reject([&]{ (void)rooted_path(root,std::string(4097,'a')); },
           "rooted path length");
    for (int byte = 0; byte < 32; ++byte)
        reject([&]{ (void)rooted_path(root,std::string("a") + static_cast<char>(byte) + "b"); },
               "control byte in rooted source path");
    reject([&]{(void)rooted_path(root,std::string("a") + static_cast<char>(127) + "b");},
           "DEL in rooted source path");
    check(rooted_path(root,std::string(4096, 'a')).string().size() > 0,
          "maximum allowed rooted length rejected");
    check(!beneath(root,"/tmp/ores-root-evil/file"),"text-prefix escape");
    check(beneath(root,"/tmp/ores-root/file"),"valid descendant");
    check(beneath(root,root),"root equality");
    check(!beneath(root,"/tmp/ores-rootish"),"sibling text-prefix");
    check(!beneath(root,"/tmp/ores-root/../../ores-rootish"),"normalized sibling escape");
    for (int n=0; n<729; ++n) {
        int v=n;
        std::string path;
        for (int depth=0; depth<6; ++depth) {
            const char* component = (v%3==0 ? "src" : v%3==1 ? ".." : ".");
            v/=3;
            if (!path.empty()) path += '/';
            path += component;
        }
        fs::path expected=(root/fs::path(path)).lexically_normal();
        const bool stays_inside = expected==root ||
            (expected.string().size()>root.string().size() &&
             expected.string().compare(0, root.string().size(), root.string())==0 &&
             expected.string()[root.string().size()]=='/');
        if(stays_inside) check(rooted_path(root,path)==expected,"enumerated rooted path mismatch");
        else reject([&]{(void)rooted_path(root,path);},"enumerated rooted path escape");
    }

    const fs::path cwd("/tmp/work"),project("/tmp/work/project");
    auto defaults=search_roots(project,std::nullopt,{},"","",':',cwd);
    check(defaults.size()==1 && defaults[0]==project,"default project root");
    auto disabled=search_roots(project,std::vector<std::string>{},{},"","",':',cwd);
    check(disabled.empty(),"explicit empty roots");
    auto ordered=search_roots(project,std::vector<std::string>{"src","src"},
        {"vendor","src"},":/tmp/ambient::/tmp/ambient:local:",
        "/tmp/legacy:/tmp/ambient",':',cwd);
    const std::vector<fs::path> expected={
        project/"src",project/"vendor","/tmp/ambient",cwd/"local","/tmp/legacy"};
    check(ordered==expected,"search-order dedup and empty components");
    reject([&]{(void)search_roots(project,std::vector<std::string>{" "},
       {},"","",':',cwd);},"blank manifest root accepted");
    reject([&]{(void)search_roots(project,
        std::vector<std::string>{std::string("src\0outside",11)},
        {},"","",':',cwd);},"embedded NUL manifest root");
    reject([&]{(void)search_roots(project,std::nullopt,{},
        std::string_view("/tmp/ok\0evil",12),"",':',cwd);},"embedded NUL env root");
    reject([&]{(void)search_roots(project,std::nullopt,{},
        "/tmp/ok\n/malicious","",':',cwd);},"newline env root");
    reject([&]{std::vector<fs::path> tmp;append_list(tmp,"path",'\0',cwd);},
           "NUL separator");
    std::cout<<"Native import-path policy: host-only cases passed\n";
    return EXIT_SUCCESS;
  } catch(const std::exception& e) {
    std::cerr<<"Native import-path policy failed: "<<e.what()<<"\n";
    return EXIT_FAILURE;
  }
}
