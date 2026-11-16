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
        // Direct validator defense: resolver callers may provide their own
        // manifest readers, not necessarily the bounded test reader above.
        using oreslang::project_root_import_detail::validate_manifest;
        must_fail("direct oversized manifest", "64 KiB", [&] {
            validate_manifest(std::string(65537, 'x'));
        });
        const std::string with_nul =
            std::string(R"manifest(schema_version = "1"
note = "good"
)manifest") + std::string(1, '\0');
        must_fail("direct manifest NUL", "contains NUL", [&] {
            validate_manifest(with_nul);
        });
        validate_manifest(std::string(R"manifest(schema_version = "1"
)manifest"));
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
                   "schema_version = \"1\"\n[project]\nroot = \"application\"\n");
        write_text(dir/"application/src/main.ores", "");
        write_text(dir/"application/src/lib.atto", "");
        if (resolve(dir/"application/src/main.ores", "+/src/lib") !=
                fs::canonical(dir/"application/src/lib.atto"))
            throw std::runtime_error("configured project.root did not select canonical file");
    }
    {
        const auto dir = temp / "zed-package-marker";
        write_text(dir/".ores.pkg.toml", "[package]\nname = 'consumer'\n");
        write_text(dir/"src/main.atto", "");
        write_text(dir/"src/lib.atto", "");
        if (resolve(dir/"src/main.atto", "+/src/lib") !=
                fs::canonical(dir/"src/lib.atto"))
            throw std::runtime_error("native Zed marker did not authorize project-root import");
    }
    {
        const auto dir = temp / "zed-missing-name";
        write_text(dir/".ores.pkg.toml", "[package]\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.atto", "");
        must_fail("missing native identity", "Zed [package].name",
                  [&] { (void)resolve(dir/"main.atto", "+/lib"); });
        write_text(dir/".ores.pkg.toml", "[package]\nname = '../outside'\n");
        must_fail("malformed native identity", "safe single identity",
                  [&] { (void)resolve(dir/"main.atto", "+/lib"); });
    }
    {
        const auto dir = temp / "project-root-traversal";
        write_text(dir/".ores.pkg.toml",
                   "schema_version = \"1\"\n[project]\nroot = \"../outside\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.atto", "");
        must_fail("configured root traversal", "project.root",
                  [&] { (void)resolve(dir/"main.atto", "+/lib"); });
    }
    {
        const auto dir = temp / "project-root-duplicate";
        write_text(dir/".ores.pkg.toml",
                   "schema_version = \"1\"\n[project]\nroot = \"src\"\nroot = \"other\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.atto", "");
        must_fail("duplicate configured root", "project.root",
                  [&] { (void)resolve(dir/"main.atto", "+/lib"); });
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
    {
        const auto dir = temp / "nonregular-legacy-import";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.ores", "");
        // Previously fs::canonical accepted an existing .ores directory and
        // returned it as a loadable source path.
        fs::create_directories(dir/"lib.ores");
        must_fail("legacy source directory", "source must be a regular file",
                  [&] { (void)resolve(dir/"main.ores", "+/lib.ores"); });
        must_fail("implicit legacy directory", "source must be a regular file",
                  [&] { (void)resolve(dir/"main.ores", "+/lib"); });
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
        must_fail("manifest symlink escape", "regular non-symlink file",
                  [&] { (void)resolve(dir/"main.ores", "+/lib"); });
    }
    {
        const auto dir = temp / "dangling-manifest";
        write_text(dir/"main.ores", "");
        write_text(dir/"lib.ores", "");
        fs::create_symlink(dir/"missing.toml", dir/".ores.pkg.toml");
        must_fail("dangling manifest marker", "regular non-symlink file",
                  [&] { (void)resolve(dir/"main.ores", "+/lib"); });
    }
    {
        const auto dir = temp / "manifest-canonical-read";
        write_text(dir/"manifest-target.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.ores", "");
        write_text(dir/"lib.ores", "");
        fs::create_symlink(dir/"manifest-target.toml", dir/".ores.pkg.toml");
        // A same-directory symlink used to be accepted after canonicalization.
        // It is still mutable package authority even if its current target
        // remains in this directory, so the reader must not be invoked.
        bool reader_invoked = false;
        must_fail("same-directory manifest symlink", "regular non-symlink file",
            [&] {
                (void)oreslang::resolve_project_root_import(
                    dir/"main.ores", "+/lib",
                    [&](const fs::path& manifest) {
                        reader_invoked = true;
                        return read_manifest(manifest);
                    });
            });
        if (reader_invoked)
            throw std::runtime_error("symlinked manifest reached reader");
    }
#endif
    {
        const auto dir = temp / "modern-source";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"app/main.atto", "");
        write_text(dir/"lib.atto", "");
        if (resolve(dir/"app/main.atto", "+/lib") != fs::canonical(dir/"lib.atto") ||
                resolve(dir/"app/main.atto", "+/lib.atto") != fs::canonical(dir/"lib.atto"))
            throw std::runtime_error(".atto root import did not resolve");
    }
    {
        const auto dir = temp / "ambiguous-modern-legacy-source";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.atto", "");
        write_text(dir/"lib.ores", "");
        if (resolve(dir/"main.atto", "+/lib") != fs::canonical(dir/"lib.atto"))
            throw std::runtime_error("implicit import did not prefer canonical .atto over .ores");
        if (resolve(dir/"main.atto", "+/lib.atto") != fs::canonical(dir/"lib.atto")
                || resolve(dir/"main.atto", "+/lib.ores") != fs::canonical(dir/"lib.ores"))
            throw std::runtime_error("explicit .atto/.ores suffix must disambiguate");
        write_text(dir/"upper.ATTO", "");
        if (resolve(dir/"main.atto", "+/upper.ATTO") != fs::canonical(dir/"upper.ATTO"))
            throw std::runtime_error("explicit case-insensitive .ATTO suffix must be admitted");
    }
    {
        const auto dir = temp / "modern-directory-shadows-legacy";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.ores", "");
        fs::create_directory(dir/"lib.atto");
        must_fail("directory canonical leaf must never select legacy", "canonical .atto source entry",
                  [&] { (void)resolve(dir/"main.atto", "+/lib"); });
        must_fail("explicit directory canonical leaf", "canonical .atto source entry",
                  [&] { (void)resolve(dir/"main.atto", "+/lib.atto"); });
    }
    {
        const auto dir = temp / "modern-missing-legacy-fallback";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.ores", "");
        if (resolve(dir/"main.atto", "+/lib") != fs::canonical(dir/"lib.ores"))
            throw std::runtime_error("missing canonical .atto must allow explicit migration fallback");
        must_fail("explicit missing .atto cannot downgrade", "cannot resolve native project-root import",
                  [&] { (void)resolve(dir/"main.atto", "+/lib.atto"); });
    }
    {
        const auto dir = temp / "modern-nested-boundary";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"nested/.ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"nested/lib.atto", "");
        must_fail("modern nested boundary", "nested package boundary",
                  [&] { (void)resolve(dir/"main.atto", "+/nested/lib"); });
    }
#if !defined(_WIN32)
    {
        const auto dir = temp / "modern-dangling-symlink";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.ores", "");
        fs::create_symlink(dir/"absent.atto", dir/"lib.atto");
        must_fail("dangling .atto must not downgrade to legacy sibling", "canonical .atto source entry",
                  [&] { (void)resolve(dir/"main.atto", "+/lib"); });
    }
    {
        const auto dir = temp / "same-inode-hardlink-twins";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.atto", "");
        fs::create_hard_link(dir/"lib.atto", dir/"lib.ores");
        if (resolve(dir/"main.atto", "+/lib") != fs::canonical(dir/"lib.atto"))
            throw std::runtime_error("same-inode hardlink must prefer canonical .atto spelling");
    }
    {
        const auto dir = temp / "same-inode-symlink-twins";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.atto", "");
        fs::create_symlink(dir/"lib.atto", dir/"lib.ores");
        if (resolve(dir/"main.atto", "+/lib") != fs::canonical(dir/"lib.atto"))
            throw std::runtime_error("legacy symlink shadow must not override canonical .atto");
    }
    {
        const auto dir = temp / "modern-symlink-escape";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(temp/"outside-modern.atto", "");
        fs::create_symlink(temp/"outside-modern.atto", dir/"lib.atto");
        must_fail(".atto symlink target escape", "canonical .atto source entry",
                  [&] { (void)resolve(dir/"main.atto", "+/lib"); });
    }
#endif
    {
        const auto dir = temp / "relative-package-boundary";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"app/main.atto", "");
        write_text(dir/"lib.atto", "");
        oreslang::project_root_import_detail::enforce_relative_import_package_boundary(
            dir/"app/main.atto", dir/"lib.atto");
        write_text(temp/"outside-relative.atto", "");
        must_fail("relative package escape", "escapes physical package root", [&] {
            oreslang::project_root_import_detail::enforce_relative_import_package_boundary(
                dir/"app/main.atto", temp/"outside-relative.atto");
        });
        write_text(dir/"nested/.ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"nested/lib.atto", "");
        must_fail("relative nested package crossing", "nested package boundary", [&] {
            oreslang::project_root_import_detail::enforce_relative_import_package_boundary(
                dir/"app/main.atto", dir/"nested/lib.atto");
        });
    }
    {
        const auto dir = temp / "relative-package-less";
        write_text(dir/"app/main.atto", "");
        write_text(temp/"package-less-relative.atto", "");
        oreslang::project_root_import_detail::enforce_relative_import_package_boundary(
            dir/"app/main.atto", temp/"package-less-relative.atto");
    }
#if !defined(_WIN32)
    {
        const auto dir = temp / "relative-symlink-escape";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(temp/"outside-relative-target.atto", "");
        fs::create_symlink(temp/"outside-relative-target.atto", dir/"lib.atto");
        must_fail("relative symlink package escape", "escapes physical package root", [&] {
            oreslang::project_root_import_detail::enforce_relative_import_package_boundary(
                dir/"main.atto", dir/"lib.atto");
        });
    }
#endif
    {
        const auto dir = temp / "extension";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.ores", "");
        write_text(dir/"lib.txt", "");
        must_fail("extension", ".atto or legacy .ores extension",
                  [&] { (void)resolve(dir/"main.ores", "+/lib.txt"); });
    }

    std::cout << "PASS project-root import resolver security cases\n";
    return 0;
}
