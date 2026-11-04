#include "oreslang/frontend/AttoManifestSnapshot.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
namespace fs = std::filesystem;
using oreslang::frontend::AttoManifestSnapshot;

static void write_file(const fs::path& file, const std::string& bytes) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create manifest fixture");
    output << bytes;
    output.close();
    if (!output) throw std::runtime_error("failed writing manifest fixture");
}
template<class Fn>
static void rejects(const std::string& case_name, const std::string& expected, Fn fn) {
    try { fn(); } catch (const std::exception& e) {
        if (std::string(e.what()).find(expected) != std::string::npos) return;
        throw std::runtime_error(case_name + ": unexpected diagnostic: " + e.what());
    }
    throw std::runtime_error(case_name + ": unexpectedly admitted");
}
int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() /
        ("atto-manifest-snapshot-" + std::to_string(stamp));
    struct Cleanup {
        fs::path dir;
        ~Cleanup() { std::error_code error; fs::remove_all(dir, error); }
    } cleanup{dir};
    fs::create_directories(dir);
    const fs::path marker = dir / ".atto.pkg.toml";
    const std::string valid = "schema_version = \"1\"\n[project]\nroot = \".\"\n";
    write_file(marker, valid);
    const AttoManifestSnapshot unchanged(marker);
    unchanged.recheck();
    unchanged.recheck();

    // A change after admission fails even if the replacement manifest is
    // independently valid: source-identity continuity takes precedence.
    write_file(marker, valid + "# changed\n");
    rejects("changed content", "ATTO_UNIT_MANIFEST_CHANGED", [&] { unchanged.recheck(); });

    write_file(marker, valid);
    const AttoManifestSnapshot before_inode_swap(marker);
    const fs::path replacement = dir / "replacement.toml";
    write_file(replacement, valid);
    fs::rename(replacement, marker);
    rejects("same bytes new inode", "ATTO_UNIT_MANIFEST_CHANGED",
            [&] { before_inode_swap.recheck(); });

    const AttoManifestSnapshot before_removal(marker);
    fs::remove(marker);
    rejects("missing after admission", "ATTO_UNIT_MANIFEST_CHANGED",
            [&] { before_removal.recheck(); });

    write_file(marker, valid);
    const AttoManifestSnapshot before_symlink(marker);
    fs::remove(marker);
    write_file(replacement, valid);
    fs::create_symlink(replacement, marker);
    rejects("symlink swap", "ATTO_UNIT_MANIFEST_CHANGED",
            [&] { before_symlink.recheck(); });
    rejects("symlink first admission", "symlinked AttoLang source path",
            [&] { (void)AttoManifestSnapshot(marker); });
    fs::remove(marker);

    write_file(marker, "schema_version = \"999\"\n");
    rejects("invalid schema", "schema_version",
            [&] { (void)AttoManifestSnapshot(marker); });
    write_file(marker, std::string(65537, 'x'));
    rejects("oversized manifest", "64 KiB",
            [&] { (void)AttoManifestSnapshot(marker); });
    write_file(marker, valid + std::string("\xFF", 1));
    rejects("malformed UTF-8", "valid UTF-8",
            [&] { (void)AttoManifestSnapshot(marker); });

    std::cout << "ATTO_MANIFEST_SNAPSHOT_HOST_ONLY_OK checks=9\n";
}
