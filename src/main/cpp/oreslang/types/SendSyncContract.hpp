#pragma once

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::types {

// Conservative compiler-side marker reference, NOT actual LLVM guest lowering.
// The frontend must mint Shape/Primitive and trusted nominal provenance;
// source-level strings and guest-declared classes may not forge these tags.
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

inline bool worker_marker_proven(const WorkerType& type, WorkerMarker marker,
                                const VerifiedNominal& nominal,
                                std::size_t depth, std::size_t& remaining) {
    // Bounded, fail-closed checks on recursively supplied type descriptions.
    if (depth >= 128 || remaining == 0) return false;
    --remaining;
    const auto child = [&](const WorkerType& nested, WorkerMarker required) {
        return worker_marker_proven(nested, required, nominal, depth + 1, remaining);
    };

    switch (type.shape) {
    case Shape::Primitive: {
        if (!type.args.empty()) return false;
        const auto& n = type.name;
        // void/null/unknown host primitive names are never owned Send/Sync data.
        return n == "bool" || n == "int" || n == "uint"
            || n == "i8" || n == "i16" || n == "i32" || n == "i64"
            || n == "u8" || n == "u16" || n == "u32" || n == "u64"
            || n == "f32" || n == "f64" || n == "float" || n == "decimal"
            || n == "bigint" || n == "string"
            || n == "complex" || n == "complex64" || n == "complex128";
    }
    case Shape::Unknown: case Shape::Generic: case Shape::Callable:
    case Shape::ActorStorage: case Shape::Negative: return false;
    case Shape::BorrowShared:
        return type.args.size() == 1
               && child(type.args.front(), WorkerMarker::Sync);
    case Shape::BorrowMut:
        return type.args.size() == 1
               && child(type.args.front(), marker == WorkerMarker::Sync
                   ? WorkerMarker::Sync : WorkerMarker::Send);
    case Shape::Record:
        // Readonly *views* are not immutable allocations. Other aliases
        // may mutate the backing storage. Require a separate frozen value
        // proof before any structural record receives Sync.
        if (marker == WorkerMarker::Sync || type.args.empty()) return false;
        for (const auto& arg : type.args)
            if (!child(arg, WorkerMarker::Send)) return false;
        return true;
    case Shape::List:
        // A movable List<T> can be Send, but mutable backing lacks Sync.
        if (marker == WorkerMarker::Sync || type.args.size() != 1) return false;
        return child(type.args.front(), WorkerMarker::Send);
    case Shape::Tuple: case Shape::Union:
        if (type.args.empty()) return false;
        for (const auto& arg : type.args)
            if (!child(arg, marker)) return false;
        return true;
    case Shape::Named: {
        // A missing compiler nominal authority must deny the proof, not
        // escape as std::bad_function_call during source admission.
        if (!nominal) return false;
        const auto& n = type.name;
        const auto& args = type.args;
        // Intrinsic identity must be proven by compiler metadata before
        // accepting an Option/Result/IteratorResult spelling. The callback
        // is trusted compiler-only and never a guest-supplied assertion.
        if (n == "Option" || n == "IteratorResult")
            return args.size() == 1 && nominal(type, marker)
                   && child(args[0], marker);
        if (n == "Result")
            return args.size() == 2 && nominal(type, marker)
                   && child(args[0], marker) && child(args[1], marker);
        if (n == "OptionUnwrapError")
            return args.empty() && nominal(type, marker);
        // No worker-safe Channel/Arc/Mutex/actor/FFI ABI is proven yet.
        // Even a permissive nominal callback must not enable these.
        if (n == "Atomic" || n == "AtomicRef" || n == "AtomicBool" ||
            n == "AtomicInt" || n == "AtomicLong" ||
            n == "AtomicI8" || n == "AtomicI16" || n == "AtomicI32" ||
            n == "AtomicI64" || n == "AtomicU8" || n == "AtomicU16" ||
            n == "AtomicU32" || n == "AtomicU64" || n == "Box" ||
            n == "Channel" || n == "Arc" || n == "Weak" ||
            n == "Shared" || n == "RwLock" || n == "Mutex" ||
            n == "MutexGuard" || n == "SharedMutex" || n == "Future" ||
            n == "Task" || n == "ActorRef" || n == "ActorMail" ||
            n == "ActorGroup" || n == "Supervisor" || n == "TRef" ||
            n == "Ref" || n == "UnsafeCell" || n == "AtomicRef" ||
            n == "NativeHandle" || n == "FfiHandle" ||
            n == "RawPointer" || n == "NativePointer" ||
            n == "ThreadLocal" || n == "ThreadBound" ||
            n == "GpuContext" || n == "ThreadToken" ||
            n == "Iterator" || n == "AsyncIterator" ||
            n == "TcpConnection" || n == "HttpExchange" ||
            n == "HttpServer" || n == "HttpHandler")
            return false;
        for (const auto& arg : args)
            if (!child(arg, marker)) return false;
        return nominal(type, marker);
    }
    }
    return false;
}

inline bool worker_marker(const WorkerType& type, WorkerMarker marker,
                          const VerifiedNominal& nominal) {
    // Invalid enum representations and failing trusted proof callbacks must
    // never promote a type to Send/Sync or escape as guest-visible exceptions.
    if (marker != WorkerMarker::Send && marker != WorkerMarker::Sync) return false;
    std::size_t remaining = 4096;
    try {
        return worker_marker_proven(type, marker, nominal, 0, remaining);
    } catch (...) {
        return false;
    }
}

inline void require_worker_marker(const WorkerType& type, WorkerMarker marker,
                                  const VerifiedNominal& nominal) {
    if (!worker_marker(type, marker, nominal))
        throw std::invalid_argument("worker-boundary missing trusted Send/Sync proof");
}
}  // namespace oreslang::types
