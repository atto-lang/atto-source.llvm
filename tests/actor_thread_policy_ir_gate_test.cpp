#include "oreslang/backend/LlvmCodegen.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
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

static void must_reject_second_actor_after_valid_sibling() {
    oreslang::Program program;
    oreslang::ActorDecl valid;
    valid.name = "Existing";
    valid.min_threads = 1;
    valid.max_threads = 1;
    program.actors.push_back(std::move(valid));
    oreslang::ActorDecl forged;
    forged.name = "ForgedSecond";
    forged.min_threads = 1;
    forged.max_threads = 3;
    program.actors.push_back(std::move(forged));
    try {
        (void)oreslang::emit_verified_llvm_ir(program, false, false);
    } catch (const std::runtime_error& rejected) {
        if (std::string(rejected.what()).find("unsupported physical parallelism")
                != std::string::npos) return;
        std::cerr << "FAIL: wrong second-actor forgery diagnostic\n";
        std::exit(EXIT_FAILURE);
    }
    std::cerr << "FAIL: emitted IR containing a forged second actor\n";
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
    // A malformed AST can bypass source-parser range/underscore checks.
    // Every nondefault policy remains denied at final native IR emission.
    must_reject(1, 2);
    must_reject(2, 2);
    must_reject(3, 3);
    must_reject(3, 2);
    must_reject(-1, 1);
    must_reject(0, 0);
    must_reject(1, 4);
    must_reject(1, std::numeric_limits<int>::max());
    must_reject(std::numeric_limits<int>::min(), 1);
    must_reject(1, 2, true, false);
    must_reject(1, 2, false, true);
    must_reject_second_actor_after_valid_sibling();
    std::cout << "PASS guarded LLVM source actor IR admission\n";
}
