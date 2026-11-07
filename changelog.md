# Changelog

## v0.4.0
- Add an opt-in Metal/OpenGL ES interop resource probe using a Core Video pixel buffer with both compatibility flags enabled.
- Create Metal and OpenGL ES texture views from the same pixel buffer during the active Cocos draw callback.
- Validate the OpenGL ES texture in the current game context and report the result in the live HUD and persistent diagnostics.
- Link CoreVideo and OpenGLES frameworks. This is an interop foundation test only; it does not change rendering or apply visibility results.


## v0.3.9
- Use object-layer-local bounds directly when a decoration is already parented to the object layer, avoiding an unnecessary world-space round trip.
- Add a clearly labeled detached-object bounds fallback for entries in PlayLayer::m_objects that have no Cocos parent; this is measurement-only and never affects rendering.
- Track direct-layer, transformed-parent, detached-fallback, and invalid bounds separately in the HUD and persistent diagnostics.
- Keep the GameObject render hook removed and preserve safe measurement-only behavior.


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
