/* Trusted-host-only native contract. Does NOT prove hostile native isolation. */
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/TenantDomainHost.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "tenant domain host test: line %d: %s\n", __LINE__, #condition); \
    exit(1); } } while (0)
#define STATUS(actual, expected) CHECK((actual) == (expected))

static OresDomainPolicy policy(uint16_t port, uint64_t max_send, uint32_t max_caps) {
  OresDomainPolicy p = {0};
  p.abi_version = ORES_TENANT_ABI_VERSION;
  p.max_socket_capabilities = max_caps;
  p.max_queued_datagrams = max_send;
  p.allowed_udp_loopback_port = port;
  return p;
}
static void authorization_and_revocation(void) {
  OresTenantHost *h = ores_tenant_host_new();
  CHECK(h);
  OresDomainPolicy p = policy(42424, 2, 1);
  OresDomainId a = 0, b = 0, replacement = 0;
  OresTenantCap ca = {0}, cb = {0}, future = {0};
  const uint8_t bytes[] = {3, 7, 11, 17};
  STATUS(ores_tenant_domain_create(h, &p, &a), ORES_TENANT_OK);
  STATUS(ores_tenant_domain_create(h, &p, &b), ORES_TENANT_OK);
  CHECK(a != b && a && b);
  STATUS(ores_tenant_open_loopback_udp(h, a, 42425, &ca), ORES_TENANT_DENIED);
  CHECK(ca.handle == 0);
  STATUS(ores_tenant_open_loopback_udp(h, a, 42424, &ca), ORES_TENANT_OK);
  STATUS(ores_tenant_open_loopback_udp(h, a, 42424, &future), ORES_TENANT_QUOTA);
  STATUS(ores_tenant_open_loopback_udp(h, b, 42424, &cb), ORES_TENANT_OK);
  CHECK(ca.handle != cb.handle);
  STATUS(ores_tenant_submit_datagram(h, b, ca, bytes, sizeof bytes), ORES_TENANT_DENIED);
  STATUS(ores_tenant_submit_datagram(h, a, cb, bytes, sizeof bytes), ORES_TENANT_DENIED);
  STATUS(ores_tenant_submit_datagram(h, a, ca, bytes, 0), ORES_TENANT_INVALID);
  STATUS(ores_tenant_submit_datagram(h, a, ca, bytes, sizeof bytes), ORES_TENANT_PENDING);
  OresPendingDatagram intent = {0};
  STATUS(ores_tenant_next_pending(h, &intent), ORES_TENANT_PENDING);
  CHECK(intent.owner == a && intent.capability.handle == ca.handle &&
        intent.length == sizeof bytes && !memcmp(intent.payload, bytes, sizeof bytes));
  STATUS(ores_tenant_revalidate_pending(h, &intent), ORES_TENANT_OK);
  STATUS(ores_tenant_next_pending(h, &intent), ORES_TENANT_OK);
  STATUS(ores_tenant_submit_datagram(h, a, ca, bytes, sizeof bytes), ORES_TENANT_PENDING);
  STATUS(ores_tenant_submit_datagram(h, a, ca, bytes, sizeof bytes), ORES_TENANT_QUOTA);
  STATUS(ores_tenant_revoke_capability(h, b, ca), ORES_TENANT_DENIED);
  STATUS(ores_tenant_revoke_capability(h, a, ca), ORES_TENANT_OK);
  STATUS(ores_tenant_submit_datagram(h, a, ca, bytes, sizeof bytes), ORES_TENANT_DENIED);
  STATUS(ores_tenant_next_pending(h, &intent), ORES_TENANT_OK); // pending intent removed
  STATUS(ores_tenant_open_loopback_udp(h, a, 42424, &future), ORES_TENANT_OK);
  CHECK(future.handle != ca.handle);
  STATUS(ores_tenant_domain_revoke(h, a), ORES_TENANT_OK);
  STATUS(ores_tenant_submit_datagram(h, a, future, bytes, sizeof bytes), ORES_TENANT_REVOKED);
  STATUS(ores_tenant_revalidate_pending(h, &intent), ORES_TENANT_REVOKED);
  STATUS(ores_tenant_domain_revoke(h, a), ORES_TENANT_REVOKED);
  STATUS(ores_tenant_domain_create(h, &p, &replacement), ORES_TENANT_OK);
  CHECK(replacement != a);
  STATUS(ores_tenant_submit_datagram(h, replacement, ca, bytes, sizeof bytes), ORES_TENANT_DENIED);
  STATUS(ores_tenant_domain_revoke(h, replacement), ORES_TENANT_OK);
  STATUS(ores_tenant_domain_revoke(h, b), ORES_TENANT_OK);
  ores_tenant_host_delete(h);
}
static void fail_closed_policy(void) {
  OresTenantHost *h = ores_tenant_host_new();
  CHECK(h);
  OresDomainId id = 0;
  OresDomainPolicy p = policy(12345, 1, 1);
  p.abi_version = 0;
  STATUS(ores_tenant_domain_create(h, &p, &id), ORES_TENANT_INVALID);
  p.abi_version = ORES_TENANT_ABI_VERSION; p.reserved2 = 1;
  STATUS(ores_tenant_domain_create(h, &p, &id), ORES_TENANT_INVALID);
  p.reserved2 = 0;
  STATUS(ores_tenant_domain_create(h, &p, &id), ORES_TENANT_OK);
  OresTenantCap cap = {0};
  STATUS(ores_tenant_open_loopback_udp(h, id, 12345, &cap), ORES_TENANT_OK);
  uint8_t too_big[ORES_TENANT_MAX_DATAGRAM + 1] = {0};
  STATUS(ores_tenant_submit_datagram(h, id, cap, too_big, sizeof too_big), ORES_TENANT_INVALID);
  STATUS(ores_tenant_submit_datagram(h, id, cap, too_big, 1), ORES_TENANT_PENDING);
  STATUS(ores_tenant_submit_datagram(h, id, cap, too_big, 1), ORES_TENANT_QUOTA);
  OresPendingDatagram pending = {0};
  STATUS(ores_tenant_next_pending(h, &pending), ORES_TENANT_PENDING);
  STATUS(ores_tenant_domain_revoke(h, id), ORES_TENANT_OK);
  STATUS(ores_tenant_revalidate_pending(h, &pending), ORES_TENANT_REVOKED);
  ores_tenant_host_delete(h);
}
typedef struct { OresTenantHost *h; uint32_t tid; int failed; } ThreadArgs;
static void *thread_main(void *arg) {
  ThreadArgs *t = (ThreadArgs *)arg;
  OresDomainPolicy p = policy(50000, 1, 1);
  OresDomainId d = 0;
  OresTenantCap cap = {0};
  uint8_t payload = (uint8_t)t->tid;
  if (ores_tenant_domain_create(t->h, &p, &d) != ORES_TENANT_OK ||
      ores_tenant_open_loopback_udp(t->h, d, 50000, &cap) != ORES_TENANT_OK ||
      ores_tenant_submit_datagram(t->h, d, cap, &payload, 1) != ORES_TENANT_PENDING)
    t->failed = 1;
  // Keep domain alive until after the supervisor has drained intents.
  return NULL;
}
static void parallel_100_tenants(void) {
  OresTenantHost *h = ores_tenant_host_new(); CHECK(h);
  pthread_t threads[100]; ThreadArgs args[100] = {{0}};
  for (unsigned i = 0; i < 100; ++i) {
    args[i].h = h; args[i].tid = i;
    CHECK(pthread_create(&threads[i], NULL, thread_main, &args[i]) == 0);
  }
  for (unsigned i = 0; i < 100; ++i) {
    CHECK(pthread_join(threads[i], NULL) == 0); CHECK(!args[i].failed);
  }
  uint8_t seen[100] = {0};
  for (unsigned i = 0; i < 100; ++i) {
    OresPendingDatagram p = {0};
    STATUS(ores_tenant_next_pending(h, &p), ORES_TENANT_PENDING);
    STATUS(ores_tenant_revalidate_pending(h, &p), ORES_TENANT_OK);
    CHECK(p.length == 1 && p.payload[0] < 100 && !seen[p.payload[0]]);
    seen[p.payload[0]] = 1;
  }
  OresPendingDatagram empty = {0};
  STATUS(ores_tenant_next_pending(h, &empty), ORES_TENANT_OK);
  ores_tenant_host_delete(h);
}
int main(void) {
  authorization_and_revocation();
  fail_closed_policy();
  parallel_100_tenants();
  puts("PASS: trusted-host domain capability, quotas, revocation, and 100-thread admission");
  return 0;
}
