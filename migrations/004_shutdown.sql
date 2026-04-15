-- Shutdown orchestration: targets and groups
--
-- Groups execute sequentially (by execution_order).
-- Targets within a group execute in parallel.
-- Final implicit group: UPS shutdown command + self-shutdown.

CREATE TABLE IF NOT EXISTS shutdown_groups (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    name            TEXT    NOT NULL UNIQUE,
    execution_order INTEGER NOT NULL DEFAULT 0,
    parallel        INTEGER NOT NULL DEFAULT 1   -- 1 = targets run in parallel
);

CREATE TABLE IF NOT EXISTS shutdown_targets (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    group_id          INTEGER NOT NULL REFERENCES shutdown_groups(id) ON DELETE CASCADE,
    name              TEXT    NOT NULL,
    method            TEXT    NOT NULL DEFAULT 'ssh_password',  -- ssh_password, ssh_key, command
    host              TEXT,              -- hostname or IP
    username          TEXT,              -- SSH user
    credential        TEXT,              -- password or key path
    command           TEXT    NOT NULL,  -- shutdown command to execute
    timeout_sec       INTEGER NOT NULL DEFAULT 180,
    order_in_group    INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_targets_group ON shutdown_targets(group_id);
