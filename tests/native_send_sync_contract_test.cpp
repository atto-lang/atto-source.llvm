#include "oreslang/types/SendSyncContract.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace oreslang::types;

int main() {
    const VerifiedNominal none = [](const WorkerType&, WorkerMarker) { return false; };
    const VerifiedNominal permissive = [](const WorkerType&, WorkerMarker) { return true; };
    const VerifiedNominal knownIntrinsic = [](const WorkerType& t, WorkerMarker) {
        return t.name == "Option" || t.name == "Result" ||
               t.name == "IteratorResult" || t.name == "OptionUnwrapError";
    };
    const WorkerType number{Shape::Primitive, "int", {}};
    const WorkerType unknown{Shape::Unknown, "", {}};
    const WorkerType record{Shape::Record, "", {number, number}};
    const WorkerType list{Shape::List, "", {number}};
    const WorkerType tuple{Shape::Tuple, "", {number, number}};

    assert(worker_marker(number, WorkerMarker::Send, none));
    assert(worker_marker(number, WorkerMarker::Sync, none));
    assert(worker_marker(record, WorkerMarker::Send, none));
    assert(!worker_marker(record, WorkerMarker::Sync, none));
    assert(worker_marker(list, WorkerMarker::Send, none));
    assert(!worker_marker(list, WorkerMarker::Sync, none));
    assert(worker_marker(tuple, WorkerMarker::Sync, none));
    assert(!worker_marker(unknown, WorkerMarker::Send, none));

    // Structural readonly metadata alone is not a frozen backing proof.
    assert(!worker_marker(WorkerType{Shape::BorrowShared, "", {record}},
                          WorkerMarker::Send, none));
    assert(!worker_marker(WorkerType{Shape::BorrowShared, "", {list}},
                          WorkerMarker::Sync, none));
    assert(!worker_marker(WorkerType{Shape::Named, "Option", {record}},
                          WorkerMarker::Sync, knownIntrinsic));

    // Even a maximally permissive nominal proof callback must not override
    // unimplemented synchronizing handles and guest-fabricated capability names.
    for (const char* name : {"Atomic", "AtomicRef", "AtomicBool",
                                    "AtomicInt", "AtomicLong", "AtomicI8",
                                    "AtomicI16", "AtomicI32", "AtomicI64",
                                    "AtomicU8", "AtomicU16", "AtomicU32",
                                    "AtomicU64", "Box", "Arc", "Weak", "Shared", "Mutex",
                                    "RwLock", "Channel", "Task", "ActorRef",
                                    "NativeHandle", "FfiHandle", "UnsafeCell"}) {
        const WorkerType handle{Shape::Named, name, {number}};
        assert(!worker_marker(handle, WorkerMarker::Send, permissive));
        assert(!worker_marker(handle, WorkerMarker::Sync, permissive));
    }

    // Names without authenticated intrinsic provenance cannot gain the
    // Option/Result fast path, even when the payload is a valid primitive.
    const WorkerType option{Shape::Named, "Option", {number}};
    const WorkerType result{Shape::Named, "Result", {number, number}};
    assert(!worker_marker(option, WorkerMarker::Send, none));
    assert(!worker_marker(result, WorkerMarker::Sync, none));
    assert(worker_marker(option, WorkerMarker::Send, knownIntrinsic));
    assert(worker_marker(result, WorkerMarker::Sync, knownIntrinsic));

    // No nominal proof callback is a checked denial, never a C++
    // bad_function_call (which could bypass normal source error handling).
    const VerifiedNominal missingNominal{};
    assert(worker_marker(number, WorkerMarker::Send, missingNominal));
    assert(!worker_marker(option, WorkerMarker::Send, missingNominal));
    assert(!worker_marker(result, WorkerMarker::Sync, missingNominal));
    assert(!worker_marker(WorkerType{Shape::Named, "NotVerified", {}},
                          WorkerMarker::Send, missingNominal));
    bool missingProofCaught = false;
    try { require_worker_marker(option, WorkerMarker::Send, missingNominal); }
    catch (const std::invalid_argument&) { missingProofCaught = true; }
    assert(missingProofCaught);

    // An out-of-range marker and a throwing nominal verifier cannot turn a
    // failed proof into an allowed worker crossing or uncaught C++ exception.
    assert(!worker_marker(number, static_cast<WorkerMarker>(99), permissive));
    const VerifiedNominal throwingProof = [](const WorkerType&, WorkerMarker) -> bool {
        throw std::runtime_error("nominal proof failed");
    };
    assert(!worker_marker(option, WorkerMarker::Send, throwingProof));
    assert(!worker_marker(WorkerType{Shape::Named, "Throwing", {}},
                          WorkerMarker::Sync, throwingProof));
    bool thrownProofCaught = false;
    try { require_worker_marker(option, WorkerMarker::Send, throwingProof); }
    catch (const std::invalid_argument&) { thrownProofCaught = true; }
    assert(thrownProofCaught);

    const WorkerType owned{Shape::Named, "OwnedOnly", {}};
    const VerifiedNominal ownerOnly = [](const WorkerType& t, WorkerMarker m) {
        return t.name == "OwnedOnly" && m == WorkerMarker::Send;
    };
    assert(worker_marker(owned, WorkerMarker::Send, ownerOnly));
    assert(!worker_marker(owned, WorkerMarker::Sync, ownerOnly));
    assert(worker_marker(WorkerType{Shape::BorrowMut, "", {owned}},
                         WorkerMarker::Send, ownerOnly));
    assert(!worker_marker(WorkerType{Shape::BorrowShared, "", {owned}},
                          WorkerMarker::Send, ownerOnly));

    for (const char* invalid : {"", "void", "null", "Foo",
                                       "Thread", "Object"}) {
        assert(!worker_marker(WorkerType{Shape::Primitive, invalid, {}},
                              WorkerMarker::Send, permissive));
    }
    assert(!worker_marker(WorkerType{Shape::Primitive, "int", {number}},
                          WorkerMarker::Send, none));
    assert(!worker_marker(WorkerType{Shape::Named, "Result", {number}},
                          WorkerMarker::Send, knownIntrinsic));

    // Fail closed under deep and wide type inputs; never recurse into a
    // stack overflow or approve a partially examined aggregate.
    WorkerType deep = number;
    for (int i = 0; i != 200; ++i)
        deep = WorkerType{Shape::BorrowShared, "", {std::move(deep)}};
    assert(!worker_marker(deep, WorkerMarker::Send, none));
    const WorkerType wide{Shape::Tuple, "",
                          std::vector<WorkerType>(4100, number)};
    assert(!worker_marker(wide, WorkerMarker::Sync, none));

    bool caught = false;
    try { require_worker_marker(unknown, WorkerMarker::Send, none); }
    catch (const std::invalid_argument&) { caught = true; }
    assert(caught);
    std::cout << "NATIVE_SEND_SYNC_CONTRACT_PASS\n";
}
