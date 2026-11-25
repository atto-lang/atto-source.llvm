#include "oreslang/frontend/AttoSourcePath.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using oreslang::project_root_import_detail::resolve_attolang_source_candidate;

struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        char name[] = "/tmp/attolang-source-path-XXXXXX";
        const char* created = mkdtemp(name);
        assert(created);
        path = created;
    }
    ~TemporaryDirectory() { std::error_code ec; fs::remove_all(path, ec); }
};
static void write(const fs::path& path) {
    std::ofstream out(path);
    assert(out.good());
    out << "fnc main(): int { return 42; }\n";
    assert(out.good());
}
static void rejects(const fs::path& path, const std::string& diagnostic) {
    try {
        (void)resolve_attolang_source_candidate(path);
    } catch (const std::runtime_error& error) {
        assert(std::string(error.what()).find(diagnostic) != std::string::npos);
        return;
    }
    assert(!"expected source path rejection");
}
int main() {
    TemporaryDirectory tmp;
    const fs::path unit = tmp.path / "counter";
    const fs::path modern = tmp.path / "counter.atto";
    const fs::path legacy = tmp.path / "counter.ores";
    const fs::path unsupported = tmp.path / "counter.toml";

    // Missing source defaults to the new format; caller later canonicalizes
    // and checks file identity. Do not silently invent a legacy filename.
    assert(resolve_attolang_source_candidate(unit) == modern);
    write(modern);
    assert(resolve_attolang_source_candidate(unit) == modern);
    assert(resolve_attolang_source_candidate(modern) == modern);
    rejects(unsupported, "require .atto");

    fs::remove(modern);
    write(legacy);
    assert(resolve_attolang_source_candidate(unit) == legacy);
    assert(resolve_attolang_source_candidate(legacy) == legacy);
    write(modern);
    rejects(unit, "ambiguous AttoLang module import");
    // Ambiguity must never prevent an explicit filename from binding to its
    // intended unit during migration.
    assert(resolve_attolang_source_candidate(modern) == modern);
    assert(resolve_attolang_source_candidate(legacy) == legacy);

    // A dangling new-format symlink is an attempted module binding, not a
    // reason to silently load an older source file with the same basename.
    fs::remove(modern);
    fs::create_symlink(tmp.path / "missing-target.atto", modern);
    rejects(unit, "ambiguous AttoLang module import");

    // Nonexistent relative sources must remain new-format, not .ores guesses.
    assert(resolve_attolang_source_candidate(tmp.path / "absent")
           == tmp.path / "absent.atto");
    for (int i = 0; i < 1000; ++i) {
        assert(resolve_attolang_source_candidate(legacy) == legacy);
        rejects(unit, "ambiguous AttoLang module import");
    }
}
