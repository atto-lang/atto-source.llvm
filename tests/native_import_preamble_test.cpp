#include "oreslang/imports/ImportPreamble.hpp"
#include <stdexcept>
#include <functional>
#include <random>
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

    // Preserve function source offsets even through CRLF and comment lines.
    const std::string crlf = "// leading\r\n  import \"./a\"; // trailing\r\n\timport \"pkg:x/b\";\r\npub fnc main(): int {return 42;}\r\n";
    auto crlf_result = split_import_preamble(crlf);
    check(crlf_result.imports.size()==2 && crlf_result.imports[0]=="./a" &&
          crlf_result.imports[1]=="pkg:x/b");
    check(crlf_result.stripped_source.size()==crlf.size());
    auto eof=split_import_preamble("import \"./a\";");
    check(eof.imports.size()==1 && eof.stripped_source.size()==13);
    auto max_path=split_import_preamble("import \""+std::string(4096,'x')+"\";\n");
    check(max_path.imports.size()==1 && max_path.imports[0].size()==4096);
    rejects("import \""+std::string(4097,'x')+"\";\n");
    for(unsigned int byte=0;byte<32;++byte)
        rejects("import \"a"+std::string(1,static_cast<char>(byte))+"b\";\n");
    rejects("import \"a"+std::string(1,static_cast<char>(127))+"b\";\n");
    std::string many;
    for(int i=0;i<128;++i) many+="import \"./x\";\n";
    auto bound=split_import_preamble(many);
    check(bound.imports.size()==128 && bound.stripped_source.size()==many.size());
    rejects(many+"import \"./x\";\n");
    rejects("import \"x\";;\n");
    rejects("import \"x\" // missing semicolon\n");
    auto prefix=split_import_preamble("imported \"x\";\nimport \"later\";\n");
    check(prefix.imports.empty());

    // Deterministic malformed-preamble fuzz: grammar errors may reject, but
    // successful scans must preserve every source byte offset and stay bounded.
    std::mt19937_64 rng(0x15C0FFEEULL);
    const std::string alphabet="import \";./\\\t\r\n#*/abcdefghijklmnopqrstuvwxyz0123456789";
    for(int i=0;i<15000;++i){
        std::string random_source;
        const auto n=static_cast<std::size_t>(rng()%150);
        for(std::size_t j=0;j<n;++j)
            random_source+=alphabet[static_cast<std::size_t>(rng()%alphabet.size())];
        try {
            const auto parsed=split_import_preamble(random_source);
            check(parsed.stripped_source.size()==random_source.size());
            check(parsed.imports.size()<=128);
        } catch(const std::invalid_argument&) {
            // Rejection is correct for malformed generated import syntax.
        }
    }
    std::cout << "native import preamble selftest pass\n";
}
