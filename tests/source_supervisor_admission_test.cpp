#include "oreslang/frontend/Parser.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <iostream>

static void reject_supervisor(std::string_view source) {
    try {
        (void) oreslang::parse_program(source, false);
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        if (message.find("source supervisor on_panic requires typed Failure")
                == std::string::npos
            || message.find("compiler-lowered CONTROL turns")
                == std::string::npos) {
            throw std::runtime_error(
                "source supervisor produced unexpected native diagnostic: " + message);
        }
        return;
    }
    throw std::runtime_error("LLVM admitted source supervisor without CONTROL lowering");
}

int main() {
    reject_supervisor(R"(
        define supervisor Root as
          pub async on_panic(f: Failure): void { return; }
        end
        pub fnc main(): int { return 1; }
    )");
    reject_supervisor(R"(
        define module M as
          define supervisor Root as
            on_panic(f: Failure): void { return; }
          end
        end
    )");
    std::cout << "NATIVE_SOURCE_SUPERVISOR_FAIL_CLOSED_PASS\n";
}
