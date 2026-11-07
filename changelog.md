# Changelog

## v0.3.8
- Added a bundled 256×256 root-level logo.png mod icon.
- Removed the GameObject::visit render interception completely.
- Removed the per-object GameObject pointer cache from safe mode.
- Changed GPU visibility results to measurement-only; normal Geometry Dash drawing remains unchanged.
- Attached the measurement driver to the PlayLayer HUD lifetime rather than the global running scene.
- Added persistent chronological crash breadcrumbs in the Geode mod save directory, with a rotating 1 MiB log.
- Added level-scan filter counts, device/settings records, HUD and driver heartbeats, and Metal setup/dispatch/completion event logging.
- Added asynchronous Metal completion failure counters and last-error reporting.
- Increased HUD readability and made update and driver counters visible.
