#ifndef ORESLANG_TENANT_DOMAIN_HOST_H
#define ORESLANG_TENANT_DOMAIN_HOST_H
/*
 * Trusted-host native tenant-domain admission substrate, ABI version 1.
 * NOT an in-process sandbox for arbitrary native code. A native caller could
 * forge domain IDs, access process memory, or bypass this library entirely.
 * Only a trusted runtime/supervisor may call these functions.
 *
 * No socket or host I/O is performed here. Successful submission returns
 * ORES_TENANT_PENDING, NEVER delivery success. The broker must independently
 * revalidate authority before dispatch, and revocation races remain a concern
 * until an authenticated broker with atomic dispatch admission is integrated.
 */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ORES_TENANT_ABI_VERSION 1u
#define ORES_TENANT_MAX_DATAGRAM 1200u

typedef struct OresTenantHost OresTenantHost;
typedef uint64_t OresDomainId;
typedef struct { uint64_t handle; uint64_t generation; } OresTenantCap;
typedef enum {
  ORES_TENANT_OK = 0,
  ORES_TENANT_PENDING = -115,
  ORES_TENANT_DENIED = -13,
  ORES_TENANT_INVALID = -22,
  ORES_TENANT_IO_ERROR = -5,
  ORES_TENANT_QUOTA = -122,
  ORES_TENANT_REVOKED = -125
} OresTenantStatus;
typedef struct {
  uint32_t abi_version;
  uint32_t max_socket_capabilities;
  uint64_t max_queued_datagrams;
  uint16_t allowed_udp_loopback_port;
  uint16_t reserved;
  uint32_t reserved2;
} OresDomainPolicy;
typedef struct {
  OresDomainId owner;
  OresTenantCap capability;
  uint16_t port;
  uint16_t length;
  uint8_t payload[ORES_TENANT_MAX_DATAGRAM];
} OresPendingDatagram;

OresTenantHost *ores_tenant_host_new(void);
void ores_tenant_host_delete(OresTenantHost *host);
OresTenantStatus ores_tenant_domain_create(OresTenantHost *host, const OresDomainPolicy *policy, OresDomainId *out);
OresTenantStatus ores_tenant_domain_revoke(OresTenantHost *host, OresDomainId domain);
OresTenantStatus ores_tenant_open_loopback_udp(OresTenantHost *host, OresDomainId authenticated_host_domain,
                                               uint16_t port, OresTenantCap *out);
OresTenantStatus ores_tenant_revoke_capability(OresTenantHost *host, OresDomainId authenticated_host_domain,
                                               OresTenantCap cap);
OresTenantStatus ores_tenant_submit_datagram(OresTenantHost *host, OresDomainId authenticated_host_domain,
                                            OresTenantCap cap, const uint8_t *bytes, size_t size);
/* Trusted supervisor drain. Returns PENDING when one intent is copied; OK if empty. */
OresTenantStatus ores_tenant_next_pending(OresTenantHost *host, OresPendingDatagram *out);
/* Diagnostic recheck, not atomic broker admission or authorization for native guests. */
OresTenantStatus ores_tenant_revalidate_pending(OresTenantHost *host, const OresPendingDatagram *entry);
#ifdef __cplusplus
}
#endif
#endif
