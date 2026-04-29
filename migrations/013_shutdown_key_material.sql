-- shutdown_targets.credential semantics changed for ssh_key:
-- previously a filesystem path to a private key, now the key material itself.
-- Existing rows hold paths that are meaningless under the new code, so blank
-- them and force operators to re-enter the key via the UI on first save.

UPDATE shutdown_targets SET credential = '' WHERE method = 'ssh_key';
