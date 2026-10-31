#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/PinnedNamespaceLoader.hpp"

#include <algorithm>
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

PinnedNamespaceSourceInput& source(
        PinnedNamespaceProgramInput& input, const std::string& id) {
    const auto found = std::find_if(
        input.sources.begin(), input.sources.end(),
        [&](const auto& item) { return item.source_unit_id == id; });
    if (found == input.sources.end())
        throw std::runtime_error("test source not found: " + id);
    return *found;
}

PinnedNamespaceProgramInput make_program() {
    PinnedNamespaceProgramInput input;
    input.entry_source_unit_id = "app/main";
    input.pinned_aliases = {
        {"app@1", {{"foo", "foo@1"}}}
    };

    // Common-source #189 contract shape: foo contributes ten physical source
    // units to three package-local namespaces. Folder placement is irrelevant.
    // Namespace a deliberately forms an acyclic provider call chain across
    // three files: aggregate/1 -> double/1 -> add/2.
    input.sources = {
        {"foo/compute/math/add", "foo@1",
            "namespace a; pub fnc add(int a, int b): int { return a + b; }\n"},
        {"foo/compute/stats/double", "foo@1",
            "namespace a; pub fnc double(int value): int { return add(value, value); }\n"},
        {"foo/other/aggregation/aggregate", "foo@1",
            "namespace a; pub fnc aggregate(int value): int { return double(value) + 1; }\n"},

        {"foo/data/model", "foo@1",
            "namespace b; pub fnc model_code(): int { return 1; }\n"},
        {"foo/data/queries", "foo@1",
            "namespace b; pub fnc query_code(): int { return 2; }\n"},
        {"foo/data/storage", "foo@1",
            "namespace b; pub fnc storage_code(): int { return 3; }\n"},
        {"foo/data/validation", "foo@1",
            "namespace b; pub fnc validation_code(): int { return 4; }\n"},

        {"foo/api/routes", "foo@1",
            "namespace c; pub fnc route_ok(int code): bool { return code eq 43; }\n"},
        {"foo/api/handlers", "foo@1",
            "namespace c; pub fnc handler_ok(int code): bool { return route_ok(code); }\n"},
        {"foo/api/responses", "foo@1",
            "namespace c; pub fnc response_delta(): int { return handler_ok(43) ? 0 : 1; }\n"},

        // Same spelling and a colliding member in another package instance
        // must stay isolated. The app pin selects foo@1 exactly.
        {"other/colliding-a", "other@1",
            "namespace a; pub fnc add(int a, int b): int { return 99; }\n"},

        {"app/main", "app@1",
            "import ns a from \"pkg:foo\";\n"
            "import ns c from \"pkg:foo\";\n"
            "pub fnc main(): int { return a.aggregate(21) + c.response_delta(); }\n"}
    };
    return input;
}

} // namespace

int main() {
    try {
        // Missing package alias must never fall back by namespace spelling.
        {
            auto candidate = make_program();
            candidate.pinned_aliases.clear();
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "undeclared pinned package alias");
        }

        // A pin to the other same-named namespace is exact, not unioned with
        // foo@1. It also lacks namespace c; neither import may fall back.
        {
            auto candidate = make_program();
            candidate.pinned_aliases["app@1"]["foo"] = "other@1";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "no cross-package fallback");
        }

        // Provider calls are same-package, same-namespace, exact-arity only.
        {
            auto candidate = make_program();
            source(candidate, "foo/other/aggregation/aggregate").source =
                "namespace a; pub fnc aggregate(int value): int { return missing(value); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "unknown namespace member or wrong arity");
        }
        {
            auto candidate = make_program();
            source(candidate, "foo/compute/stats/double").source =
                "namespace a; pub fnc double(int value): int { return add(value); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "unknown namespace member or wrong arity");
        }

        // The pre-graph signature pass validates provider scalar argument
        // types and keeps named/generic provider calls outside this ABI slice.
        {
            auto candidate = make_program();
            source(candidate, "foo/api/handlers").source =
                "namespace c; pub fnc handler_ok(int code): bool { return route_ok(true); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "provider argument requires int, got bool");
        }
        {
            auto candidate = make_program();
            source(candidate, "foo/api/handlers").source =
                "namespace c; pub fnc handler_ok(int code): bool { return route_ok(code: code); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "provider calls require flat positional non-generic syntax");
        }

        // Same package is not enough: another namespace in foo@1 is still a
        // distinct identity and cannot be reached without explicit syntax.
        {
            auto candidate = make_program();
            source(candidate, "foo/other/aggregation/aggregate").source =
                "namespace a; pub fnc aggregate(int value): int { return route_code(); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "unknown namespace member or wrong arity");
        }

        // Provider dependency cycles, including multi-file recursion, are
        // rejected before final whole-program compiler validation/codegen.
        {
            auto candidate = make_program();
            source(candidate, "foo/compute/math/add").source =
                "namespace a; pub fnc add(int a, int b): int { return aggregate(a); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "cyclic namespace provider call graph");
        }

        // Private helpers are not namespace exports and cannot be reached by
        // either the entry source or another provider.
        {
            auto candidate = make_program();
            source(candidate, "foo/other/aggregation/aggregate").source =
                "namespace a; "
                "fnc hidden(int value): int { return value; } "
                "pub fnc aggregate(int value): int { return hidden(value); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "unknown namespace member or wrong arity");
        }
        {
            auto candidate = make_program();
            source(candidate, "app/main").source =
                "import ns a from \"pkg:foo\"; "
                "pub fnc main(): int { return a.hidden(21); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "unknown namespace member or wrong arity");
        }

        // Final ordinary scalar validation remains authoritative after graph
        // rewriting; namespace calls cannot bypass argument typing.
        {
            auto candidate = make_program();
            source(candidate, "app/main").source =
                "import ns a from \"pkg:foo\"; "
                "pub fnc main(): int { return a.add(true, 1); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "requires int, got bool");
        }

        // Duplicate public signature in one package namespace stays forbidden.
        {
            auto candidate = make_program();
            candidate.sources.push_back({
                "foo/duplicate-add", "foo@1",
                "namespace a; pub fnc add(int left, int right): int { return 0; }\n"});
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "duplicate public namespace function/arity");
        }

        // Namespace imports are lexical bindings and cannot be shadowed by an
        // entry function/parameter/local.
        {
            auto candidate = make_program();
            source(candidate, "app/main").source =
                "import ns a from \"pkg:foo\"; "
                "fnc a(): int { return 0; } "
                "pub fnc main(): int { return a.aggregate(21); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "entry function shadows namespace binding");
        }

        // Duplicate lexical imports and malformed package selectors fail in
        // the parser before graph resolution.
        {
            auto candidate = make_program();
            source(candidate, "app/main").source =
                "import ns a from \"pkg:foo\"; "
                "import ns a from \"pkg:foo\"; "
                "pub fnc main(): int { return a.aggregate(21); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "duplicate imported binding");
        }
        {
            auto candidate = make_program();
            source(candidate, "app/main").source =
                "import ns a from \"foo\"; "
                "pub fnc main(): int { return 0; }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "exact pkg:<alias> selector");
        }

        // Host-supplied identities and inventory budgets are bounded before
        // they can influence graph construction or generated symbols.
        {
            auto candidate = make_program();
            candidate.sources.front().source_unit_id = "foo/\nforged";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "bounded control-free source/package identity");
        }
        {
            auto candidate = make_program();
            candidate.sources.push_back(candidate.sources.front());
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "duplicate source unit identity");
        }
        {
            auto candidate = make_program();
            while (candidate.sources.size() <= 128) {
                const auto index = candidate.sources.size();
                candidate.sources.push_back({
                    "extra/" + std::to_string(index), "foo@1", ""});
            }
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "source inventory must contain 1..128 units");
        }
        {
            auto candidate = make_program();
            candidate.sources.front().source =
                std::string(8u * 1024u * 1024u + 1u, ' ');
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "source inventory exceeds 8 MiB");
        }

        // A provider init remains an observable once-only lifecycle hook even
        // when private. The namespace graph must reject it.
        {
            auto candidate = make_program();
            source(candidate, "foo/compute/math/add").source =
                "namespace a; "
                "fnc init(): int { return 0; } "
                "pub fnc add(int a, int b): int { return a + b; }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "init lifecycle");
        }

        // The host API is a closed inventory, not a directory scan. An extra
        // non-entry source without namespace identity is refused.
        {
            auto candidate = make_program();
            candidate.sources.push_back({
                "foo/ambient", "foo@1",
                "pub fnc ambient(): int { return 1; }\n"});
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "non-entry source must declare a package-owned namespace");
        }

        // Internal symbol assignment and emitted IR must be independent of
        // host inventory ordering.
        auto first = make_program();
        const std::string first_ir = oreslang::emit_verified_llvm_ir(
            oreslang::load_pinned_namespace_program(first));
        auto reordered = make_program();
        std::reverse(reordered.sources.begin(), reordered.sources.end());
        const std::string reordered_ir = oreslang::emit_verified_llvm_ir(
            oreslang::load_pinned_namespace_program(reordered));
        if (first_ir != reordered_ir)
            throw std::runtime_error(
                "pinned namespace lowering is not deterministic across inventory order");
        if (first_ir.find("define internal i64 @__ores_namespace_") ==
                std::string::npos)
            throw std::runtime_error(
                "namespace provider implementation did not use internal linkage");
        if (first_ir.find("define i64 @__ores_namespace_") !=
                std::string::npos)
            throw std::runtime_error(
                "namespace provider implementation leaked external linkage");

        std::cout << first_ir;
        std::cout.flush();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native_pinned_namespace_ir_test: " << error.what() << '\n';
        return 1;
    }
}
