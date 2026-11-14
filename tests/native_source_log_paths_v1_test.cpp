#include "oreslang/backend/NativeSourceLogPathsV1.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    namespace fs = std::filesystem;
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = fs::temp_directory_path() / ("atto-log-path-check-" + suffix);
    assert(fs::create_directory(root));
    struct Cleanup {
        fs::path root;
        ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); }
    } cleanup{root};
    const auto source = root / "main.atto";
    const auto manifest = root / "source-ids.json";
    { std::ofstream out(source); out << "pub fnc main():int{return 42;}"; }
    { std::ofstream out(manifest); out << "{}"; }
    const auto verify = [&](const fs::path& a, const fs::path& b) {
        oreslang::telemetry::require_exact_source_log_paths(root, a, b);
    };
    const auto deny = [&](const fs::path& a, const fs::path& b) {
        try { verify(a, b); return false; }
        catch (const fs::filesystem_error&) { return true; }
        catch (const std::runtime_error&) { return true; }
    };
    verify("main.atto", "source-ids.json");
    assert(deny(root / "main.atto", "source-ids.json"));
    assert(deny("./main.atto", "source-ids.json"));
    assert(deny("../main.atto", "source-ids.json"));
    assert(deny("main.atto", "../source-ids.json"));
    assert(deny("main.atto", "main.atto"));
    assert(deny("main.atto", "missing.json"));
    assert(deny("main.atto", "."));
    fs::create_symlink(source, root / "linked.atto");
    assert(deny("linked.atto", "source-ids.json"));
    fs::create_symlink(manifest, root / "linked.json");
    assert(deny("main.atto", "linked.json"));
    fs::create_hard_link(source, root / "hardlinked-ids.json");
    assert(deny("main.atto", "hardlinked-ids.json"));
    fs::create_directory(root / "directory.json");
    assert(deny("main.atto", "directory.json"));
    std::cout << "LLVM_SOURCE_LOG_PATHS_HOST_PASS:11" << std::endl;
}
