#include "oreslang/frontend/NativeScalarSmt.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using oreslang::formal::NativeScalarSmt;

static std::string read_source(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open native proof fixture");
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}
static void save(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open proof output");
    out << contents;
    if (!out) throw std::runtime_error("cannot finish proof output");
}
static std::string replace_once(std::string s, const std::string& from,
                                const std::string& to) {
    const auto at = s.find(from);
    if (at == std::string::npos || s.find(from, at + from.size()) != std::string::npos)
        throw std::runtime_error("mutated source identity mismatch");
    s.replace(at, from.size(), to);
    return s;
}
// The native exporter writes precisely two named function definitions and two
// or three scalar argument declarations. A valid Z3 transcript is not proof
// if a preceding injected reset/set-option changes solver state.
static void validate_scalar_prelude(const std::string& prelude) {
    if (prelude.find_first_of(";\"\r|") != std::string::npos)
        throw std::runtime_error("unsupported native SMT prelude token");
    std::vector<std::string> forms;
    std::size_t cursor=0;
    while (cursor < prelude.size()) {
        if (prelude[cursor]!='(')
            throw std::runtime_error("unexpected native SMT prelude text");
        const auto start=cursor;
        int depth=0;
        do {
            const auto ch=prelude[cursor++];
            if (ch=='(') ++depth;
            else if (ch==')') --depth;
            if (depth<0) throw std::runtime_error("unbalanced native prelude");
        } while (depth>0 && cursor<prelude.size());
        if (depth!=0 || cursor>=prelude.size() || prelude[cursor++]!='\n')
            throw std::runtime_error("unterminated native prelude command");
        forms.emplace_back(prelude.substr(start,cursor-start-1));
    }
    if ((forms.size()!=5 && forms.size()!=6)
            || forms[0]!="(set-logic QF_BV)"
            || forms[1].rfind("(define-fun implementation (",0)!=0
            || forms[2].rfind("(define-fun contract (",0)!=0)
        throw std::runtime_error("unsupported native SMT prelude commands");
    const std::array<const char*,3> vars={"x","y","z"};
    for (std::size_t i=3; i<forms.size(); ++i) {
        const auto expected=std::string("(declare-const ")+vars[i-3]+" (_ BitVec 64))";
        if (forms[i]!=expected)
            throw std::runtime_error("unexpected native scalar variable declaration");
    }
}

// Restrictive query splitting for the *real* Parser/OwnershipChecker-derived
// scalar SMT adapter. This is not a source-to-machine refinement proof.
static std::array<std::string, 2> separate_scalar_queries(const std::string& combined) {
    constexpr const char* marker = "(push 1)\n";
    constexpr const char* tail = "\n(check-sat)\n(pop 1)\n";
    if (combined.size() > 65536
            || combined.rfind("(set-logic QF_BV)\n", 0) != 0
            || combined.find("(echo ") != std::string::npos
            || combined.find("(exit)") != std::string::npos)
        throw std::runtime_error("unsupported SMT document");
    const auto first = combined.find(marker);
    const auto second = first == std::string::npos ? first
        : combined.find(marker, first + std::string(marker).size());
    if (first == std::string::npos || second == std::string::npos
            || combined.find(marker, second + std::string(marker).size()) != std::string::npos)
        throw std::runtime_error("scalar proof must have exactly two queries");
    const std::string prefix = combined.substr(0, first);
    validate_scalar_prelude(prefix);
    const std::string proof = combined.substr(first + std::string(marker).size(),
        second - first - std::string(marker).size());
    const std::string mutant = combined.substr(second + std::string(marker).size());
    for (const auto& body : {proof, mutant}) {
        const std::string terminator(tail);
        if (body.rfind("(assert ", 0) != 0
                || body.size() < terminator.size()
                || body.compare(body.size()-terminator.size(),terminator.size(),terminator) != 0
                || body.find("(check-sat)") != body.rfind("(check-sat)")
                || body.find("(push 1)") != std::string::npos
                || body.find("(echo ") != std::string::npos)
            throw std::runtime_error("bad or incomplete scalar query block");
        // Precisely one parenthesized top-level assertion; no extra commands
        // such as (reset) or (assert true) may precede the SAT check.
        const auto assertion=body.substr(0, body.size()-terminator.size());
        if (assertion.find_first_of("\n\r;\"|") != std::string::npos)
            throw std::runtime_error("unrecognized SMT assertion token");
        int depth=0;
        for (std::size_t i=0; i<assertion.size(); ++i) {
            if (assertion[i]=='(') ++depth;
            else if (assertion[i]==')') --depth;
            if (depth<0 || (depth==0 && i+1!=assertion.size()))
                throw std::runtime_error("extra top-level SMT command");
        }
        if (depth!=0) throw std::runtime_error("unbalanced SMT assertion");
    }
    return {prefix + marker + proof, prefix + marker + mutant};
}

static void save_slices(const fs::path& dir, const std::string& label,
                        const std::string& combined) {
    const auto slices = separate_scalar_queries(combined);
    save(dir / (label + "-implementation.smt2"), slices[0]);
    save(dir / (label + "-negative-control.smt2"), slices[1]);
}
template<typename F> static void must_reject(F&& run) {
    try {
        run();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("unsafe native proof input was accepted");
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: scalar-smt fixture.ores output-dir");
        const std::string source = read_source(argv[1]);
        const fs::path dir = argv[2];
        fs::create_directories(dir);
        for (const char* name: {"min_int", "max_int", "clamp_int"}) {
            const auto smt = NativeScalarSmt::from_source(source, name);
            if (smt.find("(check-sat)") == std::string::npos
                    || smt.find("(define-fun implementation") == std::string::npos)
                throw std::runtime_error("native exporter omitted proof obligations");
            save(dir / (std::string(name) + ".smt2"), smt);
            save_slices(dir, name, smt);
        }

        const auto bad_min = replace_once(source,
            "if (left < right)", "if (left > right)");
        const auto bad_clamp = replace_once(source,
            "if (lower > upper)", "if (lower < upper)");
        const auto bad_min_smt = NativeScalarSmt::from_source(bad_min, "min_int");
        const auto bad_clamp_smt = NativeScalarSmt::from_source(bad_clamp, "clamp_int");
        save(dir / "mutated-min_int.smt2", bad_min_smt);
        save(dir / "mutated-clamp_int.smt2", bad_clamp_smt);
        save_slices(dir, "mutated-min_int", bad_min_smt);
        save_slices(dir, "mutated-clamp_int", bad_clamp_smt);
        // Injected commands and truncated obligations cannot be relabelled
        // as independently replayable checked AST proof queries.
        const auto checked_min = NativeScalarSmt::from_source(source, "min_int");
        must_reject([&] {
            (void)separate_scalar_queries(checked_min + "(check-sat)\n");
        });
        must_reject([&] {
            (void)separate_scalar_queries(checked_min + "(echo \"unsat\")\n");
        });
        must_reject([&] {
            auto injected=checked_min;
            const auto first=injected.find("(set-logic QF_BV)\n");
            injected.insert(first+std::string("(set-logic QF_BV)\n").size(),"(reset)\n");
            (void)separate_scalar_queries(injected);
        });
        must_reject([&] {
            auto injected=checked_min;
            const auto first=injected.find("(set-logic QF_BV)\n");
            injected.insert(first+std::string("(set-logic QF_BV)\n").size(),
                "(set-option :produce-models true)\n");
            (void)separate_scalar_queries(injected);
        });
        must_reject([&] {
            auto injected=checked_min;
            const auto first=injected.find("(declare-const x (_ BitVec 64))\n");
            injected.insert(first+std::string("(declare-const x (_ BitVec 64))\n").size(),
                "(declare-const evil (_ BitVec 64))\n");
            (void)separate_scalar_queries(injected);
        });
        const auto check_at=checked_min.find("(check-sat)");
        if (check_at==std::string::npos)
            throw std::runtime_error("missing native check-sat control");
        must_reject([&] {
            auto injected=checked_min;
            injected.insert(check_at,"(reset)\n");
            (void)separate_scalar_queries(injected);
        });
        must_reject([&] {
            auto injected=checked_min;
            injected.insert(check_at,"(assert true)\n");
            (void)separate_scalar_queries(injected);
        });
        must_reject([&] {
            (void)separate_scalar_queries(checked_min.substr(
                std::string("(push 1)\n").size()));
        });
        // A parser/type-correct arithmetic return is outside the restricted
        // *pure parameter-return* SMT slice and must reject. No default values
        // or guessed effects are allowed into a proof.
        const auto arithmetic = replace_once(source,
            "if (left < right) { return left; } fi\n  return right;",
            "if (left < right) { return left; } fi\n  return right + 1;");
        must_reject([&] {
            (void)NativeScalarSmt::from_source(arithmetic, "min_int");
        });
        must_reject([&] {
            (void)NativeScalarSmt::from_source(source, "unknown_function");
        });
        must_reject([&] {
            (void)NativeScalarSmt::from_source(
                std::string(2'000'001, ' '), "min_int");
        });
        must_reject([&] {
            (void)NativeScalarSmt::from_source(source + std::string(1, '\0'), "min_int");
        });
        must_reject([&] {
            (void)NativeScalarSmt::from_source(source + std::string(1, char(0xff)), "min_int");
        });
        std::cout << "NATIVE-CHECKED-SMT-FIXTURES-EMITTED\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "native scalar proof FAILED: " << e.what() << '\n';
        return 1;
    }
}
