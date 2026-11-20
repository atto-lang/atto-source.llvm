-- Durable Actor Dispatch v1, PostgreSQL 14+. HOST ONLY.
-- Apply in an isolated, trusted database with a separate migration owner.
-- No Oreslang guest is allowed access to the schema or credentials.
BEGIN;
-- Fail closed on preexisting/unreviewed schemas or tables; this migration
-- applies once under a dedicated non-superuser migration owner.
CREATE SCHEMA ores_dispatch;
REVOKE ALL ON SCHEMA ores_dispatch FROM PUBLIC;

CREATE TABLE ores_dispatch.actor_fence_v1 (
    tenant_id varchar(128) COLLATE "C" NOT NULL
      CHECK (tenant_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    actor_id varchar(128) COLLATE "C" NOT NULL
      CHECK (actor_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    actor_generation bigint NOT NULL CHECK (actor_generation > 0),
    activation_epoch bigint NOT NULL CHECK (activation_epoch > 0),
    exact_revision varchar(40) COLLATE "C" NOT NULL
      CHECK (exact_revision ~ '^[0-9a-f]{40}$'),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (tenant_id, actor_id)
);

-- Independently administered authorization of an authenticated DB login.
-- session_user (not current_user) is checked in the function because
-- SECURITY DEFINER changes current_user to the function owner.
CREATE TABLE ores_dispatch.runtime_binding_v1 (
    login_role name NOT NULL,
    tenant_id varchar(128) COLLATE "C" NOT NULL
      CHECK (tenant_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    actor_id varchar(128) COLLATE "C" NOT NULL
      CHECK (actor_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    enabled boolean NOT NULL DEFAULT false,
    PRIMARY KEY (login_role, tenant_id, actor_id)
);

-- No generation, activation epoch, revision or digest in this PK:
-- changing actor incarnation or payload NEVER resets logical idempotency.
CREATE TABLE ores_dispatch.claim_v1 (
    tenant_id varchar(128) COLLATE "C" NOT NULL
      CHECK (tenant_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    actor_id varchar(128) COLLATE "C" NOT NULL
      CHECK (actor_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    dispatch_id varchar(128) COLLATE "C" NOT NULL
      CHECK (dispatch_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    context_sha256 varchar(64) COLLATE "C" NOT NULL
      CHECK (context_sha256 ~ '^[0-9a-f]{64}$'),
    actor_generation bigint NOT NULL CHECK (actor_generation > 0),
    activation_epoch bigint NOT NULL CHECK (activation_epoch > 0),
    exact_revision varchar(40) COLLATE "C" NOT NULL
      CHECK (exact_revision ~ '^[0-9a-f]{40}$'),
    expires_at_ms bigint NOT NULL CHECK (expires_at_ms > 0),
    utf8_bytes integer NOT NULL CHECK (utf8_bytes BETWEEN 1 AND 65536),
    measured_tokens integer NOT NULL CHECK (measured_tokens BETWEEN 1 AND 16384),
    claimed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (tenant_id, actor_id, dispatch_id)
);
REVOKE ALL ON ALL TABLES IN SCHEMA ores_dispatch FROM PUBLIC;

CREATE FUNCTION ores_dispatch.claim_once_v1(
    p_tenant text, p_actor text, p_dispatch text, p_digest text,
    p_generation bigint, p_epoch bigint, p_revision text,
    p_expires_ms bigint, p_utf8_bytes integer, p_measured_tokens integer
) RETURNS boolean
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $claim$
DECLARE
    claimed boolean;
BEGIN
    IF p_tenant IS NULL OR p_actor IS NULL OR p_dispatch IS NULL OR
       p_digest IS NULL OR p_revision IS NULL OR
       p_generation IS NULL OR p_epoch IS NULL OR
       p_expires_ms IS NULL OR p_utf8_bytes IS NULL OR
       p_measured_tokens IS NULL THEN
        RETURN false;
    END IF;
    IF p_tenant !~ '^[A-Za-z0-9_.:@/-]{1,128}$' OR
       p_actor !~ '^[A-Za-z0-9_.:@/-]{1,128}$' OR
       p_dispatch !~ '^[A-Za-z0-9_.:@/-]{1,128}$' OR
       p_digest !~ '^[0-9a-f]{64}$' OR
       p_revision !~ '^[0-9a-f]{40}$' OR
       p_generation <= 0 OR p_epoch <= 0 OR
       p_utf8_bytes NOT BETWEEN 1 AND 65536 OR
       p_measured_tokens NOT BETWEEN 1 AND 16384 OR
       current_setting('fsync') <> 'on' OR
       current_setting('synchronous_commit') NOT IN ('on', 'remote_apply') OR
       p_expires_ms <= floor(extract(epoch FROM clock_timestamp()) * 1000)::bigint
    THEN
        RETURN false;
    END IF;

    -- Lock the authorization and generation fence through COMMIT. The caller
    -- has EXECUTE only, never ownership or direct table privileges.
    PERFORM 1 FROM ores_dispatch.runtime_binding_v1
      WHERE login_role = session_user
        AND tenant_id = p_tenant AND actor_id = p_actor AND enabled
      FOR SHARE;
    IF NOT FOUND THEN
        RETURN false;
    END IF;
    PERFORM 1 FROM ores_dispatch.actor_fence_v1
      WHERE tenant_id = p_tenant AND actor_id = p_actor
        AND actor_generation = p_generation
        AND activation_epoch = p_epoch
        AND exact_revision = p_revision
      FOR SHARE;
    IF NOT FOUND THEN
        RETURN false;
    END IF;

    -- In production the TRUSTED host submits one autocommit statement and
    -- does not mutate synchronous_commit after this check. This function
    -- alone CANNOT prevent an explicitly malicious client from turning off
    -- synchronous_commit before its outer COMMIT.
    -- Recheck the database clock immediately before the atomic admission.
    INSERT INTO ores_dispatch.claim_v1
        (tenant_id, actor_id, dispatch_id, context_sha256,
         actor_generation, activation_epoch, exact_revision,
         expires_at_ms, utf8_bytes, measured_tokens)
    SELECT p_tenant, p_actor, p_dispatch, p_digest,
           p_generation, p_epoch, p_revision,
           p_expires_ms, p_utf8_bytes, p_measured_tokens
     WHERE p_expires_ms > floor(extract(epoch FROM clock_timestamp()) * 1000)::bigint
    ON CONFLICT (tenant_id, actor_id, dispatch_id) DO NOTHING
    RETURNING true INTO claimed;
    RETURN coalesce(claimed, false);
END
$claim$;

-- A SECURITY DEFINER function defaults to PUBLIC EXECUTE at creation.
-- Revoke in the same migration transaction, before callers can observe it.
REVOKE ALL ON FUNCTION ores_dispatch.claim_once_v1(
    text, text, text, text, bigint, bigint, text, bigint, integer, integer
) FROM PUBLIC;
COMMIT;
