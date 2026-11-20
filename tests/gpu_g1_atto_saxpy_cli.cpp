#include "oreslang/backend/GpuG1AttoSaxpySource.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "GPU-G1-SOURCE-001: requires one .atto file\n"; return 2; }
    try {
        const std::string path(argv[1]);
        if (path.size() < 6 || path.compare(path.size() - 5, 5, ".atto") != 0)
            throw std::invalid_argument("GPU-G1-SOURCE-001: .atto extension required");
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::invalid_argument("GPU-G1-SOURCE-001: unreadable source");
        std::string source;
        char chunk[4096];
        while (input) {
            input.read(chunk, sizeof(chunk));
            const auto n = input.gcount();
            if (n < 0 || source.size() + static_cast<std::size_t>(n) > 65536)
                throw std::invalid_argument("GPU-G1-SOURCE-001: source byte budget exceeded");
            source.append(chunk, static_cast<std::size_t>(n));
        }
        if (input.bad()) throw std::invalid_argument("GPU-G1-SOURCE-001: source read failed");
        const auto out = oreslang::gpu::g1::compile_g1_saxpy_source(source);
        std::cout << out.llvm_ir;
        return std::cout ? 0 : 2;
    } catch (const std::exception& error) {
        // Closed diagnostic strings only; never echo untrusted source or path.
        std::cerr << error.what() << '\n';
        return 2;
    }
}
