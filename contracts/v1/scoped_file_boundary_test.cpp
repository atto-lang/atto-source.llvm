#include "scoped_file_boundary.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#if defined(__linux__)
#include <cstdlib>
#include <unistd.h>
#endif
using namespace oreslang::permissions;

static void require(bool valid, const char* msg) {
    if (!valid) throw std::runtime_error(msg);
}
template <typename F> static void reject(F&& fn, const char* msg) {
    try { fn(); } catch (const std::exception&) { return; }
    throw std::runtime_error(msg);
}

int main() {
    const ScopedPolicy deny_all;
    reject([&] { (void)ScopedFileBoundary::read_existing(deny_all, "/tmp/denied"); },
           "default read allowed");
    reject([&] { ScopedFileBoundary::append_existing(deny_all, "/tmp/denied", "x"); },
           "default write allowed");
#if defined(__linux__) && defined(SYS_openat2)
    char name[] = "/tmp/atto-scoped-files-XXXXXX";
    const char* dir = ::mkdtemp(name);
    if (!dir) throw std::runtime_error("mkdtemp failed");
    const std::filesystem::path root(dir);
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::error_code e; std::filesystem::remove_all(p, e); }
    } cleanup{root};
    const auto allowed_dir = root / "allowed";
    const auto denied_dir = root / "denied";
    std::filesystem::create_directories(allowed_dir);
    std::filesystem::create_directories(denied_dir);
    const auto good = allowed_dir / "public.txt";
    const auto secret = denied_dir / "secret.txt";
    { std::ofstream out(good); out << "PUBLIC"; }
    { std::ofstream out(secret); out << "SECRET"; }
    const auto allow_read = ScopedPolicy::from_host(
        {Permission::Read}, {{Permission::Read, allowed_dir.string(), false}});
    const auto scoped = ScopedPolicy::from_host(
        {Permission::Read, Permission::Write},
        {{Permission::Read, allowed_dir.string(), false},
         {Permission::Write, allowed_dir.string(), false},
         {Permission::Read, good.string(), true}});
    require(ScopedFileBoundary::read_existing(allow_read, good.string()) == "PUBLIC",
            "allowed read failed");
    reject([&] { (void)ScopedFileBoundary::read_existing(scoped, good.string()); },
           "explicit read deny bypassed");
    reject([&] { (void)ScopedFileBoundary::read_existing(allow_read, secret.string()); },
           "sibling path allowed");
    reject([&] { ScopedFileBoundary::append_existing(allow_read, good.string(), "!"); },
           "read implies write");
    reject([&] { ScopedFileBoundary::append_existing(scoped, secret.string(), "!"); },
           "sibling write allowed");
    ScopedFileBoundary::append_existing(scoped, good.string(), "!");
    require(ScopedFileBoundary::read_existing(allow_read, good.string()) == "PUBLIC!",
            "scoped append failed");
    reject([&] { (void)ScopedFileBoundary::read_existing(allow_read, good.string(), 2); },
           "bounded read accepted excess");
    reject([&] { (void)ScopedFileBoundary::read_existing(allow_read, allowed_dir.string()); },
           "directory opened as file");
    reject([&] { (void)ScopedFileBoundary::read_existing(
        allow_read, allowed_dir.string() + "/../denied/secret.txt"); },
           "dotdot traversal allowed");
    const auto link = allowed_dir / "link.txt";
    std::filesystem::create_symlink(secret, link);
    reject([&] { (void)ScopedFileBoundary::read_existing(allow_read, link.string()); },
           "final symlink read allowed");
    reject([&] { ScopedFileBoundary::append_existing(scoped, link.string(), "!"); },
           "final symlink write allowed");
    const auto linkdir = allowed_dir / "alias";
    std::filesystem::create_directory_symlink(denied_dir, linkdir);
    reject([&] { (void)ScopedFileBoundary::read_existing(allow_read,
        (linkdir / "secret.txt").string()); }, "parent symlink allowed");
    // Deterministic path substitutions; not a concurrency or rename-race proof.
    const auto changing = allowed_dir / "changing.txt";
    for (unsigned i = 0; i < 128; ++i) {
        if (i) std::filesystem::remove(changing);
        if ((i & 1U) == 0) {
            { std::ofstream out(changing); out << "PUBLIC"; }
            require(ScopedFileBoundary::read_existing(allow_read, changing.string()) == "PUBLIC",
                    "regular-file read failed");
        } else {
            std::filesystem::create_symlink(secret, changing);
            reject([&] { (void)ScopedFileBoundary::read_existing(allow_read, changing.string()); },
                   "path-swap symlink read allowed");
        }
    }
    { std::ifstream in(secret); std::string body; in >> body;
      require(body == "SECRET", "denied content modified"); }
#elif defined(__linux__)
    throw std::runtime_error("Linux openat2 unavailable: positive tests cannot execute");
#endif
    std::cout << "NATIVE_SCOPED_FILE_HOST_BOUNDARY_PASS\n";
    return 0;
}
