#include "oreslang/types/NativeHasFutureContract.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace oreslang;

int main() {
    constexpr NativeAwaitDescriptor direct {
        NativeAwaitTargetKind::ActorRef, NativeAwaitForm::DirectHasFuture,
        "Worker", "worker", true, false, false, ""
    };
    constexpr NativeAwaitDescriptor method {
        NativeAwaitTargetKind::ActorRef, NativeAwaitForm::ExplicitGetReady,
        "Worker", "worker", true, false, false, ""
    };
    constexpr NativeAwaitDescriptor typed {
        NativeAwaitTargetKind::FutureVoid, NativeAwaitForm::FutureValue,
        "Worker", "worker", true, true, true, "worker"
    };
    static_assert(resolve_native_has_future(direct).abi == NativeAwaitAbi::SourceActorReadyControl);
    static_assert(resolve_native_has_future(method).returns_void);
    static_assert(resolve_native_has_future(typed).consumes_future_token);
    constexpr NativeAwaitDescriptor foreign_future {
        NativeAwaitTargetKind::FutureVoid, NativeAwaitForm::FutureValue,
        "Worker", "worker", true, true, true, "other"
    };
    static_assert(resolve_native_has_future(foreign_future).abi == NativeAwaitAbi::Unsupported);
    assert(!resolve_native_has_future(direct).consumes_future_token);
    assert(resolve_native_has_future(method).abi == resolve_native_has_future(direct).abi);
    assert(resolve_native_has_future(typed).abi == resolve_native_has_future(direct).abi);

    constexpr NativeAwaitTargetKind kinds[] = {
        NativeAwaitTargetKind::Unknown, NativeAwaitTargetKind::Scalar,
        NativeAwaitTargetKind::Class, NativeAwaitTargetKind::ActorRef,
        NativeAwaitTargetKind::FutureVoid, NativeAwaitTargetKind::FutureI64
    };
    constexpr NativeAwaitForm forms[] = {
        NativeAwaitForm::None, NativeAwaitForm::DirectHasFuture,
        NativeAwaitForm::ExplicitGetReady, NativeAwaitForm::ExplicitGetDone,
        NativeAwaitForm::FutureValue
    };
    std::uint64_t admitted = 0;
    std::uint64_t rejected = 0;
    for (auto kind : kinds) {
        for (auto form : forms) {
            for (int live = 0; live <= 1; ++live) {
                for (int provenance = 0; provenance <= 1; ++provenance) {
                    for (int unconsumed = 0; unconsumed <= 1; ++unconsumed) {
                        for (int name = 0; name <= 1; ++name) {
                            for (int nominal = 0; nominal <= 1; ++nominal) {
                                NativeAwaitDescriptor candidate{kind, form,
                                    nominal ? "Worker" : "",
                                    name ? "worker" : "",
                                    live != 0, provenance != 0, unconsumed != 0,
                                    kind == NativeAwaitTargetKind::FutureVoid ? "worker" : ""};
                                const auto actual = resolve_native_has_future(candidate);
                                const bool valid_actor = kind == NativeAwaitTargetKind::ActorRef &&
                                    (form == NativeAwaitForm::DirectHasFuture ||
                                     form == NativeAwaitForm::ExplicitGetReady);
                                const bool valid_future = kind == NativeAwaitTargetKind::FutureVoid &&
                                    form == NativeAwaitForm::FutureValue && provenance && unconsumed;
                                const bool accepted = live && name && nominal &&
                                    (valid_actor || valid_future);
                                assert((actual.abi == NativeAwaitAbi::SourceActorReadyControl) == accepted);
                                assert(actual.returns_void == accepted);
                                assert(actual.consumes_future_token == (accepted && valid_future));
                                if (kind == NativeAwaitTargetKind::FutureVoid) {
                                    auto wrong_producer = candidate;
                                    wrong_producer.future_producer_actor_identity = "other";
                                    assert(resolve_native_has_future(wrong_producer).abi == NativeAwaitAbi::Unsupported);
                                }
                                if (accepted) ++admitted; else ++rejected;
                            }
                        }
                    }
                }
            }
        }
    }
    assert(admitted == 9);
    assert(rejected == 951);
    std::cout << "PASS native HasFuture checked admission matrix: "
              << admitted << " admitted, " << rejected << " rejected\n";
}
