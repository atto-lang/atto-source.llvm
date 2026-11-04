#include "oreslang/frontend/NativeLogicalSourceAdmission.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using oreslang::frontend::NativeLogicalSourceAdmission;
using oreslang::frontend::logical_source_detail::validate_logical_id;

static void write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out || !(out << bytes)) throw std::runtime_error("cannot write fixture");
}

static SourceIdentity identity(const fs::path& path) {
    const auto text = path.string();
    return capture_source_identity(text.c_str(), 1024 * 1024, "test source too large");
}

template <class F>
static void rejects(const char* label, const std::string& needle, F&& work) {
    try { work(); }
    catch (const std::exception& error) {
        if (std::string(error.what()).find(needle) != std::string::npos) return;
        throw std::runtime_error(std::string(label) + ": wrong diagnostic: " + error.what());
    }
    throw std::runtime_error(std::string(label) + ": unexpectedly accepted");
}

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp = fs::temp_directory_path() /
        ("atto-logical-identity-" + std::to_string(stamp));
    struct Cleanup { fs::path p; ~Cleanup() {
        std::error_code error; fs::remove_all(p, error);
    } } cleanup{temp};

    const auto project = temp / "project";
    const auto entry = project / "app/main.atto";
    const auto producer = project / "lib/producer.atto";
    write(project / ".atto.pkg.toml", "[project]\nroot = \".\"\n");
    write(entry, "pub fnc main(): int { return 42; }");
    write(producer, "define module Lib as pub fnc value():int{return 42;} end");
    NativeLogicalSourceAdmission source(entry);
    if (source.admit(entry, identity(entry)) != "project/app/main.atto" ||
        source.admit(producer, identity(producer)) != "project/lib/producer.atto")
        throw std::runtime_error("project-relative logical ID mismatch");
    if (source.admit(producer, identity(producer)) != "project/lib/producer.atto")
        throw std::runtime_error("repeated backedge was not idempotent");

    // No absolute host path becomes a logical ID, and no external root is
    // implicitly trusted as a compiler-owned or macro-authorized source.
    const auto external = temp / "outside.atto";
    write(external, "pub fnc answer():int{return 7;}");
    rejects("cross-project", "outside the admitted project root",
            [&] { source.admit(external, identity(external)); });
    rejects("logical absolute", "bounded relative", [&] {
        validate_logical_id("/tmp/producer.atto");
    });
    rejects("drive prefix", "colon", [&] {
        validate_logical_id("C:/producer.atto");
    });
    rejects("traversal", "traversal", [&] {
        validate_logical_id("project/gone/../producer.atto");
    });
    rejects("dot segment", "traversal", [&] {
        validate_logical_id("project/./producer.atto");
    });
    rejects("bidi", "nonportable Unicode", [&] {
        validate_logical_id("project/\xE2\x80\xAE/../producer.atto");
    });
    rejects("supplementary format", "nonportable Unicode", [&] {
        validate_logical_id("project/\xF3\xA0\x80\x81.atto");
    });
    rejects("broken UTF8", "UTF-8", [&] {
        validate_logical_id("project/\xC0\xAF.atto");
    });
    validate_logical_id("project/emoji-\xF0\x9F\x99\x82.atto");

    // A second build can use the same relocatable source ID from a different
    // project path, without sharing any registry state.
    const auto second = temp / "another";
    const auto second_entry = second / "app/main.atto";
    write(second / ".atto.pkg.toml", "[project]\nroot = \".\"\n");
    write(second_entry, "pub fnc main():int{return 42;}");
    NativeLogicalSourceAdmission separate(second_entry);
    if (separate.admit(second_entry, identity(second_entry)) != "project/app/main.atto")
        throw std::runtime_error("relocation changed logical source ID");

#if !defined(_WIN32)
    const auto symlink = project / "lib/shortcut.atto";
    fs::create_symlink(producer, symlink);
    rejects("file symlink", "symlink alias", [&] {
        source.admit(symlink, identity(symlink));
    });
    const auto shortcut_directory = project / "mirror";
    fs::create_directory_symlink(project / "lib", shortcut_directory);
    rejects("directory symlink", "symlink alias", [&] {
        source.admit(shortcut_directory / "producer.atto", identity(producer));
    });
#endif

#if !defined(_WIN32)
    // Reject aliases even when the second link is outside the import closure.
    const auto alias = project / "lib/hardlink.atto";
    fs::create_hard_link(producer, alias);
    rejects("hardlink producer", "hardlink", [&] {
        source.admit(producer, identity(producer));
    });
    rejects("hardlink alias", "hardlink", [&] {
        source.admit(alias, identity(alias));
    });
    fs::remove(alias);
#endif

    // A path-replaced source cannot reuse the registry's previous identity.
    const auto retained = project / "old-producer.atto";
    fs::rename(producer, retained);
    write(producer, "pub fnc changed():int{return 9001;}");
    rejects("replacement", "duplicate native logical source ID", [&] {
        source.admit(producer, identity(producer));
    });

    // The project-less fallback does not adopt an arbitrary ancestor.
    const auto no_manifest = temp / "standalone";
    const auto plain = no_manifest / "main.atto";
    write(plain, "pub fnc main():int{return 42;}");
    NativeLogicalSourceAdmission standalone(plain);
    if (standalone.admit(plain, identity(plain)) != "project/main.atto")
        throw std::runtime_error("package-less entry relative unit ID mismatch");
    rejects("package-less outside", "outside the admitted project root", [&] {
        standalone.admit(external, identity(external));
    });
    std::cout << "PASS native source-unit logical identity admission (host-only)\n";
    return 0;
}
