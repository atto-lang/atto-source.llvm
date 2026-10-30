-- Oreslang trusted-host distributed dispatch v1: PostgreSQL 14+.
BEGIN;
-- HOST ONLY. This migration never grants an Oreslang guest direct DB access.
-- Run by a dedicated migration owner on a private DB, not by the runtime role.
CREATE SCHEMA IF NOT EXISTS ores_dispatch;

CREATE TABLE IF NOT EXISTS ores_dispatch.actor_fence_v1 (
    tenant_id varchar(128) COLLATE "C" NOT NULL
        CHECK (tenant_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    actor_id varchar(128) COLLATE "C" NOT NULL
        CHECK (actor_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    actor_generation bigint NOT NULL CHECK (actor_generation > 0),
    activation_epoch bigint NOT NULL CHECK (activation_epoch > 0),
    exact_revision char(40) COLLATE "C" NOT NULL
        CHECK (exact_revision ~ '^[0-9a-f]{40}$'),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (tenant_id, actor_id)
);

-- Privileged operators map a DB login to authorized tenant/actor scopes.
-- Inside a SECURITY DEFINER function current_user is the FUNCTION OWNER;
-- session_user is the authenticated login and is not spoofable by SET ROLE.
CREATE TABLE IF NOT EXISTS ores_dispatch.runtime_binding_v1 (
    login_role name NOT NULL,
    tenant_id varchar(128) COLLATE "C" NOT NULL
        CHECK (tenant_id ~ '^[A-Za-z0-9_.:@/-]{1,128}
-- payload digest. Restart/reload/changed payload must NOT reset idempotency.
CREATE TABLE IF NOT EXISTS ores_dispatch.claim_v1 (
    tenant_id varchar(128) COLLATE "C" NOT NULL
        CHECK (tenant_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    actor_id varchar(128) COLLATE "C" NOT NULL
        CHECK (actor_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    dispatch_id varchar(128) COLLATE "C" NOT NULL
        CHECK (dispatch_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    context_sha256 char(64) COLLATE "C" NOT NULL
        CHECK (context_sha256 ~ '^[0-9a-f]{64}$'),
    actor_generation bigint NOT NULL CHECK (actor_generation > 0),
    activation_epoch bigint NOT NULL CHECK (activation_epoch > 0),
    exact_revision char(40) COLLATE "C" NOT NULL
        CHECK (exact_revision ~ '^[0-9a-f]{40}$'),
    expires_at_ms bigint NOT NULL CHECK (expires_at_ms > 0),
    utf8_bytes integer NOT NULL CHECK (utf8_bytes BETWEEN 1 AND 65536),
    measured_tokens integer NOT NULL CHECK (measured_tokens BETWEEN 1 AND 16384),
    claimed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (tenant_id, actor_id, dispatch_id)
);


-- Lock the actor fence and make the one-shot insert in ONE DB transaction.
-- A dispatch caller cannot advance its own generation, alter a claim, or
-- reset idempotency after restart. A separate trusted supervisor maintains
-- actor_fence_v1; its UPDATE waits for in-flight row-share locks.
CREATE OR REPLACE FUNCTION ores_dispatch.claim_once_v1(
    p_tenant text, p_actor text, p_dispatch text, p_digest text,
    p_generation bigint, p_epoch bigint, p_revision text,
    p_expires_ms bigint, p_utf8_bytes integer, p_measured_tokens integer
) RETURNS boolean
LANGUAGE plpgsql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $fn$
DECLARE
    inserted boolean;
BEGIN
    IF p_tenant IS NULL OR p_actor IS NULL OR p_dispatch IS NULL OR
       p_digest IS NULL OR p_revision IS NULL OR p_generation IS NULL OR
       p_epoch IS NULL OR p_expires_ms IS NULL OR p_utf8_bytes IS NULL OR
       p_measured_tokens IS NULL THEN
        RETURN false;
    END IF;
    IF p_tenant !~ '^[A-Za-z0-9_.:@/-]{1,128} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_actor !~ '^[A-Za-z0-9_.:@/-]{1,128} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_dispatch !~ '^[A-Za-z0-9_.:@/-]{1,128} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_digest !~ '^[0-9a-f]{64} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_revision !~ '^[0-9a-f]{40} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_generation <= 0 OR p_epoch <= 0 OR
       p_utf8_bytes NOT BETWEEN 1 AND 65536 OR
       p_measured_tokens NOT BETWEEN 1 AND 16384 OR
       p_expires_ms <= floor(extract(epoch FROM clock_timestamp()) * 1000)::bigint
    THEN
        RETURN false;
    END IF;

    -- Bind the caller's authenticated login to the requested tenant/actor.
    -- An application credential cannot edit this table or impersonate another
    -- tenant, even when it knows that tenant's current generation/revision.
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

    INSERT INTO ores_dispatch.claim_v1
       (tenant_id, actor_id, dispatch_id, context_sha256,
        actor_generation, activation_epoch, exact_revision,
        expires_at_ms, utf8_bytes, measured_tokens)
    SELECT p_tenant, p_actor, p_dispatch, p_digest,
           p_generation, p_epoch, p_revision,
           p_expires_ms, p_utf8_bytes, p_measured_tokens
     WHERE p_expires_ms > floor(extract(epoch FROM clock_timestamp()) * 1000)::bigint
    ON CONFLICT (tenant_id, actor_id, dispatch_id) DO NOTHING
    RETURNING true INTO inserted;

    RETURN coalesce(inserted, false);
END
$fn$;

-- PostgreSQL grants function EXECUTE to PUBLIC by default: revoke it
-- within the SAME migration transaction before any runtime can connect.
REVOKE ALL ON SCHEMA ores_dispatch FROM PUBLIC;
REVOKE ALL ON ALL TABLES IN SCHEMA ores_dispatch FROM PUBLIC;
REVOKE ALL ON FUNCTION ores_dispatch.claim_once_v1(
    text, text, text, text, bigint, bigint, text, bigint, integer, integer
) FROM PUBLIC;

-- Runtime role: grant USAGE on schema and EXECUTE on claim_once_v1 ONLY.
-- Do NOT grant direct table access. Only independently authorized supervisor
-- administration can advance actor_fence_v1; the function owner is trusted.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
COMMIT;
),
    actor_id varchar(128) COLLATE "C" NOT NULL
        CHECK (actor_id ~ '^[A-Za-z0-9_.:@/-]{1,128}
-- payload digest. Restart/reload/changed payload must NOT reset idempotency.
CREATE TABLE IF NOT EXISTS ores_dispatch.claim_v1 (
    tenant_id varchar(128) COLLATE "C" NOT NULL
        CHECK (tenant_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    actor_id varchar(128) COLLATE "C" NOT NULL
        CHECK (actor_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    dispatch_id varchar(128) COLLATE "C" NOT NULL
        CHECK (dispatch_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    context_sha256 char(64) COLLATE "C" NOT NULL
        CHECK (context_sha256 ~ '^[0-9a-f]{64}$'),
    actor_generation bigint NOT NULL CHECK (actor_generation > 0),
    activation_epoch bigint NOT NULL CHECK (activation_epoch > 0),
    exact_revision char(40) COLLATE "C" NOT NULL
        CHECK (exact_revision ~ '^[0-9a-f]{40}$'),
    expires_at_ms bigint NOT NULL CHECK (expires_at_ms > 0),
    utf8_bytes integer NOT NULL CHECK (utf8_bytes BETWEEN 1 AND 65536),
    measured_tokens integer NOT NULL CHECK (measured_tokens BETWEEN 1 AND 16384),
    claimed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (tenant_id, actor_id, dispatch_id)
);


-- Lock the actor fence and make the one-shot insert in ONE DB transaction.
-- A dispatch caller cannot advance its own generation, alter a claim, or
-- reset idempotency after restart. A separate trusted supervisor maintains
-- actor_fence_v1; its UPDATE waits for in-flight row-share locks.
CREATE OR REPLACE FUNCTION ores_dispatch.claim_once_v1(
    p_tenant text, p_actor text, p_dispatch text, p_digest text,
    p_generation bigint, p_epoch bigint, p_revision text,
    p_expires_ms bigint, p_utf8_bytes integer, p_measured_tokens integer
) RETURNS boolean
LANGUAGE plpgsql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $fn$
DECLARE
    inserted boolean;
BEGIN
    IF p_tenant IS NULL OR p_actor IS NULL OR p_dispatch IS NULL OR
       p_digest IS NULL OR p_revision IS NULL OR p_generation IS NULL OR
       p_epoch IS NULL OR p_expires_ms IS NULL OR p_utf8_bytes IS NULL OR
       p_measured_tokens IS NULL THEN
        RETURN false;
    END IF;
    IF p_tenant !~ '^[A-Za-z0-9_.:@/-]{1,128} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_actor !~ '^[A-Za-z0-9_.:@/-]{1,128} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_dispatch !~ '^[A-Za-z0-9_.:@/-]{1,128} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_digest !~ '^[0-9a-f]{64} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_revision !~ '^[0-9a-f]{40} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_generation <= 0 OR p_epoch <= 0 OR
       p_utf8_bytes NOT BETWEEN 1 AND 65536 OR
       p_measured_tokens NOT BETWEEN 1 AND 16384 OR
       p_expires_ms <= floor(extract(epoch FROM clock_timestamp()) * 1000)::bigint
    THEN
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

    INSERT INTO ores_dispatch.claim_v1
       (tenant_id, actor_id, dispatch_id, context_sha256,
        actor_generation, activation_epoch, exact_revision,
        expires_at_ms, utf8_bytes, measured_tokens)
    SELECT p_tenant, p_actor, p_dispatch, p_digest,
           p_generation, p_epoch, p_revision,
           p_expires_ms, p_utf8_bytes, p_measured_tokens
     WHERE p_expires_ms > floor(extract(epoch FROM clock_timestamp()) * 1000)::bigint
    ON CONFLICT (tenant_id, actor_id, dispatch_id) DO NOTHING
    RETURNING true INTO inserted;

    RETURN coalesce(inserted, false);
END
$fn$;

-- PostgreSQL grants function EXECUTE to PUBLIC by default: revoke it
-- within the SAME migration transaction before any runtime can connect.
REVOKE ALL ON SCHEMA ores_dispatch FROM PUBLIC;
REVOKE ALL ON ALL TABLES IN SCHEMA ores_dispatch FROM PUBLIC;
REVOKE ALL ON FUNCTION ores_dispatch.claim_once_v1(
    text, text, text, text, bigint, bigint, text, bigint, integer, integer
) FROM PUBLIC;

-- Runtime role: grant USAGE on schema and EXECUTE on claim_once_v1 ONLY.
-- Do NOT grant direct table access. Only independently authorized supervisor
-- administration can advance actor_fence_v1; the function owner is trusted.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
COMMIT;
),
    enabled boolean NOT NULL DEFAULT false,
    PRIMARY KEY (login_role, tenant_id, actor_id)
);

-- This primary key deliberately excludes incarnation, epoch, revision and
-- payload digest. Restart/reload/changed payload must NOT reset idempotency.
CREATE TABLE IF NOT EXISTS ores_dispatch.claim_v1 (
    tenant_id varchar(128) COLLATE "C" NOT NULL
        CHECK (tenant_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    actor_id varchar(128) COLLATE "C" NOT NULL
        CHECK (actor_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    dispatch_id varchar(128) COLLATE "C" NOT NULL
        CHECK (dispatch_id ~ '^[A-Za-z0-9_.:@/-]{1,128}$'),
    context_sha256 char(64) COLLATE "C" NOT NULL
        CHECK (context_sha256 ~ '^[0-9a-f]{64}$'),
    actor_generation bigint NOT NULL CHECK (actor_generation > 0),
    activation_epoch bigint NOT NULL CHECK (activation_epoch > 0),
    exact_revision char(40) COLLATE "C" NOT NULL
        CHECK (exact_revision ~ '^[0-9a-f]{40}$'),
    expires_at_ms bigint NOT NULL CHECK (expires_at_ms > 0),
    utf8_bytes integer NOT NULL CHECK (utf8_bytes BETWEEN 1 AND 65536),
    measured_tokens integer NOT NULL CHECK (measured_tokens BETWEEN 1 AND 16384),
    claimed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (tenant_id, actor_id, dispatch_id)
);


-- Lock the actor fence and make the one-shot insert in ONE DB transaction.
-- A dispatch caller cannot advance its own generation, alter a claim, or
-- reset idempotency after restart. A separate trusted supervisor maintains
-- actor_fence_v1; its UPDATE waits for in-flight row-share locks.
CREATE OR REPLACE FUNCTION ores_dispatch.claim_once_v1(
    p_tenant text, p_actor text, p_dispatch text, p_digest text,
    p_generation bigint, p_epoch bigint, p_revision text,
    p_expires_ms bigint, p_utf8_bytes integer, p_measured_tokens integer
) RETURNS boolean
LANGUAGE plpgsql SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $fn$
DECLARE
    inserted boolean;
BEGIN
    IF p_tenant IS NULL OR p_actor IS NULL OR p_dispatch IS NULL OR
       p_digest IS NULL OR p_revision IS NULL OR p_generation IS NULL OR
       p_epoch IS NULL OR p_expires_ms IS NULL OR p_utf8_bytes IS NULL OR
       p_measured_tokens IS NULL THEN
        RETURN false;
    END IF;
    IF p_tenant !~ '^[A-Za-z0-9_.:@/-]{1,128} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_actor !~ '^[A-Za-z0-9_.:@/-]{1,128} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_dispatch !~ '^[A-Za-z0-9_.:@/-]{1,128} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_digest !~ '^[0-9a-f]{64} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_revision !~ '^[0-9a-f]{40} GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
 OR
       p_generation <= 0 OR p_epoch <= 0 OR
       p_utf8_bytes NOT BETWEEN 1 AND 65536 OR
       p_measured_tokens NOT BETWEEN 1 AND 16384 OR
       p_expires_ms <= floor(extract(epoch FROM clock_timestamp()) * 1000)::bigint
    THEN
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

    INSERT INTO ores_dispatch.claim_v1
       (tenant_id, actor_id, dispatch_id, context_sha256,
        actor_generation, activation_epoch, exact_revision,
        expires_at_ms, utf8_bytes, measured_tokens)
    SELECT p_tenant, p_actor, p_dispatch, p_digest,
           p_generation, p_epoch, p_revision,
           p_expires_ms, p_utf8_bytes, p_measured_tokens
     WHERE p_expires_ms > floor(extract(epoch FROM clock_timestamp()) * 1000)::bigint
    ON CONFLICT (tenant_id, actor_id, dispatch_id) DO NOTHING
    RETURNING true INTO inserted;

    RETURN coalesce(inserted, false);
END
$fn$;

-- PostgreSQL grants function EXECUTE to PUBLIC by default: revoke it
-- within the SAME migration transaction before any runtime can connect.
REVOKE ALL ON SCHEMA ores_dispatch FROM PUBLIC;
REVOKE ALL ON ALL TABLES IN SCHEMA ores_dispatch FROM PUBLIC;
REVOKE ALL ON FUNCTION ores_dispatch.claim_once_v1(
    text, text, text, text, bigint, bigint, text, bigint, integer, integer
) FROM PUBLIC;

-- Runtime role: grant USAGE on schema and EXECUTE on claim_once_v1 ONLY.
-- Do NOT grant direct table access. Only independently authorized supervisor
-- administration can advance actor_fence_v1; the function owner is trusted.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
COMMIT;
