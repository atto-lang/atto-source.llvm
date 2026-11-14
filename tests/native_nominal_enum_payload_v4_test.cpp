#include "oreslang/frontend/NominalEnumV1.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

// Emits actual scalar guest source for oreslang-llvmc; no host-side result
// execution is counted as native AOT evidence.
static const std::string source = R"atto(
define enum WorkerMsg {
    Run { job_id: int },
    Cancel { job_id: int },
    Stop
}
pub fnc main(): int {
    val WorkerMsg msg = WorkerMsg.Run { job_id: 37 };
    do match msg over
        on Run { job_id } as r -> { return r.job_id; }
        on Cancel as c -> { return c.job_id; }
        on Stop -> { return 0; }
    end
}
)atto";

static std::string changed(std::string original, const std::string& old,
                           const std::string& replacement) {
    const auto offset = original.find(old);
    if (offset == std::string::npos) throw std::runtime_error("mutation anchor missing");
    original.replace(offset, old.size(), replacement);
    return original;
}

static void rejected(const std::string& source_text, const std::string& expected) {
    try { (void)oreslang::frontend::lower_nominal_enum_v1(source_text); }
    catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(expected) != std::string::npos) return;
        throw std::runtime_error("unexpected admission reason: " + std::string(error.what())
                                 + ", expected: " + expected);
    }
    throw std::runtime_error("invalid nominal payload source admitted: " + expected);
}

int main() {
    try {
        const auto lowered = oreslang::frontend::lower_nominal_enum_v1(source);
        if (lowered.find("val int msg = 1;") == std::string::npos
                || lowered.find("val int msg_case_payload = 37;") == std::string::npos
                || lowered.find("if msg == 1 then return msg_case_payload; fi") == std::string::npos
                || lowered.find("if msg == 2 then return msg_case_payload; fi") == std::string::npos
                || lowered.find("return 0;") == std::string::npos
                || lowered.find("define enum WorkerMsg") != std::string::npos)
            throw std::runtime_error("expected typed payload static CFG lowering absent");

        rejected(changed(source, "on Run { job_id } as r",
                 "on Run { missing } as r"), "pattern field is not declared");
        rejected(changed(source, "on Run { job_id } as r",
                 "on Run { job_id } as job_id"), "whole alias conflicts");
        rejected(changed(source, "return r.job_id;",
                 "return r.unknown;"), "expected 'job_id'");
        rejected(changed(source, "return r.job_id;",
                 "return unknown;"), "unknown or unbound case field");
        rejected(changed(source, "WorkerMsg.Run { job_id: 37 }",
                 "Other.Run { job_id: 37 }"), "constructor has foreign nominal owner");
        rejected(changed(source, "WorkerMsg.Run { job_id: 37 }",
                 "WorkerMsg.Run"), "expected '{'");
        rejected(changed(source, "WorkerMsg.Run { job_id: 37 }",
                 "WorkerMsg.Run { bad: 37 }"), "expected 'job_id'");
        rejected(changed(source, "Run { job_id: int },",
                 "Run { job_id: Bytes },"), "payload AOT admits only Copy int");
        rejected(changed(source, "Run { job_id: int },",
                 "Run { job_id: int, other: int },"), "exactly one int field");
        rejected(changed(source, "on Stop ->",
                 "on Unknown ->"), "match has unknown case");
        rejected(changed(source, "on Cancel as c ->",
                 "on Run as c ->"), "duplicate match arm");
        rejected(changed(source, "        on Stop -> { return 0; }\n",
                 ""), "nonexhaustive nominal case match");
        rejected(changed(source, "on Run { job_id } as r",
                 "on Run { job_id, other } as r"), "exactly one named integer field");
        rejected(changed(source, "val WorkerMsg msg = WorkerMsg.Run { job_id: 37 }",
                 "val WorkerMsg msg = 1"), "expected unreserved identifier");

        std::cout << "ATTO_LLVM_NOMINAL_PAYLOAD_V4_HOST_PASS=14\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ATTO_LLVM_NOMINAL_PAYLOAD_V4_HOST_FAIL=" << error.what() << '\n';
        return 1;
    }
}
