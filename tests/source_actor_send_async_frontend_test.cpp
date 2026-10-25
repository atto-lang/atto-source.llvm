#include "oreslang/frontend/Parser.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#define CHECK(x) do { if (!(x)) throw std::runtime_error("send_async frontend: " #x); } while (false)

static std::string program(const std::string& body) {
    return R"(define actor Worker as {
      let total: int = 0;
      pub fnc on_message(int n): int { total = total + n; return total; }
    }
    pub fnc main(): int {
      const worker: ActorRef<Worker> = spawn Worker();
    )" + body + " }";
}

static void rejects(const std::string& source, const std::string& diagnostic) {
    bool rejected = false;
    try { (void)oreslang::parse_program(source); }
    catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(diagnostic) == std::string::npos)
            throw std::runtime_error("expected '" + diagnostic + "', got '" +
                                     error.what() + "'");
        rejected = true;
    }
    CHECK(rejected);
}

int main() {
    auto admitted = oreslang::parse_program(program(
        "await worker.send_async(1); await worker.send_async(2); "
        "worker.send(3); stop worker; return 43;"));
    CHECK(admitted.functions.size() == 1);
    const auto& main = admitted.functions.front();
    CHECK(main.actor_sends.size() == 3 && main.actor_actions.size() == 4);
    CHECK(main.actor_sends[0].await_admission);
    CHECK(main.actor_sends[1].await_admission);
    CHECK(!main.actor_sends[2].await_admission);
    for (std::size_t i = 0; i != 3; ++i) {
        CHECK(main.actor_actions[i].kind == oreslang::ActorAction::Kind::Send);
        CHECK(main.actor_actions[i].index == i);
        CHECK(main.actor_sends[i].reference == "worker");
    }
    CHECK(main.actor_actions[3].kind == oreslang::ActorAction::Kind::Stop);

    rejects(program("worker.send_async(1); stop worker; return 43;"),
            "native send_async requires direct await");
    rejects(program("await missing.send_async(1); stop worker; return 43;"),
            "unknown actor message receiver");
    rejects(program("stop worker; await worker.send_async(1); return 43;"),
            "actor message sent after stop");
    rejects(program("await worker.send_async(true); stop worker; return 43;"),
            "native actor message payload");
    rejects(program("await worker.send_async(worker); stop worker; return 43;"),
            "unknown parameter");
    rejects(program("const Future<void> pending = worker.send_async(1); "
                    "stop worker; return 43;"), "get_ready");
    rejects(program("await worker.get_ready(); await worker.send_async(1); "
                    "stop worker; return 43;"),
            "native source supports only one direct ActorRef lifecycle await");
    rejects(program("await worker; await worker.send_async(1); "
                    "stop worker; return 43;"),
            "native source supports only one direct ActorRef lifecycle await");
    rejects(program("stop worker; await worker.get_done(); "
                    "await worker.send_async(1); return 43;"),
            "native source supports only one direct ActorRef lifecycle await");
    rejects(R"(define actor Worker as {
      let total: int = 0;
      pub fnc on_message(int n): int { await self.send_async(n); return n; }
    }
    pub fnc main(): int {
      const worker = spawn Worker(); worker.send(1); stop worker; return 43;
    })", "native send_async direct await requires trusted zero-argument main");

    // These checks intentionally establish rejection, not Channel/Select
    // execution parity. A real compiler positive control above must pass.
    for (const auto* boundary : {
            "select { }", "nb select { }", "readch input;", "writech output, 1;"})
        rejects(program(std::string(boundary) + " stop worker; return 43;"),
                "native async/channel/select boundary unsupported");

    std::cout << "SOURCE_SEND_ASYNC_FRONTEND_PASS positive=1 negative=14\n";
}
