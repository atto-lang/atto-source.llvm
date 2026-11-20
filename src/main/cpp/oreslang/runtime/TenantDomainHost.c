#define _POSIX_C_SOURCE 200809L
#include "TenantDomainHost.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define DOMAIN_SLOTS 256u
#define CAP_SLOTS 1024u
#define QUEUE_SLOTS 128u

typedef struct {
  OresDomainId id;
  uint64_t max_queued, submitted;
  uint32_t max_caps, open_caps;
  uint16_t port;
  int active;
} Domain;
typedef struct {
  OresTenantCap token;
  OresDomainId owner;
  uint16_t port;
  int active;
} Capability;
typedef struct {
  OresPendingDatagram data;
  uint64_t sequence;
  int active;
} QueueEntry;
struct OresTenantHost {
  pthread_mutex_t mutex;
  Domain domains[DOMAIN_SLOTS];
  Capability capabilities[CAP_SLOTS];
  QueueEntry queue[QUEUE_SLOTS];
  uint64_t next_domain, next_cap, next_sequence;
};

static Domain *find_domain(OresTenantHost *h, OresDomainId id) {
  for (size_t i = 0; i != DOMAIN_SLOTS; ++i)
    if (h->domains[i].active && h->domains[i].id == id) return &h->domains[i];
  return NULL;
}
static Capability *find_capability(OresTenantHost *h, OresTenantCap cap) {
  for (size_t i = 0; i != CAP_SLOTS; ++i)
    if (h->capabilities[i].active && h->capabilities[i].token.handle == cap.handle &&
        h->capabilities[i].token.generation == cap.generation) return &h->capabilities[i];
  return NULL;
}
static OresTenantStatus lock_host(OresTenantHost *h) {
  return !h ? ORES_TENANT_INVALID : pthread_mutex_lock(&h->mutex) ? ORES_TENANT_IO_ERROR : ORES_TENANT_OK;
}
static OresTenantStatus done(OresTenantHost *h, OresTenantStatus status) {
  if (pthread_mutex_unlock(&h->mutex)) return ORES_TENANT_IO_ERROR;
  return status;
}
OresTenantHost *ores_tenant_host_new(void) {
  OresTenantHost *h = calloc(1, sizeof *h);
  if (!h) return NULL;
  if (pthread_mutex_init(&h->mutex, NULL)) { free(h); return NULL; }
  h->next_domain = h->next_cap = h->next_sequence = 1;
  return h;
}
void ores_tenant_host_delete(OresTenantHost *h) {
  if (!h) return;
  /* The trusted supervisor must serialize destruction against API calls. */
  pthread_mutex_destroy(&h->mutex);
  free(h);
}
OresTenantStatus ores_tenant_domain_create(OresTenantHost *h, const OresDomainPolicy *p, OresDomainId *out) {
  if (!p || !out || p->abi_version != ORES_TENANT_ABI_VERSION || p->reserved || p->reserved2 ||
      p->allowed_udp_loopback_port == 0 || p->max_socket_capabilities > CAP_SLOTS ||
      p->max_queued_datagrams > 1000000u) return ORES_TENANT_INVALID;
  OresTenantStatus status = lock_host(h);
  if (status != ORES_TENANT_OK) return status;
  if (h->next_domain == UINT64_MAX) return done(h, ORES_TENANT_QUOTA);
  for (size_t i = 0; i != DOMAIN_SLOTS; ++i) if (!h->domains[i].active) {
    Domain d = {0};
    d.id = h->next_domain++;
    d.max_queued = p->max_queued_datagrams;
    d.max_caps = p->max_socket_capabilities;
    d.port = p->allowed_udp_loopback_port;
    d.active = 1;
    h->domains[i] = d;
    *out = d.id;
    return done(h, ORES_TENANT_OK);
  }
  return done(h, ORES_TENANT_QUOTA);
}
OresTenantStatus ores_tenant_domain_revoke(OresTenantHost *h, OresDomainId id) {
  OresTenantStatus s = lock_host(h);
  if (s != ORES_TENANT_OK) return s;
  Domain *d = find_domain(h, id);
  if (!d) return done(h, ORES_TENANT_REVOKED);
  d->active = 0;
  for (size_t i = 0; i != CAP_SLOTS; ++i)
    if (h->capabilities[i].active && h->capabilities[i].owner == id) h->capabilities[i].active = 0;
  for (size_t i = 0; i != QUEUE_SLOTS; ++i)
    if (h->queue[i].active && h->queue[i].data.owner == id) h->queue[i].active = 0;
  return done(h, ORES_TENANT_OK);
}
OresTenantStatus ores_tenant_open_loopback_udp(OresTenantHost *h, OresDomainId id, uint16_t port, OresTenantCap *out) {
  if (!out || port == 0) return ORES_TENANT_INVALID;
  OresTenantStatus s = lock_host(h);
  if (s != ORES_TENANT_OK) return s;
  Domain *d = find_domain(h, id);
  if (!d) return done(h, ORES_TENANT_REVOKED);
  if (port != d->port) return done(h, ORES_TENANT_DENIED);
  if (d->open_caps >= d->max_caps || h->next_cap == UINT64_MAX) return done(h, ORES_TENANT_QUOTA);
  for (size_t i = 0; i != CAP_SLOTS; ++i) if (!h->capabilities[i].active) {
    Capability cap = {0};
    cap.token.handle = h->next_cap++;
    cap.token.generation = 1; /* Handles are never reused in this host lifetime. */
    cap.owner = id; cap.port = port; cap.active = 1;
    h->capabilities[i] = cap;
    ++d->open_caps;
    *out = cap.token;
    return done(h, ORES_TENANT_OK);
  }
  return done(h, ORES_TENANT_QUOTA);
}
OresTenantStatus ores_tenant_revoke_capability(OresTenantHost *h, OresDomainId id, OresTenantCap cap) {
  OresTenantStatus s = lock_host(h);
  if (s != ORES_TENANT_OK) return s;
  Domain *d = find_domain(h, id);
  if (!d) return done(h, ORES_TENANT_REVOKED);
  Capability *c = find_capability(h, cap);
  if (!c || c->owner != id) return done(h, ORES_TENANT_DENIED);
  c->active = 0;
  --d->open_caps;
  for (size_t i = 0; i != QUEUE_SLOTS; ++i)
    if (h->queue[i].active && h->queue[i].data.capability.handle == cap.handle)
      h->queue[i].active = 0;
  return done(h, ORES_TENANT_OK);
}
OresTenantStatus ores_tenant_submit_datagram(OresTenantHost *h, OresDomainId id,
                                            OresTenantCap cap, const uint8_t *bytes, size_t size) {
  if (!bytes || size == 0 || size > ORES_TENANT_MAX_DATAGRAM) return ORES_TENANT_INVALID;
  OresTenantStatus s = lock_host(h);
  if (s != ORES_TENANT_OK) return s;
  Domain *d = find_domain(h, id);
  if (!d) return done(h, ORES_TENANT_REVOKED);
  Capability *c = find_capability(h, cap);
  if (!c || c->owner != id) return done(h, ORES_TENANT_DENIED);
  if (d->submitted >= d->max_queued || h->next_sequence == UINT64_MAX)
    return done(h, ORES_TENANT_QUOTA);
  for (size_t i = 0; i != QUEUE_SLOTS; ++i) if (!h->queue[i].active) {
    QueueEntry *q = &h->queue[i];
    q->data.owner = id; q->data.capability = cap; q->data.port = c->port;
    q->data.length = (uint16_t)size;
    memcpy(q->data.payload, bytes, size);
    q->sequence = h->next_sequence++;
    q->active = 1;
    ++d->submitted;
    return done(h, ORES_TENANT_PENDING);
  }
  return done(h, ORES_TENANT_QUOTA);
}
OresTenantStatus ores_tenant_next_pending(OresTenantHost *h, OresPendingDatagram *out) {
  if (!out) return ORES_TENANT_INVALID;
  OresTenantStatus s = lock_host(h);
  if (s != ORES_TENANT_OK) return s;
  QueueEntry *first = NULL;
  for (size_t i = 0; i != QUEUE_SLOTS; ++i) if (h->queue[i].active &&
      (!first || h->queue[i].sequence < first->sequence)) first = &h->queue[i];
  if (!first) return done(h, ORES_TENANT_OK);
  *out = first->data;
  first->active = 0;
  return done(h, ORES_TENANT_PENDING);
}
OresTenantStatus ores_tenant_revalidate_pending(OresTenantHost *h, const OresPendingDatagram *entry) {
  if (!entry || entry->length == 0 || entry->length > ORES_TENANT_MAX_DATAGRAM) return ORES_TENANT_INVALID;
  OresTenantStatus s = lock_host(h);
  if (s != ORES_TENANT_OK) return s;
  Domain *d = find_domain(h, entry->owner);
  if (!d) return done(h, ORES_TENANT_REVOKED);
  Capability *c = find_capability(h, entry->capability);
  if (!c || c->owner != entry->owner || c->port != entry->port)
    return done(h, ORES_TENANT_DENIED);
  return done(h, ORES_TENANT_OK);
}
