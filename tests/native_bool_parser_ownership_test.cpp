#include "oreslang/frontend/Parser.hpp"

#include <stdexcept>
#include <string>

using oreslang::Expression;
using oreslang::Program;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void rejected(const std::string& source, const std::string& diagnostic) {
    try {
        (void)oreslang::parse_program(source);
    } catch (const std::exception& ex) {
        if (std::string(ex.what()).find(diagnostic) != std::string::npos) return;
        throw;
    }
    throw std::runtime_error("invalid native bool source was admitted");
}

int main() {
    {
        Program program = oreslang::parse_program(R"(
            fnc combine(bool a, bool b, bool c): bool {
                return And(a, b, c);
            }
        )");
        require(program.functions.size() == 1, "missing bool function");
        const auto& fn = program.functions.front();
        require(fn.returns_bool, "bool result flag missing");
        require(fn.parameters.size() == 3, "bool parameter count mismatch");
        for (const auto& param : fn.parameters) {
            require(param.boolean, "bool parameter flag missing");
            require(param.class_name.empty(), "bool parameter became class");
            require(param.mode == oreslang::Parameter::Mode::Value,
                    "bool parameter acquired borrow mode");
        }
        require(fn.result && fn.result->kind == Expression::Kind::Call,
                "scalar And call AST missing");
        require(fn.result->name == "And" && fn.result->arguments.size() == 3,
                "scalar And call shape mismatch");
    }

    {
        Program program = oreslang::parse_program(R"(
            fnc qualified(bool a, bool b, bool c): bool {
                return BooleanOps.And(a, b, c);
            }
        )");
        const auto& call = *program.functions.front().result;
        require(call.kind == Expression::Kind::Call &&
                call.name == "BooleanOps.And" &&
                call.arguments.size() == 3,
                "qualified BooleanOps.And call shape mismatch");
    }

    {
        Program program = oreslang::parse_program(R"(
            fnc precedence(): bool {
                return true or true ^^ true and false;
            }
        )");
        const auto& result = *program.functions.front().result;
        require(result.kind == Expression::Kind::LogicalOr,
                "or must have lowest logical precedence");
        require(result.right &&
                result.right->kind == Expression::Kind::LogicalXor,
                "xor must bind more tightly than or");
        require(result.right->right &&
                result.right->right->kind == Expression::Kind::LogicalAnd,
                "and must bind more tightly than xor");
    }

    {
        Program program = oreslang::parse_program(R"(
            define module BooleanOps as
                pub fnc And(bool left, bool right): bool {
                    return left;
                }
                pub fnc probe(bool left, bool right): bool {
                    return BooleanOps.And(left, right);
                }
            end
        )");
        require(program.module == "BooleanOps",
                "qualified shadow module identity lost");
        require(program.functions.size() == 2,
                "qualified shadow program shape mismatch");
        const auto& call = *program.functions[1].result;
        require(call.kind == Expression::Kind::Call &&
                call.name == "BooleanOps.And",
                "real BooleanOps.And source call was stolen by intrinsic");
    }

    {
        Program program = oreslang::parse_program(R"(
            fnc And(bool left, bool right): bool {
                return left;
            }
            fnc probe(bool left, bool right): bool {
                return And(left, right);
            }
        )");
        require(program.functions.size() == 2, "shadow program shape mismatch");
        const auto& call = *program.functions[1].result;
        require(call.kind == Expression::Kind::Call && call.name == "And",
                "user-defined And did not remain a source call");
    }

    {
        Program program = oreslang::parse_program(R"(
            fnc leaf(bool a, bool b): bool {
                return a && b;
            }
            fnc forward(bool a, bool b): bool {
                return leaf(b: b, a: a);
            }
        )");
        const auto& call = *program.functions[1].result;
        require(call.kind == Expression::Kind::Call,
                "bool forward call missing");
        require(call.argument_names.size() == 2 &&
                call.argument_names[0] == "b" &&
                call.argument_names[1] == "a",
                "named bool call source order lost");
    }

    rejected(R"(
        fnc And(bool value): bool {
            return value;
        }
        fnc probe(bool left, bool right): bool {
            return And(left, right);
        }
    )", "unknown bool function or wrong arity 'And'");

    rejected(R"(
        fnc bad(bool a, bool b, bool c): bool {
            return Xor(a, b, c);
        }
    )", "native Xor expects exactly two scalar bool arguments");

    rejected(R"(
        fnc bad(bool a, bool b, bool c): bool {
            return BooleanOps.Xor(a, b, c);
        }
    )", "native Xor expects exactly two scalar bool arguments");

    rejected(R"(
        fnc bad(bool a, bool b): bool {
            return BooleanOps.And(left: a, right: b);
        }
    )", "native Boolean intrinsics do not accept named arguments");

    rejected(R"(
        fnc bad(bool a, bool b): bool {
            return BooleanOps.Nand(a, b);
        }
    )", "unknown native BooleanOps member 'Nand'");

    rejected(R"(
        fnc bad(bool a): bool {
            return And(a);
        }
    )", "native And/Or aggregate reducers are unsupported");

    rejected(R"(
        fnc bad(int value): bool {
            return true;
        }
    )", "native bool functions currently require only by-value bool parameters");

    rejected(R"(
        fnc bad(bool value): int {
            return 1;
        }
    )", "native bool parameters require a bool-returning pure function");

    rejected(R"(
        fnc bad(): int {
            return And(true, true);
        }
    )", "typed bool intrinsic cannot be used as a native int expression");
}
