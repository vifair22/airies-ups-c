-- Drop the telemetry table and its index. Telemetry collection has been
-- removed for 1.0.2 — there's no UI consuming it and the table just grows
-- unbounded. Migrations 001 and 009 stay in place for history; this one
-- supersedes them by dropping what they created.

DROP INDEX IF EXISTS idx_telemetry_ts;
DROP TABLE IF EXISTS telemetry;
