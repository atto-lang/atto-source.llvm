#include "oreslang/types/SendSyncContract.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace oreslang::types;

int main() {
    const VerifiedNominal none = [](const WorkerType&, WorkerMarker){ return false; };
    const WorkerType number{Shape::Primitive, "int", {}};
    const WorkerType unknown{Shape::Unknown, "", {}};
    const WorkerType data{Shape::Record, "", {number, number}};
    assert(worker_marker(data, WorkerMarker::Send, none));
    assert(worker_marker(data, WorkerMarker::Sync, none));
    assert(!worker_marker(unknown, WorkerMarker::Send, none));
    assert(!worker_marker(WorkerType{Shape::Named, "Shared", {number}},
                          WorkerMarker::Sync, none));

    const WorkerType owned{Shape::Named, "OwnedOnly", {}};
    const VerifiedNominal resolver=[](const WorkerType& t, WorkerMarker m) {
        return t.name=="OwnedOnly" && m==WorkerMarker::Send;
    };
    assert(worker_marker(owned, WorkerMarker::Send, resolver));
    assert(!worker_marker(owned, WorkerMarker::Sync, resolver));
    assert(worker_marker(WorkerType{Shape::BorrowMut, "", {owned}},
                         WorkerMarker::Send, resolver));
    assert(!worker_marker(WorkerType{Shape::BorrowShared, "", {owned}},
                          WorkerMarker::Send, resolver));
    assert(worker_marker(WorkerType{Shape::Named, "Channel", {owned}},
                         WorkerMarker::Sync, resolver));
    assert(!worker_marker(WorkerType{Shape::Named, "Channel", {unknown}},
                          WorkerMarker::Send, resolver));
    assert(!worker_marker(WorkerType{Shape::Named, "Result", {number}},
                          WorkerMarker::Send, resolver));
    bool caught=false;
    try { require_worker_marker(unknown,WorkerMarker::Send,none); }
    catch(const std::invalid_argument&) { caught=true; }
    assert(caught);
    std::cout<<"NATIVE_SEND_SYNC_CONTRACT_PASS\n";
}
