#include "oreslang/frontend/VerifiedNamespaceGraphV2.hpp"
#include <openssl/sha.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace oreslang;
using namespace oreslang::frontend;

static std::string sha(std::string_view bytes) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto c : digest) { result += hex[c >> 4]; result += hex[c & 15]; }
    return result;
}
static void write(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("v2 fixture could not be written");
    stream << contents;
}
template <class F> static void denied(F&& fn, const std::string& message) {
    try { fn(); }
    catch (const std::invalid_argument& e) {
        if (std::string(e.what()).find(message) != std::string::npos) return;
        throw std::runtime_error("v2 unexpectedly rejected with: " + std::string(e.what()));
    }
    throw std::runtime_error("v2 failed to reject: " + message);
}
static Program host_parse(std::string_view bytes) {
    Program program;
    if (bytes == "provider") {
        program.namespace_name = "billing";
        Function fn;
        fn.name = "answer";
        fn.exported = true;
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Integer;
        fn.result->resolved_type = ScalarType::I64;
        program.functions.push_back(std::move(fn));
    } else if (bytes == "caller") {
        program.namespace_imports.push_back({"billing", "payments", "+/", 0});
    } else if (bytes == "external") {
        program.namespace_imports.push_back({"billing", "payments", "other", 0});
    } else if (bytes == "duplicate-alias") {
        program.namespace_imports.push_back({"billing", "payments", "+/", 0});
        program.namespace_imports.push_back({"billing", "payments", "+/", 0});
    } else if (bytes == "legacy") {
        program.imports.emplace_back();
    } else if (bytes == "private") {
        program.namespace_name = "billing";
        Function fn;
        fn.name = "answer";
        fn.exported = false;
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Integer;
        fn.result->resolved_type = ScalarType::I64;
        program.functions.push_back(std::move(fn));
    } else {
        throw std::invalid_argument("v2 test host parser rejected unexpected bytes");
    }
    return program;
}
int main() {
    const fs::path root = fs::temp_directory_path() /
        ("verified-v2-graph-" + std::to_string(::getpid()));
    struct Cleanup { fs::path p; ~Cleanup() {std::error_code ec; fs::remove_all(p,ec);} } cleanup{root};
    const std::string manifest = "schema_version = \"2\"\n[project]\nroot = \".\"\n"
        "[packages.checkout]\nfiles = [\"src/provider.ores\", \"src/caller.ores\"]\n";
    write(root / ".ores.pkg.toml", manifest);
    write(root / "src/provider.ores", "provider");
    write(root / "src/caller.ores", "caller");
    const std::vector<std::string> files{"src/provider.ores", "src/caller.ores"};
    auto make_snapshot = [&] {
        const auto content_digest = sha(
            "src/caller.ores\n" + sha("caller") + "\n"
            "src/provider.ores\n" + sha("provider") + "\n");
        return load_verified_namespace_source_snapshot_v2(
            root, files, sha(manifest), content_digest, sha);
    };
    const auto snapshot = make_snapshot();
    int cases = 0;
    auto build = [&] {
        return link_verified_namespace_snapshots_v2({{&snapshot}}, host_parse);
    };
    const auto graph = build();
    if (graph.imports.size() != 1 || graph.namespaces.size() != 1)
        throw std::runtime_error("v2 verified namespace graph is incomplete");
    ++cases;
    const auto imported = graph.imports.begin();
    if (imported->first.second != "payments")
        throw std::runtime_error("v2 local alias is not preserved");
    ++cases;
    const auto call = resolve_package_namespace_scalar_call(
        graph, imported->first.first, "payments", "answer", {});
    if (call.function_name != "answer" || call.source_unit_id.find("src/provider.ores") == std::string::npos)
        throw std::runtime_error("v2 wrong physical source provider");
    ++cases;
    if (call.owner.package_instance_id.find(snapshot.inventory().sources_sha256())
            == std::string::npos)
        throw std::runtime_error("v2 provider owner lacks pinned inventory identity");
    ++cases;
    denied([&] {
        (void)resolve_package_namespace_scalar_call(graph, imported->first.first,
            "payments", "other", {});
    }, "not exported"); ++cases;
    denied([&] {
        (void)link_verified_namespace_snapshots_v2({{&snapshot}, {&snapshot}}, host_parse);
    }, "duplicate physical source identity"); ++cases;
    denied([&] {
        (void)link_verified_namespace_snapshots_v2({}, host_parse);
    }, "bounded verified snapshots"); ++cases;
    denied([&] {
        (void)link_verified_namespace_snapshots_v2({{nullptr}}, host_parse);
    }, "package snapshot missing"); ++cases;
    denied([&] {
        (void)link_verified_namespace_snapshots_v2({{&snapshot}},
            NamespaceParseCapturedSourceV2{});
    }, "bounded verified snapshots"); ++cases;
    // An authenticated source snapshot cannot acquire a different package's
    // alias just because a callback claims the import is syntactically valid.
    denied([&] {
        (void)link_verified_namespace_snapshots_v2({{&snapshot}},
            [](std::string_view bytes) {
                return host_parse(bytes == "caller" ? "external" : bytes);
            });
    }, "external dependency imports require verified lockfile pins"); ++cases;
    denied([&] {
        (void)link_verified_namespace_snapshots_v2({{&snapshot}},
            [](std::string_view bytes) {
                return host_parse(bytes == "caller" ? "duplicate-alias" : bytes);
            });
    }, "duplicate lexical namespace import binding"); ++cases;
    denied([&] {
        (void)link_verified_namespace_snapshots_v2({{&snapshot}},
            [](std::string_view bytes) {
                return host_parse(bytes == "caller" ? "legacy" : bytes);
            });
    }, "legacy module imports lack verified ownership"); ++cases;
    // Consume the frozen bytes, not whichever bytes a pathname now resolves to.
    write(root / "src/provider.ores", "private");
    if (build().namespaces.size() != 1)
        throw std::runtime_error("v2 mutable physical filesystem escaped frozen snapshot");
    ++cases;
    denied([&] { (void)make_snapshot(); }, "SHA-256 mismatch"); ++cases;
    std::cout << "PASS verified v2 namespace owner graph cases=" << cases << "\n";
}
