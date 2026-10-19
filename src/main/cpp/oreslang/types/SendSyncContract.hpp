#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::types {

// Conservative native/compiler-side marker model. This is NOT yet source-level
// syntax or a certified LLVM lowering. Unknown FFI/host allocations fail closed.
enum class WorkerMarker { Send, Sync };
enum class Shape {
    Primitive, Named, Record, Tuple, List, Union, BorrowShared, BorrowMut,
    Generic, Unknown, Callable, ActorStorage, Negative
};
struct WorkerType {
    Shape shape;
    std::string name;
    std::vector<WorkerType> args;
};
using VerifiedNominal = std::function<bool(const WorkerType&, WorkerMarker)>;

inline bool worker_marker(const WorkerType& type, WorkerMarker marker,
                          const VerifiedNominal& nominal) {
    switch (type.shape) {
    case Shape::Primitive: return type.name != "null";
    case Shape::Unknown: case Shape::Generic: case Shape::Callable:
    case Shape::ActorStorage: case Shape::Negative: return false;
    case Shape::BorrowShared:
        return type.args.size() == 1 &&
               worker_marker(type.args.front(), WorkerMarker::Sync, nominal);
    case Shape::BorrowMut:
        return type.args.size() == 1 &&
               worker_marker(type.args.front(), marker == WorkerMarker::Sync
                   ? WorkerMarker::Sync : WorkerMarker::Send, nominal);
    case Shape::Record: case Shape::Tuple: case Shape::List: case Shape::Union:
        if (type.args.empty()) return false;
        for (const auto& arg : type.args)
            if (!worker_marker(arg, marker, nominal)) return false;
        return true;
    case Shape::Named: {
        const auto& n = type.name;
        const auto& args = type.args;
        if (n == "Option" || n == "IteratorResult")
            return args.size() == 1 && worker_marker(args[0], marker, nominal);
        if (n == "Result")
            return args.size() == 2 && worker_marker(args[0], marker, nominal)
                   && worker_marker(args[1], marker, nominal);
        if (n == "OptionUnwrapError") return args.empty();
        if (n == "Channel")
            return args.size() == 1 &&
                   worker_marker(args[0], WorkerMarker::Send, nominal);
        if (n == "Shared" || n == "ActorRef" || n == "Mutex" ||
            n == "MutexGuard" || n == "SharedMutex" || n == "Future" ||
            n == "Task" || n == "NativeHandle" || n == "GpuContext" ||
            n == "ThreadToken" || n == "Iterator" || n == "AsyncIterator" ||
            n == "TcpConnection" || n == "HttpExchange" ||
            n == "HttpServer" || n == "HttpHandler")
            return false;
        for (const auto& arg : args)
            if (!worker_marker(arg, marker, nominal)) return false;
        return nominal(type, marker);
    }
    }
    return false;
}
inline void require_worker_marker(const WorkerType& type, WorkerMarker marker,
                                  const VerifiedNominal& nominal) {
    if (!worker_marker(type, marker, nominal))
        throw std::invalid_argument("worker-boundary missing trusted Send/Sync proof");
}
}  // namespace oreslang::types
