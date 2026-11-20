#pragma once
// Phase 0: compile-time actor-local inverted *class* MIR admission.
// NO .atto syntax enablement, arena allocation, runtime actor authentication,
// native code emission, or escaped pointers. Independent MIR re-verification
// must run before an eventual backend emits memory operations.
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::dop::class_mir {

enum class ScalarKind { I64, Bool, F64, Pointer };

enum class Kind {
    Allocate, Push, CallMoveBy, ReadField, Snapshot, ReadSnapshotField,
    EraseSwap, Suspend, ExportHandle, Close
};

struct Schema {
    std::string namespace_name;
    std::string actor_name;
    std::string class_name;
    std::string first_field;
    std::string second_field;
    std::uint64_t owner_actor = 0;
    std::size_t capacity = 0;
    ScalarKind first_kind = ScalarKind::I64;
    ScalarKind second_kind = ScalarKind::I64;
};

// Source-lowered operations refer to LOCAL symbolic handles only; they cannot
// carry a native address, guest pointer, or creator-chosen row generation.
struct SourceOperation {
    Kind kind;
    std::uint64_t acting_actor = 0;
    std::string handle;
    std::string field;
    std::string snapshot_name;
    std::string receiver_class;
    std::string method;
    bool mut_self = false;
};

struct Guard {
    bool actor = false;     // authenticated by backend, never by guest int
    bool owner = false;     // owner exists and remains live
    bool generation = false;// slot issuer + generation checked
    bool bounds = false;    // logical row/slot bounds
    bool capacity = false;  // insertion capacity/overflow
    bool arithmetic = false;// checked i64 receiver computation before commit
};

inline bool operator==(Guard a, Guard b) noexcept {
    return a.actor == b.actor && a.owner == b.owner &&
           a.generation == b.generation && a.bounds == b.bounds &&
           a.capacity == b.capacity && a.arithmetic == b.arithmetic;
}

inline Guard guards_for(Kind kind) {
    switch (kind) {
        case Kind::Allocate: return {true, false, false, false, false, false};
        case Kind::Push: return {true, true, false, false, true, false};
        case Kind::CallMoveBy: return {true, true, true, true, false, true};
        case Kind::ReadField:
        case Kind::Snapshot:
        case Kind::EraseSwap: return {true, true, true, true, false, false};
        case Kind::ReadSnapshotField: return {true, false, false, false, false, false};
        case Kind::Close: return {true, true, false, false, false, false};
        case Kind::Suspend:
        case Kind::ExportHandle: throw std::logic_error(
            "inverted class handle export and suspension are not admitted");
    }
    throw std::logic_error("unknown inverted class opcode");
}

struct Instruction {
    SourceOperation op;
    Guard required;
};

class VerifiedModule final {
    Schema schema_;
    std::vector<Instruction> code_;

    static void checked_name(const std::string& name, const char* label) {
        if (name.empty() || name.size() > 128)
            throw std::invalid_argument(std::string("invalid inverted class ") + label);
        for (const unsigned char c : name) {
            if (!((c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_'))
                throw std::invalid_argument("invalid inverted class identifier");
        }
        if (name[0] >= '0' && name[0] <= '9')
            throw std::invalid_argument("inverted class identifier starts with a digit");
    }

    static void validate(const Schema& schema,
                         const std::vector<Instruction>& code) {
        checked_name(schema.namespace_name, "namespace");
        checked_name(schema.actor_name, "actor");
        checked_name(schema.class_name, "class");
        checked_name(schema.first_field, "first field");
        checked_name(schema.second_field, "second field");
        if (schema.first_field == schema.second_field)
            throw std::invalid_argument("duplicate inverted class field");
        if (schema.first_kind != ScalarKind::I64 ||
                schema.second_kind != ScalarKind::I64)
            throw std::invalid_argument("phase-zero class MIR admits only two i64 fields");
        if (schema.owner_actor == 0 || schema.capacity == 0 ||
                schema.capacity > 256)
            throw std::invalid_argument("inverted class actor/capacity invalid");
        if (code.size() < 2 || code.size() > 4096 ||
                code.front().op.kind != Kind::Allocate ||
                code.back().op.kind != Kind::Close)
            throw std::invalid_argument("inverted class MIR requires allocate and terminal close");

        std::unordered_set<std::string> issued;
        std::unordered_set<std::string> live;
        std::unordered_set<std::string> snapshots;
        std::size_t length = 0;
        for (std::size_t i = 0; i < code.size(); ++i) {
            const auto& inst = code[i];
            const auto& op = inst.op;
            if (op.acting_actor != schema.owner_actor)
                throw std::logic_error("cross-actor inverted class operation");
            if (!(inst.required == guards_for(op.kind)))
                throw std::logic_error("inverted class MIR runtime guards missing or inconsistent");
            if (op.kind != Kind::CallMoveBy &&
                    (!op.receiver_class.empty() || !op.method.empty() || op.mut_self))
                throw std::logic_error("unadmitted inverted class receiver metadata");
            if (op.kind != Kind::ReadField && op.kind != Kind::ReadSnapshotField &&
                    !op.field.empty())
                throw std::logic_error("unexpected inverted class field metadata");
            if (op.kind != Kind::Snapshot && op.kind != Kind::ReadSnapshotField &&
                    !op.snapshot_name.empty())
                throw std::logic_error("unexpected inverted class snapshot metadata");
            if (op.kind != Kind::Push && op.kind != Kind::CallMoveBy &&
                    op.kind != Kind::ReadField && op.kind != Kind::Snapshot &&
                    op.kind != Kind::EraseSwap && !op.handle.empty())
                throw std::logic_error("unexpected inverted class handle metadata");
            switch (op.kind) {
                case Kind::Allocate:
                    if (i != 0) throw std::logic_error("duplicate class owner allocation");
                    break;
                case Kind::Push:
                    checked_name(op.handle, "local handle");
                    if (length >= schema.capacity || snapshots.count(op.handle) ||
                            !issued.insert(op.handle).second)
                        throw std::logic_error("capacity exceeded or handle binding reused");
                    live.insert(op.handle);
                    ++length;
                    break;
                case Kind::CallMoveBy:
                    if (!live.count(op.handle) || op.receiver_class != schema.class_name ||
                            op.method != "move_by" || !op.mut_self)
                        throw std::logic_error("invalid mutable inverted class receiver");
                    break;
                case Kind::ReadField:
                    if (!live.count(op.handle))
                        throw std::logic_error("stale inverted class handle used after erase");
                    if (op.field != schema.first_field && op.field != schema.second_field)
                        throw std::logic_error("invalid inverted class field read");
                    break;
                case Kind::Snapshot:
                    checked_name(op.snapshot_name, "snapshot binding");
                    if (!live.count(op.handle) ||
                            !snapshots.insert(op.snapshot_name).second ||
                            issued.count(op.snapshot_name))
                        throw std::logic_error("invalid or duplicate detached class snapshot");
                    break;
                case Kind::ReadSnapshotField:
                    if (!snapshots.count(op.snapshot_name) ||
                            (op.field != schema.first_field && op.field != schema.second_field))
                        throw std::logic_error("unknown inverted class snapshot field");
                    break;
                case Kind::EraseSwap:
                    if (!live.erase(op.handle))
                        throw std::logic_error("stale inverted class handle used after erase");
                    --length;
                    break;
                case Kind::Close:
                    if (i != code.size() - 1 || !op.handle.empty())
                        throw std::logic_error("inverted class close must be terminal");
                    break;
                case Kind::Suspend:
                case Kind::ExportHandle:
                    throw std::logic_error("inverted class source operation not supported");
                default: throw std::logic_error("unknown inverted class MIR opcode");
            }
        }
    }

    VerifiedModule(Schema schema, std::vector<Instruction> code)
        : schema_(std::move(schema)), code_(std::move(code)) {
        validate(schema_, code_);
    }
public:
    // Trusted source->MIR builder calculates the required guard contract.
    // This is not a runtime caller-authentication token or an LLVM emitter.
    static VerifiedModule lower(Schema schema,
                                const std::vector<SourceOperation>& source) {
        std::vector<Instruction> code;
        if (source.size() > 4096)
            throw std::invalid_argument("inverted class source operation budget exceeded");
        code.reserve(source.size());
        for (const auto& op : source)
            code.push_back(Instruction{op, guards_for(op.kind)});
        return VerifiedModule(std::move(schema), std::move(code));
    }

    // Revalidates independently imported MIR. A future emitter must accept
    // only this verified result and still implement every required guard.
    static VerifiedModule import_checked(Schema schema,
                                         std::vector<Instruction> untrusted) {
        return VerifiedModule(std::move(schema), std::move(untrusted));
    }
    const Schema& schema() const noexcept { return schema_; }
    const std::vector<Instruction>& code() const noexcept { return code_; }
};

} // namespace oreslang::dop::class_mir
