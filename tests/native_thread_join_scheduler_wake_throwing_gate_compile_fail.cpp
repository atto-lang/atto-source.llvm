#include "oreslang/runtime/NativeThreadJoinSchedulerWakeV1.hpp"

#include <memory>

// Intentional compile-fail: the join reaper must not accept callbacks that
// can throw across the native completion boundary.
struct ThrowingGate {
    bool notify_ready() { return true; }
};

int main() {
    using namespace oreslang::runtime;
    return NativeThreadJoinSchedulerWakeV1::subscribe(
        std::shared_ptr<NativeJoinCompletionV1>{},
        std::weak_ptr<ThrowingGate>{},
        std::weak_ptr<NativeJoinSchedulerWakeQueueV1>{}) ? 0 : 1;
}
