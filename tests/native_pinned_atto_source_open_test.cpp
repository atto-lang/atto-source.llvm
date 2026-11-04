#include "PinnedAttoSourceOpen.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

namespace fs = std::filesystem;

static void write_file(const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << "pub fnc main():int{return 42;}\n";
}
static void must_fail(const fs::path& file) {
    try {
        const int fd = ores_open_source_fd(file.c_str());
        if (fd >= 0) ::close(fd);
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find("symlinked AttoLang source path") != std::string::npos)
            return;
        throw;
    }
    throw std::runtime_error("unexpected accepted path: " + file.string());
}
static void must_open(const fs::path& file) {
    int fd = ores_open_source_fd(file.c_str());
    if (fd < 0) throw std::runtime_error("expected successful open");
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) throw std::runtime_error("not a regular file");
    ::close(fd);
}
int main() {
    char templ[] = "/tmp/atto-pinned-test-XXXXXX";
    char* path = ::mkdtemp(templ);
    if (!path) throw std::runtime_error("mkdtemp failed");
    const fs::path base(path);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code err; fs::remove_all(path,err); } } clean{base};
    const auto real = base / "real";
    write_file(real / "main.atto");
    write_file(real / "uppercase.ATTO");
    write_file(real / "old.ores");
    write_file(real / ".atto.pkg.toml");
    fs::create_symlink(real / "main.atto", base / "leaf.atto");
    fs::create_directory_symlink(real, base / "parent-link");
    fs::create_symlink(real / ".atto.pkg.toml", base / ".atto.pkg.toml");
    fs::create_symlink(real / "old.ores", base / "legacy.ores");
    fs::create_symlink(base / "missing.atto", base / "dangling.atto");

    must_open(real / "main.atto");
    must_open(real / "uppercase.ATTO");
    must_open(real / ".atto.pkg.toml");
    must_open(real / ".." / "real" / "main.atto");
    must_open(base / "legacy.ores");
    must_fail(base / "leaf.atto");
    must_fail(base / "parent-link" / "main.atto");
    must_fail(base / ".atto.pkg.toml");
    must_fail(base / "dangling.atto");
    for (int i=0;i<1000;++i) must_open(real / "main.atto");
    std::cout << "PINNED_ATTO_SOURCE_OPEN=PASS success=5 deny=4 descriptor_reopen=1000\n";
}
