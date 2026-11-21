// Executable source-admission regression using the production header only.
// No LLVM or OresVM dependency; safe on supported POSIX host compilers.
#include "oreslang/frontend/SourceInput.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

namespace fs = std::filesystem;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F&& operation, const char* reason) {
    bool rejected = false;
    try { operation(); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, reason);
}
int main() {
    try {
        const fs::path dir = fs::temp_directory_path() /
            ("ores-source-input-admission-" + std::to_string(::getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directory(dir);
        struct Cleanup {
            fs::path path;
            ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
        } cleanup{dir};
        const auto source = dir / "main.ores";
        { std::ofstream output(source, std::ios::binary); output << "abcdef"; }
        const auto name = source.string();
        SourceIdentity original;
        check(read_regular_source(name.c_str(), 6, "source too large", &original) == "abcdef",
              "regular source did not load");
        check(original.valid, "regular source omitted inode identity");
        rejects([&] { (void)read_regular_source(name.c_str(), 5, "source too large"); },
                "source budget not enforced");
        const auto bound = dir / "identity-bound.ores";
        { std::ofstream output(bound, std::ios::binary); output << "approved"; }
        const auto bound_name = bound.string();
        const auto approved_identity =
            capture_source_identity(bound_name.c_str(), 64, "source too large");
        SourceIdentity bound_identity;
        check(read_regular_source(
                  bound_name.c_str(), 64, "source too large",
                  &bound_identity, &approved_identity) == "approved",
              "unchanged expected source identity rejected");
        check(ores_source_identity_equal(approved_identity, bound_identity),
              "expected source identity changed on stable read");

        const auto replacement = dir / "identity-replacement.ores";
        { std::ofstream output(replacement, std::ios::binary); output << "replacement"; }
        fs::rename(replacement, bound);
        rejects([&] {
            (void)read_regular_source(
                bound_name.c_str(), 64, "source too large",
                nullptr, &approved_identity);
        }, "same-path regular-file replacement escaped expected identity");

        const auto alias = dir / "regular-link.ores";
        fs::create_symlink("main.ores", alias);
        const auto alias_expected =
            capture_source_identity(alias.c_str(), 6, "source too large");
        SourceIdentity alias_id;
        check(read_regular_source(
                  alias.c_str(), 6, "source too large",
                  &alias_id, &alias_expected) == "abcdef",
              "regular file symlink rejected");
        check(alias_id.valid && alias_id.device == original.device &&
              alias_id.inode == original.inode, "symlink changed source identity");
        const auto hard = dir / "hard-link.ores";
        fs::create_hard_link(source, hard);
        SourceIdentity hard_id;
        check(read_regular_source(hard.c_str(), 6, "source too large", &hard_id) == "abcdef",
              "hard-linked source rejected");
        check(hard_id.valid && hard_id.inode == original.inode &&
              hard_id.device == original.device, "hard link lost inode identity");
        const auto fifo = dir / "nonregular.ores";
        check(::mkfifo(fifo.c_str(), 0600) == 0, "cannot create FIFO fixture");
        rejects([&] { (void)read_regular_source(fifo.c_str(), 10, "source too large"); },
                "FIFO was admitted or blocked");
        rejects([&] { (void)read_regular_source("/dev/null", 10, "source too large"); },
                "character device was admitted");
        struct stat before{};
        check(::stat(source.c_str(), &before) == 0, "stat fixture failed");
        check(ores_source_stat_stable(before, before), "stable stat rejected");
        auto altered = before;
        ++altered.st_size;
        check(!ores_source_stat_stable(before, altered), "size change accepted");
        altered = before; ++altered.st_ino;
        check(!ores_source_stat_stable(before, altered), "inode change accepted");
#if defined(__APPLE__)
        altered = before; ++altered.st_mtimespec.tv_nsec;
#else
        altered = before; ++altered.st_mtim.tv_nsec;
#endif
        check(!ores_source_stat_stable(before, altered), "mtime change accepted");
        require_valid_source_utf8("valid ASCII");
        rejects([&] { require_valid_source_utf8(std::string("\xC0\xAF", 2)); },
                "overlong UTF-8 admitted");
        rejects([&] { require_valid_source_utf8(std::string("ab\0cd", 5)); },
                "binary NUL admitted");
        rejects([&] { require_valid_source_utf8(std::string("/*\x1b*/", 5)); },
                "ESC inside comment was admitted");
        rejects([&] { require_valid_source_utf8(std::string("\"\x7f\"", 3)); },
                "DEL inside literal was admitted");
        require_valid_source_utf8("\t\r\nordinary ASCII");
        // A regular-file size check must apply to both the exact buffer edge
        // and growth past that edge, not merely small fixture files.
        const auto large = dir / "buffer-boundary.ores";
        const std::string block(8192, 'a');
        { std::ofstream output(large, std::ios::binary); output.write(block.data(), block.size()); }
        check(read_regular_source(large.c_str(), 8192, "source too large").size() == 8192,
              "8 KiB exact boundary rejected");
        rejects([&] { (void)read_regular_source(large.c_str(), 8191, "source too large"); },
                "8 KiB over-budget source admitted");
        { std::ofstream output(large, std::ios::binary); output << block << 'b'; }
        check(read_regular_source(large.c_str(), 8193, "source too large").size() == 8193,
              "8 KiB plus one byte rejected");
        rejects([&] { (void)read_regular_source(large.c_str(), 8192, "source too large"); },
                "growth past 8 KiB limit admitted");
        { std::ofstream output(large, std::ios::binary); }
        check(read_regular_source(large.c_str(), 0, "source too large").empty(),
              "empty regular source rejected at zero-byte limit");
        rejects([&] { (void)read_regular_source(dir.c_str(), 8192, "source too large"); },
                "directory admitted as source");
        rejects([&] { (void)read_regular_source((dir / "not-found.ores").c_str(),
                                             8192, "source too large"); },
                "nonexistent source admitted");
        const std::string malformed[] = {
            std::string("\x80", 1), std::string("\xC1\xBF", 2),
            std::string("\xE0\x80\x80", 3), std::string("\xED\xA0\x80", 3),
            std::string("\xF4\x90\x80\x80", 4), std::string("\xF5\x80\x80\x80", 4),
            std::string("\xF0\x90", 2),
        };
        for (const auto& input : malformed)
            rejects([&] { require_valid_source_utf8(input); }, "malformed Unicode admitted");
        require_valid_source_utf8(std::string("\xE4\xB8\xAD", 3)); // CJK
        require_valid_source_utf8(std::string("\xF0\x9F\x98\x80", 4)); // emoji
        std::cout << "PASS native source admission: regular/links, FIFO, 8 KiB bounds, snapshot metadata, UTF-8\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL native source admission: " << error.what() << '\n';
        return 1;
    }
}
