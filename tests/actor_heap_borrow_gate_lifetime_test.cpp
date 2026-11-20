#include "oreslang/runtime/ActorHeapRetirementProof.hpp"
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <utility>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace oreslang::runtime;

static void require(bool ok, const char* description) {
    if (!ok) throw std::runtime_error(description);
}

int main() {
    const ActorHeapIdentity owner{317, 2, 9};
    {
        ActorHeapBorrowGate gate(owner);
        auto first = gate.acquire(owner);
        auto second = gate.acquire(owner);
        require(first.has_value() && second.has_value(), "valid borrows rejected");
        require(!gate.seal_and_quiesced(owner), "live loans bypassed seal");
        require(gate.sealed() && !gate.quiesced(), "sealed state not retained");
        require(!gate.acquire(owner), "sealed gate admitted new frame");
        first->reset();
        require(!gate.quiesced(), "second loan ignored");
        second->reset();
        require(gate.quiesced(), "final loan not retired");
        require(gate.seal_and_quiesced(owner), "idempotent seal failed");
    }
    {
        ActorHeapBorrowGate gate(owner);
        auto loan = gate.acquire(owner);
        require(loan.has_value(), "move-only lease admission failed");
        auto moved = std::move(*loan);
        loan.reset();
        require(!gate.seal_and_quiesced(owner), "moved loan lost its pin");
        moved.reset();
        require(gate.quiesced(), "move-only loan cleanup leaked");
    }
#if defined(__unix__) || defined(__APPLE__)
    const pid_t child = fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        std::set_terminate([]() noexcept { std::_Exit(73); });
        auto* gate = new ActorHeapBorrowGate(owner);
        auto loan = gate->acquire(owner);
        if (!loan) std::_Exit(74);
        gate->seal_and_quiesced(owner);
        delete gate; // must terminate before storage can be freed.
        std::_Exit(75);
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child, "waitpid failed");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 73,
            "live borrow gate destruction failed to terminate");
#endif
    return 0;
}
