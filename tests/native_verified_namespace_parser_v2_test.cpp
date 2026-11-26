#include "oreslang/frontend/Parser.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
template <class F>
void rejected(F&& test, const std::string& diagnostic) {
    try {
        test();
    } catch (const std::exception& error) {
        if (std::string(error.what()).find(diagnostic) != std::string::npos)
            return;
        throw std::runtime_error("unexpected rejection: " + std::string(error.what()));
    }
    throw std::runtime_error("unsupported namespace bypassed compiler gate");
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    constexpr std::string_view importer =
        "import namespace billing as payments from \"+/\";\n"
        "pub fnc main():int{return 1;}\n";
    constexpr std::string_view provider =
        "namespace billing;\n"
        "pub fnc answer():int{return 42;}\n";

    // Host-only parser admits syntax as unresolved AST, never as IR.
    auto client = oreslang::parse_verified_namespace_source_unresolved(importer);
    require(client.namespace_imports.size() == 1, "expected one AST import");
    require(client.namespace_imports.front().namespace_name == "billing",
            "namespace selector was lost");
    require(client.namespace_imports.front().local_name == "payments",
            "local namespace alias was lost");
    require(client.namespace_imports.front().package_selector == "+/",
            "project-root package selector was lost");

    auto source = oreslang::parse_verified_namespace_source_unresolved(provider);
    require(source.namespace_name == "billing" && source.functions.size() == 1,
            "namespace provider function was not parsed");

    // The ordinary executable path must continue to reject both kinds of
    // namespace source, even when a host-only preparatory pass accepted them.
    rejected([&] { (void)oreslang::parse_unresolved_program(importer); },
             "NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED");
    rejected([&] { (void)oreslang::parse_program(importer); },
             "NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED");
    rejected([&] { (void)oreslang::parse_program(provider); },
             "NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED");
    rejected([&] { oreslang::resolve_and_validate_program(client); },
             "NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED");
    rejected([&] { oreslang::resolve_and_validate_program(source); },
             "NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED");

    // Host parser still enforces canonical selectors and duplicate local
    // bindings, rather than treating snapshot authentication as syntax proof.
    rejected([&] {
        (void)oreslang::parse_verified_namespace_source_unresolved(
            "import namespace billing as p from \"../escape\";\n"
            "pub fnc main():int{return 1;}\n");
    }, "invalid native namespace package alias");
    rejected([&] {
        (void)oreslang::parse_verified_namespace_source_unresolved(
            "import namespace billing as p from \"+/\";\n"
            "import namespace other as p from \"+/\";\n"
            "pub fnc main():int{return 1;}\n");
    }, "duplicate imported namespace binding");

    std::cout << "PASS verified namespace host-parser/executable boundary cases=12\n";
}
