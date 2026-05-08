-- Persisted history of register-2 (transfer reason) transitions captured
-- by the fast-poll loop.
--
-- Why this exists: the fast-poll ring (src/monitor/xfer_ring.c) is the
-- only thing that sees brief mains glitches that resolve before the slow
-- poll lands. When a transition was annotation-only, the ring is volatile
-- and lost on restart — leaving us with no forensic record of sub-poll
-- power events. This table makes those transitions durable.
--
-- One row per transition (xfer_ring's de-duplication still applies — the
-- ring rejects no-op duplicates upstream of this insert). 7-day rolling
-- retention pruned by the daily config-snapshot job in the monitor.
--
-- status_bits is the UPS status register at the same fast tick. Lets a
-- post-mortem distinguish "register 2 reported a reason while UPS stayed
-- Online" (firmware noise / AVR ridethrough) from "register 2 reported
-- a reason while UPS was on battery" (actual transient transfer that the
-- slow poll missed). NULL on drivers that don't yet populate it.
CREATE TABLE IF NOT EXISTS xfer_history (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp   TEXT    NOT NULL,           -- "YYYY-MM-DD HH:MM:SS.mmm" UTC
    reason_code INTEGER NOT NULL,           -- raw register 2 value
    reason_str  TEXT    NOT NULL,           -- decoded via ups_transfer_reason_str
    status_bits INTEGER                     -- UPS_ST_* mask, NULL when not captured
);
CREATE INDEX IF NOT EXISTS idx_xfer_history_ts ON xfer_history(timestamp);
