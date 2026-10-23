// Trusted-host ORC entrypoint and fail-closed admission regression.
//
// This directly exercises the production IR runner. It does not establish
// Oreslang source-level parity or provide an untrusted JIT sandbox.
#include "oreslang/backend/OrcJitRunner.hpp"

#include <llvm/Config/llvm-config.h>

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


#if LLVM_VERSION_MAJOR >= 15
constexpr const char* alias_ir =
    "@guest_alias = alias i32 (), ptr @main\n"
    "define i32 @main() { ret i32 42 }\n";
constexpr const char* global_ctors_ir =
    "@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] "
    "[{ i32, ptr, ptr } { i32 65535, ptr @init, ptr null }]\n"
    "define void @init() { ret void }\n"
    "define i32 @main() { ret i32 42 }\n";
constexpr const char* global_dtors_ir =
    "@llvm.global_dtors = appending global [1 x { i32, ptr, ptr }] "
    "[{ i32, ptr, ptr } { i32 65535, ptr @fini, ptr null }]\n"
    "define void @fini() { ret void }\n"
    "define i32 @main() { ret i32 42 }\n";
constexpr const char* ifunc_ir =
    "@choose = ifunc i32 (), ptr @resolver\n"
    "define ptr @resolver() { ret ptr @main }\n"
    "define i32 @main() { ret i32 42 }\n";
#else
constexpr const char* alias_ir =
    "@guest_alias = alias i32 (), i32 ()* @main\n"
    "define i32 @main() { ret i32 42 }\n";
constexpr const char* global_ctors_ir =
    "@llvm.global_ctors = appending global [1 x { i32, void ()*, i8* }] "
    "[{ i32, void ()*, i8* } { i32 65535, void ()* @init, i8* null }]\n"
    "define void @init() { ret void }\n"
    "define i32 @main() { ret i32 42 }\n";
constexpr const char* global_dtors_ir =
    "@llvm.global_dtors = appending global [1 x { i32, void ()*, i8* }] "
    "[{ i32, void ()*, i8* } { i32 65535, void ()* @fini, i8* null }]\n"
    "define void @fini() { ret void }\n"
    "define i32 @main() { ret i32 42 }\n";
#endif

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
    {
        "global_constructor", global_ctors_ir, false, 0,
        "global constructors/destructors"
    },
    {
        "global_destructor", global_dtors_ir, false, 0,
        "global constructors/destructors"
    },
    {
        "global_alias", alias_ir, false, 0, "aliases and indirect functions"
    },
#if LLVM_VERSION_MAJOR >= 15
    {
        "global_ifunc", ifunc_ir, false, 0, "aliases and indirect functions"
    },
#endif
};

} // namespace

int main() {
    int accepted = 0;
    int rejected = 0;
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
            ++accepted;
        } catch (const std::exception& error) {
            if (example.accepted || !example.rejection ||
                    std::string(error.what()).find(example.rejection) == std::string::npos) {
                std::cerr << example.name << ": unexpected ORC failure: "
                          << error.what() << "\n";
                return 1;
            }
            ++rejected;
        }
    }
    std::cout << "ORC host entrypoint: " << accepted << " executions + "
              << rejected << " rejection cases PASS\n";
    return 0;
}
