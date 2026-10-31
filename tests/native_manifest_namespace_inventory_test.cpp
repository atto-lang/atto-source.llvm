#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/PackageManifestNamespaceInventory.hpp"
#include "oreslang/frontend/PinnedNamespaceLoader.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create manifest inventory fixture");
    out << text;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read manifest inventory fixture");
    return std::string(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
}

template <typename F>
void must_fail(const std::string& fragment, F&& operation) {
    try {
        operation();
    } catch (const std::exception& error) {
        if (std::string(error.what()).find(fragment) == std::string::npos)
            throw std::runtime_error(
                "wrong failure; expected '" + fragment + "', saw '" +
                std::string(error.what()) + "'");
        return;
    }
    throw std::runtime_error("expected failure containing: " + fragment);
}

struct Fixture {
    fs::path root;
    fs::path app;
    fs::path foo;
    fs::path app_manifest_path;
    fs::path foo_manifest_path;
    fs::path app_main;
    oreslang::VerifiedPackageManifestV2 app_manifest;
    oreslang::VerifiedPackageManifestV2 foo_manifest;
    oreslang::VerifiedPackageLockSnapshot lock;

    Fixture() {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() /
            ("ores-pinned-manifest-namespace-" + std::to_string(stamp));
        app = root / "app";
        foo = root / "foo";
        app_manifest_path = app / ".ores.pkg.toml";
        foo_manifest_path = foo / ".ores.pkg.toml";
        app_main = app / "src/main.ores";

        const std::string app_manifest_bytes =
            "schema_version = \"2\"\n"
            "[project]\n"
            "name = \"app\"\n"
            "root = \".\"\n"
            "[source]\n"
            "roots = [\"src\"]\n"
            "[entrypoints]\n"
            "main = \"src/main.ores\"\n"
            "[packages.app]\n"
            "files = [\"src/main.ores\"]\n";
        const std::string foo_manifest_bytes =
            "schema_version = \"2\"\n"
            "[project]\n"
            "name = \"foo\"\n"
            "root = \".\"\n"
            "[source]\n"
            "roots = [\"src\"]\n"
            "[packages.foo]\n"
            "files = [\n"
            "  \"src/compute/math.ores\",\n"
            "  \"src/compute/stats.ores\",\n"
            "  \"src/compute/aggregation.ores\",\n"
            "  \"src/data/models.ores\",\n"
            "  \"src/data/queries.ores\",\n"
            "  \"src/data/storage.ores\",\n"
            "  \"src/data/validation.ores\",\n"
            "  \"src/api/routes.ores\",\n"
            "  \"src/api/handlers.ores\",\n"
            "  \"src/api/responses.ores\",\n"
            "]\n"
            "[packages.shadow]\n"
            "files = [\"src/shadow.ores\"]\n";

        write_text(app_manifest_path, app_manifest_bytes);
        write_text(foo_manifest_path, foo_manifest_bytes);

        write_text(app_main,
            "import ns a from \"pkg:foo\";\n"
            "import ns c from \"pkg:foo\";\n"
            "pub fnc main(): int { return a.aggregate(21) + c.response_delta(); }\n");

        write_text(foo / "src/compute/math.ores",
            "namespace a; pub fnc add(int a, int b): int { return a + b; }\n");
        write_text(foo / "src/compute/stats.ores",
            "namespace a; pub fnc double(int value): int { return add(value, value); }\n");
        write_text(foo / "src/compute/aggregation.ores",
            "namespace a; pub fnc aggregate(int value): int { return double(value) + 1; }\n");

        write_text(foo / "src/data/models.ores",
            "namespace b; pub fnc model_code(): int { return 1; }\n");
        write_text(foo / "src/data/queries.ores",
            "namespace b; pub fnc query_code(): int { return 2; }\n");
        write_text(foo / "src/data/storage.ores",
            "namespace b; pub fnc storage_code(): int { return 3; }\n");
        write_text(foo / "src/data/validation.ores",
            "namespace b; pub fnc validation_code(): int { return 4; }\n");

        write_text(foo / "src/api/routes.ores",
            "namespace c; pub fnc route_ok(int code): bool { return code eq 43; }\n");
        write_text(foo / "src/api/handlers.ores",
            "namespace c; pub fnc handler_ok(int code): bool { return route_ok(code); }\n");
        write_text(foo / "src/api/responses.ores",
            "namespace c; pub fnc response_delta(): int { return handler_ok(43) ? 0 : 1; }\n");

        // Explicitly declared different package group in the same manifest.
        // The lock selects packages.foo, so this colliding namespace must not
        // enter the executable namespace inventory.
        write_text(foo / "src/shadow.ores",
            "namespace a; pub fnc aggregate(int value): int { return 99; }\n");

        // Unlisted source must never be discovered by directory scanning.
        write_text(foo / "src/ambient.ores",
            "namespace a; pub fnc aggregate(int value): int { return 98; }\n");

        app_manifest = oreslang::load_verified_package_manifest_v2(
            app_manifest_path, read_text);
        foo_manifest = oreslang::load_verified_package_manifest_v2(
            foo_manifest_path, read_text);

        lock.root_package_instance_id = "app@sha:2222";
        lock.dependency_aliases = {{"foo", "foo@sha:1111"}};
        add_lock_entry(
            "app@sha:2222", app_manifest, "app");
        add_lock_entry(
            "foo@sha:1111", foo_manifest, "foo");
    }

    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    void add_lock_entry(
            const std::string& id,
            const oreslang::VerifiedPackageManifestV2& manifest,
            const std::string& group) {
        oreslang::VerifiedLockedPackageInstance entry;
        entry.manifest_path = manifest.manifest_path;
        entry.package_key = group;
        entry.manifest_sha256 = manifest.manifest_sha256;
        for (const auto& member : manifest.packages.at(group)) {
            entry.source_sha256.emplace(
                member.manifest_relative_path,
                oreslang::namespace_inventory_sha256(
                    read_text(member.physical_path)));
        }
        lock.package_instances[id] = std::move(entry);
    }

    oreslang::PinnedNamespaceProgramInput materialize(
            const oreslang::VerifiedPackageLockSnapshot& snapshot) const {
        return oreslang::build_pinned_namespace_program_from_manifests(
            app_manifest_path, {foo_manifest_path}, snapshot, app_main,
            read_text, read_text);
    }
};

} // namespace

int main() {
    try {
        if (oreslang::namespace_inventory_sha256("") !=
                "e3b0c44298fc1c149afbf4c8996fb924"
                "27ae41e4649b934ca495991b7852b855")
            throw std::runtime_error("SHA-256 empty-vector mismatch");
        if (oreslang::namespace_inventory_sha256("abc") !=
                "ba7816bf8f01cfea414140de5dae2223"
                "b00361a396177a9cb410ff61f20015ad")
            throw std::runtime_error("SHA-256 abc-vector mismatch");

        Fixture fixture;

        // Manifest parser is strict schema v2; schema 1 cannot silently enter
        // the package namespace pipeline.
        {
            const auto dir = fixture.root / "schema1";
            write_text(dir / ".ores.pkg.toml",
                "schema_version = \"1\"\n"
                "[project]\nname = \"bad\"\n"
                "[packages.bad]\nfiles = []\n");
            must_fail("schema_version = \"2\"", [&] {
                (void)oreslang::load_verified_package_manifest_v2(
                    dir / ".ores.pkg.toml", read_text);
            });
        }

        // Configured project.root is the filesystem authority, not the
        // manifest directory itself.
        {
            const auto dir = fixture.root / "configured-root";
            write_text(dir / "app/src/x.ores",
                "namespace x; pub fnc x(): int { return 1; }\n");
            write_text(dir / ".ores.pkg.toml",
                "schema_version = \"2\"\n"
                "[project]\nname = \"configured\"\nroot = \"app\"\n"
                "[source]\nroots = [\"src\"]\n"
                "[packages.configured]\nfiles = [\"src/x.ores\"]\n");
            const auto manifest =
                oreslang::load_verified_package_manifest_v2(
                    dir / ".ores.pkg.toml", read_text);
            if (manifest.project_root != fs::canonical(dir / "app"))
                throw std::runtime_error("configured project.root was ignored");
        }

        // Trailing material after a nominally closed TOML array must not be
        // silently ignored by the strict subset parser.
        {
            const auto dir = fixture.root / "array-tail";
            write_text(dir / "src/x.ores",
                "namespace x; pub fnc x(): int { return 1; }\n");
            write_text(dir / ".ores.pkg.toml",
                "schema_version = \"2\"\n"
                "[project]\nname = \"tail\"\n"
                "[packages.tail]\n"
                "files = [\"src/x.ores\"] junk ]\n");
            must_fail("malformed manifest array", [&] {
                (void)oreslang::load_verified_package_manifest_v2(
                    dir / ".ores.pkg.toml", read_text);
            });
        }

        // One physical file cannot acquire two package identities through
        // multiple manifest groups.
        {
            const auto dir = fixture.root / "duplicate-membership";
            write_text(dir / "src/x.ores", "namespace x; pub fnc x(): int { return 1; }\n");
            write_text(dir / ".ores.pkg.toml",
                "schema_version = \"2\"\n"
                "[project]\nname = \"dup\"\n"
                "[packages.one]\nfiles = [\"src/x.ores\"]\n"
                "[packages.two]\nfiles = [\"src/x.ores\"]\n");
            must_fail("more than one explicit package group", [&] {
                (void)oreslang::load_verified_package_manifest_v2(
                    dir / ".ores.pkg.toml", read_text);
            });
        }

        // Nested package boundaries remain independent authorities.
        {
            const auto dir = fixture.root / "nested";
            write_text(dir / "src/child/x.ores",
                "namespace x; pub fnc x(): int { return 1; }\n");
            write_text(dir / "src/child/.ores.pkg.toml",
                "schema_version = \"2\"\n"
                "[project]\nname = \"child\"\n");
            write_text(dir / ".ores.pkg.toml",
                "schema_version = \"2\"\n"
                "[project]\nname = \"parent\"\n"
                "[packages.parent]\nfiles = [\"src/child/x.ores\"]\n");
            must_fail("nested package boundary", [&] {
                (void)oreslang::load_verified_package_manifest_v2(
                    dir / ".ores.pkg.toml", read_text);
            });
        }

        // Discovery begins at the physical entry source. A nearer legacy
        // marker is an independent boundary and cannot be skipped in favor of
        // the valid ancestor modern manifest.
        {
            const auto dir = fixture.root / "discovery";
            write_text(dir / ".ores.pkg.toml",
                "schema_version = \"2\"\n"
                "[project]\nname = \"ancestor\"\n"
                "[packages.ancestor]\nfiles = [\"child/src/main.ores\"]\n");
            write_text(dir / "child/src/main.ores",
                "pub fnc main(): int { return 0; }\n");
            const auto discovered =
                oreslang::discover_verified_package_manifest_v2(
                    dir / "child/src/main.ores", read_text);
            if (discovered.manifest_path !=
                    fs::canonical(dir / ".ores.pkg.toml"))
                throw std::runtime_error("nearest modern manifest discovery mismatch");

            write_text(dir / "child/.oreslangc.cfg.toml", "legacy = true\n");
            must_fail("nearest package authority is legacy", [&] {
                (void)oreslang::discover_verified_package_manifest_v2(
                    dir / "child/src/main.ores", read_text);
            });
        }

        // Manifest authority is content-addressed.
        {
            auto stale = fixture.lock;
            stale.package_instances.at("foo@sha:1111").manifest_sha256 =
                std::string(64, '0');
            must_fail("manifest SHA-256 mismatch", [&] {
                (void)fixture.materialize(stale);
            });
        }

        // The build boundary reparses the current manifest bytes rather than
        // trusting a previously constructed C++ manifest object.
        {
            const auto original = read_text(fixture.foo_manifest_path);
            write_text(
                fixture.foo_manifest_path,
                original + "# stale-after-lock\n");
            must_fail("manifest SHA-256 mismatch", [&] {
                (void)fixture.materialize(fixture.lock);
            });
            write_text(fixture.foo_manifest_path, original);
        }

        // Membership list is part of the lock, not rediscovered from disk.
        {
            auto stale = fixture.lock;
            stale.package_instances.at("foo@sha:1111").source_sha256.erase(
                "src/data/models.ores");
            must_fail("source membership set mismatch", [&] {
                (void)fixture.materialize(stale);
            });
        }

        // Physical ownership is compared by file identity across manifests,
        // not merely by canonical spelling. If hard links are supported on
        // this filesystem, a dependency cannot claim the root entry inode.
        {
            const auto model = fixture.foo / "src/data/models.ores";
            const auto original = read_text(model);
            std::error_code link_error;
            fs::remove(model, link_error);
            if (link_error)
                throw std::runtime_error("could not prepare hard-link ownership test");
            fs::create_hard_link(fixture.app_main, model, link_error);
            if (!link_error) {
                must_fail("multiple locked package instances", [&] {
                    (void)fixture.materialize(fixture.lock);
                });
                fs::remove(model, link_error);
                if (link_error)
                    throw std::runtime_error("could not clear hard-link ownership test");
            }
            write_text(model, original);
        }

        // Source bytes are content-addressed too.
        {
            const auto path = fixture.foo / "src/data/models.ores";
            const auto original = read_text(path);
            write_text(path,
                "namespace b; pub fnc model_code(): int { return 999; }\n");
            must_fail("source SHA-256 mismatch", [&] {
                (void)fixture.materialize(fixture.lock);
            });
            write_text(path, original);
        }

        // The entry must be a member of the exact root package group.
        must_fail("entry source is not a member", [&] {
            (void)oreslang::build_pinned_namespace_program_from_manifests(
                fixture.app_manifest_path, {fixture.foo_manifest_path},
                fixture.lock, fixture.foo / "src/compute/math.ores",
                read_text, read_text);
        });

        // Same manifest, different packages.<key> is a distinct package
        // instance authority. Selecting shadow cannot silently pull foo files.
        {
            auto wrong_group = fixture.lock;
            auto& foo = wrong_group.package_instances.at("foo@sha:1111");
            foo.package_key = "shadow";
            foo.source_sha256.clear();
            for (const auto& member : fixture.foo_manifest.packages.at("shadow")) {
                foo.source_sha256.emplace(
                    member.manifest_relative_path,
                    oreslang::namespace_inventory_sha256(
                        read_text(member.physical_path)));
            }
            auto input = fixture.materialize(wrong_group);
            if (input.sources.size() != 2)
                throw std::runtime_error("wrong package group materialized extra sources");
            must_fail("no cross-package fallback", [&] {
                (void)oreslang::load_pinned_namespace_program(input);
            });
        }

        // Positive lock closure contains exactly app + ten foo files. Neither
        // the other declared package group nor ambient unlisted source enters.
        auto input = fixture.materialize(fixture.lock);
        if (input.sources.size() != 11)
            throw std::runtime_error("locked manifest inventory did not contain exactly 11 sources");
        if (input.pinned_aliases.at("app@sha:2222").at("foo")
                != "foo@sha:1111")
            throw std::runtime_error("dependency alias pin changed during materialization");
        for (const auto& source : input.sources) {
            if (source.source_unit_id.find("shadow.ores") != std::string::npos
                    || source.source_unit_id.find("ambient.ores") != std::string::npos)
                throw std::runtime_error("unselected source entered locked namespace inventory");
        }

        auto linked = oreslang::load_pinned_namespace_program(input);
        std::cout << oreslang::emit_verified_llvm_ir(linked);
        std::cout.flush();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native_manifest_namespace_inventory_test: "
                  << error.what() << '\n';
        return 1;
    }
}
