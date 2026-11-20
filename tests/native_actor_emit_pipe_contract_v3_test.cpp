#include "oreslang/runtime/ActorEmitPipeContractV3.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using oreslang::runtime::ActorEmitStageV3;
using oreslang::runtime::check_actor_emit_pipeline_v3;

static void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

static void reject(const std::vector<ActorEmitStageV3>& stages,
                   const std::string& code) {
    try { (void)check_actor_emit_pipeline_v3(stages); }
    catch (const std::invalid_argument& e) {
        require(std::string(e.what()).find(code + ":") == 0,
                "actor emit gate used unexpected diagnostic");
        return;
    }
    throw std::runtime_error("actor emit gate admitted forbidden shape: " + code);
}

static ActorEmitStageV3 stage(std::string id, std::string actor,
                             std::string input,
                             std::vector<std::string> emit = {}) {
    return {std::move(id), std::move(actor), std::move(input), std::move(emit), false};
}

int main() {
    // Same v3 vectors as Java and .NET; this is type admission, NOT
    // native guest execution or authenticated actor-to-actor routing.
    const auto scalar = check_actor_emit_pipeline_v3({
        stage("p0", "Parse", "int", {"int"}),
        stage("v0", "Validate", "int", {"int", "int"}),
        stage("s0", "Sink", "int")});
    require(scalar.size() == 2 && scalar[0].source == "p0"
            && scalar[0].target == "v0" && scalar[0].payload == "int"
            && scalar[1].target == "s0", "scalar v3 chain wrong");

    const auto heterogeneous = check_actor_emit_pipeline_v3({
        stage("parse-0", "Worker", "int", {"bool"}),
        stage("validate-1", "Worker", "bool", {"bool"}),
        stage("sink-2", "Sink", "bool")});
    require(heterogeneous.size() == 2 && heterogeneous[0].payload == "bool"
            && heterogeneous[1].payload == "bool", "int->bool typed v3 chain wrong");

    reject({stage("a", "Parse", "int", {"int", "bool"}),
            stage("b", "Sink", "int")}, "E_ACTOR_PIPE_EMIT_TYPE");
    reject({stage("a", "Parse", "int", {"bool"}),
            stage("b", "Sink", "int")}, "E_ACTOR_PIPE_EMIT_TYPE");
    reject({stage("a", "Sink", "int"),
            stage("b", "Validate", "int")}, "E_ACTOR_PIPE_STAGE_NO_EMIT");
    reject({stage("a", "Parse", "int", {"int"}),
            stage("b", "Sink", "int", {"int"})}, "E_ACTOR_PIPE_SINK_EMIT");
    reject({stage("same", "Parse", "int", {"int"}),
            stage("same", "Sink", "int")}, "E_ACTOR_PIPE_DUPLICATE_INSTANCE");

    auto supervisor = stage("b", "Supervisor", "int");
    supervisor.supervisor = true;
    reject({stage("a", "Parse", "int", {"int"}), supervisor},
           "E_ACTOR_PIPE_SUPERVISOR");
    reject({stage("a", "Parse", "int", {"int"})}, "E_ACTOR_PIPE_LIMIT");

    std::vector<ActorEmitStageV3> too_many;
    for (int i = 0; i < 17; ++i) {
        too_many.push_back(stage("instance" + std::to_string(i), "Worker",
                                 "int", i == 16 ?
                                 std::vector<std::string>{} :
                                 std::vector<std::string>{"int"}));
    }
    reject(too_many, "E_ACTOR_PIPE_LIMIT");
    reject({stage("", "Parse", "int", {"int"}),
            stage("b", "Sink", "int")}, "E_ACTOR_PIPE_IDENTITY");
    reject({stage("a", "Parse", "int", {"int"}),
            stage("b", "Sink", "string")}, "E_ACTOR_PIPE_PAYLOAD_DOMAIN");
    reject({stage("a", "Parse", "int", {"float"}),
            stage("b", "Sink", "int")}, "E_ACTOR_PIPE_PAYLOAD_DOMAIN");
    reject({stage("a", "Parse", "int", {"int"}),
            stage("b", "Sink", "int", {"bool"})}, "E_ACTOR_PIPE_SINK_EMIT");
    reject({stage(std::string(257, 'x'), "Parse", "int", {"int"}),
            stage("b", "Sink", "int")}, "E_ACTOR_PIPE_IDENTITY");
    reject({stage("a", std::string(1, static_cast<char>(0xc0)), "int", {"int"}),
            stage("b", "Sink", "int")}, "E_ACTOR_PIPE_IDENTITY");

    // Shared Java/.NET v3 identity admission vectors: strict 256 UTF-8 bytes.
    require(check_actor_emit_pipeline_v3({
        stage(std::string(256, 'x'), "Parse", "int", {"bool"}),
        stage("sink", "Sink", "bool")}).size() == 1,
        "256-byte ASCII actor identity must be admitted");
    std::string unicode_256;
    for (int i = 0; i < 64; ++i) unicode_256 += "\xF0\x9F\x8C\x8D";
    require(check_actor_emit_pipeline_v3({
        stage(unicode_256, "Parse", "int", {"bool"}),
        stage("sink", "Sink", "bool")}).size() == 1,
        "256-byte multibyte actor identity must be admitted");
    reject({stage(std::string(257, 'x'), "Parse", "int", {"bool"}),
            stage("sink", "Sink", "bool")}, "E_ACTOR_PIPE_IDENTITY");
    reject({stage(unicode_256 + "\xF0\x9F\x8C\x8D", "Parse", "int", {"bool"}),
            stage("sink", "Sink", "bool")}, "E_ACTOR_PIPE_IDENTITY");
    reject({stage((std::string("abc") + std::string(1, char(0)) + "def"), "Parse", "int", {"bool"}),
            stage("sink", "Sink", "bool")}, "E_ACTOR_PIPE_IDENTITY");
    reject({stage(std::string(1'000'000, 'x'), "Parse", "int", {"bool"}),
            stage("sink", "Sink", "bool")}, "E_ACTOR_PIPE_IDENTITY");

    // Compiler evidence must be bounded even if all observations agree.
    require(check_actor_emit_pipeline_v3({
        stage("p0", "Parse", "int", std::vector<std::string>(1024, "bool")),
        stage("sink", "Sink", "bool")}).size() == 1,
        "1024 typed emissions must remain valid");
    reject({
        stage("p0", "Parse", "int", std::vector<std::string>(1025, "bool")),
        stage("sink", "Sink", "bool")
    }, "E_ACTOR_PIPE_EMIT_LIMIT");
    reject({
        stage("p0", "Parse", "int", std::vector<std::string>(1000000, "int")),
        stage("sink", "Sink", "int")
    }, "E_ACTOR_PIPE_EMIT_LIMIT");

    for (int n = 2; n <= 16; ++n) {
        std::vector<ActorEmitStageV3> chain;
        for (int i = 0; i < n; ++i) {
            chain.push_back(stage("i" + std::to_string(i), "Worker",
                                  i == 0 ? "int" : "bool",
                                  i == n - 1 ? std::vector<std::string>{}
                                               : std::vector<std::string>{"bool"}));
        }
        require(check_actor_emit_pipeline_v3(chain).size()
                == static_cast<std::size_t>(n - 1), "v3 chain length changed");
        chain[1].receive_type = "int"; // negative type mutation
        reject(chain, "E_ACTOR_PIPE_EMIT_TYPE");
    }
}
