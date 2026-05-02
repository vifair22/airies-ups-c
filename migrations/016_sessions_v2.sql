-- Auth rewrite: opaque tokens move into HttpOnly cookies (with the
-- Authorization header path retained for CLI / API tokens / curl), and
-- the schema gains per-token metadata in preparation for multi-admin
-- and long-lived API tokens.
--
-- Drop-and-recreate: there's one user, they re-login once after this
-- migration. ALTER chains for a single forced logout would be
-- over-engineering.
--
-- Column rationale:
--   user_id      — readiness for future multi-admin (NULL = single-admin)
--   kind         — 'session' (browser) | 'api_token' (long-lived, named).
--                  Discriminator drives different lifecycle behavior:
--                  sessions get sliding 90-day expiry; api_tokens do not
--                  slide and live until explicitly revoked or expired.
--   name         — human label for api_tokens (e.g. "Home Assistant")
--   scopes       — JSON array; NULL = full access. Future work.
--   last_used_at — observability / future idle timeout
--   expires_at   — bumped on each successful auth for kind='session'
--                  (sliding 90-day window). Hard-set for api_token rows.
--   revoked_at   — soft delete for audit; queries filter via
--                  revoked_at IS NULL to keep the row for forensics.

DROP TABLE IF EXISTS sessions;
CREATE TABLE sessions (
    token        TEXT PRIMARY KEY,
    user_id      INTEGER,
    kind         TEXT NOT NULL DEFAULT 'session',
    name         TEXT,
    scopes       TEXT,
    created_at   TEXT NOT NULL DEFAULT (datetime('now')),
    last_used_at TEXT,
    expires_at   TEXT NOT NULL,
    revoked_at   TEXT
);

CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at);
CREATE INDEX IF NOT EXISTS idx_sessions_user    ON sessions(user_id);
CREATE INDEX IF NOT EXISTS idx_sessions_kind    ON sessions(kind);
