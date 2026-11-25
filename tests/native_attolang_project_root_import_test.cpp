#include "oreslang/frontend/ProjectRootImport.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;
using oreslang::resolve_project_root_import;

struct TempRoot {
    fs::path path;
    TempRoot() {
        char name[] = "/tmp/attolang-package-root-XXXXXX";
        const char* actual = mkdtemp(name);
        assert(actual);
        path = actual;
    }
    ~TempRoot() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};
static void write(const fs::path& path, const std::string& text) {
    std::ofstream output(path);
    assert(output.good());
    output << text;
    assert(output.good());
}
static std::string read_manifest(const fs::path& path) {
    std::ifstream input(path);
    assert(input.good());
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}
static fs::path resolve(const fs::path& source, std::string_view import) {
    return resolve_project_root_import(source, import, read_manifest);
}
static void rejects(const std::function<void()>& action,
                    const std::string& fragment) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        assert(std::string(error.what()).find(fragment) != std::string::npos);
        return;
    }
    assert(!"expected import rejection");
}
int main() {
    TempRoot tmp;
    const fs::path pkg = tmp.path / "package";
    const fs::path source = pkg / "src" / "main.atto";
    const fs::path modern = pkg / "lib" / "numbers.atto";
    const fs::path legacy = pkg / "lib" / "numbers.ores";
    const fs::path manifest = pkg / ".atto.pkg.toml";
    const fs::path old_manifest = pkg / ".ores.pkg.toml";
    fs::create_directories(pkg / "src");
    fs::create_directories(pkg / "lib");
    write(source, "fnc main(): int { return 42; }\n");
    write(manifest, "schema_version = \"1\"\n");
    write(modern, "define module numbers as end\n");

    assert(resolve(source, "+/lib/numbers") == fs::canonical(modern));
    assert(resolve(source, "+/lib/numbers.atto") == fs::canonical(modern));
    for (int i = 0; i < 100; ++i)
        assert(resolve(source, "+/lib/numbers") == fs::canonical(modern));

    write(legacy, "define module numbers as end\n");
    rejects([&] { (void)resolve(source, "+/lib/numbers"); },
            "ambiguous AttoLang module import");
    assert(resolve(source, "+/lib/numbers.atto") == fs::canonical(modern));
    assert(resolve(source, "+/lib/numbers.ores") == fs::canonical(legacy));
    fs::remove(modern);
    assert(resolve(source, "+/lib/numbers") == fs::canonical(legacy));

    rejects([&] { (void)resolve(source, "+/lib/../numbers"); },
            "traversal components");
    rejects([&] { (void)resolve(source, "+/lib/numbers.txt"); },
            "require .atto");
    rejects([&] { (void)resolve(source, "+/lib/"); },
            "empty/dot/traversal components");

    // The new manifest name is canonical; two competing package markers at
    // the same level must never silently choose a different package identity.
    write(old_manifest, "schema_version = \"1\"\n");
    rejects([&] { (void)resolve(source, "+/lib/numbers.ores"); },
            "ambiguous AttoLang package root");
    fs::remove(manifest);
    assert(resolve(source, "+/lib/numbers.ores") == fs::canonical(legacy));
    fs::remove(old_manifest);
    rejects([&] { (void)resolve(source, "+/lib/numbers.ores"); },
            "nearest .atto.pkg.toml");

    write(manifest, "schema_version = \"2\"\n");
    rejects([&] { (void)resolve(source, "+/lib/numbers.ores"); },
            "schema_version");
    write(manifest, "schema_version = \"1\"\n");

    fs::create_directories(pkg / "lib" / "nested");
    write(pkg / "lib" / "nested" / ".atto.pkg.toml",
          "schema_version = \"1\"\n");
    write(pkg / "lib" / "nested" / "inner.atto", "define module inner as end\n");
    rejects([&] { (void)resolve(source, "+/lib/nested/inner.atto"); },
            "nested package boundary");
    fs::remove(pkg / "lib" / "nested" / ".atto.pkg.toml");
    write(pkg / "lib" / "nested" / ".ores.pkg.toml",
          "schema_version = \"1\"\n");
    rejects([&] { (void)resolve(source, "+/lib/nested/inner.atto"); },
            "nested package boundary");

    // A symlink may exist lexically inside the package but resolve outside
    // its physical root. Canonicalization must catch that escape.
    const fs::path outside = tmp.path / "outside.atto";
    write(outside, "define module outside as end\n");
    fs::create_symlink(outside, pkg / "lib" / "escape.atto");
    rejects([&] { (void)resolve(source, "+/lib/escape.atto"); },
            "escapes physical package root");
}
