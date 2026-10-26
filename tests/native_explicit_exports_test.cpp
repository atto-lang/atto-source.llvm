#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/ModuleLoader.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace oreslang;

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <class F>
void rejects_code(F&& operation, std::string_view code) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::runtime_error& error) {
        rejected = true;
        if (std::string_view(error.what()).find(code) == std::string_view::npos) {
            throw std::runtime_error(
                "expected diagnostic " + std::string(code) + ", got: " + error.what());
        }
    }
    if (!rejected)
        throw std::runtime_error("expected rejection " + std::string(code));
}

Program parse(std::string_view source) {
    return parse_unresolved_program(source);
}

const SourceExportBinding& one_binding(const Program& program) {
    check(program.source_exports.size() == 1, "expected exactly one source export binding");
    return program.source_exports.front();
}

bool has_binding(
        const Program& program,
        std::string_view local,
        std::string_view external,
        SourceExportKind kind,
        SourceExportScope scope) {
    return std::any_of(program.source_exports.begin(), program.source_exports.end(),
        [&](const SourceExportBinding& binding) {
            return binding.local_name == local &&
                   binding.external_name == external &&
                   binding.local_kind == kind &&
                   binding.scope == scope;
        });
}

void syntax_matrix() {
    // Positive grammar cases are paired with representable native declarations.
    check(has_binding(parse(R"(
        export X;
        define type X = int;
    )"), "X", "X", SourceExportKind::Type, SourceExportScope::Public),
        "untyped export did not resolve");

    check(has_binding(parse(R"(
        export[pkg] actor Counter;
        define actor Counter as
          let value: int = 0;
          pub fnc on_message(int message): int { return message; }
        end
    )"), "Counter", "Counter", SourceExportKind::Actor, SourceExportScope::Package),
        "package actor export did not resolve");

    {
        auto program = parse(R"(
            export(package) class {X, Y as Z,};
            define class X as
            end
            define class Y as
            end
        )");
        check(has_binding(program, "X", "X", SourceExportKind::Class, SourceExportScope::Package),
              "group class X missing");
        check(has_binding(program, "Y", "Z", SourceExportKind::Class, SourceExportScope::Package),
              "group alias Z missing");
    }

    rejects_code([&] {
        (void)parse(R"(
            export {X, Y as Z, #ready,};
            define class X as
            end
            define class Y as
            end
        )");
    }, "SYMBOL_DECLARATION_NOT_LOWERED");

    rejects_code([&] {
        (void)parse("export symbol {#ready, #failed,};");
    }, "SYMBOL_DECLARATION_NOT_LOWERED");

    check(has_binding(parse(R"(
        export class {X};
        define class X as
        end
    )"), "X", "X", SourceExportKind::Class, SourceExportScope::Public),
        "singleton group failed");

    rejects_code([&] { (void)parse("export symbol #ready as #ready;"); },
                 "SYMBOL_DECLARATION_NOT_LOWERED");

    rejects_code([&] { (void)parse("export X"); }, "EXPORT_SEMICOLON_REQUIRED");
    rejects_code([&] { (void)parse("export {};"); }, "EXPORT_EMPTY_GROUP");
    rejects_code([&] { (void)parse("export class {};"); }, "EXPORT_EMPTY_GROUP");
    rejects_code([&] { (void)parse("export actor {,X};"); }, "EXPORT_MALFORMED_LIST");
    rejects_code([&] { (void)parse("export actor {X,,Y};"); }, "EXPORT_MALFORMED_LIST");
    rejects_code([&] { (void)parse("export actor {X Y};"); }, "EXPORT_MALFORMED_LIST");
    rejects_code([&] { (void)parse("export actor {{X}};"); }, "EXPORT_NESTED_GROUP");
    rejects_code([&] { (void)parse("export *;"); }, "EXPORT_WILDCARD_UNSUPPORTED");
    rejects_code([&] { (void)parse("export * from \"./dep\";"); }, "EXPORT_REEXPORT_UNSUPPORTED");
    rejects_code([&] { (void)parse("export from \"./dep\" {X};"); }, "EXPORT_REEXPORT_UNSUPPORTED");
    rejects_code([&] { (void)parse("export dep.X;"); }, "EXPORT_QUALIFIED_TARGET_UNSUPPORTED");
    rejects_code([&] { (void)parse("export dep::X;"); }, "EXPORT_QUALIFIED_TARGET_UNSUPPORTED");
    rejects_code([&] { (void)parse("export Foo<int>;"); }, "EXPORT_GENERIC_TARGET_UNSUPPORTED");
    rejects_code([&] { (void)parse("export foo(int);"); }, "EXPORT_CALLABLE_SIGNATURE_UNSUPPORTED");
    rejects_code([&] { (void)parse("export {class X, actor Y};"); }, "EXPORT_KIND_PREFIX_IN_GROUP");
    rejects_code([&] { (void)parse("export actor {actor X};"); }, "EXPORT_KIND_PREFIX_IN_GROUP");
    rejects_code([&] { (void)parse("export symbol #ready as Ready;"); },
                 "EXPORT_SYMBOL_BINDING_ALIAS_UNSUPPORTED");
    rejects_code([&] { (void)parse("export symbol #ready as #ok;"); },
                 "EXPORT_SYMBOL_IDENTITY_MISMATCH");
    rejects_code([&] { (void)parse("export[pkg][file] X;"); }, "EXPORT_MULTIPLE_SCOPES");
    rejects_code([&] { (void)parse("export[project] X;"); }, "EXPORT_UNKNOWN_SCOPE");
    rejects_code([&] { (void)parse("export symbol ready as #ready;"); },
                 "EXPORT_SYMBOL_IDENTITY_TARGET_REQUIRES_SYMBOL_SOURCE");
    rejects_code([&] { (void)parse("export string ready as 'ready';"); },
                 "EXPORT_UNKNOWN_DECLARATION_KIND");
    rejects_code([&] { (void)parse("export const ready as 'ready';"); },
                 "EXPORT_LITERAL_ALIAS_UNSUPPORTED");
    rejects_code([&] { (void)parse("export class Foo as \"foo\";"); },
                 "EXPORT_LITERAL_ALIAS_UNSUPPORTED");
}

void semantic_matrix() {
    check(has_binding(parse(R"(
        export class X;
        define class X as
        end
    )"), "X", "X", SourceExportKind::Class, SourceExportScope::Public),
        "forward export failed");

    check(has_binding(parse(R"(
        define class X as
        end
        export class X;
    )"), "X", "X", SourceExportKind::Class, SourceExportScope::Public),
        "post-declaration export failed");

    check(has_binding(parse(R"(
        export class X;
        import class Dep from "./dep";
        define class X as
        end
    )"), "X", "X", SourceExportKind::Class, SourceExportScope::Public),
        "export incorrectly closed import prelude");

    rejects_code([&] {
        (void)parse(R"(
            define class X as
            end
            import class Dep from "./dep";
        )");
    }, "EXPORT_IMPORT_PRELUDE");

    {
        auto program = parse(R"(
            pub define class X as
            end
        )");
        const auto& binding = one_binding(program);
        check(binding.local_name == "X", "implicit pub local name mismatch");
        check(binding.origin == SourceExportOrigin::ImplicitPub, "pub did not produce implicit source binding");
    }

    rejects_code([&] {
        (void)parse(R"(
            pub define class X as
            end
            export[pkg] class X;
        )");
    }, "EXPORT_SCOPE_CONFLICT");

    rejects_code([&] {
        (void)parse(R"(
            define class X as
            end
            export actor X;
        )");
    }, "EXPORT_KIND_MISMATCH");

    rejects_code([&] { (void)parse("export class Missing;"); },
                 "EXPORT_UNKNOWN_TARGET");

    rejects_code([&] {
        (void)parse(R"(
            export X;
            import class X from "./dep";
            fnc main(): void { return; }
        )");
    }, "EXPORT_IMPORTED_NAME");

    {
        auto program = parse(R"(
            export class Internal as Public;
            define class Internal as
            end
        )");
        const auto& binding = one_binding(program);
        check(binding.local_name == "Internal" && binding.external_name == "Public",
              "alias binding mismatch");
    }

    {
        auto program = parse(R"(
            pub define class Internal as
            end
            export class Internal as Public;
        )");
        check(program.source_exports.size() == 2, "pub + alias should expose two source names");
        check(has_binding(program, "Internal", "Internal", SourceExportKind::Class, SourceExportScope::Public),
              "implicit original binding missing");
        check(has_binding(program, "Internal", "Public", SourceExportKind::Class, SourceExportScope::Public),
              "explicit alias binding missing");
    }

    rejects_code([&] {
        (void)parse(R"(
            export class A as Public;
            export class B as Public;
            define class A as
            end
            define class B as
            end
        )");
    }, "EXPORT_ALIAS_COLLISION");

    {
        auto program = parse(R"(
            export class X;
            export class X;
            define class X as
            end
        )");
        check(program.source_exports.size() == 1, "duplicate identical export should be idempotent");
    }

    {
        auto program = parse(R"(
            export[pkg] type Hidden;
            define type Hidden = int;
            export[pkg] fnc id;
            fnc id(Hidden value): Hidden { return value; }
        )");
        check(has_binding(program, "Hidden", "Hidden", SourceExportKind::Type, SourceExportScope::Package),
              "package type export missing");
        check(has_binding(program, "id", "id", SourceExportKind::Fnc, SourceExportScope::Package),
              "package API using package type should be admitted");
    }

    rejects_code([&] {
        (void)parse(R"(
            export[file] type Hidden;
            define type Hidden = int;
            export[pkg] fnc id;
            fnc id(Hidden value): Hidden { return value; }
        )");
    }, "EXPORT_API_LEAK");

    rejects_code([&] {
        (void)parse(R"(
            define type Hidden = int;
            export fnc id;
            fnc id(Hidden value): Hidden { return value; }
        )");
    }, "EXPORT_API_LEAK");

    {
        auto program = parse(R"(
            export Count;
            define type Count = int;
        )");
        check(has_binding(program, "Count", "Count", SourceExportKind::Type, SourceExportScope::Public),
              "untyped single local symbol failed");
    }

    {
        auto program = parse(R"(
            export[file] type Local;
            define type Local = int;
        )");
        check(has_binding(program, "Local", "Local", SourceExportKind::Type, SourceExportScope::File),
              "file export scope lost");
    }

    rejects_code([&] {
        (void)parse(R"(
            export class A as Shared;
            export type T as Shared;
            define class A as
            end
            define type T = int;
        )");
    }, "EXPORT_ALIAS_COLLISION");

    {
        auto program = parse(R"(
            export fnc work;
            fnc work(): int { return 1; }
            fnc work(int value): int { return value; }
        )");
        check(program.source_exports.size() == 1, "overload family should create one source binding");
        check(program.source_exports.front().local_kind == SourceExportKind::Fnc,
              "fnc family kind lost");
    }

    rejects_code([&] {
        (void)parse(R"(
            export fnc work;
            pub fnc work(): int { return 1; }
            fnc work(int value): int { return value; }
        )");
    }, "OVERLOAD_VISIBILITY_CONFLICT");

    {
        auto program = parse(R"(
            export module Service;
            define module Service as
              export fnc value;
              fnc value(): int { return 1; }
            end
        )");
        check(has_binding(program, "Service", "Service", SourceExportKind::Module, SourceExportScope::Public),
              "root module export missing");
        check(has_binding(program, "value", "value", SourceExportKind::Fnc, SourceExportScope::Public),
              "module-local function export missing");
        const auto module_binding = std::find_if(
            program.source_exports.begin(), program.source_exports.end(),
            [](const SourceExportBinding& binding) { return binding.local_name == "value"; });
        check(module_binding != program.source_exports.end() &&
              module_binding->owner_namespace_id.find("::Service") != std::string::npos,
              "module-local export owner identity lost");
    }
}

void module_loader_uses_source_bindings_not_native_linkage() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("oreslang-explicit-export-loader-v1-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    struct Cleanup {
        fs::path root;
        ~Cleanup() { std::error_code error; fs::remove_all(root, error); }
    } cleanup{root};

    const fs::path library = root / "lib.ores";
    const fs::path main = root / "main.ores";
    const std::string lib_source = R"(
        define module Lib as
          export fnc answer as public_answer;
          fnc answer(): int { return 42; }
        end
    )";
    const std::string main_source = R"(
        import module Lib from "./lib";
        pub fnc main(): int { return Lib.public_answer(); }
    )";
    { std::ofstream out(library); out << lib_source; }
    { std::ofstream out(main); out << main_source; }

    Program linked = load_native_program(
        main.string(), main_source, SourceIdentity{}, false, false, true);
    const auto imported = std::find_if(
        linked.functions.begin(), linked.functions.end(),
        [](const Function& fn) { return fn.name == "__ores_module_1.answer"; });
    check(imported != linked.functions.end(), "source-export alias did not link to local function");
    check(!imported->exported, "source export alias mutated native exported linkage flag");
    check(imported->native_internal, "imported source-only export should retain internal native linkage");

    const std::string pkg_lib = R"(
        define module Lib as
          export[pkg] fnc answer;
          fnc answer(): int { return 42; }
        end
    )";
    { std::ofstream out(library); out << pkg_lib; }
    rejects_code([&] {
        (void)load_native_program(
            main.string(),
            R"(import module Lib from "./lib"; pub fnc main():int{return Lib.answer();})",
            SourceIdentity{}, false, false, true);
    }, "private or unknown");
}

void linkage_separation() {
    auto program = parse(R"(
        export fnc api;
        fnc api(): int { return 7; }
    )");
    check(program.functions.size() == 1, "expected one function");
    check(!program.functions.front().exported,
          "explicit source export must not mutate native linkage exported flag");
    check(has_binding(program, "api", "api", SourceExportKind::Fnc, SourceExportScope::Public),
          "explicit source function binding missing");

    auto pub_program = parse(R"(
        pub fnc api(): int { return 7; }
    )");
    check(pub_program.functions.front().exported,
          "legacy pub/native linkage behavior unexpectedly changed");
    check(pub_program.source_exports.size() == 1 &&
          pub_program.source_exports.front().origin == SourceExportOrigin::ImplicitPub,
          "inline pub did not create implicit source export binding");
}
} // namespace

int main() {
    try {
        syntax_matrix();
        semantic_matrix();
        linkage_separation();
        module_loader_uses_source_bindings_not_native_linkage();
        std::cout << "PASS native explicit export parser/binding contract\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL native explicit export parser/binding contract: "
                  << error.what() << '\n';
        return 1;
    }
}
