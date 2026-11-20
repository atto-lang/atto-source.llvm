#pragma once
/*
 * Trusted-host-only Offset32 to actor-bound asynchronous filesystem adapter.
 *
 * This is NOT an exported guest callback table, a bearer capability, or
 * confinement. Only a trusted scheduler may supply the freshly validated
 * live turn, cage window, tenant number and bound service. Never construct
 * these from guest-supplied pointers/identities. ABI-v5 guest admission and
 * native hostile-guest execution remain DISABLED.
 *
 * This adapter does not itself authenticate TLS, perform signed-image
 * admission, or pin mapped guest memory. A future scheduler hook MUST call
 * authenticated_fs_turn before invoking it; the actor turn must remain
 * executing for the entire call. The live witness MUST be recomputed from
 * scheduler state on each operation, not supplied by guest code.
 */
#include "oreslang/runtime/TenantVmFsTurnAuth.hpp"
#include "oreslang/runtime/TenantVmIoAbiV5.h"
#include "atto/host_fs_async.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace oreslang::runtime::vm::internal {

class HostFsDispatch final {
    const FsTurnAuth bound_;
    const std::uint64_t tenant_id_;
    std::uint8_t* const cage_;
    const std::uint64_t cage_bytes_;
    atto_fs_async_service* const service_;

    bool trusted(const FsTurnAuth& live) const noexcept {
        if (!same_fs_turn(bound_, live) || !tenant_id_ || !cage_ || !service_ ||
            !cage_bytes_ || cage_bytes_ > (std::uint64_t{1} << 32)) return false;
        const std::uint64_t actor_end =
            std::uint64_t{bound_.actor_offset} + bound_.actor_bytes;
        if (actor_end > cage_bytes_) return false;
        const atto_fs_host_actor owner{tenant_id_, bound_.tenant_generation,
                                       bound_.actor_ref, bound_.actor_incarnation};
        return atto_fs_async_owner_matches(service_, &owner) == 1;
    }

    atto_fs_host_actor owner() const noexcept {
        return {tenant_id_, bound_.tenant_generation,
                bound_.actor_ref, bound_.actor_incarnation};
    }

    bool actor_range(std::uint32_t offset, std::uint32_t length) const noexcept {
        return actor_fs_span(bound_, offset, length) &&
               std::uint64_t{offset} + length <= cage_bytes_;
    }

    bool output_range(std::uint32_t offset, std::uint32_t length) const noexcept {
        return scratch_fs_span(bound_, offset, length) &&
               std::uint64_t{offset} + length <= cage_bytes_;
    }

    bool load_path(std::array<char, ATTO_FS_SCOPE_LIMIT>& storage,
                   std::uint32_t offset, std::uint32_t length) const noexcept {
        if (!length || length >= storage.size() || !actor_range(offset, length))
            return false;
        const void* data = cage_ + offset;
        // An embedded zero must NOT truncate the validated path and
        // silently change the object being authorized by the broker.
        if (std::memchr(data, 0, length)) return false;
        std::memcpy(storage.data(), data, length);
        storage[length] = '\0';
        return true;
    }

    static std::int64_t status(int result, bool polling) noexcept {
        switch (result) {
        case 0: return 0;
        case -EAGAIN: return polling ? ORES_VM_IO_PENDING : ORES_VM_IO_WOULD_BLOCK;
        case -EACCES:
        case -EPERM: return ORES_VM_IO_PERMISSION_DENIED;
        case -ESTALE: return ORES_VM_IO_STALE_TICKET;
        case -ECANCELED: return ORES_VM_IO_CANCELLED;
        case -EINPROGRESS: return ORES_VM_IO_WRITE_INDETERMINATE;
        case -EALREADY: return ORES_VM_IO_ALREADY_DONE;
        case -EFBIG:
        case -EMSGSIZE:
        case -EOVERFLOW: return ORES_VM_IO_TOO_LARGE;
        default: return ORES_VM_IO_HOST_ERROR;
        }
    }

public:
    HostFsDispatch(const FsTurnAuth& authenticated_bound, std::uint64_t trusted_tenant_id,
                   std::uint8_t* host_cage_base, std::uint64_t host_cage_window_bytes,
                   atto_fs_async_service* actor_owned_service) noexcept
        : bound_(authenticated_bound), tenant_id_(trusted_tenant_id),
          cage_(host_cage_base), cage_bytes_(host_cage_window_bytes),
          service_(actor_owned_service) {}

    std::int64_t submit_read(const FsTurnAuth& live, std::uint32_t path_offset,
                             std::uint32_t path_length, std::uint32_t limit) const noexcept {
        if (!trusted(live)) return ORES_VM_IO_PERMISSION_DENIED;
        if (!limit || limit > ORES_VM_IO_MAX_BYTES) return ORES_VM_IO_TOO_LARGE;
        std::array<char, ATTO_FS_SCOPE_LIMIT> path{};
        if (!load_path(path, path_offset, path_length)) return ORES_VM_IO_PERMISSION_DENIED;
        const auto actor = owner();
        std::uint64_t ticket = 0;
        const int result = atto_fs_async_submit_read(service_, &actor, path.data(), limit, &ticket);
        if (result) return status(result, false);
        if (!ticket || ticket > static_cast<std::uint64_t>(INT64_MAX))
            return ORES_VM_IO_HOST_ERROR;
        return static_cast<std::int64_t>(ticket);
    }

    std::int64_t submit_write(const FsTurnAuth& live, std::uint32_t path_offset,
                              std::uint32_t path_length, std::uint32_t payload_offset,
                              std::uint32_t payload_length) const noexcept {
        if (!trusted(live)) return ORES_VM_IO_PERMISSION_DENIED;
        if (payload_length > ORES_VM_IO_MAX_BYTES) return ORES_VM_IO_TOO_LARGE;
        std::array<char, ATTO_FS_SCOPE_LIMIT> path{};
        if (!load_path(path, path_offset, path_length) ||
            !actor_range(payload_offset, payload_length))
            return ORES_VM_IO_PERMISSION_DENIED;
        const auto actor = owner();
        std::uint64_t ticket = 0;
        // The async service synchronously copies this actor-local input into
        // its owned heap request. Workers never retain a cage pointer.
        const int result = atto_fs_async_submit_write(
            service_, &actor, path.data(), cage_ + payload_offset, payload_length, &ticket);
        if (result) return status(result, false);
        if (!ticket || ticket > static_cast<std::uint64_t>(INT64_MAX))
            return ORES_VM_IO_HOST_ERROR;
        return static_cast<std::int64_t>(ticket);
    }

    std::int64_t poll(const FsTurnAuth& live, std::uint64_t ticket,
                      std::uint32_t output_offset, std::uint32_t capacity) const noexcept {
        if (!trusted(live)) return ORES_VM_IO_PERMISSION_DENIED;
        if (capacity > ORES_VM_IO_MAX_BYTES) return ORES_VM_IO_TOO_LARGE;
        // Even a PENDING poll must reject a foreign/overflowed publication
        // destination. The service also rechecks revocation at publication.
        if (!output_range(output_offset, capacity)) return ORES_VM_IO_PERMISSION_DENIED;
        if (!ticket) return ORES_VM_IO_STALE_TICKET;
        const auto actor = owner();
        std::size_t copied = 0;
        const int result = atto_fs_async_poll(
            service_, &actor, ticket, cage_ + output_offset, capacity, &copied);
        if (result) return status(result, true);
        if (copied > capacity || copied > ORES_VM_IO_MAX_BYTES)
            return ORES_VM_IO_HOST_ERROR;
        return static_cast<std::int64_t>(copied);
    }

    std::int32_t cancel(const FsTurnAuth& live, std::uint64_t ticket) const noexcept {
        if (!trusted(live)) return static_cast<std::int32_t>(ORES_VM_IO_PERMISSION_DENIED);
        if (!ticket) return static_cast<std::int32_t>(ORES_VM_IO_STALE_TICKET);
        const auto actor = owner();
        return static_cast<std::int32_t>(status(
            atto_fs_async_cancel(service_, &actor, ticket), false));
    }
};

} // namespace oreslang::runtime::vm::internal
