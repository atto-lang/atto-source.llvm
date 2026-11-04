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
        const auto dir = temp / "project-root-table-forms";
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.atto", "");
        const std::string schema = "schema_version = \"1\"\n";
        const std::string fixtures[] = {
            "[project.root]\nnote = \"unsupported\"\n",
            "[[project.root]]\nnote = \"unsupported\"\n",
            "['project'.'root']\nnote = \"unsupported\"\n",
            "[\"project\" . \"root\"] # nested\nnote = \"unsupported\"\n",
            "[[project]]\nroot = \"src\"\n",
            "[[\"project\"]]\nroot = \"src\"\n",
            "[[ 'project' ]]\nroot = \"src\"\n",
        };
        for (std::size_t i = 0; i < sizeof(fixtures)/sizeof(fixtures[0]); ++i) {
            write_text(dir/".ores.pkg.toml", schema + fixtures[i]);
            must_fail("nested project root table " + std::to_string(i), "project.root",
                      [&] { (void)resolve(dir/"main.atto", "+/lib"); });
        }
        // TOML basic keys can Unicode-escape project/root to evade a
        // byte-only table-name check. This bounded parser rejects escapes
        // in keys rather than pretending to decode full TOML.
        const std::string escaped_keys[] = {
            "[\"pro\\u006aect\".root]\nnote=\"unsupported\"\n",
            "[project.\"ro\\u006ft\"]\nnote=\"unsupported\"\n",
            "project.\"ro\\u006ft\" = \"src\"\n",
            "\"proj\\u0065ct\".root = \"src\"\n",
        };
        for (std::size_t i = 0; i < sizeof(escaped_keys)/sizeof(escaped_keys[0]); ++i) {
            write_text(dir/".ores.pkg.toml", schema + escaped_keys[i]);
            must_fail("escaped project/root key " + std::to_string(i), "escaped keys",
                      [&] { (void)resolve(dir/"main.atto", "+/lib"); });
        }
        const std::string deeper_tables[] = {
            "[project.root.extra]\nnote=\"unsupported\"\n",
            "[project.logging]\nnote=\"unsupported\"\n",
        };
        for (std::size_t i = 0; i < sizeof(deeper_tables)/sizeof(deeper_tables[0]); ++i) {
            write_text(dir/".ores.pkg.toml", schema + deeper_tables[i]);
            must_fail("unsupported project subtable " + std::to_string(i), "nested project table",
                      [&] { (void)resolve(dir/"main.atto", "+/lib"); });
        }
        // A duplicated TOML table declaration is invalid even when no
        // root key is present. Accepting it can hide conflicting policy.
        write_text(dir/".ores.pkg.toml",
                   schema + "[project]\nname=\"first\"\n[project]\nname=\"second\"\n");
        must_fail("duplicate project table", "duplicate project table",
                  [&] { (void)resolve(dir/"main.atto", "+/lib"); });
    }
    {
        const auto dir = temp / "unrelated-root-table-allowed";
        write_text(dir/".ores.pkg.toml",
                   "schema_version = \"1\"\n[build.logging]\nname=\"safe\"\n[build]\nroot=\"src\"\n[project]\nname=\"demo\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.atto", "");
        if (resolve(dir/"main.atto", "+/lib") != fs::canonical(dir/"lib.atto"))
            throw std::runtime_error("unrelated build.root table must not alter source-root admission");
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
        const auto dir = temp / "atto-extension";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"src/main.atto", "");
        write_text(dir/"src/main.ores", "");
        write_text(dir/"lib.atto", "");
        write_text(dir/"lib.ores", "");
        // Both files exist: extensionless imports must preserve source dialect.
        if (resolve(dir/"src/main.atto", "+/lib") != fs::canonical(dir/"lib.atto"))
            throw std::runtime_error("AttoLang project-root import selected legacy file");
        if (resolve(dir/"src/main.ores", "+/lib") != fs::canonical(dir/"lib.ores"))
            throw std::runtime_error("legacy project-root import selected AttoLang file");
        // Explicit cross-extension import is unambiguous for migration.
        if (resolve(dir/"src/main.atto", "+/lib.ores") != fs::canonical(dir/"lib.ores"))
            throw std::runtime_error("explicit legacy source import mismatch");
        if (resolve(dir/"src/main.ores", "+/lib.atto") != fs::canonical(dir/"lib.atto"))
            throw std::runtime_error("explicit AttoLang source import mismatch");
    }
    {
        const auto dir = temp / "atto-no-fallback";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.ores", "");
        must_fail("AttoLang missing native source", "cannot resolve native project-root import",
                  [&] { (void)resolve(dir/"main.atto", "+/lib"); });
    }
    {
        const auto dir = temp / "atto-nested-package";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"nested/.ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"nested/lib.atto", "");
        must_fail("AttoLang nested package boundary", "nested package boundary",
                  [&] { (void)resolve(dir/"main.atto", "+/nested/lib"); });
    }
#if !defined(_WIN32)
    {
        const auto dir = temp / "atto-symlink-escape";
        const auto outside = temp / "outside-source.atto";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(outside, "");
        fs::create_symlink(outside, dir/"lib.atto");
        must_fail("AttoLang symlink target escape", "escapes physical package root",
                  [&] { (void)resolve(dir/"main.atto", "+/lib"); });
    }
#endif
#if !defined(_WIN32)
    {
        const auto dir = temp / "atto-cross-suffix-importing-source";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"real.ores", "");
        write_text(dir/"lib.atto", "");
        fs::create_symlink("real.ores", dir/"main.atto");
        must_fail("AttoLang source alias cannot switch dialect",
                  "native importing source suffix changes across symlink",
                  [&] { (void)resolve(dir/"main.atto", "+/lib"); });
    }
    {
        const auto dir = temp / "legacy-cross-suffix-importing-source";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"real.atto", "");
        write_text(dir/"lib.ores", "");
        fs::create_symlink("real.atto", dir/"main.ores");
        must_fail("legacy source alias cannot switch dialect",
                  "native importing source suffix changes across symlink",
                  [&] { (void)resolve(dir/"main.ores", "+/lib"); });
    }
    {
        const auto dir = temp / "atto-cross-suffix-import-target";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.atto", "");
        write_text(dir/"real.ores", "");
        fs::create_symlink("real.ores", dir/"lib.atto");
        must_fail("AttoLang target alias cannot switch dialect",
                  "native project-root import source suffix changes across symlink",
                  [&] { (void)resolve(dir/"main.atto", "+/lib"); });
    }
    {
        const auto dir = temp / "legacy-cross-suffix-import-target";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.ores", "");
        write_text(dir/"real.atto", "");
        fs::create_symlink("real.atto", dir/"lib.ores");
        must_fail("legacy target alias cannot switch dialect",
                  "native project-root import source suffix changes across symlink",
                  [&] { (void)resolve(dir/"main.ores", "+/lib"); });
    }
    {
        const auto dir = temp / "atto-same-suffix-import-symlink";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"real-main.atto", "");
        write_text(dir/"real-lib.atto", "");
        fs::create_symlink("real-main.atto", dir/"main.atto");
        fs::create_symlink("real-lib.atto", dir/"lib.atto");
        if (resolve(dir/"main.atto", "+/lib") != fs::canonical(dir/"real-lib.atto"))
            throw std::runtime_error("same-suffix AttoLang symlink import should remain valid");
    }
#endif
    {
        const auto dir = temp / "invalid-importing-source-extension";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"lib.atto", "");
        write_text(dir/"lib.ores", "");
        for (const char* filename : {"main", "main.txt", "main.ATTO", "main.ores.bak"}) {
            write_text(dir/filename, "");
            must_fail(std::string("invalid importing source ") + filename,
                      "native importing source must use .atto or .ores extension",
                      [&] { (void)resolve(dir/filename, "+/lib"); });
        }
    }
    {
        const auto dir = temp / "extension";
        write_text(dir/".ores.pkg.toml", "schema_version = \"1\"\n");
        write_text(dir/"main.ores", "");
        write_text(dir/"main.atto", "");
        write_text(dir/"lib.txt", "");
        must_fail("extension", ".atto or .ores source files",
                  [&] { (void)resolve(dir/"main.ores", "+/lib.txt"); });
        must_fail("AttoLang extension", ".atto or .ores source files",
                  [&] { (void)resolve(dir/"main.atto", "+/lib.txt"); });
    }

    std::cout << "PASS project-root import resolver security cases\n";
    return 0;
}
