#include "oreslang/frontend/HostLogicalUnitIds.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <iostream>

namespace fs = std::filesystem;
using oreslang::frontend::HostLogicalUnitIds;
using oreslang::frontend::validate_logical_unit_id;
static void write(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream file(p);
    if (!file) throw std::runtime_error("fixture creation failed");
    file << "pub fnc main():int{return 42;}\n";
}
template<class F> static void rejects(const std::string& code, F&& f) {
    try { f(); } catch (const std::exception& e) {
        if (std::string(e.what()).find(code) != std::string::npos) return;
        throw std::runtime_error("wrong rejection: " + std::string(e.what()));
    }
    throw std::runtime_error("unsafe unit identity admitted");
}
int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tmp = fs::temp_directory_path() / ("atto-unit-identity-" + std::to_string(stamp));
    struct Cleanup { fs::path path; ~Cleanup(){std::error_code ec; fs::remove_all(path,ec);} } cleanup{tmp};
    const auto root = tmp / "project";
    const auto main = root / "main.atto";
    const auto producer = root / "lib" / "producer.atto";
    write(main); write(producer);
    write(tmp / "outside.atto");
    HostLogicalUnitIds ids(root);
    if (ids.admit(main) != "project/main.atto" ||
        ids.admit(producer) != "project/lib/producer.atto" ||
        ids.admit(main) != "project/main.atto")
        throw std::runtime_error("incorrect project logical ID");
    rejects("ATTO_UNIT_ROOT_ESCAPE", [&]{ ids.admit(tmp / "outside.atto"); });
    rejects("ATTO_UNIT_ROOT_ESCAPE", [&]{ ids.admit(root / ".." / "outside.atto"); });
    rejects("ATTO_UNIT_ID_INVALID", []{ validate_logical_unit_id("/tmp/consumer.atto"); });
    rejects("ATTO_UNIT_ID_INVALID", []{ validate_logical_unit_id("C:/producer.atto"); });
    rejects("ATTO_UNIT_ID_INVALID", []{ validate_logical_unit_id("project\\alias.atto"); });
    rejects("ATTO_UNIT_ID_INVALID", []{ validate_logical_unit_id("project/../producer.atto"); });
    rejects("ATTO_UNIT_ID_INVALID", []{ validate_logical_unit_id("project/\xE2\x80\xAEproducer.atto"); });
    rejects("ATTO_UNIT_ID_INVALID", []{ validate_logical_unit_id("project/\xC0\xAF.atto"); });
    rejects("ATTO_UNIT_ID_INVALID", []{ validate_logical_unit_id(std::string(4097, 'a')); });
    // Full codepoint validation: allow ordinary emoji but reject Cf
    // controls, zero-width characters and Unicode tag characters.
    validate_logical_unit_id("project/emoji-\xF0\x9F\x99\x82.atto");
    rejects("ATTO_UNIT_ID_INVALID", []{
        validate_logical_unit_id("project/soft-hyphen-\xC2\xAD.atto");
    });
    rejects("ATTO_UNIT_ID_INVALID", []{
        validate_logical_unit_id("project/zero-width-\xE2\x80\x8B.atto");
    });
    rejects("ATTO_UNIT_ID_INVALID", []{
        validate_logical_unit_id("project/tag-\xF3\xA0\x80\x81.atto");
    });
    rejects("ATTO_UNIT_ID_INVALID", []{
        validate_logical_unit_id("project/with//empty.atto");
    });
    rejects("ATTO_UNIT_ID_INVALID", []{
        validate_logical_unit_id("project/trailing.atto/");
    });
    // The same logical unit cannot change its descriptor-backed source identity.
    oreslang::frontend::SourceStamp original{};
    original.device = 1;
    original.inode = 200;
    original.size = 42;
    original.valid = true;
    ids.bind_opened_source("project/main.atto", original);
    ids.bind_opened_source("project/main.atto", original);
    auto replaced = original;
    replaced.inode = 201;
    rejects("ATTO_UNIT_SOURCE_CHANGED", [&]{
        ids.bind_opened_source("project/main.atto", replaced);
    });
    rejects("ATTO_UNIT_SOURCE_UNVERIFIED", [&]{
        ids.bind_opened_source("project/unknown.atto", original);
    });
    rejects("ATTO_UNIT_SOURCE_ALIAS", [&]{
        ids.bind_opened_source("project/lib/producer.atto", original);
    });
    const auto nested = root / "nested";
    write(nested / ".atto.pkg.toml");
    write(nested / "foreign.atto");
    rejects("ATTO_UNIT_NESTED_PACKAGE", [&]{
        ids.admit(nested / "foreign.atto");
    });
    const auto parent_link = root / "alias-directory";
    fs::create_directory_symlink(root / "lib", parent_link);
    rejects("ATTO_UNIT_SOURCE_ALIAS", [&]{
        ids.admit(parent_link / "producer.atto");
    });
    const auto link = root / "link.atto";
    fs::create_symlink(main, link);
    rejects("ATTO_UNIT_SOURCE_ALIAS", [&]{ ids.admit(link); });
    const auto hard = root / "hard.atto";
    fs::create_hard_link(main, hard);
    rejects("ATTO_UNIT_SOURCE_ALIAS", [&]{ ids.admit(hard); });
    rejects("ATTO_UNIT_SOURCE_ALIAS", [&]{ ids.admit(main); });
    std::cout << "ATTO_LOGICAL_UNIT_HOST_ONLY_OK\n";
}
