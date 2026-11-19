// Real frontend IR integrity check: no LLVM runtime or mock resolver.
#include "oreslang/frontend/MethodResolver.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace oreslang;

void expect_rejection(Program program, const std::string& fragment) {
    try {
        resolve_native_methods(program);
    } catch (const std::runtime_error& err) {
        if (std::string(err.what()).find(fragment) != std::string::npos) return;
        throw std::runtime_error("incorrect rejection diagnostic: " +
                                 std::string(err.what()) + " (expected " + fragment + ")");
    }
    throw std::runtime_error("accepted invalid compiler-owned method IR: " + fragment);
}

Program class_program(const std::string& name) {
    Program p;
    ClassDecl cls;
    cls.name = name;
    p.classes.push_back(std::move(cls));
    return p;
}
}

int main() {
    try {
        {
            Program p = class_program("Counter");
            ClassDecl duplicate;
            duplicate.name = "Counter";
            p.classes.push_back(std::move(duplicate));
            expect_rejection(std::move(p), "duplicate compiler-owned nominal class identity");
        }
        {
            Program p = class_program("Counter");
            p.classes.front().methods.push_back(ClassMethod{"read", 0, true, false});
            p.classes.front().methods.push_back(ClassMethod{"read", 0, true, true});
            // No functions or calls: the duplicate must still be rejected.
            expect_rejection(std::move(p), "duplicate compiler-owned native method slot");
        }
        {
            Program p = class_program("Counter");
            p.classes.front().methods.push_back(ClassMethod{"read", 0, true, false});
            p.classes.front().methods.push_back(ClassMethod{"read", 1, true, false});
            // Distinct arities form distinct compiler-owned static slots.
            resolve_native_methods(p);
        }
        {
            Program p = class_program("Counter");
            Function malformed;
            malformed.name = "Counter.read";
            malformed.method_owner = "Counter";
            // Missing compiler-owned self parameter may not gain privilege.
            p.functions.push_back(std::move(malformed));
            expect_rejection(std::move(p), "invalid compiler-owned method receiver");
        }
        {
            Program p = class_program("Counter");
            Function malformed;
            malformed.name = "Counter.read";
            malformed.method_owner = "Counter";
            Parameter self;
            self.name = "self";
            self.class_name = "OtherClass";
            malformed.parameters.push_back(std::move(self));
            p.functions.push_back(std::move(malformed));
            expect_rejection(std::move(p), "invalid compiler-owned method receiver");
        }
        std::cout << "PASS: compiler frontend rejects duplicate classes, "
                     "unreferenced duplicate slots, and forged method receivers; "
                     "retains valid same-name distinct-arity slots\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
