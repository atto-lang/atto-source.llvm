#include "oreslang/types/ManagedRefLayoutPlan.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace oreslang::analysis;
using oreslang::runtime::CompressedActorRef;

static void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

int main() {
    try {
        RefLayoutEvidence proven{};
        proven.actor_heap_bound = true;
        proven.owner_proven = true;
        proven.lifetime_proven = true;
        proven.maximum_arena_span = 1024;
        auto compressed = plan_managed_reference(proven);
        check(compressed && compressed->storage == PhysicalRefStorage::ActorOffset32
                 && compressed->field_bytes == 4
                 && compressed->needs_actor_base_at_dereference,
              "proven actor-local reference must lower to i32 and require actor base");
        auto unsigned_ref = proven;
        unsigned_ref.lifetime_proven = false;
        check(!plan_managed_reference(unsigned_ref),
              "unknown lifetime was incorrectly compressed");
        unsigned_ref = proven;
        unsigned_ref.actor_heap_bound = false;
        check(!plan_managed_reference(unsigned_ref),
              "reference without authenticated heap base was admitted");
        unsigned_ref = proven;
        unsigned_ref.owner_proven = false;
        check(!plan_managed_reference(unsigned_ref),
              "foreign actor provenance was admitted");
        auto boundary = proven;
        boundary.maximum_arena_span = CompressedActorRef::max_addressable_span;
        check(plan_managed_reference(boundary)->storage
                  == PhysicalRefStorage::ActorOffset32,
              "maximum compact span rejected");
        ++boundary.maximum_arena_span;
        check(plan_managed_reference(boundary)->storage
                  == PhysicalRefStorage::ActorAddress64,
              "oversized compressed arena did not switch to native64");
        boundary = proven;
        boundary.minimum_alignment = 4;
        check(!plan_managed_reference(boundary),
              "unaligned native64 layout incorrectly admitted");
        boundary = proven;
        boundary.force_native64 = true;
        check(plan_managed_reference(boundary)->field_bytes == 8,
              "explicit native64 selection ignored");
        // 32-bit hosts (i686, ARMv7, wasm32) use the same 4-byte ref32 word.
        auto host32 = proven;
        host32.target_pointer_bits = 32;
        auto plan32 = plan_managed_reference(host32);
        check(plan32 && plan32->storage == PhysicalRefStorage::ActorOffset32
                 && plan32->field_bytes == 4 && plan32->alignment == 8
                 && plan32->needs_actor_base_at_dereference,
              "32-bit target must lower actor-local refs to the shared ref32 word");
        check(plan32->field_bytes == compressed->field_bytes
                 && plan32->alignment == compressed->alignment,
              "ref32 field layout differs between 32-bit and 64-bit targets");
        host32.maximum_arena_span = UINT32_MAX;
        check(plan_managed_reference(host32)->storage
                  == PhysicalRefStorage::ActorOffset32,
              "full 32-bit address-space arena rejected");
        host32.maximum_arena_span = UINT64_C(1) << 32;
        check(!plan_managed_reference(host32),
              "arena larger than a 32-bit address space admitted on a 32-bit target");
        host32 = proven;
        host32.target_pointer_bits = 32;
        host32.lifetime_proven = false;
        check(!plan_managed_reference(host32),
              "32-bit target admitted an unproven lifetime");
        host32 = proven;
        host32.target_pointer_bits = 32;
        host32.minimum_alignment = 4;
        check(!plan_managed_reference(host32),
              "32-bit target admitted sub-quantum alignment for ref32");
        host32 = proven;
        host32.target_pointer_bits = 32;
        host32.force_native64 = true;
        check(plan_managed_reference(host32)->storage
                  == PhysicalRefStorage::ActorAddress64
                 && plan_managed_reference(host32)->field_bytes == 8,
              "explicit native64 parity mode ignored on a 32-bit target");
        auto host16 = proven;
        host16.target_pointer_bits = 16;
        check(!plan_managed_reference(host16),
              "unsupported pointer width admitted");

        RefLayoutEvidence dense{};
        dense.domain = ReferenceDomain::DenseRow;
        dense.actor_heap_bound = dense.owner_proven = dense.lifetime_proven =
            dense.dense_index_proven = true;
        dense.maximum_dense_rows = 1024;
        check(plan_managed_reference(dense)->storage
                  == PhysicalRefStorage::DenseIndex32,
              "proven ECS row identity should use i32 index");
        dense.dense_index_proven = false;
        check(!plan_managed_reference(dense),
              "unverified dense-row lifetime admitted");
        dense.dense_index_proven = true;
        dense.maximum_dense_rows = UINT32_MAX;
        check(!plan_managed_reference(dense),
              "dense index with reserved null overflow was admitted");

        RefLayoutEvidence cross{};
        cross.domain = ReferenceDomain::CrossActor;
        auto actor_capability = plan_managed_reference(cross);
        check(actor_capability
                  && actor_capability->storage == PhysicalRefStorage::ActorCapability
                  && actor_capability->field_bytes == 0,
              "ActorRef ABI must remain opaque, not be compressed as actor pointer");
        RefLayoutEvidence shared{};
        shared.domain = ReferenceDomain::SharedImmutable;
        check(!plan_managed_reference(shared), "unapproved shared region admitted");
        shared.share_authorized = true;
        auto share = plan_managed_reference(shared);
        check(share && share->storage == PhysicalRefStorage::SharedHandle
                  && share->field_bytes == 0,
              "shared immutable reference must not become actor-local pointer");
        RefLayoutEvidence ffi{};
        ffi.domain = ReferenceDomain::ForeignNative;
        check(plan_managed_reference(ffi)->field_bytes == 8,
              "native 64-bit ABI must not shrink FFI pointers");
        ffi.target_pointer_bits = 32;
        check(plan_managed_reference(ffi)->field_bytes == 4,
              "native 32-bit FFI layout incorrectly widened");
        ffi.target_pointer_bits = 128;
        check(!plan_managed_reference(ffi), "unsupported pointer ABI accepted");

        std::cout << "PASS compiler managed ref layout admission, native64 fallback, "
                     "ActorRef and FFI isolation\\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL managed ref layout plan: " << error.what() << '\n';
        return 1;
    }
}
