// Java/native parity gate for the four DOP layouts (AoS, SoA, AoSoA, SoAoS).
//
// Renders every segment and every absolute cell address of HeterogeneousPlan for
// the shared u32/f64/bool/ref32 field order and compares it with the checked-in
// golden table. oreslang-source.java checks the same file byte-for-byte in
// HeterogeneousDataLayoutTest (src/test/resources/dop/), so geometry drift on
// either backend fails one of the two suites. Run with --print to regenerate.
#include "oreslang/runtime/DopHeterogeneousLayout.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace oreslang::dop;

static std::string render() {
    const std::vector<FieldSpec> fields = {{4, 4}, {8, 8}, {1, 1}, {4, 4}};
    const std::vector<std::vector<std::vector<std::uint32_t>>> groups = {
        {{0, 2}, {1, 3}}, {{3}, {2, 0, 1}}};
    const char* names[] = {"AOS", "SOA", "AOSOA", "SOAOS"};
    std::ostringstream out;
    out << "# kind rows lanes groups | total physical | seg:align,bytes,stride,base... | row,field=abs...\n";
    for (int kind = 0; kind < 4; ++kind) {
        for (std::uint64_t rows : {0, 1, 2, 3, 5, 8, 17}) {
            const std::vector<std::uint32_t> lanes = kind == 2
                ? std::vector<std::uint32_t>{1, 2, 4, 8} : std::vector<std::uint32_t>{1};
            for (const auto lane : lanes) {
                for (int group = 0; group < (kind == 3 ? 2 : 1); ++group) {
                    const HeterogeneousPlan plan(static_cast<HeterogeneousLayout>(kind), rows, fields, lane,
                        kind == 3 ? groups[static_cast<std::size_t>(group)]
                                  : std::vector<std::vector<std::uint32_t>>{});
                    out << names[kind] << ' ' << rows << ' ' << lane << ' ' << (kind == 3 ? group : -1)
                        << ' ' << plan.total_bytes() << ' ' << plan.physical_bytes() << " S";
                    for (const auto& s : plan.segments())
                        out << ' ' << s.alignment << ',' << s.bytes << ',' << s.stride << ',' << s.base;
                    out << " A";
                    for (std::uint64_t row = 0; row < rows; ++row)
                        for (std::uint32_t field = 0; field < plan.field_count(); ++field)
                            out << ' ' << plan.absolute_address(row, field);
                    out << '\n';
                }
            }
        }
    }
    return out.str();
}

int main(int argc, char** argv) {
    const std::string actual = render();
    if (argc == 2 && std::string(argv[1]) == "--print") { std::cout << actual; return 0; }
    if (argc != 2) { std::cerr << "usage: " << argv[0] << " <golden.txt> | --print\n"; return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::cerr << "FAIL: cannot read golden table " << argv[1] << '\n'; return 1; }
    std::ostringstream expected;
    expected << in.rdbuf();
    if (expected.str() == actual) {
        std::cout << "PASS heterogeneous four-layout golden parity\n";
        return 0;
    }
    std::cerr << "FAIL: HeterogeneousPlan geometry drifted from the shared golden table\n";
    std::istringstream a(actual), e(expected.str());
    std::string la, le;
    for (int line = 1;; ++line) {
        const bool more_e = static_cast<bool>(std::getline(e, le));
        const bool more_a = static_cast<bool>(std::getline(a, la));
        if (!more_e && !more_a) break;
        if (!more_e) le = "<eof>";
        if (!more_a) la = "<eof>";
        if (la != le) {
            std::cerr << "line " << line << "\n  golden: " << le << "\n  actual: " << la << '\n';
            break;
        }
    }
    return 1;
}
