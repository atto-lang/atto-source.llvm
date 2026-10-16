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
        {"@/src/main",Kind::Root},{"pkg:math/lib/answer",Kind::Package},
        {"./foo",Kind::Relative},{"../foo",Kind::Relative},
        {".",Kind::Relative},{"..",Kind::Relative},
        {".hidden/util",Kind::Bare},{"..backup/util",Kind::Bare},
        {"lib/thing",Kind::Bare},{"/tmp/thing",Kind::Absolute},
        {"java:java.util.ArrayList",Kind::HostJava},
        {"pkg:other_2/mod",Kind::Package},{"pkg:a-b/mod",Kind::Package},
        {".\\hidden",Kind::Relative}
    };
    for(const auto& [input,kind]:cases)
        check(classify(input).kind==kind,"misclassified import");
    check(classify("pkg:math/lib").alias=="math","alias identity");
    check(classify("pkg:math/lib").suffix=="lib","package suffix");
    check(classify("@/src\\util").suffix=="src/util","normalized slash");
    for(const std::string bad : {"","@","@/","@//src","@alias/x","pkg:",
         "pkg:alias","pkg:/mod","pkg:alias/","pkg:_bad/mod",
         "pkg:bad.alias/mod","pkg:alias//file","file\nname"})
         reject([&]{(void)classify(bad);},"invalid import accepted");
    reject([&]{(void)classify(std::string(4097,'x'));},"length bound");
    const fs::path root("/tmp/ores-root");
    check(rooted_path(root,"src/../lib/util")==root/"lib/util","normalized rooted source");
    check(rooted_path(root,"..hidden/a")==root/"..hidden/a","hidden relative component");
    for(const std::string bad: {"../other/file","a/../../../other","/etc/passwd",""})
        reject([&]{(void)rooted_path(root,bad);},"root escape allowed");
    check(!beneath(root,"/tmp/ores-root-evil/file"),"text-prefix escape");
    check(beneath(root,"/tmp/ores-root/file"),"valid descendant");

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
    reject([&]{std::vector<fs::path> tmp;append_list(tmp,"path",'\0',cwd);},
           "NUL separator");
    std::cout<<"Native import-path policy: host-only cases passed\n";
    return EXIT_SUCCESS;
  } catch(const std::exception& e) {
    std::cerr<<"Native import-path policy failed: "<<e.what()<<"\n";
    return EXIT_FAILURE;
  }
}
