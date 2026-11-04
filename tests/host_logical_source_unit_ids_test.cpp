#include "oreslang/frontend/HostLogicalSourceUnitIds.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using oreslang::HostLogicalSourceUnitIds;

static void write_file(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("could not write native identity fixture");
    output << text;
}

static SourceIdentity snapshot(const fs::path& path) {
    const auto text = path.string();
    return capture_source_identity(text.c_str(), 8u * 1024u * 1024u,
                                   "source fixture exceeds bounded identity limit");
}

template <class Fn>
static void rejects(std::string_view case_name, std::string_view diagnostic, Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        if (std::string_view(e.what()).find(diagnostic) != std::string_view::npos) return;
        throw std::runtime_error(std::string(case_name) + ": wrong failure: " + e.what());
    }
    throw std::runtime_error(std::string(case_name) + ": incorrectly accepted");
}

static void equals(std::string_view actual, std::string_view expected) {
    if (actual != expected)
        throw std::runtime_error("logical unit ID mismatch: " + std::string(actual));
}

int main() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tmp = fs::temp_directory_path() /
                     ("atto-logical-units-" + std::to_string(now));
    struct Cleanup {
        fs::path root;
        ~Cleanup() { std::error_code error; fs::remove_all(root, error); }
    } cleanup{tmp};

    const auto root = tmp / "project";
    const auto entry = root / "app" / "consumer.atto";
    const auto producer = root / "producer.atto";
    write_file(root / ".atto.pkg.toml", "[project]\nroot = \".\"\n");
    write_file(entry, "import module Producer from \"../producer.atto\";");
    write_file(producer, "define module Producer as pub fnc answer():int{return 42;} end");
    HostLogicalSourceUnitIds ids(entry);
    equals(ids.admit(entry, snapshot(entry)), "project/app/consumer.atto");
    equals(ids.admit(producer, snapshot(producer)), "project/producer.atto");
    equals(ids.admit(producer, snapshot(producer)), "project/producer.atto");

    // A second checkout has the SAME logical IDs without leaking absolute paths.
    const auto second = tmp / "second" / "project";
    write_file(second / ".atto.pkg.toml", "[project]\nroot = \".\"\n");
    write_file(second / "app" / "consumer.atto", "return 42;");
    HostLogicalSourceUnitIds other(second / "app" / "consumer.atto");
    equals(other.admit(second / "app" / "consumer.atto",
                       snapshot(second / "app" / "consumer.atto")),
           "project/app/consumer.atto");

    const auto external1 = tmp / "external1";
    const auto external2 = tmp / "external2";
    write_file(external1 / "producer.atto", "pub fnc value():int{return 7;}");
    write_file(external2 / "producer.atto", "pub fnc value():int{return 8;}");
    rejects("unowned absolute file", "outside declared source roots",
            [&] { (void)ids.admit(external1 / "producer.atto",
                                   snapshot(external1 / "producer.atto")); });
    // Root denial must happen BEFORE opening an imported source; no attempt
    // to read a rejected file may be hidden behind a later ID check.
    rejects("unowned pre-read denial", "outside declared source roots",
            [&] { (void)ids.preflight_path(external1 / "producer.atto"); });
    equals(ids.preflight_path(producer), "project/producer.atto");
    HostLogicalSourceUnitIds configured(entry, {external1, external2});
    equals(configured.admit(external1 / "producer.atto",
                             snapshot(external1 / "producer.atto")),
           "external/1/producer.atto");
    equals(configured.admit(external2 / "producer.atto",
                             snapshot(external2 / "producer.atto")),
           "external/2/producer.atto");

    for (const auto& bad : {
            "/tmp/producer.atto", "C:/producer.atto", "C:producer.atto",
            "project/../../producer.atto", "project/../other.atto",
            "project/a/./b.atto", "project/a//b.atto", "project/a:b.atto",
            "project/a\\b.atto", "external//producer.atto",
            "external/one/producer.atto", "producer.atto" }) {
        rejects("host path or alias injection", "",
                [&] { (void)HostLogicalSourceUnitIds::validated_logical_id(bad); });
    }
    rejects("oversize unit ID", "budget", [] {
        (void)HostLogicalSourceUnitIds::validated_logical_id(
            "project/" + std::string(4096, 'a') + ".atto");
    });
    rejects("unicode path denied conservatively", "noncanonical", [] {
        (void)HostLogicalSourceUnitIds::validated_logical_id(
            std::string("project/") + "\xe2\x80\xae" + "back.atto");
    });

    // In-package symlink aliases never acquire a second compiler identity.
    std::error_code optional;
    fs::create_symlink(producer, root / "alias.atto", optional);
    if (!optional) {
        rejects("source symlink alias", "symlink/alias", [&] {
            (void)ids.admit(root / "alias.atto", snapshot(producer));
        });
        rejects("source symlink blocked before descriptor open", "symlink/alias",
                [&] { (void)ids.preflight_path(root / "alias.atto"); });
    }
    optional.clear();
    fs::create_hard_link(producer, root / "hardlink.atto", optional);
    if (!optional) {
        rejects("hardlink alias", "hardlink", [&] {
            (void)ids.admit(producer, snapshot(producer));
        });
        fs::remove(root / "hardlink.atto");
    }

    // File replacement cannot preserve a prior admitted unit owner.
    const auto old = snapshot(producer);
    const auto replacement = root / "replacement.tmp";
    write_file(replacement, "different content has different size");
    fs::rename(replacement, producer);
    rejects("replacement after admission", "changed", [&] {
        (void)ids.admit(producer, old);
    });

    // Canonical marker symlink must not downgrade to a legacy marker.
    const auto corrupt = tmp / "corrupt";
    write_file(corrupt / ".ores.pkg.toml", "schema_version = \"1\"");
    write_file(corrupt / "main.atto", "pub fnc main():int{return 42;}");
    write_file(tmp / "manifest-target", "irrelevant");
    optional.clear();
    fs::create_symlink(tmp / "manifest-target", corrupt / ".atto.pkg.toml", optional);
    if (!optional) {
        rejects("symlink canonical marker", "root marker", [&] {
            HostLogicalSourceUnitIds invalid(corrupt / "main.atto");
        });
    }

    // A symlink to the project-root directory itself is permitted; aliases
    // below that root are not, and the logical path must be identical.
    const auto root_alias = tmp / "project-root-alias";
    optional.clear();
    fs::create_directory_symlink(root, root_alias, optional);
    if (!optional) {
        HostLogicalSourceUnitIds aliased(root_alias / "app" / "consumer.atto");
        equals(aliased.admit(root_alias / "app" / "consumer.atto",
                             snapshot(entry)), "project/app/consumer.atto");
        equals(aliased.admit(entry, snapshot(entry)), "project/app/consumer.atto");
    }

    const auto downgrade = tmp / "marker-downgrade";
    write_file(downgrade / ".atto.pkg.toml", "[project]\nroot = \"..\"\n");
    write_file(downgrade / ".ores.pkg.toml", "schema_version = \"1\"\n");
    write_file(downgrade / "main.atto", "pub fnc main():int{return 42;}");
    rejects("invalid canonical marker must not use legacy fallback",
            "canonical", [&] {
                HostLogicalSourceUnitIds invalid(downgrade / "main.atto");
            });

    std::cout << "LLVM_HOST_LOGICAL_UNIT_IDS_COMPONENT_PASS\n";
    return 0;
}
