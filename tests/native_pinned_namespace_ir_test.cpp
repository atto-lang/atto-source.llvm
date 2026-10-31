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
    input.entry_source_unit_id = "app/src/main.ores";
    input.pinned_aliases = {
        {"app@1", {{"foo", "foo@1"}}}
    };

    // Common-source #189 contract shape: foo contributes ten physical source
    // units to three package-local namespaces. Folder placement is irrelevant.
    // Namespace a deliberately forms an acyclic provider call chain across
    // three files: aggregate/1 -> double/1 -> add/2.
    // Source bytes and source-unit paths are aligned with the immutable
    // 12-unit common-source #197 corpus (contract Git blob 34b2f245000bc5965371e0b03f64ea8af0e5d78f).
    input.sources = {
        {"foo/src/compute/math.ores", "foo@1",
            "namespace a;\npub fnc add(int a, int b): int { return a + b; }\n"},
        {"foo/src/compute/stats.ores", "foo@1",
            "namespace a;\npub fnc double(int value): int { return add(value, value); }\n"},
        {"foo/src/compute/aggregation.ores", "foo@1",
            "namespace a;\npub fnc aggregate(int value): int { return double(value) + 1; }\n"},
        {"foo/src/data/models.ores", "foo@1",
            "namespace b;\npub fnc model_code(): int { return 1; }\n"},
        {"foo/src/data/queries.ores", "foo@1",
            "namespace b;\npub fnc query_code(): int { return 2; }\n"},
        {"foo/src/data/storage.ores", "foo@1",
            "namespace b;\npub fnc storage_code(): int { return 3; }\n"},
        {"foo/src/data/validation.ores", "foo@1",
            "namespace b;\npub fnc validation_code(): int { return 4; }\n"},
        {"foo/src/api/routes.ores", "foo@1",
            "namespace c;\npub fnc route_ok(int code): bool { return code eq 43; }\n"},
        {"foo/src/api/handlers.ores", "foo@1",
            "namespace c;\npub fnc handler_ok(int code): bool { return route_ok(code); }\n"},
        {"foo/src/api/responses.ores", "foo@1",
            "namespace c;\npub fnc response_delta(): int { return handler_ok(43) ? 0 : 1; }\n"},
        {"other/src/collision.ores", "other@1",
            "namespace a;\npub fnc add(int a, int b): int { return 99; }\n"},
        {"app/src/main.ores", "app@1",
            "import ns a from \"pkg:foo\";\nimport ns c from \"pkg:foo\";\npub fnc main(): int { return a.aggregate(21) + c.response_delta(); }\n"}
    };
    return input;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const bool all_providers = argc == 2 &&
            std::string(argv[1]) == "--all-providers";
        if (argc > 2 || (argc == 2 && !all_providers))
            throw std::invalid_argument("unknown namespace fixture mode");
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

        // Isolate the package fallback invariant: both foo@1 and other@1
        // contain namespace a. Without the second c import, a missing
        // a.aggregate in other@1 must still reject; an accidental fallback
        // into foo@1 would otherwise return a misleading successful 43.
        {
            auto candidate = make_program();
            candidate.pinned_aliases["app@1"]["foo"] = "other@1";
            source(candidate, "app/src/main.ores").source =
                "import ns a from \"pkg:foo\"; "
                "pub fnc main(): int { return a.aggregate(21); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "unknown namespace member or wrong arity");
        }

        // Provider calls are same-package, same-namespace, exact-arity only.
        {
            auto candidate = make_program();
            source(candidate, "foo/src/compute/aggregation.ores").source =
                "namespace a; pub fnc aggregate(int value): int { return missing(value); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "unknown namespace member or wrong arity");
        }
        {
            auto candidate = make_program();
            source(candidate, "foo/src/compute/stats.ores").source =
                "namespace a; pub fnc double(int value): int { return add(value); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "unknown namespace member or wrong arity");
        }

        // The pre-graph signature pass validates provider scalar argument
        // types and keeps named/generic provider calls outside this ABI slice.
        {
            auto candidate = make_program();
            source(candidate, "foo/src/api/handlers.ores").source =
                "namespace c; pub fnc handler_ok(int code): bool { return route_ok(true); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "provider argument requires int, got bool");
        }
        {
            auto candidate = make_program();
            source(candidate, "foo/src/api/handlers.ores").source =
                "namespace c; pub fnc handler_ok(int code): bool { return route_ok(code: code); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "provider calls require flat positional non-generic syntax");
        }

        // Same package is not enough: another namespace in foo@1 is still a
        // distinct identity and cannot be reached without explicit syntax.
        {
            auto candidate = make_program();
            source(candidate, "foo/src/compute/aggregation.ores").source =
                "namespace a; pub fnc aggregate(int value): int { return route_code(); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "unknown namespace member or wrong arity");
        }

        // Provider dependency cycles, including multi-file recursion, are
        // rejected before final whole-program compiler validation/codegen.
        {
            auto candidate = make_program();
            source(candidate, "foo/src/compute/math.ores").source =
                "namespace a; pub fnc add(int a, int b): int { return aggregate(a); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "cyclic namespace provider call graph");
        }

        // Private helpers are not namespace exports and cannot be reached by
        // either the entry source or another provider.
        {
            auto candidate = make_program();
            source(candidate, "foo/src/compute/aggregation.ores").source =
                "namespace a; "
                "fnc hidden(int value): int { return value; } "
                "pub fnc aggregate(int value): int { return hidden(value); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "unknown namespace member or wrong arity");
        }
        {
            auto candidate = make_program();
            source(candidate, "app/src/main.ores").source =
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
            source(candidate, "app/src/main.ores").source =
                "import ns a from \"pkg:foo\"; "
                "pub fnc main(): int { return a.add(true, 1); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "requires int, got bool");
        }

        // Entry calls must not smuggle named arguments through the pinned
        // scalar namespace ABI. The general parser may understand named calls,
        // but this source-to-IR lowering intentionally admits positional only.
        {
            auto candidate = make_program();
            source(candidate, "app/src/main.ores").source =
                "import ns a from \"pkg:foo\"; "
                "pub fnc main(): int { return a.add(a: 20, b: 23); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "entry namespace calls require flat positional non-generic syntax");
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
            source(candidate, "app/src/main.ores").source =
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
            source(candidate, "app/src/main.ores").source =
                "import ns a from \"pkg:foo\"; "
                "import ns a from \"pkg:foo\"; "
                "pub fnc main(): int { return a.aggregate(21); }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "duplicate imported binding");
        }
        {
            auto candidate = make_program();
            source(candidate, "app/src/main.ores").source =
                "import ns a from \"foo\"; "
                "pub fnc main(): int { return 0; }\n";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "exact pkg:<alias> selector");
        }

        // Even host-supplied in-memory source IDs must be canonical logical
        // relative identities: no hidden aliases or future filesystem escape.
        // This is not a claim of filesystem confinement or manifest admission.
        for (const std::string invalid : {
                 std::string("../foo/source.ores"),
                 std::string("foo/../source.ores"),
                 std::string("foo/./source.ores"),
                 std::string("foo//source.ores"),
                 std::string("/absolute/source.ores"),
                 std::string("foo/"),
                 std::string("C:/escaped.ores"),
                 std::string("foo:bar.ores"),
                 std::string("foo") + '\\' + "other.ores"}) {
            auto candidate = make_program();
            candidate.sources.front().source_unit_id = invalid;
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "noncanonical source unit identity");
        }
        {
            auto candidate = make_program();
            candidate.entry_source_unit_id = "C:/app/src/main.ores";
            require_rejection(
                [&] { (void)oreslang::load_pinned_namespace_program(candidate); },
                "noncanonical entry source unit identity");
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
            source(candidate, "foo/src/compute/math.ores").source =
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
        const auto make_selected_program = [&] {
            auto program = make_program();
            if (all_providers) {
                source(program, "app/src/main.ores").source =
                    "import ns a from \"pkg:foo\";\n"
                    "import ns b from \"pkg:foo\";\n"
                    "import ns c from \"pkg:foo\";\n"
                    "pub fnc main(): int { return a.aggregate(21) + c.response_delta() "
                    "+ b.model_code() + b.query_code() + b.storage_code() "
                    "+ b.validation_code(); }\n";
            }
            return program;
        };
        auto first = make_selected_program();
        const std::string first_ir = oreslang::emit_verified_llvm_ir(
            oreslang::load_pinned_namespace_program(first));
        auto reordered = make_selected_program();
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
