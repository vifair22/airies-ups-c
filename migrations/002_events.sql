-- Event journal — canonical log of all UPS events
-- Pushover notifications are fired from events, not the other way around
CREATE TABLE IF NOT EXISTS events (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT    NOT NULL,
    severity  TEXT    NOT NULL,   -- info, warning, error, critical
    category  TEXT    NOT NULL,   -- status, alert, command, shutdown, weather, system
    title     TEXT    NOT NULL,
    message   TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_events_ts ON events(timestamp);
CREATE INDEX IF NOT EXISTS idx_events_severity ON events(severity);
CREATE INDEX IF NOT EXISTS idx_events_category ON events(category);
