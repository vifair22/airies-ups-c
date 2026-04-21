-- UPS register config snapshots
-- Tracks the current and historical state of UPS configuration registers
CREATE TABLE IF NOT EXISTS ups_config (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    register_name TEXT   NOT NULL,
    raw_value    INTEGER NOT NULL,
    display_value TEXT,
    timestamp    TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_ups_config_name ON ups_config(register_name);
CREATE INDEX IF NOT EXISTS idx_ups_config_ts ON ups_config(timestamp);
