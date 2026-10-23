// Trusted-host ORC entrypoint and fail-closed admission regression.
//
// This directly exercises the production IR runner. It does not establish
// Oreslang source-level parity or provide an untrusted JIT sandbox.
#include "oreslang/backend/OrcJitRunner.hpp"

#include <exception>
#include <iostream>
#include <string>

namespace {

struct Case {
    const char* name;
    const char* llvm_ir;
    bool accepted;
    int expected_exit;
    const char* rejection;
};

constexpr Case cases[] = {
    {
        "closed_main_42",
        "define i32 @main() { ret i32 42 }\n",
        true, 42, nullptr
    },
    {
        "closed_main_zero",
        "define i32 @main() { ret i32 0 }\n",
        true, 0, nullptr
    },
    {
        "no_native_main",
        "define i64 @helper() { ret i64 42 }\n",
        false, 0, "requires native i32 main()"
    },
    {
        "wrong_main_result",
        "define i64 @main() { ret i64 42 }\n",
        false, 0, "requires native i32 main()"
    },
    {
        "wrong_main_arity",
        "define i32 @main(i64 %arg) { ret i32 0 }\n",
        false, 0, "requires native i32 main()"
    },
    {
        "wrong_main_calling_convention",
        "define fastcc i32 @main() { ret i32 0 }\n",
        false, 0, "requires native i32 main()"
    },
    {
        "unresolved_function_even_if_dead",
        "declare i32 @forbidden_host_call()\n"
        "define i32 @main() { ret i32 42 }\n",
        false, 0, "unresolved external function"
    },
    {
        "unresolved_global_even_if_dead",
        "@forbidden_host_global = external global i32\n"
        "define i32 @main() { ret i32 42 }\n",
        false, 0, "unresolved external global"
    },
    {
        "module_inline_asm",
        "module asm \"nop\"\n"
        "define i32 @main() { ret i32 42 }\n",
        false, 0, "module inline assembly"
    },
};

} // namespace

int main() {
    for (const auto& example : cases) {
        try {
            const int result = oreslang::execute_host_llvm_ir_with_orc(example.llvm_ir);
            if (!example.accepted) {
                std::cerr << example.name << ": invalid IR unexpectedly executed\n";
                return 1;
            }
            if (result != example.expected_exit) {
                std::cerr << example.name << ": returned " << result
                          << ", expected " << example.expected_exit << "\n";
                return 1;
            }
        } catch (const std::exception& error) {
            if (example.accepted || !example.rejection ||
                    std::string(error.what()).find(example.rejection) == std::string::npos) {
                std::cerr << example.name << ": unexpected ORC failure: "
                          << error.what() << "\n";
                return 1;
            }
        }
    }
    std::cout << "ORC host entrypoint: 2 execution + 7 rejection cases PASS\n";
    return 0;
}
