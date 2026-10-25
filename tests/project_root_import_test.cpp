#include "oreslang/frontend/ProjectRootImport.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

static void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create fixture");
    out << text;
}

static std::string read_manifest(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("manifest unreadable");
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    if (bytes.size() > 64u * 1024u)
        throw std::runtime_error("native package manifest exceeds 64 KiB");
    return bytes;
}

template <typename F>
static void must_fail(const std::string& label, const std::string& needle, F&& fn) {
    try {
        fn();
    } catch (const std::exception& error) {
        if (std::string(error.what()).find(needle) != std::string::npos) return;
        throw std::runtime_error(label + ": wrong diagnostic: " + error.what());
    }
    throw std::runtime_error(label + ": unexpectedly accepted");
}

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path temp = fs::temp_directory_path() /
        ("ores-project-root-import-" + std::to_string(stamp));
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{temp};
    fs::create_directories(temp);

    const auto resolve = [&](const fs::path& source, std::string_view raw) {
        return oreslang::resolve_project_root_import(source, raw, read_manifest);
    };

    {
        const auto dir = temp / "positive";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"app/main.ores", "");
        write_text(dir/"lib.ores", "");
        if (resolve(dir/"app/main.ores", "+/lib") != fs::canonical(dir/"lib.ores"))
            throw std::runtime_error("positive project-root resolution mismatch");
    }
    {
        const auto dir = temp / "quoted-key";
        write_text(dir/".ores.pkg.toml", "\"schema_version\" = \"1\"\n");
        write_text(dir/"main.ores", "");
        write_text(dir/"lib.ores", "");
        must_fail("quoted schema key", "schema_version",
                  [&] { (void)resolve(dir/"main.ores", "+/lib.ores"); });
    }
    {
        const auto dir = temp / "nearest";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"lib.ores", "");
        write_text(dir/"nested/.ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"nested/app/main.ores", "");
        write_text(dir/"nested/lib.ores", "");
        if (resolve(dir/"nested/app/main.ores", "+/lib") != fs::canonical(dir/"nested/lib.ores"))
            throw std::runtime_error("nearest manifest was not authoritative");
    }
    {
        const auto dir = temp / "missing";
        write_text(dir/"main.ores", "");
        must_fail("missing manifest", "nearest .ores.pkg.toml",
                  [&] { (void)resolve(dir/"main.ores", "+/lib"); });
    }
    {
        const auto dir = temp / "duplicate";
        write_text(dir/".ores.pkg.toml",
                   "schema_version = \"1\"\n\"schema_version\" = \"1\"\n");
        write_text(dir/"main.ores", "");
        write_text(dir/"lib.ores", "");
        must_fail("duplicate schema", "schema_version",
                  [&] { (void)resolve(dir/"main.ores", "+/lib"); });
    }
    {
        const auto dir = temp / "conflict";
        write_text(dir/".ores.pkg.toml", "'schema_version' = \"2\"\n");
        write_text(dir/"main.ores", "");
        write_text(dir/"lib.ores", "");
        must_fail("conflicting schema", "schema_version",
                  [&] { (void)resolve(dir/"main.ores", "+/lib"); });
    }
    {
        const auto dir = temp / "multiline";
        write_text(dir/".ores.pkg.toml",
                   "note = \"\"\"schema_version = \\\"1\\\"\"\"\"\nschema_version = \"1\"\n");
        write_text(dir/"main.ores", "");
        write_text(dir/"lib.ores", "");
        must_fail("multiline TOML", "multiline TOML",
                  [&] { (void)resolve(dir/"main.ores", "+/lib"); });
    }
    {
        const auto dir = temp / "project-root";
        write_text(dir/".ores.pkg.toml",
                   "schema_version = \"1\"\n[project]\nroot = \"src\"\n");
        write_text(dir/"main.ores", "");
        write_text(dir/"lib.ores", "");
        must_fail("project.root", "project.root",
                  [&] { (void)resolve(dir/"main.ores", "+/lib"); });
    }
    {
        const auto dir = temp / "traversal";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.ores", "");
        write_text(temp/"lib.ores", "");
        must_fail("traversal", "traversal",
                  [&] { (void)resolve(dir/"main.ores", "+/../lib"); });
        must_fail("empty component", "empty/dot/traversal",
                  [&] { (void)resolve(dir/"main.ores", "+//lib"); });
    }
    {
        const auto dir = temp / "nested-boundary";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.ores", "");
        write_text(dir/"nested/.ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"nested/lib.ores", "");
        must_fail("nested package", "nested package boundary",
                  [&] { (void)resolve(dir/"main.ores", "+/nested/lib"); });
    }
#if !defined(_WIN32)
    {
        const auto dir = temp / "target-escape";
        const auto outside = temp / "outside-target.ores";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.ores", "");
        write_text(outside, "");
        fs::create_symlink(outside, dir/"lib.ores");
        must_fail("symlink target escape", "escapes physical package root",
                  [&] { (void)resolve(dir/"main.ores", "+/lib"); });
    }
    {
        const auto dir = temp / "manifest-escape";
        const auto outside = temp / "outside-manifest.toml";
        write_text(outside, "schema_version = \"1\"\n");
        fs::create_directories(dir);
        write_text(dir/"main.ores", "");
        write_text(dir/"lib.ores", "");
        fs::create_symlink(outside, dir/".ores.pkg.toml");
        must_fail("manifest symlink escape", "must reside within package directory",
                  [&] { (void)resolve(dir/"main.ores", "+/lib"); });
    }
    {
        const auto dir = temp / "manifest-canonical-read";
        write_text(dir/"manifest-target.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.ores", "");
        write_text(dir/"lib.ores", "");
        fs::create_symlink(dir/"manifest-target.toml", dir/".ores.pkg.toml");
        bool saw_canonical = false;
        const auto target = oreslang::resolve_project_root_import(
            dir/"main.ores", "+/lib",
            [&](const fs::path& manifest) {
                if (fs::is_symlink(fs::symlink_status(manifest)))
                    throw std::runtime_error("manifest reader received unresolved symlink");
                saw_canonical = fs::equivalent(manifest, dir/"manifest-target.toml");
                return read_manifest(manifest);
            });
        if (!saw_canonical || target != fs::canonical(dir/"lib.ores"))
            throw std::runtime_error("manifest canonical read binding mismatch");
    }
#endif
    {
        const auto dir = temp / "extension";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.ores", "");
        write_text(dir/"lib.txt", "");
        must_fail("extension", ".ores source files",
                  [&] { (void)resolve(dir/"main.ores", "+/lib.txt"); });
    }

    std::cout << "PASS project-root import resolver security cases\n";
    return 0;
}
