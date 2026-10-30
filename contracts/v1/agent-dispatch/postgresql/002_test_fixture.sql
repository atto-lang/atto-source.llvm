-- TEST-ONLY role/fixture for an isolated ephemeral PostgreSQL service.
-- No stored credentials; CI provisions login separately at execution time.
CREATE ROLE ores_dispatch_test_runtime NOLOGIN;
GRANT USAGE ON SCHEMA ores_dispatch TO ores_dispatch_test_runtime;
GRANT EXECUTE ON FUNCTION ores_dispatch.claim_once_v1(
  text, text, text, text, bigint, bigint, text, bigint, integer, integer
) TO ores_dispatch_test_runtime;
INSERT INTO ores_dispatch.actor_fence_v1
  (tenant_id, actor_id, actor_generation, activation_epoch, exact_revision)
VALUES ('tenant', 'worker', 7, 9, repeat('a', 40));
INSERT INTO ores_dispatch.runtime_binding_v1
  (login_role, tenant_id, actor_id, enabled)
VALUES ('ores_dispatch_test_runtime', 'tenant', 'worker', true);
-- A current fence is insufficient without independent login authorization:
INSERT INTO ores_dispatch.actor_fence_v1
  (tenant_id, actor_id, actor_generation, activation_epoch, exact_revision)
VALUES ('unbound-tenant', 'worker', 7, 9, repeat('a', 40));
-- No direct runtime table SELECT/INSERT/UPDATE is granted.
