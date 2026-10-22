#include "oreslang/frontend/NativeScalarSmt.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

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
        }

        const auto bad_min = replace_once(source,
            "if (left < right)", "if (left > right)");
        const auto bad_clamp = replace_once(source,
            "if (lower > upper)", "if (lower < upper)");
        save(dir / "mutated-min_int.smt2",
             NativeScalarSmt::from_source(bad_min, "min_int"));
        save(dir / "mutated-clamp_int.smt2",
             NativeScalarSmt::from_source(bad_clamp, "clamp_int"));
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
