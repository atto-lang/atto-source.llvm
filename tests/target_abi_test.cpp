#include "oreslang/runtime/TargetAbi.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static void check(bool ok) {
    if (!ok) throw std::runtime_error("target ABI contract mismatch");
}

template<class Error, class Fn> static void rejects(Fn fn) {
    bool caught = false;
    try { fn(); } catch (const Error&) { caught = true; }
    check(caught);
}

int main(int argc, char** argv) {
    check(argc == 2);
    std::ifstream table(argv[1]);
    check(table.is_open());
    std::string row;
    check(static_cast<bool>(std::getline(table, row)));
    check(row == "triple\tpointer_bits\tendian\tabi\temission");
    std::set<std::string> seen;
    std::size_t count = 0;
    while (std::getline(table, row)) {
        check(!row.empty());
        std::stringstream stream(row);
        std::vector<std::string> cells;
        std::string cell;
        while (std::getline(stream, cell, '\t')) cells.push_back(cell);
        check(cells.size() == 5);
        check(seen.insert(cells[0]).second);
        const auto target = oreslang::TargetAbi::parse(cells[0]);
        check(std::to_string(target.pointer_bits) == cells[1]);
        check(target.endian == cells[2] && target.abi == cells[3] && target.emission == cells[4]);
        check(target.experimental_wasm32_ir() == (cells[4] == "experimental-wasm32-ir"));
        if (target.pointer_bits == 32) {
            check(target.checked_contiguous_bytes(2147483647ULL) == 2147483647ULL);
            rejects<std::overflow_error>([&] { (void)target.checked_contiguous_bytes(2147483648ULL); });
        } else {
            check(target.checked_contiguous_bytes(1ULL << 33) == (1ULL << 33));
        }
        ++count;
    }
    check(count == 5);
    rejects<std::invalid_argument>([] { (void)oreslang::TargetAbi::parse("wasm64-unknown-unknown"); });
    rejects<std::invalid_argument>([] { (void)oreslang::TargetAbi::parse("aarch64-unknown-linux-musl"); });
    check(oreslang::TargetAbi::checked_host_bytes(0) == 0);
    rejects<std::overflow_error>([] {
        (void)oreslang::TargetAbi::checked_host_bytes(std::numeric_limits<std::uint64_t>::max());
    });
    std::cout << "target ABI table, 32-bit bounds, host allocation bounds verified\n";
}
