-- Generic parameter control: weather can control any writable UPS register
-- instead of being hardcoded to frequency tolerance.
--
-- control_register: name from the driver's config_regs (e.g., "freq_tolerance")
-- severe_raw_value: raw uint16 value to write during severe weather
-- normal_raw_value: fallback restore value when clear (used if no saved value)
ALTER TABLE weather_config ADD COLUMN control_register TEXT NOT NULL DEFAULT 'freq_tolerance';
ALTER TABLE weather_config ADD COLUMN severe_raw_value INTEGER;
ALTER TABLE weather_config ADD COLUMN normal_raw_value INTEGER;
