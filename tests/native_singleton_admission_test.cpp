#include "oreslang/frontend/Parser.hpp"
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

using oreslang::parse_program;

static void check(bool condition) { if (!condition) std::abort(); }

static void denied(std::string_view source) {
    bool rejected = false;
    try { (void)parse_program(source); }
    catch (const std::exception& error) {
        const std::string message(error.what());
        rejected = message.find("ORES-SINGLETON-UNSUPPORTED") != std::string::npos &&
                   message.find("five-backend guest execution parity") != std::string::npos;
    }
    check(rejected);
}

int main() {
    denied("define singleton Settings as end\n");
    denied("define pub singleton Settings as end\n");
    denied("pub define singleton Settings as end\n");
    denied("define module app as\n define singleton Settings as end\nend\n");
    // The same declaration must be denied after otherwise valid root code.
    // This catches parsers that only reject singleton as the first unit.
    denied("fnc main(): int { return 42; }\ndefine singleton Late as end\n");
    denied("// benign declaration\n"
           "fnc main(): int { return 42; }\n"
           "define pub singleton Late as end\n");
    denied("fnc main(): int { return 42; }\n"
           "pub define singleton Late as end\n");
    // Classification occurs before evaluating body effects or leaking
    // a receiver that has not finished immutable publication.
    denied("define singleton StateBank as\n"
           "  const int answer = StateBank.answer;\n"
           "end\n");

    // Identical Oreslang source bytes as the shared state-container candidates.
    // A parser that merely chokes on @Inject after singleton is not sufficient:
    // ORES-SINGLETON-UNSUPPORTED must be raised before body or trait lowering.
    denied("define singleton StateBank with Stateful<BankState> as\n"
           "    @Inject\n"
           "    state: MutableStateContainer<BankState>\n"
           "end\n");
    denied("define singleton StateBank implements Stateful<BankState> as\n"
           "    @Inject\n"
           "    state: MutableStateContainer<BankState>\n"
           "end\n");
    denied("define singleton StateBank as\n"
           "    state { let count: int = 0 }\n"
           "end\n");

    // singleton is contextual: ordinary callable names and comments stay legal.
    const auto program = parse_program(
        "// define singleton CommentOnly as end\n"
        "fnc singleton(): int { return 42; }\n"
        "fnc singleton_count(): int { return singleton(); }\n"
        "fnc main(): int { return singleton_count(); }\n");
    check(!program.functions.empty());
    return 0;
}
