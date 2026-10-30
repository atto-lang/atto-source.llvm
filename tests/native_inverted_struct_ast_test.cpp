// Genuine production frontend AST admission, not an experimental source adapter.
// An inverted struct is an explicitly nominal value schema, never a class;
// despite syntactic admission the program cannot pass codegen validation.
#include "oreslang/frontend/Parser.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Fn>
void rejects(Fn&& work, std::string_view diagnostic) {
    try { work(); }
    catch (const std::exception& e) {
        if (std::string(e.what()).find(diagnostic) != std::string::npos)
            return;
        throw std::runtime_error(std::string("wrong diagnostic: ") + e.what());
    }
    throw std::runtime_error("unlowered inverted source admitted without failure");
}
}

int main() {
    using namespace oreslang;
    const auto root = parse_unresolved_program(R"ores(
define inverted struct Position as
    let x: int;
    id: i64;
    alive: bool;
    weight: f64;
end
fnc main(): int { return 42; }
)ores");
    check(root.inverted_structs.size() == 1, "missing nominal inverted struct AST");
    const auto& p = root.inverted_structs.front();
    check(p.name == "Position" && !p.exported, "wrong inverted value identity");
    check(p.fields.size() == 4, "inverted field order not preserved");
    check(p.fields[0].name == "x" && p.fields[0].mutable_field &&
          p.fields[0].type == InvertedScalarKind::I64, "let i64 field incorrectly lowered");
    check(p.fields[1].name == "id" && !p.fields[1].mutable_field &&
          p.fields[1].type == InvertedScalarKind::I64, "implicit val must be immutable");
    check(p.fields[2].type == InvertedScalarKind::Bool &&
          p.fields[3].type == InvertedScalarKind::F64,
          "fixed scalar types not retained");
    check(root.classes.empty(), "inverted struct must not become a class");
    check(root.functions.size() == 1 && root.functions[0].name == "main",
          "following function lost");
    auto unresolved = parse_unresolved_program(
        "define pub inverted struct Public as id: int; end");
    check(unresolved.inverted_structs.size() == 1 &&
          unresolved.inverted_structs[0].exported, "define pub not retained");
    const auto alternate = parse_unresolved_program(
        "pub define inverted struct Public as id: int; end");
    check(alternate.inverted_structs.size() == 1 &&
          alternate.inverted_structs[0].exported, "pub define not retained");
    const auto nested = parse_unresolved_program(R"ores(
define module Mod as
    define inverted struct Point as
        x: int;
        y: int;
    end
end
)ores");
    check(nested.inverted_structs.size() == 1 &&
          nested.inverted_structs[0].name == "Point", "module nested schema lost");

    rejects([] {
        auto p = parse_unresolved_program(
            "define inverted struct Point as x: int; end");
        resolve_and_validate_program(p);
    }, "typed SoA HIR/MIR ownership lowering is not implemented");
    rejects([] {
        (void)parse_program("define inverted struct Point as x: int; end");
    }, "typed SoA HIR/MIR ownership lowering is not implemented");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted struct P as x: int; x: bool; end");
    }, "duplicate inverted struct field");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted struct P as x: Pointer<int>; end");
    }, "inverted struct member requires fixed scalar");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted struct P as x: Other; end");
    }, "inverted struct member requires fixed scalar");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted struct P as x: int; end\n"
            "define inverted struct P as y: int; end");
    }, "duplicate inverted struct declaration");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted struct Empty as end");
    }, "empty inverted struct");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted class P as x: int; end");
    }, "native inverted class requires typed one-level SoA ownership");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted struct P as x: int = 1; end");
    }, "expected ';'");
    return 0;
}
