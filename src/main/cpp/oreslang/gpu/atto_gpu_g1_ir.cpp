#include "oreslang/gpu/G1SaxpyNvvm.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: oreslang-gpu-g1-ir <kernel.atto>\n";
        return 2;
    }
    try {
        const std::string path(argv[1]);
        if (path.size() < 6 || path.substr(path.size() - 5) != ".atto")
            throw std::invalid_argument("GPU-G1-001: canonical .atto source required");
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("GPU-G1-001: cannot open source");
        // Read bounded chunks; a large or malicious input cannot exhaust
        // process memory before the source budget is enforced.
        std::string source;
        char chunk[4096];
        while (input) {
            input.read(chunk, sizeof(chunk));
            source.append(chunk, static_cast<std::size_t>(input.gcount()));
            if (source.size() > 64 * 1024)
                throw std::invalid_argument("GPU-G1-001: source exceeds byte budget");
        }
        if (input.bad()) throw std::runtime_error("GPU-G1-001: source read failure");
        const auto result = oreslang::gpu::g1::compile_saxpy_nvvm(source);
        std::cout << result.llvm_ir;
        return std::cout ? 0 : 2;
    } catch (const std::exception& err) {
        // Diagnostics are bounded and intentionally contain no source paths.
        const std::string message(err.what());
        std::cerr << message.substr(0, 160) << '\n';
        return 2;
    }
}
