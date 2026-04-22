-- Last-known UPS status snapshot — single row, UPSERT on every status change.
--
-- Purpose: when the daemon starts, the monitor and alert engine compare the
-- current UPS reading against this snapshot to detect transitions that
-- happened (or were already active) while the daemon was offline. Without
-- this, edge-triggered detection silently misses any fault that's already
-- present at the very first poll because the in-memory "previous" state is
-- zeroed at boot. See src/monitor/status_snapshot.h for the contract.
--
-- The CHECK (id = 1) constraint enforces a single-row UPSERT pattern; we
-- only ever care about the most recent state.
CREATE TABLE IF NOT EXISTS ups_status_snapshot (
    id                  INTEGER PRIMARY KEY CHECK (id = 1),

    -- Diff-relevant fields populated by the driver on every read. These
    -- are exactly the fields the monitor's transition-detection block and
    -- the alerts engine compare across polls.
    status              INTEGER NOT NULL,   -- ups_data.status (uint32, UPS_ST_* bits)
    bat_system_error    INTEGER NOT NULL,   -- ups_data.bat_system_error (uint16, UPS_BATERR_* bits)
    general_error       INTEGER NOT NULL,   -- ups_data.general_error (uint16, UPS_GENERR_* bits)
    power_system_error  INTEGER NOT NULL,   -- ups_data.power_system_error (uint32, UPS_PWRERR_* bits)
    bat_lifetime_status INTEGER NOT NULL,   -- ups_data.bat_lifetime_status (uint16, raw)

    -- When this snapshot was last written. ISO-8601 UTC ("YYYY-MM-DD HH:MM:SS").
    -- Useful in logs and for the optional "discovered active for ~Nh" copy.
    updated_at          TEXT    NOT NULL
);
