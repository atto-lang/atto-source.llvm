#include "oreslang/imports/ImportPreamble.hpp"
#include <stdexcept>
#include <functional>
#include <iostream>
using oreslang::imports::split_import_preamble;
static void check(bool good) { if (!good) throw std::runtime_error("import preamble assertion failed"); }
static void rejects(const std::string& src) {
    bool threw=false;
    try { (void)split_import_preamble(src); } catch(const std::invalid_argument&) { threw=true; }
    check(threw);
}
int main() {
    auto a=split_import_preamble("// comment\n import \"./helper\";  // note\nimport \"pkg:math/add\";\npub fnc main(): int { return add(1,2); }\n");
    check(a.imports.size()==2 && a.imports[0]=="./helper" && a.imports[1]=="pkg:math/add");
    check(a.stripped_source.size()==std::string("// comment\n import \"./helper\";  // note\nimport \"pkg:math/add\";\npub fnc main(): int { return add(1,2); }\n").size());
    check(a.stripped_source.find("import") == std::string::npos);
    rejects("import src/file;\nfnc main(): int {return 0;}");
    rejects("import \"\";\n");
    rejects("import \"a\" garbage;\n");
    rejects("import \"a\"\n");
    rejects("import \"a\nb\";\n");
    rejects("import \"a\"; import \"b\";\n");
    auto b=split_import_preamble("/*\nimport \"should-not-execute\";\n*/\nfnc main(): int {return 0;}");
    check(b.imports.empty());
    auto c=split_import_preamble("fnc main(): int {return 0;}\nimport \"late\";\n");
    check(c.imports.empty() && c.stripped_source.find("late")!=std::string::npos);
    std::cout << "native import preamble selftest pass\n";
}
