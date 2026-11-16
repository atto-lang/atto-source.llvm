#include "oreslang/backend/NativeSourceLogManifestV1.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <fstream>
#include <iterator>

static const std::string kFixture = R"json({
  "schema": "atto.telemetry.source-ids.v1",
  "source_unit": "examples/telemetry/log-info-verified.atto",
  "routines": [
    {"symbol": "main():int",
     "id": "rid_d5f8e146ba97430dac6bf17430477e2a"}
  ],
  "sites": [
    {"symbol": "main():int", "message": "hammer.started",
     "routine_id": "rid_d5f8e146ba97430dac6bf17430477e2a",
     "id": "sid_652e1632e7464fdb936fc40fe24f1c7b"},
    {"symbol": "main():int", "message": "hammer.finished",
     "routine_id": "rid_d5f8e146ba97430dac6bf17430477e2a",
     "id": "sid_77777777777777777777777777777777"}
  ]
})json";

static bool rejected(std::string source,
                     const std::string& unit = "examples/telemetry/log-info-verified.atto") {
    try {
        (void)oreslang::parse_checked_source_log_manifest_v1(source, unit);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

static std::string swap_one(std::string source, const std::string& old,
                            const std::string& replacement) {
    auto position = source.find(old);
    assert(position != std::string::npos);
    source.replace(position, old.size(), replacement);
    return source;
}

int main() {
#ifdef ATTO_SOURCE_LOG_MANIFEST_FIXTURE
    // Use the actual checked-in fixture as well as independent adversarial
    // vectors. This catches drift between documentation and compiler intake.
    std::ifstream actual_file(ATTO_SOURCE_LOG_MANIFEST_FIXTURE, std::ios::binary);
    assert(actual_file.good());
    const std::string actual_bytes(
        (std::istreambuf_iterator<char>(actual_file)),
        std::istreambuf_iterator<char>());
    auto from_source = oreslang::parse_checked_source_log_manifest_v1(
        actual_bytes, "examples/telemetry/log-info-verified.atto");
    assert(from_source.routines.size() == 1);
    assert(from_source.sites.size() == 2);
#endif
    auto manifest = oreslang::parse_checked_source_log_manifest_v1(
        kFixture, "examples/telemetry/log-info-verified.atto");
    assert(manifest.source_unit == "examples/telemetry/log-info-verified.atto");
    assert(manifest.routines.size() == 1);
    assert(manifest.routines[0].symbol == "main");
    assert(manifest.routines[0].arity == 0);
    assert(manifest.sites.size() == 2);
    assert(manifest.sites[0].message == "hammer.started");
    assert(manifest.sites[1].site_id != manifest.sites[0].site_id);

    assert(rejected(kFixture, "examples/telemetry/other.atto"));
    assert(rejected(kFixture, "../examples/telemetry/log-info-verified.atto"));
    assert(rejected(kFixture + " garbage"));
    assert(rejected(swap_one(kFixture,
        R"("schema": "atto.telemetry.source-ids.v1",)",
        R"("schema": "atto.telemetry.source-ids.v1", "schema":"atto.telemetry.source-ids.v1",)")));
    assert(rejected(swap_one(kFixture, "atto.telemetry.source-ids.v1",
        "atto.telemetry.source-ids.v2")));
    assert(rejected(swap_one(kFixture, "rid_d5f8e146ba97430dac6bf17430477e2a",
        "sid_d5f8e146ba97430dac6bf17430477e2a")));
    assert(rejected(swap_one(kFixture, "sid_652e1632e7464fdb936fc40fe24f1c7b",
        "rid_652e1632e7464fdb936fc40fe24f1c7b")));
    assert(rejected(swap_one(kFixture, "sid_77777777777777777777777777777777",
        "sid_652e1632e7464fdb936fc40fe24f1c7b")));
    assert(rejected(swap_one(kFixture, "hammer.finished", "hammer.started")));
    assert(rejected(swap_one(kFixture, "hammer.started", "danger\\nlog")));
    assert(rejected(swap_one(kFixture, "hammer.started", "danger\nlog")));
    assert(rejected(swap_one(kFixture,
        R"("source_unit": "examples/telemetry/log-info-verified.atto",)",
        R"("source_unit": "../other.atto",)")));
    assert(rejected(swap_one(kFixture,
        R"("message": "hammer.started",)",
        R"("message": "hammer.started", "message": "hammer.started",)")));
    assert(rejected(swap_one(kFixture,
        R"("message": "hammer.started",)",
        R"("message": "hammer.started", "unauthorized": "1",)")));
    assert(rejected(swap_one(kFixture, "main():int", "main(int,):int")));
    assert(rejected(swap_one(kFixture, "main():int", "main(int):float")));
    assert(rejected(kFixture + std::string(140000, ' ')));
    assert(rejected("[]"));
    assert(rejected("{}"));
    assert(rejected("{\"schema\":\"atto.telemetry.source-ids.v1\"}"));

    std::cout << "LLVM_SOURCE_LOG_MANIFEST_HOST_PASS:20" << std::endl;
}
