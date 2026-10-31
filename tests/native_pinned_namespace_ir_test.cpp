#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/PinnedNamespaceLoader.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using oreslang::PinnedNamespaceProgramInput;
using oreslang::PinnedNamespaceSourceInput;

namespace {

template <typename F>
void require_rejection(F&& operation, const std::string& fragment) {
    try {
        operation();
    } catch (const std::exception& error) {
        if (std::string(error.what()).find(fragment) == std::string::npos)
            throw std::runtime_error(
                "unexpected rejection: expected '" + fragment + "', saw '" +
                std::string(error.what()) + "'");
        return;
    }
    throw std::runtime_error("expected pinned namespace lowering rejection");
}

PinnedNamespaceProgramInput make_program() {
    PinnedNamespaceProgramInput input;
    input.entry_source_unit_id = "app/main";
    input.pinned_aliases = {
        {"app@1", {{"foo", "foo@1"}}}
    };

    // Common-source #189 contract shape: foo contributes ten physical source
    // units to three package-local namespaces. Folder placement is irrelevant.
    input.sources = {
        {"foo/compute/math/add", "foo@1",
            "namespace a; pub fnc add(int a, int b): int { return a + b; }\n"},
        {"foo/compute/stats/double", "foo@1",
            "namespace a; pub fnc double(int value): int { return value * 2; }\n"},
        {"foo/other/aggregation/aggregate", "foo@1",
            "namespace a; pub fnc aggregate(int value): int { return value + 1; }\n"},

        {"foo/data/model", "foo@1",
            "namespace b; pub fnc model_code(): int { return 1; }\n"},
        {"foo/data/queries", "foo@1",
            "namespace b; pub fnc query_code(): int { return 2; }\n"},
        {"foo/data/storage", "foo@1",
            "namespace b; pub fnc storage_code(): int { return 3; }\n"},
        {"foo/data/validation", "foo@1",
            "namespace b; pub fnc validation_code(): int { return 4; }\n"},

        {"foo/api/routes", "foo@1",
            "namespace c; pub fnc route_code(): int { return 5; }\n"},
        {"foo/api/handlers", "foo@1",
            "namespace c; pub fnc handler_code(): int { return 6; }\n"},
        {"foo/api/responses", "foo@1",
            "namespace c; pub fnc response_code(): int { return 7; }\n"},

        // Same spelling and even a colliding member in a different package
        // instance must stay isolated. The app pin selects foo@1 exactly.
        {"other/colliding-a", "other@1",
            "namespace a; pub fnc add(int a, int b): int { return 99; }\n"},

        {"app/main", "app@1",
            "import ns a from \"pkg:foo\";\n"
            "pub fnc main(): int {\n"
            "  return a.aggregate(a.double(a.add(20, 1)));\n"
            "}\n"}
    };
    return input;
}

} // namespace

int main() {
    try {
        // Missing package alias must never fall back by namespace spelling.
        {
            auto missing_pin = make_program();
            missing_pin.pinned_aliases.clear();
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(missing_pin); },
                "undeclared pinned package alias");
        }

        // A pin to the other same-named namespace is exact, not unioned with
        // foo@1. It lacks aggregate/double and therefore cannot satisfy app.
        {
            auto wrong_pin = make_program();
            wrong_pin.pinned_aliases["app@1"]["foo"] = "other@1";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(wrong_pin); },
                "unknown namespace member or wrong arity");
        }

        auto linked = oreslang::load_pinned_namespace_program(make_program());
        std::cout << oreslang::emit_verified_llvm_ir(linked);
        std::cout.flush();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native_pinned_namespace_ir_test: " << error.what() << '\n';
        return 1;
    }
}
