-- Shutdown v2: confirmation methods, delays, configurable final phases

-- Target: decouple confirmation from action, add post-confirm delay
ALTER TABLE shutdown_targets ADD COLUMN confirm_method TEXT NOT NULL DEFAULT 'ping';
ALTER TABLE shutdown_targets ADD COLUMN confirm_port INTEGER;
ALTER TABLE shutdown_targets ADD COLUMN confirm_command TEXT;
ALTER TABLE shutdown_targets ADD COLUMN post_confirm_delay INTEGER NOT NULL DEFAULT 15;

-- Group: hard timeout ceiling and post-group delay
ALTER TABLE shutdown_groups ADD COLUMN max_timeout_sec INTEGER NOT NULL DEFAULT 0;
ALTER TABLE shutdown_groups ADD COLUMN post_group_delay INTEGER NOT NULL DEFAULT 0;
