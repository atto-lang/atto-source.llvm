#include "oreslang/imports/NativeScalarImportClosure.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace oreslang::imports;
static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> static void reject(F f, std::string_view expected) {
    try { f(); } catch (const std::invalid_argument& error) {
        if (std::string_view(error.what()).find(expected) != std::string_view::npos)
            return;
        throw std::runtime_error(std::string("wrong diagnostic: ") + error.what());
    }
    throw std::runtime_error("unsafe import closure accepted");
}
static void write(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("failed opening fixture");
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!stream) throw std::runtime_error("failed writing fixture");
}
int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = fs::temp_directory_path() /
        ("ores-native-scalar-closure-" + std::to_string(nonce));
    fs::create_directories(dir);
    struct Cleanup {
        fs::path dir;
        ~Cleanup() { std::error_code ignored; fs::remove_all(dir, ignored); }
    } cleanup{dir};
    const auto valid = [](std::string_view source) {
        if (source.find('\0') != std::string_view::npos)
            throw std::invalid_argument("invalid source NUL");
    };
    const auto closure = [&](const fs::path& file) {
        return assemble_scalar_import_closure(file, valid);
    };
    try {
        write(dir/"helper.ores", "pub fnc answer(): int {return 42;}\n");
        write(dir/"main.ores", "import \"./helper\";\npub fnc main(): int {return answer();}\n");
        auto result = closure(dir/"main.ores");
        check(result.units.size() == 2, "two-unit closure missing");
        check(result.units.front().file == fs::canonical(dir/"helper.ores"),
              "dependency must precede entry");
        check(!result.units.front().entry && result.units.back().entry,
              "entry provenance wrong");
        for (const auto& unit : result.units) {
            check(unit.begin < unit.end && unit.end <= result.source.size(),
                  "invalid per-source span");
        }
        check(result.source.find("fnc answer") < result.source.find("fnc main"),
              "unexpected assembly order");

        // Shared diamond dependencies are included exactly once.
        write(dir/"left.ores", "import \"./helper\";\npub fnc left(): int {return answer();}\n");
        write(dir/"right.ores", "import \"./helper\";\npub fnc right(): int {return answer();}\n");
        write(dir/"diamond.ores",
              "import \"./left\";\nimport \"./right\";\npub fnc main(): int {return left()+right();}\n");
        auto diamond = closure(dir/"diamond.ores");
        check(diamond.units.size() == 4, "diamond repeated physical source");
        check(diamond.source.find("fnc answer") == diamond.source.rfind("fnc answer"),
              "diamond helper was assembled more than once");

        write(dir/"cycle-a.ores", "import \"./cycle-b\";\npub fnc a(): int {return 1;}\n");
        write(dir/"cycle-b.ores", "import \"./cycle-a\";\npub fnc b(): int {return 2;}\n");
        reject([&] { (void)closure(dir/"cycle-a.ores"); }, "cyclic native scalar imports");
        write(dir/"missing.ores", "import \"./absent\";\npub fnc main(): int {return 0;}\n");
        reject([&] { (void)closure(dir/"missing.ores"); }, "source not found");
        write(dir/"foreign.java", "not native\n");
        write(dir/"foreign.ores", "import \"./foreign.java\";\n");
        reject([&] { (void)closure(dir/"foreign.ores"); }, "only .ores sources");

        // A directive on a stripped import line must retain admission authority.
        write(dir/"directive.ores",
              "import \"./helper\"; // ores:restricted\npub fnc main(): int {return answer();}\n");
        reject([&] { (void)closure(dir/"directive.ores"); }, "native target admission unsupported");
        write(dir/"dependent-directive.ores",
              "import \"./helper\"; // ores:restricted\npub fnc forward(): int {return answer();}\n");
        write(dir/"uses-directive.ores",
              "import \"./dependent-directive\";\npub fnc main(): int {return forward();}\n");
        reject([&] { (void)closure(dir/"uses-directive.ores"); }, "native target admission unsupported");

        write(dir/"pkg"/".ores.pkg.toml",
              "schema_version = \"1\"\n[source]\nroots = [\"src\"]\n[dependencies]\nmath = { zed = \"acme/math\" }\n");
        write(dir/"pkg"/"zed_modules"/"acme"/"math"/".zpkg.toml",
              "[package]\norg = \"acme\"\nname = \"math\"\n");
        write(dir/"pkg"/"zed_modules"/"acme"/"math"/"src"/"num.ores",
              "pub fnc number(): int {return 42;}\n");
        write(dir/"pkg"/"src"/"main.ores",
              "import \"pkg:math/src/num\";\npub fnc main(): int {return number();}\n");
        auto packaged = closure(dir/"pkg"/"src"/"main.ores");
        check(packaged.units.size() == 2, "declared pkg alias unresolved");
        write(dir/"pkg"/"src"/"helper.ores",
              "pub fnc helper(): int {return 42;}\n");
        write(dir/"pkg"/"src"/"rooted.ores",
              "import \"+/src/helper\";\npub fnc main(): int {return helper();}\n");
        auto rooted = closure(dir/"pkg"/"src"/"rooted.ores");
        check(rooted.units.size() == 2, "canonical +/ import did not link scalar graph");
        write(dir/"pkg"/"src"/"named.ores",
              "import fnc helper from \"+/src/helper\";\npub fnc main(): int {return helper();}\n");
        auto named = closure(dir/"pkg"/"src"/"named.ores");
        check(named.units.size() == 2, "named import did not build source closure");
        check(named.named_imports.size() == 1 &&
              named.named_imports.front().name == "helper" &&
              named.named_imports.front().source_file ==
                  fs::canonical(dir/"pkg"/"src"/"helper.ores"),
              "named import lost physical source identity or requested function");
        write(dir/"pkg"/"src"/"legacy-root.ores",
              "import \"@/src/helper\";\npub fnc main(): int {return helper();}\n");
        reject([&] { (void)closure(dir/"pkg"/"src"/"legacy-root.ores"); },
               "invalid project-root import prefix");
        for (const std::string& bad : {"+/src/./helper", "+/src//helper",
                                       "+/src/../src/helper", "pkg:math/src/./num"}) {
            write(dir/"pkg"/"src"/"noncanonical.ores",
                  "import \"" + bad + "\";\npub fnc main(): int {return 0;}\n");
            reject([&] { (void)closure(dir/"pkg"/"src"/"noncanonical.ores"); },
                   "noncanonical rooted import component");
        }

        write(dir/"pkg"/"src"/"escape.ores", "import \"../../helper\";\n");
        reject([&] { (void)closure(dir/"pkg"/"src"/"escape.ores"); },
               "escapes physical search root");

        // Canonicalize symlink targets, not merely the import spelling.
        std::error_code link_error;
        fs::create_symlink("../../helper.ores", dir/"pkg"/"src"/"link.ores", link_error);
        if (!link_error) {
            write(dir/"pkg"/"src"/"symlink.ores",
                  "import \"./link\";\npub fnc main(): int {return answer();}\n");
            reject([&] { (void)closure(dir/"pkg"/"src"/"symlink.ores"); },
                   "escapes physical search root");
        }

        // The actual importer must see the same ordered ambient roots as
        // the independently tested resolver policy. The first matching
        // physical source wins, with explicit empty roots disabling ".".
        const auto roots = dir/"search";
        write(roots/".ores.pkg.toml",
              "schema_version = \"1\"\n[source]\nroots = [\"src\"]\nimport_paths = [\"vendor\"]\n");
        write(roots/"src"/"entry.ores", "import \"pick\";\npub fnc main(): int {return pick();}\n");
        write(roots/"src"/"pick.ores", "pub fnc pick(): int {return 42;}\n");
        write(roots/"vendor"/"pick.ores", "pub fnc pick(): int {return 41;}\n");
        write(dir/"ambient"/"pick.ores", "pub fnc pick(): int {return 40;}\n");
        write(dir/"legacy"/"pick.ores", "pub fnc pick(): int {return 39;}\n");
        const auto env_ores=(dir/"ambient").string();
        const auto env_legacy=(dir/"legacy").string();
        const auto chosen=[&] {
            return assemble_scalar_import_closure(roots/"src"/"entry.ores",
                                                  valid,env_ores,env_legacy,':',dir);
        };
        check(chosen().units.front().file==fs::canonical(roots/"src"/"pick.ores"),
              "source.roots must precede all other paths");
        fs::remove(roots/"src"/"pick.ores");
        check(chosen().units.front().file==fs::canonical(roots/"vendor"/"pick.ores"),
              "source.import_paths must precede ambient roots");
        fs::remove(roots/"vendor"/"pick.ores");
        check(chosen().units.front().file==fs::canonical(dir/"ambient"/"pick.ores"),
              "ORES_PATHS must precede legacy ORESLANG_PATH");
        fs::remove(dir/"ambient"/"pick.ores");
        check(chosen().units.front().file==fs::canonical(dir/"legacy"/"pick.ores"),
              "legacy fallback must remain last");
        write(roots/".ores.pkg.toml",
              "schema_version = \"1\"\n[source]\nroots = []\nimport_paths = []\n");
        write(roots/"pick.ores", "pub fnc pick(): int {return 77;}\n");
        reject([&] {(void)assemble_scalar_import_closure(roots/"src"/"entry.ores",valid);},
               "native scalar import source not found");
        check(assemble_scalar_import_closure(roots/"src"/"entry.ores",
              valid,roots.string()).units.front().file == fs::canonical(roots/"pick.ores"),
              "explicit env root should restore missing bare import");

        // A long linear chain must fail before unbounded recursive discovery.
        for (unsigned i = 0; i < 34; ++i) {
            const std::string name = "depth-" + std::to_string(i) + ".ores";
            const std::string body = i < 33
                ? "import \"./depth-" + std::to_string(i + 1) + "\";\n"
                : "pub fnc last(): int {return 1;}\n";
            write(dir/name, body);
        }
        reject([&] { (void)closure(dir/"depth-0.ores"); },
               "native scalar import depth exceeded");

        // Source count is bounded independently of path depth.
        std::string imports;
        for (unsigned i = 0; i < 64; ++i) {
            const auto name = "many-" + std::to_string(i);
            write(dir/(name+".ores"), "pub fnc helper(): int {return 1;}\n");
            imports += "import \"./" + name + "\";\n";
        }
        write(dir/"many-main.ores", imports + "pub fnc main(): int {return 0;}\n");
        reject([&] { (void)closure(dir/"many-main.ores"); }, "module count exceeded");

        std::cout << "native scalar closure filesystem contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native scalar closure test: " << error.what() << '\n';
        return 1;
    }
}
