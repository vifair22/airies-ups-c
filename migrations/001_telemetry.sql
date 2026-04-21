-- UPS telemetry time-series data
CREATE TABLE IF NOT EXISTS telemetry (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp       TEXT    NOT NULL,
    status          INTEGER NOT NULL,   -- UPSStatus_BF raw value
    charge_pct      REAL,
    runtime_sec     INTEGER,
    battery_voltage REAL,
    load_pct        REAL,
    output_voltage  REAL,
    output_frequency REAL,
    output_current  REAL,
    input_voltage   REAL,
    efficiency      REAL,
    temperature     REAL
);

CREATE INDEX IF NOT EXISTS idx_telemetry_ts ON telemetry(timestamp);
