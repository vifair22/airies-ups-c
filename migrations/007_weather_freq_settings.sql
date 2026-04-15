-- Add configurable frequency tolerance settings for weather-triggered mode changes
ALTER TABLE weather_config ADD COLUMN severe_freq_setting TEXT NOT NULL DEFAULT 'hz60_0_1';
ALTER TABLE weather_config ADD COLUMN normal_freq_setting TEXT NOT NULL DEFAULT 'auto';
