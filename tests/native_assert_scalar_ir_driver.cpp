#include "oreslang/backend/NativeAssertScalarIR.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        std::cout << oreslang::emit_native_assert_scalar_llvm_ir_v0();
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
