-- Oreslang trusted-host distributed dispatch v1: PostgreSQL 14+.
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

-- Runtime roles: GRANT USAGE ON SCHEMA; SELECT on actor_fence_v1; INSERT
-- only on claim_v1. Do NOT grant UPDATE/DELETE/TRUNCATE on claim_v1 or
-- INSERT/UPDATE/DELETE on actor_fence_v1. Only independently authorized
-- supervisor administration can advance the current actor fence.
-- No automatic claim expiration cleanup. No unsafe retry on unknown COMMIT.
-- Database durability settings must retain fsync=on and synchronous_commit=on.
