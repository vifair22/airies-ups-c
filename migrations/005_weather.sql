-- Weather subsystem configuration
-- Single-row table — only one weather config per daemon
CREATE TABLE IF NOT EXISTS weather_config (
    id              INTEGER PRIMARY KEY CHECK (id = 1),  -- enforce single row
    enabled         INTEGER NOT NULL DEFAULT 0,
    latitude        REAL,
    longitude       REAL,
    alert_zones     TEXT,     -- comma-separated NWS zone codes
    alert_types     TEXT,     -- comma-separated alert type names
    wind_speed_mph  INTEGER NOT NULL DEFAULT 40,
    severe_keywords TEXT,     -- comma-separated forecast keywords
    poll_interval   INTEGER NOT NULL DEFAULT 300
);

-- Seed with defaults (disabled)
INSERT OR IGNORE INTO weather_config (id, enabled) VALUES (1, 0);
