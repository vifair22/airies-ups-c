-- Add outlet group states and cumulative energy to telemetry
ALTER TABLE telemetry ADD COLUMN outlet_mog INTEGER;
ALTER TABLE telemetry ADD COLUMN outlet_sog0 INTEGER;
ALTER TABLE telemetry ADD COLUMN outlet_sog1 INTEGER;
ALTER TABLE telemetry ADD COLUMN output_energy_wh INTEGER;
