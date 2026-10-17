#include "oreslang/backend/LlvmCodegen.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

static void must_reject(int min_threads, int max_threads,
                        bool wasm32 = false, bool cortex_m = false) {
    oreslang::Program program;
    oreslang::ActorDecl actor;
    actor.name = "Forged";
    actor.min_threads = min_threads;
    actor.max_threads = max_threads;
    program.actors.push_back(std::move(actor));
    try {
        (void)oreslang::emit_verified_llvm_ir(program, wasm32, cortex_m);
    } catch (const std::runtime_error& rejected) {
        if (std::string(rejected.what()).find("unsupported physical parallelism")
                != std::string::npos) {
            return;
        }
        std::cerr << "FAIL: wrong diagnostic for forged actor thread policy: "
                  << rejected.what() << '\n';
        std::exit(EXIT_FAILURE);
    }
    std::cerr << "FAIL: forged actor threads "
              << min_threads << "/" << max_threads
              << " emitted LLVM IR\n";
    std::exit(EXIT_FAILURE);
}

int main() {
    // Direct Program API deliberately bypasses the parser; the code generator
    // must independently prevent unverified multi-carrier guest execution.
    must_reject(1, 3);
    must_reject(2, 3);
    must_reject(0, 1);
    must_reject(1, 0);
    must_reject(1, 3, true, false);
    must_reject(1, 3, false, true);
    std::cout << "PASS guarded LLVM source actor IR admission\n";
}
