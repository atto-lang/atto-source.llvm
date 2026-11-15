#include "oreslang/frontend/Parser.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using oreslang::Expression;

static std::string read_fixture(const std::string& root, const std::string& filename) {
    std::ifstream file(root + "/" + filename, std::ios::binary);
    if (!file) throw std::runtime_error("missing native ownership fixture: " + filename);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

static const Expression& parsed(std::string_view source, oreslang::Program& holder) {
    holder = oreslang::parse_unresolved_program(
        std::string("pub fnc main(): int { return ") + std::string(source) + "; }");
    if (holder.functions.size() != 1 || !holder.functions[0].result)
        throw std::runtime_error("expected one parsed native result");
    return *holder.functions[0].result;
}

static void equivalent(std::string_view legacy, std::string_view spaced,
                       std::string_view compact, Expression::Kind kind) {
    for (const auto source : {legacy, spaced, compact}) {
        oreslang::Program holder;
        const Expression& expr = parsed(source, holder);
        if (expr.kind != kind || !expr.left)
            throw std::runtime_error("incorrect bang alias AST for " + std::string(source));
    }
}

static void reject(std::string_view input) {
    try {
        oreslang::Program parsed_source;
        (void)parsed(input, parsed_source);
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("unexpected ownership spelling acceptance: " + std::string(input));
}

int main(int argc, char** argv) {
    if (argc != 2) throw std::runtime_error("expected canonical .atto fixture directory");
    const std::string root(argv[1]);
    equivalent("rt copy foo", "copy! foo", "copy!foo", Expression::Kind::Copy);
    equivalent("rt take foo", "take! foo", "take!foo", Expression::Kind::Take);
    equivalent("rt borrow foo", "borrow! foo", "borrow!foo", Expression::Kind::Borrow);
    equivalent("rt share foo", "share! foo", "share!foo", Expression::Kind::Share);
    equivalent("rt mut borrow foo", "mut borrow! foo", "mut borrow!foo",
               Expression::Kind::BorrowMut);
    equivalent("rt borrow mut foo", "mut borrow! foo", "mut borrow!foo",
               Expression::Kind::BorrowMut);
    equivalent("rt copy(foo + 1)", "copy! (foo + 1)", "copy!(foo + 1)",
               Expression::Kind::Copy);
    equivalent("rt borrow foo.bar", "borrow! foo.bar", "borrow!foo.bar",
               Expression::Kind::Borrow);
    for (const auto invalid : {"copy !foo", "take !foo", "borrow !foo",
                               "mut borrow !foo", "share !foo", "copy!",
                               "mut borrow!"}) {
        reject(invalid);
    }
    // Production parser+ownership checker against exact checked-in .atto
    // fixtures. CTest must execute this; source-only fixture presence is not
    // itself an executable guest or native AOT claim.
    {
        const auto program = oreslang::parse_program(
            read_fixture(root, "borrow-call-accept.atto"));
        if (program.functions.size() != 3)
            throw std::runtime_error("native borrowed-argument admission missing");
    }
    auto semantic_reject = [](std::string_view source, std::string_view expected) {
        try {
            (void)oreslang::parse_program(source);
        } catch (const std::exception& e) {
            if (std::string(e.what()).find(expected) != std::string::npos)
                return;
            throw std::runtime_error("wrong native ownership denial: " +
                                     std::string(e.what()));
        }
        throw std::runtime_error("native ownership violation accepted");
    };
    semantic_reject(read_fixture(root, "take-after-use-reject.atto"), "moved");
    semantic_reject(read_fixture(root, "borrow-escape-reject.atto"), "borrowed reference");
    semantic_reject(read_fixture(root, "share-class-reject.atto"), "non-Copy");

    // A genuine native parser+ownership check, not a lexical-model-only test.
    for (const auto source : {"copy!41", "copy! 41", "rt copy 41"}) {
        const auto program = oreslang::parse_program(
            std::string("pub fnc main(): int { return ") + source + "; }");
        if (program.functions.empty() || !program.functions[0].result)
            throw std::runtime_error("compiler ownership admission missing");
    }
    return 0;
}
