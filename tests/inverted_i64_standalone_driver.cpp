// Dependency-free executable wrapper for exact-header compilation testing.
// NOT oreslang-llvmc, not production source dispatch or HIR/MIR validation.
#include "oreslang/frontend/ExperimentalInvertedI64.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) != "--experimental-inverted-i64") {
        std::cerr << "standalone i64 driver requires --experimental-inverted-i64 <source.ores>\n";
        return 2;
    }
    try {
        std::ifstream file(argv[2], std::ios::binary);
        if (!file) throw std::runtime_error("cannot read source fixture");
        constexpr std::size_t max_size = 65536;
        // A bounded stream read prevents accidental unbounded allocation in
        // this test driver. Unlike the full compiler this does not establish
        // race-free input inode/symlink identity and is not a release path.
        std::array<char, max_size + 1> source{};
        file.read(source.data(), static_cast<std::streamsize>(source.size()));
        const auto length = file.gcount();
        if (file.bad()) throw std::runtime_error("source fixture read failed");
        if (length > static_cast<std::streamsize>(max_size))
            throw std::runtime_error("source fixture exceeds 64 KiB");
        std::cout << oreslang::experimental_inverted_i64::compile(
            std::string_view(source.data(), static_cast<std::size_t>(length)));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "inverted-i64-test-driver: " << error.what() << '\n';
        return 1;
    }
}
