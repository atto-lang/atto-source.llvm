#include "oreslang/runtime/TargetAbi.hpp"
#include <fstream>
#include <climits>
#include <cstdint>
#include <type_traits>
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
        check(std::to_string(target.pointer_bits()) == cells[1]);
        check(target.endian() == cells[2] && target.abi() == cells[3] && target.emission() == cells[4]);
        check(target.experimental_wasm32_ir() == (cells[4] == "experimental-wasm32-ir"));
        if (target.pointer_bits() == 32) {
            check(target.checked_contiguous_bytes(2147483647ULL) == 2147483647ULL);
            rejects<std::overflow_error>([&] { (void)target.checked_contiguous_bytes(2147483648ULL); });
        } else {
            check(target.checked_contiguous_bytes(1ULL << 33) == (1ULL << 33));
        }
        ++count;
    }
    // parse must not borrow the caller's temporary string storage.
    const auto stable = oreslang::TargetAbi::parse(std::string("wasm32-unknown-unknown"));
    check(stable.triple() == "wasm32-unknown-unknown");
    check(count == 5);
    static_assert(CHAR_BIT == 8, "native ABI requires 8-bit bytes");
    static_assert(sizeof(float) == 4, "DOP F32 requires 32-bit float");
    static_assert(sizeof(std::uint64_t) == 8, "Oreslang i64 requires 64-bit storage");
    static_assert(!std::is_default_constructible<oreslang::TargetAbi>::value,
                  "Native target profile must not be forgeable");
    static_assert(!std::is_assignable<oreslang::TargetAbi&, oreslang::TargetAbi>::value,
                  "Native target profile must be immutable");
    check(sizeof(void*) == 4 || sizeof(void*) == 8);
    const std::uint32_t little_endian_probe = 0x01020304U;
    const auto* first_byte = reinterpret_cast<const unsigned char*>(&little_endian_probe);
    check(first_byte[0] == 4);
    check(std::numeric_limits<float>::is_iec559);
    rejects<std::invalid_argument>([] { (void)oreslang::TargetAbi::parse("wasm64-unknown-unknown"); });
    rejects<std::invalid_argument>([] { (void)oreslang::TargetAbi::parse("aarch64-unknown-linux-musl"); });
    check(oreslang::TargetAbi::checked_host_bytes(0) == 0);
    rejects<std::overflow_error>([] {
        (void)oreslang::TargetAbi::checked_host_bytes(std::numeric_limits<std::uint64_t>::max());
    });
    std::cout << "target ABI table, 32-bit bounds, host allocation bounds verified\n";
}
