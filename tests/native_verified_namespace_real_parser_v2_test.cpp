#include "oreslang/frontend/VerifiedNamespaceGraphV2.hpp"

#include <openssl/sha.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace oreslang;
using namespace oreslang::frontend;

static std::string sha(std::string_view data) {
    unsigned char bytes[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), bytes);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
static void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot create namespace integration fixture");
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}
template<class F> static void reject(F&& f, std::string_view expected) {
    try { f(); }
    catch (const std::exception& e) {
        if (std::string(e.what()).find(expected) != std::string::npos) return;
        throw std::runtime_error(std::string("unexpected denial: ") + e.what());
    }
    throw std::runtime_error("verified namespace admission unexpectedly succeeded");
}
static void require(bool yes, const char* error) {
    if (!yes) throw std::runtime_error(error);
}

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("ores-v2-real-parser-" + std::to_string(::getpid()));
    struct Cleanup {
        fs::path dir;
        ~Cleanup() { std::error_code ec; fs::remove_all(dir, ec); }
    } cleanup{root};

    const std::string manifest = "schema_version = \"2\"\n"
        "[project]\nroot = \".\"\n"
        "[packages.checkout]\nfiles = [\"src/client.ores\", \"src/provider.ores\"]\n";
    const std::string provider = "namespace billing;\n"
        "pub fnc answer():int{return 42;}\n";
    const std::string client = "import namespace billing as payments from \"+/\";\n"
        "pub fnc main():int{return 1;}\n";
    write(root / ".ores.pkg.toml", manifest);
    write(root / "src/client.ores", client);
    write(root / "src/provider.ores", provider);
    const std::vector<std::string> files{"src/client.ores", "src/provider.ores"};

    const auto digest = [&](std::string_view caller, std::string_view callee) {
        return sha("src/client.ores\n" + sha(caller) + "\n"
                   "src/provider.ores\n" + sha(callee) + "\n");
    };
    const std::string manifest_pin = sha(manifest);
    const std::string original_source_pin = digest(client, provider);
    auto capture = [&](std::string_view pin) {
        return load_verified_namespace_source_snapshot_v2(
            root, files, manifest_pin, pin, sha);
    };
    const auto snapshot = capture(original_source_pin);
    const auto graph = link_verified_native_namespace_snapshots_v2({{&snapshot}});
    require(graph.namespaces.size() == 1, "verified provider namespace was not linked");
    require(graph.imports.size() == 1, "verified client import was not linked");
    const auto bound = graph.imports.begin();
    require(bound->first.second == "payments", "namespace local alias was dropped");
    require(bound->second.name == "billing", "namespace target spelling is wrong");
    const auto target = resolve_package_namespace_scalar_call(
        graph, bound->first.first, "payments", "answer", {});
    require(target.source_unit_id.find("src/provider.ores") != std::string::npos,
            "verified namespace call resolved the wrong source file");
    require(target.owner.package_instance_id.find(original_source_pin) != std::string::npos,
            "provider package ownership lost the source commitment");

    // The executable parser must not treat an authenticated-looking source
    // pathname or caller's constructed pins as codegen authorization.
    reject([&]{ (void)parse_program(client); }, "NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED");
    reject([&]{ (void)parse_program(provider); }, "NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED");
    reject([&]{
        (void)resolve_package_namespace_scalar_call(
            graph, bound->first.first, "payments", "private", {});
    }, "not exported");

    write(root / "src/provider.ores", "namespace billing;\n"
        "pub fnc answer():int{return 99;}\n");
    reject([&]{ (void)capture(original_source_pin); }, "SHA-256 mismatch");
    // A pinned snapshot must parse captured, original bytes even if the
    // filesystem changes after its authentication.
    const auto old_graph = link_verified_native_namespace_snapshots_v2({{&snapshot}});
    require(old_graph.namespaces.size() == 1, "host parser re-opened mutable files");

    std::cout << "PASS verified source -> production parser -> namespace graph\n";
}
