#include "oreslang/frontend/Parser.hpp"

#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

using oreslang::SourceExportBinding;
using oreslang::SourceExportKind;
using oreslang::SourceExportScope;
using oreslang::parse_unresolved_program;

static void check(bool condition) {
    if (!condition) std::abort();
}

static void denied(std::string_view source, std::string_view diagnostic) {
    bool rejected = false;
    try {
        (void)parse_unresolved_program(source);
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find(diagnostic) != std::string_view::npos;
    }
    check(rejected);
}

static const SourceExportBinding& only_binding(const oreslang::Program& program) {
    check(program.source_export_bindings.size() == 1);
    return program.source_export_bindings.front();
}

int main() {
    {
        auto program = parse_unresolved_program(R"(
export fnc api as public_api;
fnc api(): int { return 1; }
)");
        check(program.source_exports.size() == 1);
        const auto& binding = only_binding(program);
        check(binding.local_kind == SourceExportKind::Fnc);
        check(binding.local_name == "api");
        check(binding.external_name == "public_api");
        check(binding.scope == SourceExportScope::Public);
        check(binding.origin == SourceExportBinding::Origin::ExplicitExport);
        check(binding.local_symbol_id == "$root::fnc::api");
        check(!program.functions.front().exported);
    }

    {
        auto program = parse_unresolved_program(R"(
export symbol #ready as #ready;
define symbol #ready;
)");
        check(program.source_symbols.size() == 1);
        check(program.source_symbols.front().name == "#ready");
        const auto& binding = only_binding(program);
        check(binding.local_kind == SourceExportKind::Symbol);
        check(binding.local_name == "#ready");
        check(binding.external_name == "#ready");
        check(binding.local_symbol_id == "$root::symbol::#ready");
        check(binding.origin == SourceExportBinding::Origin::ExplicitExport);
    }

    // Untyped export resolves the same nominal Symbol family.
    {
        auto program = parse_unresolved_program(R"(
export #ready;
define symbol #ready;
)");
        const auto& binding = only_binding(program);
        check(binding.local_kind == SourceExportKind::Symbol);
        check(binding.local_symbol_id == "$root::symbol::#ready");
    }

    // Both public declaration spellings publish the Symbol identity directly.
    for (const auto source : {
            "pub define symbol #ready;\n",
            "define pub symbol #ready;\n"}) {
        auto program = parse_unresolved_program(source);
        check(program.source_symbols.size() == 1);
        const auto& binding = only_binding(program);
        check(binding.local_kind == SourceExportKind::Symbol);
        check(binding.external_name == "#ready");
        check(binding.origin == SourceExportBinding::Origin::ImplicitPub);
    }

    denied(R"(
define symbol #ready;
define symbol #ready;
)", "duplicate Symbol declaration");

    denied(R"(
export symbol #missing as #missing;
define symbol #ready;
)", "EXPORT_UNKNOWN_TARGET");

    // Export metadata is transparent to the import prelude in either order.
    for (const auto source : {
            R"(
export fnc api;
import module Dep from './dep';
fnc api(): int { return 1; }
)",
            R"(
import module Dep from './dep';
export fnc api;
fnc api(): int { return 1; }
)"}) {
        auto program = parse_unresolved_program(source);
        check(program.imports.size() == 1);
        check(program.source_exports.size() == 1);
        check(only_binding(program).external_name == "api");
    }

    denied(R"(
fnc api(): int { return 1; }
import module Dep from './dep';
)", "EXPORT_IMPORT_PRELUDE");

    {
        auto program = parse_unresolved_program(R"(
export[pkg] fnc api;
fnc api(): int { return 1; }
)");
        check(only_binding(program).scope == SourceExportScope::Package);
        check(!program.functions.front().exported);
    }

    {
        auto program = parse_unresolved_program(R"(
export fnc api;
pub fnc api(): int { return 1; }
)");
        check(program.source_export_bindings.size() == 1);
        check(program.source_export_bindings.front().origin ==
              SourceExportBinding::Origin::ImplicitPub);
    }

    {
        auto program = parse_unresolved_program(R"(
export fnc internal as first;
export fnc internal as second;
fnc internal(): int { return 1; }
)");
        check(program.source_export_bindings.size() == 2);
        check(program.source_export_bindings[0].local_symbol_id ==
              program.source_export_bindings[1].local_symbol_id);
        check(program.source_export_bindings[0].external_name !=
              program.source_export_bindings[1].external_name);
    }

    // Completed export table, not source order, determines visibility leakage.
    for (const auto source : {
            R"(
export fnc api;
export type Hidden;
define type Hidden = int;
fnc api(Hidden value): int { return value; }
)",
            R"(
export type Hidden;
export fnc api;
define type Hidden = int;
fnc api(Hidden value): int { return value; }
)"}) {
        auto program = parse_unresolved_program(source);
        check(program.source_export_bindings.size() == 2);
    }

    denied(R"(
export[pkg] fnc api;
export[file] type Hidden;
define type Hidden = int;
fnc api(Hidden value): int { return value; }
)", "EXPORT_API_LEAK");

    {
        auto program = parse_unresolved_program(R"(
export[file] fnc api;
export[pkg] type Hidden;
define type Hidden = int;
fnc api(Hidden value): int { return value; }
)");
        check(program.source_export_bindings.size() == 2);
    }

    // Private class storage is not part of an exported source API signature.
    {
        auto program = parse_unresolved_program(R"(
define class Hidden as
end
export class Api;
define class Api as
  private val Hidden hidden;
end
)");
        check(program.source_export_bindings.size() == 1);
        check(program.source_export_bindings.front().local_kind ==
              SourceExportKind::Class);
        check(program.source_export_bindings.front().local_name == "Api");
    }

    // Public API cannot expose a package-only local type.
    denied(R"(
export[pkg] type Hidden;
define type Hidden = int;
export fnc api;
fnc api(Hidden value): int { return value; }
)", "EXPORT_API_LEAK");

    // Matching package visibility on both sides is admissible.
    {
        auto program = parse_unresolved_program(R"(
export[pkg] type Hidden;
define type Hidden = int;
export[pkg] fnc api;
fnc api(Hidden value): int { return value; }
)");
        check(program.source_export_bindings.size() == 2);
    }

    // Exact duplicate explicit exports are idempotent.
    {
        auto program = parse_unresolved_program(R"(
export fnc api;
export fnc api;
fnc api(): int { return 1; }
)");
        check(program.source_export_bindings.size() == 1);
    }

    // Inline public plus alias exposes both names.
    {
        auto program = parse_unresolved_program(R"(
pub fnc api(): int { return 1; }
export fnc api as PublicApi;
)");
        check(program.source_export_bindings.size() == 2);
    }

    // Same local spelling in distinct logical namespaces cannot converge on
    // one external alias.
    denied(R"(
define type Thing = int;
fnc Thing(): int { return 1; }
export type Thing as Shared;
export fnc Thing as Shared;
)", "EXPORT_ALIAS_COLLISION");

    denied("export X\nfnc keep(): int { return 1; }\n", "EXPORT_SEMICOLON_REQUIRED");
    denied("export {};\nfnc keep(): int { return 1; }\n", "EXPORT_EMPTY_GROUP");
    denied("export class {};\nfnc keep(): int { return 1; }\n", "EXPORT_EMPTY_GROUP");
    denied("export actor {,X};\nfnc keep(): int { return 1; }\n", "EXPORT_MALFORMED_LIST");
    denied("export actor {X,,Y};\nfnc keep(): int { return 1; }\n", "EXPORT_MALFORMED_LIST");
    denied("export actor {X Y};\nfnc keep(): int { return 1; }\n", "EXPORT_MALFORMED_LIST");
    denied("export actor {{X}};\nfnc keep(): int { return 1; }\n", "EXPORT_NESTED_GROUP");
    denied("export *;\nfnc keep(): int { return 1; }\n", "EXPORT_WILDCARD_UNSUPPORTED");
    denied("export * from './dep';\nfnc keep(): int { return 1; }\n", "EXPORT_REEXPORT_UNSUPPORTED");
    denied("export from './dep' {X};\nfnc keep(): int { return 1; }\n", "EXPORT_REEXPORT_UNSUPPORTED");
    denied("export dep.X;\nfnc keep(): int { return 1; }\n", "EXPORT_QUALIFIED_TARGET_UNSUPPORTED");
    denied("export dep::X;\nfnc keep(): int { return 1; }\n", "EXPORT_QUALIFIED_TARGET_UNSUPPORTED");
    denied("export Foo<int>;\nfnc keep(): int { return 1; }\n", "EXPORT_GENERIC_TARGET_UNSUPPORTED");
    denied("export foo(int);\nfnc keep(): int { return 1; }\n", "EXPORT_CALLABLE_SIGNATURE_UNSUPPORTED");
    denied("export {class X, actor Y};\nfnc keep(): int { return 1; }\n", "EXPORT_KIND_PREFIX_IN_GROUP");
    denied("export actor {actor X};\nfnc keep(): int { return 1; }\n", "EXPORT_KIND_PREFIX_IN_GROUP");
    denied("export symbol #ready as Ready;\nfnc keep(): int { return 1; }\n", "EXPORT_SYMBOL_BINDING_ALIAS_UNSUPPORTED");
    denied("export symbol #ready as #ok;\nfnc keep(): int { return 1; }\n", "EXPORT_SYMBOL_IDENTITY_MISMATCH");
    denied("export symbol ready as #ready;\nfnc keep(): int { return 1; }\n", "EXPORT_SYMBOL_IDENTITY_TARGET_REQUIRES_SYMBOL_SOURCE");
    denied("export[pkg][file] X;\nfnc keep(): int { return 1; }\n", "EXPORT_MULTIPLE_SCOPES");
    denied("export[project] X;\nfnc keep(): int { return 1; }\n", "EXPORT_UNKNOWN_SCOPE");
    denied("export string ready as 'ready';\nfnc keep(): int { return 1; }\n", "EXPORT_UNKNOWN_DECLARATION_KIND");
    denied("export const ready as 'ready';\nfnc keep(): int { return 1; }\n", "EXPORT_LITERAL_ALIAS_UNSUPPORTED");
    denied("export class Foo as \"foo\";\nfnc keep(): int { return 1; }\n", "EXPORT_LITERAL_ALIAS_UNSUPPORTED");

    {
        const auto program = parse_unresolved_program(R"(
export symbol as PublicSymbol;
fnc symbol(): int { return 1; }
)");
        check(program.source_exports.size() == 1);
        check(program.source_exports.front().kind == SourceExportKind::Untyped);
        check(program.source_exports.front().items.size() == 1);
        check(program.source_exports.front().items.front().local_name == "symbol");
        check(program.source_exports.front().items.front().external_name == "PublicSymbol");
    }

    denied(R"(
export actor Box;
define class Box as
end
)", "EXPORT_KIND_MISMATCH");

    denied(R"(
import module External from './dep';
export External;
fnc keep(): int { return 1; }
)", "EXPORT_IMPORTED_NAME");

    denied(R"(
export fnc A as Public;
export fnc B as Public;
fnc A(): int { return 1; }
fnc B(): int { return 2; }
)", "EXPORT_ALIAS_COLLISION");

    denied(R"(
export[pkg] fnc api;
pub fnc api(): int { return 1; }
)", "EXPORT_SCOPE_CONFLICT");

    denied(R"(
define class Bad as
  export fnc nope;
end
)", "EXPORT_LOCAL_SCOPE");

    return 0;
}
